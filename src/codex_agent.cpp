#include "codex_agent.hpp"

#include "diff.hpp"
#include "sandbox_mcp.hpp"
#include "util.hpp"

namespace {

const size_t kMaxOutput = 16 * 1024;
const size_t kMaxDiff = 64 * 1024;

std::string idKey(const Json& id) { return id.isString() ? "s:" + id.str() : "n:" + std::to_string(id.i64()); }

std::string compact(const Json& j, size_t max = 400) { return util::truncateUtf8(j.dump(), max); }

Json fileEntries(const Json& changes, const std::wstring& cwd) {
    Json files = Json::array();
    for (const auto& c : changes.items()) {
        std::string kind = c["kind"]["type"].str("update");
        DiffStat st;
        std::string diff = normalizeCodexDiff(kind, c["diff"].str(), &st);
        std::string path = c["path"].str();
        if (kind == "update" && c["kind"]["move_path"].isString()) {
            path += " → " + c["kind"]["move_path"].str();
        }
        files.push(J({{"path", util::displayPath(path, cwd)},
                      {"change", kind},
                      {"added", st.added},
                      {"removed", st.removed},
                      {"diff", util::truncateUtf8(diff, kMaxDiff)},
                      {"truncated", diff.size() > kMaxDiff}}));
    }
    return files;
}

Json approvalOptions(bool withAbort) {
    Json o = Json::array();
    o.push(J({{"id", choice::kAllow}, {"label", "允許"}, {"style", "primary"}}));
    o.push(J({{"id", choice::kAllowSession}, {"label", "本次對話都允許"}, {"style", "secondary"}}));
    o.push(J({{"id", choice::kDeny}, {"label", "拒絕"}, {"style", "danger"}}));
    if (withAbort) o.push(J({{"id", choice::kAbort}, {"label", "拒絕並停止"}, {"style", "secondary"}}));
    return o;
}

}  // namespace

// ---------------------------------------------------------------- CodexServer

CodexServer::CodexServer(std::wstring exe) : exe_(std::move(exe)) {}

CodexServer::~CodexServer() { stop(); }

bool CodexServer::start(std::string* error) {
    if (running()) return true;
    ready_ = false;
    ProcessOptions opt;
    opt.exe = exe_;
    opt.args = {L"app-server"};
    // Never the home folder: Codex's Windows sandbox grants itself write access to the
    // workspace it is given, and a server-level fallback to $HOME would cover everything there.
    opt.cwd = util::joinPath(util::localAppDataDir(), L"codex-server");
    util::ensureDir(opt.cwd);
    auto proc = std::make_unique<ChildProcess>();
    ChildProcess* raw = proc.get();
    std::weak_ptr<CodexServer> self = shared_from_this();
    bool ok = proc->start(
        opt,
        [self, raw](const std::string& line) {
            postToUi([self, raw, line] {
                auto s = self.lock();
                if (s && s->proc_.get() == raw) s->handleLine(line);
            });
        },
        [self, raw](const std::string& line) {
            postToUi([self, raw, line] {
                auto s = self.lock();
                if (s && s->proc_.get() == raw) s->stderrTail_ = util::tailUtf8(s->stderrTail_ + line + "\n", 4000);
            });
        },
        [self, raw](unsigned long code) {
            postToUi([self, raw, code] {
                auto s = self.lock();
                if (!s || s->proc_.get() != raw) return;
                s->ready_ = false;
                auto replies = std::move(s->replies_);
                s->replies_.clear();
                Json err = J({{"code", -1}, {"message", "Codex 程序已結束"}});
                for (auto& r : replies) {
                    if (r.second) r.second(Json(), err);
                }
                std::string reason = "Codex app-server 結束（代碼 " + std::to_string(code) + "）";
                std::string tail = util::trim(s->stderrTail_);
                if (!tail.empty()) reason += "：" + util::tailUtf8(tail, 600);
                if (s->onExit) s->onExit(reason);
            });
        },
        error);
    if (!ok) return false;
    proc_ = std::move(proc);

    long long id = nextId_++;
    std::weak_ptr<CodexServer> weak = shared_from_this();
    replies_[id] = [weak](const Json&, const Json& err) {
        auto s = weak.lock();
        if (!s) return;
        if (!err.isNull()) {
            if (s->onExit) s->onExit("Codex 初始化失敗：" + err["message"].str());
            return;
        }
        s->write(J({{"method", "initialized"}}));
        s->ready_ = true;
        while (!s->queued_.empty()) {
            s->write(s->queued_.front());
            s->queued_.pop_front();
        }
        if (s->onReady) s->onReady();
    };
    write(J({{"id", id},
             {"method", "initialize"},
             {"params", J({{"clientInfo", J({{"name", "cca"}, {"title", "CCA"}, {"version", "0.1.0"}})}})}}));
    return true;
}

void CodexServer::stop() {
    if (proc_) {
        proc_->closeStdin();
        proc_.reset();
    }
    ready_ = false;
}

void CodexServer::write(const Json& msg) {
    if (proc_) proc_->writeLine(msg.dump());
}

void CodexServer::request(const std::string& method, Json params, Reply reply) {
    long long id = nextId_++;
    if (reply) replies_[id] = std::move(reply);
    Json msg = J({{"id", id}, {"method", method}, {"params", params.isNull() ? Json::object() : std::move(params)}});
    if (!ready_) queued_.push_back(std::move(msg));
    else write(msg);
}

void CodexServer::respond(const Json& id, Json result) { write(J({{"id", id}, {"result", std::move(result)}})); }

void CodexServer::respondError(const Json& id, int code, const std::string& message) {
    write(J({{"id", id}, {"error", J({{"code", code}, {"message", message}})}}));
}

void CodexServer::setThreadHandler(const std::string& threadId, Handler h) { threads_[threadId] = std::move(h); }

void CodexServer::removeThreadHandler(const std::string& threadId) { threads_.erase(threadId); }

void CodexServer::handleLine(const std::string& line) {
    if (line.empty() || line[0] != '{') return;
    Json msg = Json::parse(line);
    if (!msg.isObject()) return;
    bool hasId = msg.has("id") && !msg["id"].isNull();
    if (hasId && (msg.has("result") || msg.has("error")) && !msg.has("method")) {
        auto it = replies_.find(msg["id"].i64());
        if (it != replies_.end()) {
            Reply r = std::move(it->second);
            replies_.erase(it);
            if (r) r(msg["result"], msg["error"]);
        }
        return;
    }
    std::string method = msg["method"].str();
    const Json& params = msg["params"];
    std::string threadId = params["threadId"].str();
    auto it = threadId.empty() ? threads_.end() : threads_.find(threadId);
    if (it != threads_.end()) {
        Handler h = it->second;  // the handler may remove itself
        h(method, params, hasId ? msg["id"] : Json());
        return;
    }
    if (hasId) {
        respondError(msg["id"], -32601, "CCA does not handle " + method);
        return;
    }
    if (onGlobal) onGlobal(method, params);
}

// ---------------------------------------------------------------- CodexThread

CodexThread::CodexThread(std::shared_ptr<CodexServer> server, AgentEventFn onEvent)
    : server_(std::move(server)), onEvent_(std::move(onEvent)) {}

CodexThread::~CodexThread() { shutdown(); }

Json CodexThread::sandboxPolicy(const std::string& preset) {
    if (preset == "bypass") return J({{"type", "dangerFullAccess"}});
    if (preset == "edit") {
        return J({{"type", "workspaceWrite"}, {"writableRoots", Json::array()}, {"networkAccess", false},
                  {"excludeTmpdirEnvVar", false}, {"excludeSlashTmp", false}});
    }
    return J({{"type", "readOnly"}, {"networkAccess", false}});
}

std::string CodexThread::sandboxMode(const std::string& preset) {
    if (preset == "bypass") return "danger-full-access";
    if (preset == "edit") return "workspace-write";
    return "read-only";
}

std::string CodexThread::approvalPolicy(const std::string& preset) { return preset == "bypass" ? "never" : "on-request"; }

void CodexThread::emit(AgentEvent::Kind kind, const std::string& id, Json data) {
    if (onEvent_) onEvent_(AgentEvent{kind, id, std::move(data)});
}

void CodexThread::configure(const AgentConfig& cfg) {
    bool rebind = !threadId_.empty() && (cfg.cwd != cfg_.cwd || cfg.instructions != cfg_.instructions);
    cfg_ = cfg;
    if (threadId_.empty() && !cfg.resumeId.empty()) threadId_ = cfg.resumeId;
    // Model, effort and permissions are sent with every turn, so only folder or role
    // changes need the thread to be resumed again with new settings.
    if (rebind) needsRebind_ = true;
    if (needsRebind_ && !busy_) {
        server_->removeThreadHandler(threadId_);
        binding_ = false;
        needsRebind_ = false;
    }
}

Json CodexThread::threadParams() const {
    Json p = J({{"cwd", util::narrow(cfg_.cwd)},
                {"approvalPolicy", approvalPolicy(cfg_.permission)},
                {"sandbox", sandboxMode(cfg_.permission)}});
    if (!cfg_.model.empty()) p["model"] = cfg_.model;
    // An explicit empty override clears CCA's prior rules when the user turns
    // humanize off in a DM with no persona. Omitting it can retain old rules.
    p["developerInstructions"] = cfg_.instructions;
    // Codex's own Windows sandbox is the part that keeps failing, so it also gets CCA's Docker sandbox as a tool.
    if (cfg_.sandbox.on()) p["config"] = codexSandboxConfig(cfg_);
    return p;
}

void CodexThread::ensureThread(std::function<void(bool, const std::string&)> then) {
    // Settings may change during a turn. Keep that turn's handler until it ends,
    // then resume with the new instructions before sending the next message.
    if (needsRebind_) {
        server_->removeThreadHandler(threadId_);
        binding_ = false;
        needsRebind_ = false;
    }
    if (binding_) {
        then(true, "");
        return;
    }
    std::weak_ptr<CodexThread> self = shared_from_this();
    auto bind = [self](const std::string& id) {
        auto s = self.lock();
        if (!s) return;
        s->threadId_ = id;
        s->binding_ = true;
        std::weak_ptr<CodexThread> w = s;
        s->server_->setThreadHandler(id, [w](const std::string& m, const Json& p, const Json& rid) {
            if (auto t = w.lock()) t->handle(m, p, rid);
        });
        s->emit(AgentEvent::Kind::SessionInfo, "", J({{"threadId", id}}));
    };
    auto startNew = [self, bind, then] {
        auto s = self.lock();
        if (!s) return;
        s->server_->request("thread/start", s->threadParams(), [self, bind, then](const Json& r, const Json& e) {
            if (!e.isNull()) {
                then(false, "無法建立 Codex 對話：" + e["message"].str());
                return;
            }
            bind(r["thread"]["id"].str());
            if (auto s2 = self.lock()) {
                s2->emit(AgentEvent::Kind::SessionInfo, "", J({{"model", r["model"].str()}}));
            }
            then(true, "");
        });
    };
    if (threadId_.empty()) {
        startNew();
        return;
    }
    Json params = threadParams();
    params["threadId"] = threadId_;
    params["excludeTurns"] = true;
    server_->request("thread/resume", params, [self, bind, startNew, then](const Json& r, const Json& e) {
        auto s = self.lock();
        if (!s) return;
        if (!e.isNull()) {
            s->emit(AgentEvent::Kind::Notice, "",
                    J({{"text", "找不到先前的 GPT 對話紀錄，已開始新的對話。"}, {"level", "warn"}}));
            s->threadId_.clear();
            startNew();
            return;
        }
        bind(r["thread"]["id"].str(s->threadId_));
        then(true, "");
    });
}

bool CodexThread::send(const std::string& text, std::string* error) {
    if (busy_) {
        if (error) *error = "GPT 還在處理上一則訊息";
        return false;
    }
    if (!server_->running()) {
        std::string err;
        if (!server_->start(&err)) {
            if (error) *error = err;
            return false;
        }
    }
    busy_ = true;
    interrupted_ = false;
    turnId_.clear();
    lastUsage_ = Json();
    emit(AgentEvent::Kind::TurnStarted, "", Json::object());
    std::weak_ptr<CodexThread> self = shared_from_this();
    ensureThread([self, text](bool ok, const std::string& err) {
        auto s = self.lock();
        if (!s) return;
        if (!ok) {
            s->busy_ = false;
            s->emit(AgentEvent::Kind::TurnDone, "", J({{"status", "failed"}, {"error", err}}));
            return;
        }
        s->startTurn(text);
    });
    return true;
}

void CodexThread::startTurn(const std::string& text) {
    std::string body = text;
    if (cfg_.permission == "plan") {
        body = "（規劃模式：只閱讀和分析程式碼並提出計畫，不要修改任何檔案。）\n\n" + text;
    }
    Json input = Json::array();
    input.push(J({{"type", "text"}, {"text", body}, {"text_elements", Json::array()}}));
    Json params = J({{"threadId", threadId_},
                     {"input", input},
                     {"cwd", util::narrow(cfg_.cwd)},
                     {"approvalPolicy", approvalPolicy(cfg_.permission)},
                     {"sandboxPolicy", sandboxPolicy(cfg_.permission)}});
    if (!cfg_.model.empty()) params["model"] = cfg_.model;
    if (!cfg_.effort.empty()) params["effort"] = cfg_.effort;
    std::weak_ptr<CodexThread> self = shared_from_this();
    server_->request("turn/start", params, [self](const Json& r, const Json& e) {
        auto s = self.lock();
        if (!s) return;
        if (!e.isNull()) {
            s->busy_ = false;
            s->emit(AgentEvent::Kind::TurnDone, "", J({{"status", "failed"}, {"error", e["message"].str("turn/start 失敗")}}));
            return;
        }
        if (s->turnId_.empty()) s->turnId_ = r["turn"]["id"].str();
    });
}

void CodexThread::interrupt() {
    if (!busy_ || threadId_.empty()) return;
    interrupted_ = true;
    if (!turnId_.empty()) server_->request("turn/interrupt", J({{"threadId", threadId_}, {"turnId", turnId_}}));
}

void CodexThread::shutdown() {
    if (!threadId_.empty()) server_->removeThreadHandler(threadId_);
    binding_ = false;
    busy_ = false;
}

void CodexThread::serverLost() {
    binding_ = false;
    for (auto& p : pending_) emit(AgentEvent::Kind::Resolved, p.first, J({{"state", "cancelled"}}));
    pending_.clear();
    if (busy_) {
        busy_ = false;
        emit(AgentEvent::Kind::TurnDone, "", J({{"status", "failed"}, {"error", "Codex 程序意外結束"}}));
    }
}

void CodexThread::handle(const std::string& method, const Json& p, const Json& rid) {
    using K = AgentEvent::Kind;
    if (!rid.isNull()) {
        std::string key = idKey(rid);
        if (method == "item/commandExecution/requestApproval") {
            Json card = J({{"agent", "codex"}, {"kind", "command"}, {"title", "GPT 想要執行指令"},
                           {"command", p["command"].str()}, {"cwd", util::displayPath(p["cwd"].str(), cfg_.cwd)}});
            if (p["networkApprovalContext"].isObject()) {
                card["title"] = "GPT 想要連上網路";
                card["detail"] = compact(p["networkApprovalContext"]);
            }
            if (!p["reason"].str().empty()) card["reason"] = p["reason"].str();
            card["options"] = approvalOptions(true);
            pending_[key] = PendingApproval{rid, "command", Json()};
            emit(K::Approval, key, card);
        } else if (method == "item/fileChange/requestApproval") {
            Json card = J({{"agent", "codex"}, {"kind", "edit"}, {"title", "GPT 想要修改檔案"},
                           {"files", fileEntries(fileChanges_[p["itemId"].str()], cfg_.cwd)}});
            if (!p["reason"].str().empty()) card["reason"] = p["reason"].str();
            if (!p["grantRoot"].str().empty()) card["note"] = "同時要求寫入 " + p["grantRoot"].str() + " 的權限";
            card["options"] = approvalOptions(true);
            pending_[key] = PendingApproval{rid, "file", Json()};
            emit(K::Approval, key, card);
        } else if (method == "item/permissions/requestApproval") {
            Json card = J({{"agent", "codex"}, {"kind", "permission"}, {"title", "GPT 要求額外權限"},
                           {"detail", compact(p["permissions"], 800)}});
            if (!p["reason"].str().empty()) card["reason"] = p["reason"].str();
            card["options"] = approvalOptions(false);
            pending_[key] = PendingApproval{rid, "permissions", p["permissions"]};
            emit(K::Approval, key, card);
        } else if (method == "item/tool/requestUserInput") {
            Json qs = Json::array();
            for (const auto& q : p["questions"].items()) {
                Json opts = Json::array();
                for (const auto& o : q["options"].items()) {
                    opts.push(J({{"label", o["label"].str()}, {"description", o["description"].str()}}));
                }
                qs.push(J({{"id", q["id"].str()}, {"header", q["header"].str()}, {"question", q["question"].str()},
                           {"multiSelect", false}, {"options", opts}, {"isOther", q["isOther"].boolean()},
                           {"isSecret", q["isSecret"].boolean()}}));
            }
            pending_[key] = PendingApproval{rid, "question", qs};
            emit(K::Question, key, J({{"agent", "codex"}, {"questions", qs}}));
        } else if (method == "mcpServer/elicitation/request") {
            server_->respond(rid, J({{"action", "decline"}}));
            emit(K::Notice, "", J({{"text", "GPT 的 MCP 伺服器要求填寫資料，CCA 目前不支援，已自動拒絕。"}, {"level", "warn"}}));
        } else {
            server_->respondError(rid, -32601, "CCA does not handle " + method);
        }
        return;
    }

    if (method == "turn/started") {
        turnId_ = p["turn"]["id"].str(turnId_);
    } else if (method == "item/started") {
        handleItem(p["item"], false);
    } else if (method == "item/completed") {
        handleItem(p["item"], true);
    } else if (method == "item/agentMessage/delta") {
        emit(K::TextDelta, p["itemId"].str(), J({{"delta", p["delta"].str()}}));
    } else if (method == "item/reasoning/summaryTextDelta" || method == "item/reasoning/textDelta") {
        emit(K::Thinking, "", Json::object());
    } else if (method == "item/commandExecution/outputDelta") {
        emit(K::ToolOutput, p["itemId"].str(), J({{"delta", p["delta"].str()}}));
    } else if (method == "turn/plan/updated") {
        Json items = Json::array();
        for (const auto& step : p["plan"].items()) {
            items.push(J({{"text", step["step"].str()}, {"status", step["status"].str()}}));
        }
        emit(K::Todo, "", J({{"items", items}}));
    } else if (method == "thread/tokenUsage/updated") {
        lastUsage_ = p["tokenUsage"];
    } else if (method == "turn/completed") {
        const Json& turn = p["turn"];
        const Json& last = lastUsage_["last"];
        if (lastUsage_.isObject()) {
            emit(K::Usage, "",
                 J({{"inputTokens", last["inputTokens"].i64()},
                    {"cachedTokens", last["cachedInputTokens"].i64()},
                    {"outputTokens", last["outputTokens"].i64()},
                    {"contextUsed", last["inputTokens"].i64() + last["outputTokens"].i64()},
                    {"contextWindow", lastUsage_["modelContextWindow"].i64()},
                    {"durationMs", turn["durationMs"].i64()}}));
        }
        std::string status = turn["status"].str("completed");
        if (interrupted_ && status != "completed") status = "interrupted";
        busy_ = false;
        interrupted_ = false;
        for (auto& pa : pending_) emit(K::Resolved, pa.first, J({{"state", "cancelled"}}));
        pending_.clear();
        fileChanges_.clear();
        emit(K::TurnDone, "", J({{"status", status}, {"error", turn["error"]["message"].str()}, {"turnId", turn["id"].str(turnId_)}}));
    } else if (method == "error") {
        const Json& e = p["error"];
        std::string text = e["message"].str("Codex 發生錯誤");
        if (!e["additionalDetails"].str().empty()) text += "\n" + e["additionalDetails"].str();
        if (p["willRetry"].boolean()) emit(K::Retry, "", J({{"error", text}}));
        else
            emit(K::Error, "",
                 J({{"text", text}, {"code", e["codexErrorInfo"].isString() ? e["codexErrorInfo"].str() : ""}}));
    } else if (method == "model/rerouted") {
        emit(K::Notice, "", J({{"text", "Codex 改用模型 " + p["toModel"].str(p["model"].str()) + "。"}, {"level", "info"}}));
    } else if (method == "thread/compacted") {
        emit(K::Notice, "", J({{"text", "對話太長，GPT 已自動壓縮較早的內容。"}, {"level", "info"}}));
    } else if (method == "warning" || method == "guardianWarning" || method == "configWarning") {
        std::string text = p["message"].str(p["summary"].str());
        if (!text.empty()) emit(K::Notice, "", J({{"text", text}, {"level", "warn"}}));
    } else if (method == "serverRequest/resolved") {
        std::string key = idKey(p["requestId"]);
        if (pending_.erase(key)) emit(K::Resolved, key, J({{"state", "cancelled"}}));
    }
}

void CodexThread::handleItem(const Json& item, bool completed) {
    using K = AgentEvent::Kind;
    std::string type = item["type"].str();
    std::string id = item["id"].str();
    if (type == "agentMessage") {
        if (completed) emit(K::Text, id, J({{"text", item["text"].str()}}));
    } else if (type == "reasoning") {
        if (!completed) emit(K::Thinking, "", Json::object());
    } else if (type == "commandExecution") {
        if (!completed) {
            emit(K::ToolStart, id,
                 J({{"kind", "command"}, {"tool", "shell"}, {"title", "執行指令"}, {"command", item["command"].str()},
                    {"cwd", util::displayPath(item["cwd"].str(), cfg_.cwd)}}));
        } else {
            std::string st = item["status"].str();
            long long code = item["exitCode"].i64(-1);
            std::string status = st == "declined" ? "declined" : (st == "failed" || code != 0 ? "error" : "ok");
            std::string out = item["aggregatedOutput"].str();
            Json end = J({{"status", status}, {"output", util::tailUtf8(out, kMaxOutput)},
                          {"durationMs", item["durationMs"].i64()}});
            if (item["exitCode"].isNumber()) end["exitCode"] = code;
            emit(K::ToolEnd, id, end);
            if (!sandboxWarned_ && out.find("Failed to create unified exec process") != std::string::npos) {
                sandboxWarned_ = true;
                emit(K::Notice, "",
                     J({{"text", "GPT 的 Windows 沙箱沒辦法啟動指令（" + util::truncateUtf8(util::trim(out), 160) +
                                     "）。常見原因是 Codex 桌面版正在執行並鎖住了它的檔案：關閉 Codex 桌面版後再試，"
                                     "或把權限模式改成「每次詢問」，讓指令經你核准後在沙箱外執行。"},
                        {"level", "warn"}}));
            }
        }
    } else if (type == "fileChange") {
        if (!completed) {
            fileChanges_[id] = item["changes"];
            return;
        }
        std::string st = item["status"].str();
        if (st == "declined") {
            emit(K::Notice, "", J({{"text", "已拒絕的檔案修改沒有套用。"}, {"level", "info"}}));
            return;
        }
        if (st == "failed") {
            emit(K::Error, "", J({{"text", "GPT 套用檔案修改失敗。"}}));
            return;
        }
        Json files = fileEntries(item["changes"], cfg_.cwd);
        for (size_t i = 0; i < files.size(); ++i) emit(K::FileChange, id + ":" + std::to_string(i), files.at(i));
    } else if (type == "mcpToolCall") {
        if (!completed) {
            Json args = item["arguments"];
            if (args.isString()) args = Json::parse(args.str());
            std::string command;
            if (item["server"].str() == kSandboxServer && item["tool"].str() == "sandbox_run")
                for (const auto& arg : args["command"].items()) command += (command.empty() ? "" : " ") + arg.str();
            emit(K::ToolStart, id,
                 J({{"kind", "mcp"},
                    {"tool", item["tool"].str()},
                    {"title", "MCP 工具"},
                    {"detail", item["server"].str() + " · " + item["tool"].str()},
                    {"input", compact(item["arguments"])},
                    {"command", command}}));
        } else {
            bool failed = item["status"].str() == "failed" || item["result"]["isError"].boolean();
            std::string out = failed ? item["error"]["message"].str() : compact(item["result"]["content"], kMaxOutput);
            if (out.empty()) out = compact(item["result"]["content"], kMaxOutput);
            Json end = J({{"status", failed ? "error" : "ok"}, {"output", out}});
            if (item["server"].str() == kSandboxServer && item["tool"].str() == "sandbox_run") {
                Json sandbox = sandboxCompletion(item["result"]["content"]);
                if (sandbox.isObject()) {
                    end["exitCode"] = sandbox["exitCode"];
                    if (sandbox["status"].str() == "error") end["status"] = "error";
                }
            }
            emit(K::ToolEnd, id, end);
        }
    } else if (type == "webSearch") {
        if (!completed) {
            emit(K::ToolStart, id, J({{"kind", "web"}, {"tool", "web_search"}, {"title", "搜尋網路"}, {"detail", item["query"].str()}}));
        } else {
            emit(K::ToolEnd, id, J({{"status", "ok"}, {"detail", item["query"].str()}}));
        }
    } else if (type == "imageView") {
        if (completed) {
            emit(K::ToolStart, id, J({{"kind", "read"}, {"tool", "view_image"}, {"title", "查看圖片"},
                                       {"path", util::displayPath(item["path"].str(), cfg_.cwd)}}));
            emit(K::ToolEnd, id, J({{"status", "ok"}}));
        }
    } else if (type == "contextCompaction" && completed) {
        emit(K::Notice, "", J({{"text", "GPT 已壓縮較早的對話內容。"}, {"level", "info"}}));
    }
}

void CodexThread::revert(const std::string& beforeTurnId, std::function<void(bool, const std::string&)> done) {
    if (threadId_.empty() || beforeTurnId.empty()) {
        done(true, "");
        return;
    }
    std::weak_ptr<CodexThread> self = shared_from_this();
    std::string turn = beforeTurnId;
    ensureThread([self, turn, done](bool ok, const std::string& err) {
        auto s = self.lock();
        if (!s) return;
        if (!ok) { done(false, err); return; }
        s->server_->request("thread/revert", J({{"threadId", s->threadId_}, {"beforeTurnId", turn}}),
                            [done](const Json&, const Json& e) { done(e.isNull(), e["message"].str()); });
    });
}

void CodexThread::respondApproval(const std::string& id, const std::string& ch, const std::string& feedback) {
    auto it = pending_.find(id);
    if (it == pending_.end()) return;
    PendingApproval p = it->second;
    pending_.erase(it);
    if (p.kind == "permissions") {
        bool allow = ch == choice::kAllow || ch == choice::kAllowSession;
        Json resp = J({{"permissions", allow ? p.questions : Json::object()}});
        if (allow) resp["scope"] = ch == choice::kAllowSession ? "session" : "turn";
        server_->respond(p.rpcId, resp);
    } else {
        std::string decision = ch == choice::kAllow          ? "accept"
                               : ch == choice::kAllowSession ? "acceptForSession"
                               : ch == choice::kAbort        ? "cancel"
                                                             : "decline";
        server_->respond(p.rpcId, J({{"decision", decision}}));
    }
    // Codex decisions carry no message, so a reason for declining is steered into the turn.
    if (ch == choice::kDeny && !feedback.empty() && !turnId_.empty()) {
        Json input = Json::array();
        input.push(J({{"type", "text"}, {"text", "我拒絕了剛才的操作，原因：" + feedback}, {"text_elements", Json::array()}}));
        server_->request("turn/steer", J({{"threadId", threadId_}, {"expectedTurnId", turnId_}, {"input", input}}));
    }
    emit(AgentEvent::Kind::Resolved, id, J({{"state", ch}, {"feedback", feedback}}));
}

void CodexThread::respondQuestion(const std::string& id, const Json& answers) {
    auto it = pending_.find(id);
    if (it == pending_.end()) return;
    PendingApproval p = it->second;
    pending_.erase(it);
    Json out = Json::object();
    for (const auto& q : p.questions.items()) {
        std::string qid = q["id"].str();
        const Json& a = answers.has(qid) ? answers[qid] : answers[q["question"].str()];
        Json list = Json::array();
        if (a.isArray()) {
            for (const auto& x : a.items()) list.push(x.str());
        } else if (a.isString()) {
            list.push(a.str());
        }
        out[qid] = J({{"answers", list}});
    }
    server_->respond(p.rpcId, J({{"answers", out}}));
    emit(AgentEvent::Kind::Resolved, id, J({{"state", "answered"}, {"answers", answers}}));
}
