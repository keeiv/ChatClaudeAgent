#include "claude_agent.hpp"

#include "diff.hpp"
#include "sandbox_mcp.hpp"
#include "takeover.hpp"
#include "util.hpp"

namespace {

const size_t kMaxOutput = 16 * 1024;
const size_t kMaxDiff = 64 * 1024;

// Variables a parent Claude Code session sets for its own children. Passing them on
// would make our CLI think it is nested inside that session.
const wchar_t* kParentSessionVars[] = {
    L"CLAUDECODE", L"CLAUDE_CODE_ENTRYPOINT", L"CLAUDE_CODE_SESSION_ID", L"CLAUDE_CODE_HOST_SESSION_ID",
    L"CLAUDE_CODE_CHILD_SESSION", L"CLAUDE_CODE_MESSAGING_SOCKET", L"CLAUDE_CODE_MESSAGING_TOKEN",
    L"CLAUDE_CODE_SDK_HAS_HOST_AUTH_REFRESH", L"CLAUDE_CODE_DESKTOP_APP_VERSION", L"CLAUDE_CODE_SESSION_ATTENDED",
    L"CLAUDE_CODE_ACCOUNT_UUID", L"CLAUDE_CODE_ORGANIZATION_UUID", L"CLAUDE_CODE_USER_EMAIL",
    L"CLAUDE_CODE_OAUTH_SCOPES", L"CLAUDE_CODE_TERMINAL_MCP_TOOLS", L"CLAUDE_CODE_REPORT_FINDINGS",
    L"CLAUDE_CODE_STARTUP_FAILURE_RESULTS", L"CLAUDE_CODE_EAGER_FLUSH", L"CLAUDE_CODE_EMIT_TOOL_USE_SUMMARIES",
    L"CLAUDE_CODE_ENABLE_SDK_FILE_CHECKPOINTING", L"CLAUDE_CODE_SIMPLE", L"CLAUDE_CODE_DISABLE_CRON",
    L"CLAUDE_CODE_DISABLE_TERMINAL_TITLE", L"CLAUDE_CODE_SUBSCRIPTION_TYPE", L"CLAUDE_CODE_RATE_LIMIT_TIER",
    L"CLAUDE_AGENT_SDK_*", L"CLAUDE_PID", L"CLAUDE_PREVIEW_CLASSIFIER_FLOOR",
    // CCA promises subscription use; an API key in the environment would bill the API instead.
    L"ANTHROPIC_API_KEY", L"ANTHROPIC_AUTH_TOKEN",
};

std::string toolResultText(const Json& content) {
    if (content.isString()) return content.str();
    std::string out;
    for (const auto& c : content.items()) {
        std::string t = c["type"].str();
        if (t == "text") {
            if (!out.empty()) out += "\n";
            out += c["text"].str();
        } else if (t == "image") {
            out += "[圖片]";
        }
    }
    return out;
}

using util::displayPath;

std::string compact(const Json& j, size_t max = 400) { return util::truncateUtf8(j.dump(), max); }

Json describeTool(const std::string& name, const Json& in, const std::wstring& cwd) {
    Json d = J({{"tool", name}});
    auto set = [&](const char* kind, const char* title) {
        d["kind"] = kind;
        d["title"] = title;
    };
    if (name == "Bash" || name == "PowerShell") {
        set("command", "執行指令");
        d["command"] = in["command"].str();
        d["detail"] = in["description"].str();
    } else if (name == "Read") {
        set("read", "讀取檔案");
        d["path"] = displayPath(in["file_path"].str(), cwd);
    } else if (name == "Glob") {
        set("search", "搜尋檔案");
        d["detail"] = in["pattern"].str();
    } else if (name == "Grep") {
        set("search", "搜尋內容");
        d["detail"] = in["pattern"].str();
    } else if (name == "Edit" || name == "MultiEdit") {
        set("edit", "編輯檔案");
        d["path"] = displayPath(in["file_path"].str(), cwd);
    } else if (name == "Write") {
        set("edit", "寫入檔案");
        d["path"] = displayPath(in["file_path"].str(), cwd);
    } else if (name == "NotebookEdit") {
        set("edit", "編輯筆記本");
        d["path"] = displayPath(in["notebook_path"].str(), cwd);
    } else if (name == "WebFetch") {
        set("web", "讀取網頁");
        d["detail"] = in["url"].str();
    } else if (name == "WebSearch") {
        set("web", "搜尋網路");
        d["detail"] = in["query"].str();
    } else if (name == "Task" || name == "Agent") {
        set("task", "交給子代理");
        d["detail"] = in["description"].str();
    } else if (name == "Skill") {
        set("tool", "使用技能");
        d["detail"] = in["skill"].str();
    } else if (util::startsWith(name, "mcp__")) {
        set("mcp", "MCP 工具");
        std::string rest = name.substr(5);
        size_t sep = rest.find("__");
        d["detail"] = sep == std::string::npos ? rest : rest.substr(0, sep) + " · " + rest.substr(sep + 2);
        d["input"] = compact(in);
        if (name == std::string("mcp__") + kSandboxServer + "__sandbox_run") {
            std::string command;
            for (const auto& arg : in["command"].items()) command += (command.empty() ? "" : " ") + arg.str();
            d["command"] = command;
        }
    } else {
        set("tool", "工具");
        d["detail"] = name;
        d["input"] = compact(in);
    }
    return d;
}

// Human label for the "always allow in this conversation" button.
std::string sessionAllowLabel(const Json& suggestions) {
    for (const auto& s : suggestions.items()) {
        std::string type = s["type"].str();
        if (type == "setMode" && s["mode"].str() == "acceptEdits") return "本次對話自動接受編輯";
        if (type == "addRules" && s["rules"].size() > 0) {
            const Json& r = s["rules"].at(0);
            std::string rule = r["toolName"].str();
            if (!r["ruleContent"].str().empty()) rule += "(" + r["ruleContent"].str() + ")";
            return "本次對話都允許 " + util::truncateUtf8(rule, 60);
        }
        if (type == "addDirectories") return "本次對話允許存取此資料夾";
    }
    return "本次對話都允許";
}

Json todoItems(const Json& todos) {
    Json items = Json::array();
    for (const auto& t : todos.items()) {
        std::string status = t["status"].str();
        if (status == "in_progress") status = "inProgress";
        items.push(J({{"text", t["content"].str()}, {"status", status}}));
    }
    return items;
}

std::string assistantErrorText(const std::string& code) {
    if (code == "authentication_failed") return "Claude 登入已失效，請到設定重新登入。";
    if (code == "billing_error") return "Claude 帳號的付款或方案有問題。";
    if (code == "rate_limit") return "已達 Claude 的用量上限，請等額度重置。";
    if (code == "overloaded" || code == "server_error") return "Claude 伺服器暫時無法回應。";
    if (code == "max_output_tokens") return "回覆超過長度上限，已截斷。";
    return "Claude 回報錯誤：" + code;
}

}  // namespace

ClaudeSession::ClaudeSession(std::wstring exe, AgentEventFn onEvent)
    : exe_(std::move(exe)), onEvent_(std::move(onEvent)) {}

ClaudeSession::~ClaudeSession() { shutdown(); }

std::vector<std::wstring> ClaudeSession::envToDrop() {
    return std::vector<std::wstring>(std::begin(kParentSessionVars), std::end(kParentSessionVars));
}

std::string ClaudeSession::permissionMode(const std::string& preset) {
    if (preset == "plan") return "plan";
    if (preset == "ask") return "default";
    if (preset == "bypass") return "bypassPermissions";
    return "acceptEdits";
}

void ClaudeSession::emit(AgentEvent::Kind kind, const std::string& id, Json data) {
    if (onEvent_) onEvent_(AgentEvent{kind, id, std::move(data)});
}

void ClaudeSession::configure(const AgentConfig& cfg) {
    AgentConfig old = cfg_;
    cfg_ = cfg;
    if (sessionId_.empty()) sessionId_ = cfg.resumeId;
    if (!alive()) return;
    if (cfg.cwd != runningCfg_.cwd || cfg.effort != runningCfg_.effort ||
        cfg.instructions != runningCfg_.instructions) {
        needsRestart_ = true;
    }
    if (cfg.model != old.model) {
        sendControl("set_model", J({{"model", cfg.model.empty() ? Json("default") : Json(cfg.model)}}));
        runningCfg_.model = cfg.model;
    }
    if (cfg.permission != old.permission) {
        sendControl("set_permission_mode", J({{"mode", permissionMode(cfg.permission)}}));
        runningCfg_.permission = cfg.permission;
    }
}

bool ClaudeSession::start(std::string* error) {
    if (alive() && !needsRestart_) return true;
    if (alive() && needsRestart_) {
        if (busy_) return true;  // restart once the current turn finishes
        proc_.reset();
    }
    needsRestart_ = false;

    ProcessOptions opt;
    opt.exe = exe_;
    opt.cwd = cfg_.cwd;
    opt.args = {L"-p", L"--input-format", L"stream-json", L"--output-format", L"stream-json", L"--verbose",
                L"--include-partial-messages", L"--permission-prompt-tool", L"stdio",
                L"--allow-dangerously-skip-permissions", L"--permission-mode",
                util::widen(permissionMode(cfg_.permission))};
    if (!cfg_.model.empty()) {
        opt.args.push_back(L"--model");
        opt.args.push_back(util::widen(cfg_.model));
    }
    if (!cfg_.effort.empty()) {
        opt.args.push_back(L"--effort");
        opt.args.push_back(util::widen(cfg_.effort));
    }
    if (freshStart_) {
        sessionId_.clear();
        freshStart_ = false;
    }
    if (!sessionId_.empty()) {
        opt.args.push_back(L"--resume");
        opt.args.push_back(util::widen(sessionId_));
        if (!resumeAt_.empty()) {
            opt.args.push_back(L"--resume-session-at");
            opt.args.push_back(util::widen(resumeAt_));
            opt.args.push_back(L"--fork-session");
        }
    }
    resumeAt_.clear();
    if (!cfg_.instructions.empty()) {
        std::wstring dir = util::joinPath(util::localAppDataDir(), L"prompts");
        util::ensureDir(dir);
        if (promptFile_.empty()) promptFile_ = util::joinPath(dir, util::widen("claude-" + util::newId() + ".md"));
        util::writeFileAtomic(promptFile_, cfg_.instructions);
        opt.args.push_back(L"--append-system-prompt-file");
        opt.args.push_back(promptFile_);
    }
    if (cfg_.sandbox.on()) {
        std::wstring dir = util::joinPath(util::localAppDataDir(), L"prompts");
        util::ensureDir(dir);
        if (mcpFile_.empty()) mcpFile_ = util::joinPath(dir, util::widen("claude-mcp-" + util::newId() + ".json"));
        // The sandbox only runs in a throwaway container, so its tools skip the per-call approval card.
        if (util::writeFileAtomic(mcpFile_, claudeSandboxConfig(cfg_).dump())) {
            opt.args.push_back(L"--mcp-config");
            opt.args.push_back(mcpFile_);
            opt.args.push_back(L"--allowedTools");
            opt.args.push_back(util::widen(std::string("mcp__") + kSandboxServer));
        }
    }
    opt.dropEnv = envToDrop();

    auto proc = std::make_unique<ChildProcess>();
    std::weak_ptr<ClaudeSession> self = shared_from_this();
    // Allocators may reuse a ChildProcess address before its queued exit reaches the UI.
    unsigned generation = ++processGeneration_;
    bool ok = proc->start(
        opt,
        [self, generation](const std::string& line) {
            postToUi([self, generation, line] {
                auto s = self.lock();
                if (s && s->processGeneration_ == generation && s->proc_) s->handleLine(line);
            });
        },
        [self, generation](const std::string& line) {
            postToUi([self, generation, line] {
                auto s = self.lock();
                if (s && s->processGeneration_ == generation && s->proc_)
                    s->stderrTail_ = util::tailUtf8(s->stderrTail_ + line + "\n", 4000);
            });
        },
        [self, generation](unsigned long code) {
            postToUi([self, generation, code] {
                auto s = self.lock();
                if (!s || s->processGeneration_ != generation || !s->proc_) return;
                bool wasBusy = s->busy_;
                s->busy_ = false;
                for (auto& p : s->pending_) s->emit(AgentEvent::Kind::Resolved, p.first, J({{"state", "cancelled"}}));
                s->pending_.clear();
                s->emit(AgentEvent::Kind::Exited, "", J({{"code", (long long)code}, {"stderr", s->stderrTail_}}));
                if (wasBusy) {
                    std::string err = util::trim(s->stderrTail_);
                    s->emit(AgentEvent::Kind::TurnDone, "",
                            J({{"status", "failed"},
                               {"quotaExceeded", s->quotaExceeded_},
                               {"error", err.empty() ? "Claude 程序意外結束（代碼 " + std::to_string(code) + "）"
                                                     : util::tailUtf8(err, 800)}}));
                }
            });
        },
        error);
    if (!ok) return false;
    proc_ = std::move(proc);
    runningCfg_ = cfg_;
    stderrTail_.clear();
    sendControl("initialize", Json::object());
    return true;
}

bool ClaudeSession::send(const std::string& text, std::string* error) {
    if (busy_) {
        if (error) *error = "Claude 還在處理上一則訊息";
        return false;
    }
    if (!start(error)) return false;
    Json msg = J({{"type", "user"},
                  {"message", J({{"role", "user"},
                                 {"content", Json(Json::Array{J({{"type", "text"}, {"text", text}})})}})},
                  {"parent_tool_use_id", nullptr}});
    if (!sessionId_.empty()) msg["session_id"] = sessionId_;
    if (!proc_->writeLine(msg.dump())) {
        if (error) *error = "無法傳送訊息給 Claude";
        return false;
    }
    busy_ = true;
    interrupted_ = false;
    openText_.clear();
    streamIndexToId_.clear();
    quotaExceeded_ = false;
    emit(AgentEvent::Kind::TurnStarted, "", Json::object());
    return true;
}

void ClaudeSession::requestMcpStatus() {
    sendControl("mcp_status", Json::object());
}

void ClaudeSession::interrupt() {
    if (!busy_ || !alive()) return;
    interrupted_ = true;
    sendControl("interrupt", Json::object());
}

void ClaudeSession::rewindTo(const std::string& uuid) {
    if (proc_) {
        proc_->closeStdin();
        proc_.reset();
    }
    busy_ = false;
    pending_.clear();
    if (uuid.empty()) freshStart_ = true;
    else resumeAt_ = uuid;
}

void ClaudeSession::shutdown() {
    if (proc_) {
        proc_->closeStdin();
        proc_.reset();
    }
    busy_ = false;
    if (!promptFile_.empty()) DeleteFileW(promptFile_.c_str());
    if (!mcpFile_.empty()) DeleteFileW(mcpFile_.c_str());
}

void ClaudeSession::sendControl(const std::string& subtype, Json extra) {
    if (!alive()) return;
    Json req = std::move(extra);
    if (!req.isObject()) req = Json::object();
    req["subtype"] = subtype;
    Json msg = J({{"type", "control_request"},
                  {"request_id", "cca-" + std::to_string(++controlSeq_) + "-" + subtype},
                  {"request", req}});
    proc_->writeLine(msg.dump());
}

void ClaudeSession::sendControlResponse(const std::string& requestId, Json response) {
    if (!alive()) return;
    Json msg = J({{"type", "control_response"},
                  {"response", J({{"subtype", "success"}, {"request_id", requestId}, {"response", response}})}});
    proc_->writeLine(msg.dump());
}

void ClaudeSession::handleLine(const std::string& line) {
    if (line.empty() || line[0] != '{') return;
    const Json msg = Json::parse(line);
    if (!msg.isObject()) return;
    std::string type = msg["type"].str();

    if (type == "stream_event") {
        if (msg["parent_tool_use_id"].isNull()) handleStreamEvent(msg["event"]);
    } else if (type == "assistant") {
        handleAssistant(msg);
    } else if (type == "user") {
        handleToolResults(msg);
    } else if (type == "result") {
        handleResult(msg);
    } else if (type == "system") {
        std::string sub = msg["subtype"].str();
        if (sub == "init") {
            sessionId_ = msg["session_id"].str(sessionId_);
            Json info = J({{"sessionId", sessionId_},
                           {"model", msg["model"].str()},
                           {"permissionMode", msg["permissionMode"].str()},
                           {"mcpServers", msg["mcp_servers"]},
                           {"plugins", msg["plugins"]}});
            emit(AgentEvent::Kind::SessionInfo, "", info);
        } else if (sub == "api_retry") {
            emit(AgentEvent::Kind::Retry, "",
                 J({{"attempt", msg["attempt"].i64()},
                    {"max", msg["max_retries"].i64()},
                    {"delayMs", msg["retry_delay_ms"].i64()},
                    {"error", msg["error"].str()},
                    {"status", msg["error_status"]}}));
        } else if (sub == "compact_boundary") {
            emit(AgentEvent::Kind::Notice, "", J({{"text", "對話太長，Claude 已自動壓縮較早的內容。"}, {"level", "info"}}));
        } else if (sub == "permission_denied") {
            emit(AgentEvent::Kind::Notice, "",
                 J({{"text", "權限模式拒絕了 " + msg["tool_name"].str("一個操作") + "。"}, {"level", "warn"}}));
        }
    } else if (type == "control_request") {
        const Json& req = msg["request"];
        std::string rid = msg["request_id"].str();
        if (req["subtype"].str() == "can_use_tool") {
            handlePermissionRequest(rid, req);
        } else {
            Json resp = J({{"type", "control_response"},
                           {"response", J({{"subtype", "error"}, {"request_id", rid},
                                           {"error", "CCA does not handle " + req["subtype"].str()}})}});
            if (alive()) proc_->writeLine(resp.dump());
        }
    } else if (type == "control_cancel_request") {
        std::string rid = msg["request_id"].str();
        if (pending_.erase(rid)) emit(AgentEvent::Kind::Resolved, rid, J({{"state", "cancelled"}}));
    } else if (type == "control_response") {
        const Json& r = msg["response"];
        std::string rid = r["request_id"].str();
        if (r["subtype"].str() == "error") {
            if (util::endsWith(rid, "-set_model")) {
                emit(AgentEvent::Kind::Notice, "", J({{"text", "無法切換模型：" + r["error"].str()}, {"level", "warn"}}));
            } else if (util::endsWith(rid, "-set_permission_mode")) {
                emit(AgentEvent::Kind::Notice, "",
                     J({{"text", "無法切換權限模式：" + r["error"].str()}, {"level", "warn"}}));
            }
        } else if (util::endsWith(rid, "-initialize")) {
            const Json& body = r["response"];
            emit(AgentEvent::Kind::SessionInfo, "",
                 J({{"models", body["models"]}, {"account", body["account"]},
                    {"version", body["claude_code_version"]}}));
        } else if (util::endsWith(rid, "-mcp_status")) {
            // The raw status includes server environment/configuration; never forward those fields.
            Json servers = Json::array();
            for (const auto& server : r["response"]["mcpServers"].items()) {
                Json tools = Json::array();
                for (const auto& tool : server["tools"].items()) tools.push(tool["name"].str());
                servers.push(J({{"name", server["name"]}, {"status", server["status"]}, {"tools", tools}}));
            }
            emit(AgentEvent::Kind::SessionInfo, "", J({{"mcpServers", servers}}));
        }
    } else if (type == "rate_limit_event") {
        if (isClaudeQuotaRejected(msg["rate_limit_info"])) quotaExceeded_ = true;
        emit(AgentEvent::Kind::RateLimit, "", msg["rate_limit_info"]);
    }
}

void ClaudeSession::handleStreamEvent(const Json& ev) {
    std::string t = ev["type"].str();
    if (t == "message_start") {
        streamMsgId_ = ev["message"]["id"].str(util::newId());
        streamIndexToId_.clear();
    } else if (t == "content_block_start") {
        int idx = (int)ev["index"].i64();
        const Json& cb = ev["content_block"];
        std::string ct = cb["type"].str();
        if (ct == "text") {
            std::string id = streamMsgId_ + ":" + std::to_string(idx);
            streamIndexToId_[idx] = id;
            openText_.push_back(id);
            if (!cb["text"].str().empty()) emit(AgentEvent::Kind::TextDelta, id, J({{"delta", cb["text"].str()}}));
        } else if (ct == "thinking" || ct == "redacted_thinking") {
            emit(AgentEvent::Kind::Thinking, "", Json::object());
        }
    } else if (t == "content_block_delta") {
        int idx = (int)ev["index"].i64();
        const Json& d = ev["delta"];
        if (d["type"].str() == "text_delta") {
            auto it = streamIndexToId_.find(idx);
            if (it == streamIndexToId_.end()) {
                std::string id = streamMsgId_ + ":" + std::to_string(idx);
                it = streamIndexToId_.emplace(idx, id).first;
                openText_.push_back(id);
            }
            emit(AgentEvent::Kind::TextDelta, it->second, J({{"delta", d["text"].str()}}));
        }
    }
}

void ClaudeSession::handleAssistant(const Json& msg) {
    if (!msg["parent_tool_use_id"].isNull()) return;  // subagent traffic stays inside its tool card
    const Json& m = msg["message"];
    if (m["usage"].isObject()) lastUsage_ = m["usage"];
    if (!msg["uuid"].str().empty()) lastUuid_ = msg["uuid"].str();
    if (msg["error"].isString()) {
        if (isClaudeQuotaError(msg["error"].str(), "")) quotaExceeded_ = true;
        emit(AgentEvent::Kind::Error, "", J({{"text", assistantErrorText(msg["error"].str())}}));
    }
    int n = 0;
    for (const auto& block : m["content"].items()) {
        std::string bt = block["type"].str();
        if (bt == "text") {
            std::string id;
            if (!openText_.empty()) {
                id = openText_.front();
                openText_.erase(openText_.begin());
            } else {
                id = m["id"].str(util::newId()) + ":final" + std::to_string(n);
            }
            emit(AgentEvent::Kind::Text, id, J({{"text", block["text"].str()}}));
        } else if (bt == "tool_use") {
            std::string id = block["id"].str();
            std::string name = block["name"].str();
            tools_[id] = ToolUse{name, block["input"]};
            if (name == "TodoWrite") {
                emit(AgentEvent::Kind::Todo, "", J({{"items", todoItems(block["input"]["todos"])}}));
            } else if (name != "AskUserQuestion" && name != "ExitPlanMode") {
                emit(AgentEvent::Kind::ToolStart, id, describeTool(name, block["input"], cfg_.cwd));
            }
        }
        ++n;
    }
}

void ClaudeSession::handleToolResults(const Json& msg) {
    if (!msg["parent_tool_use_id"].isNull()) return;
    const Json& content = msg["message"]["content"];
    if (!content.isArray()) return;
    const Json& tur = msg["tool_use_result"];
    for (const auto& block : content.items()) {
        if (block["type"].str() != "tool_result") continue;
        std::string id = block["tool_use_id"].str();
        auto it = tools_.find(id);
        std::string name = it == tools_.end() ? "" : it->second.name;
        const Json& input = it == tools_.end() ? Json() : it->second.input;
        bool isErr = block["is_error"].boolean();
        std::string text = toolResultText(block["content"]);
        if (name == "TodoWrite" || name == "AskUserQuestion" || name == "ExitPlanMode") continue;

        bool isEdit = name == "Edit" || name == "MultiEdit" || name == "Write" || name == "NotebookEdit";
        if (isEdit && !isErr) {
            std::string path = tur["filePath"].str(input["file_path"].str(input["notebook_path"].str()));
            std::string change = tur["type"].str() == "create" ? "add" : "update";
            DiffStat st;
            std::string diff;
            if (tur["structuredPatch"].size() > 0) {
                diff = hunksToDiff(tur["structuredPatch"], &st);
            } else if (tur["type"].str() == "create" || (name == "Write" && tur.isNull())) {
                diff = newFileDiff(tur["content"].str(input["content"].str()), &st);
                change = "add";
            } else if (name == "Edit") {
                diff = lineDiff(input["old_string"].str(), input["new_string"].str(), &st);
            }
            emit(AgentEvent::Kind::FileChange, id,
                 J({{"path", displayPath(path, cfg_.cwd)},
                    {"change", change},
                    {"added", st.added},
                    {"removed", st.removed},
                    {"diff", util::truncateUtf8(diff, kMaxDiff)},
                    {"truncated", diff.size() > kMaxDiff}}));
            continue;
        }
        if (name == "Bash" || name == "PowerShell") {
            if (tur.isObject()) {
                std::string out = tur["stdout"].str();
                std::string err = tur["stderr"].str();
                if (!err.empty()) out += (out.empty() ? "" : "\n") + err;
                if (!out.empty() || text.empty()) text = out;
            }
        }
        bool declined = isErr && (text.find("doesn't want to") != std::string::npos ||
                                  text.find("拒絕") != std::string::npos || text.find("denied") != std::string::npos);
        Json end = J({{"status", declined ? "declined" : (isErr ? "error" : "ok")},
                      {"output", util::tailUtf8(text, kMaxOutput)}});
        if (name == std::string("mcp__") + kSandboxServer + "__sandbox_run") {
            Json sandbox = sandboxCompletion(block["content"]);
            if (sandbox.isObject()) {
                end["exitCode"] = sandbox["exitCode"];
                if (sandbox["status"].str() == "error") end["status"] = "error";
            }
        }
        emit(AgentEvent::Kind::ToolEnd, id, end);
    }
}

void ClaudeSession::handlePermissionRequest(const std::string& rid, const Json& req) {
    std::string tool = req["tool_name"].str();
    const Json& in = req["input"];
    pending_[rid] = Pending{tool, in, req["permission_suggestions"]};

    if (tool == "AskUserQuestion") {
        emit(AgentEvent::Kind::Question, rid, J({{"agent", "claude"}, {"questions", in["questions"]}}));
        return;
    }

    Json card;
    if (tool == "ExitPlanMode") {
        card = J({{"kind", "plan"}, {"title", "Claude 擬好了計畫，等你核准"}, {"plan", in["plan"].str()}});
        card["options"] = Json(Json::Array{
            J({{"id", choice::kAllow}, {"label", "核准並開始實作"}, {"style", "success"}}),
            J({{"id", choice::kDeny}, {"label", "繼續規劃"}, {"style", "secondary"}}),
        });
    } else {
        card = describeTool(tool, in, cfg_.cwd);
        std::string kind = card["kind"].str();
        std::string title = req["title"].str();
        if (title.empty()) {
            if (kind == "command") title = "Claude 想要執行指令";
            else if (kind == "edit") title = "Claude 想要修改檔案";
            else if (kind == "web") title = "Claude 想要連上網路";
            else if (kind == "mcp") title = "Claude 想要使用 MCP 工具";
            else if (kind == "read" || kind == "search") title = "Claude 想要讀取專案外的檔案";
            else title = "Claude 想要使用 " + req["display_name"].str(tool);
        }
        card["title"] = title;
        DiffStat st;
        std::string diff;
        if (tool == "Edit") {
            diff = lineDiff(in["old_string"].str(), in["new_string"].str(), &st);
            if (in["replace_all"].boolean()) card["note"] = "會取代檔案中所有相符的內容";
        } else if (tool == "MultiEdit") {
            for (const auto& e : in["edits"].items()) {
                DiffStat one;
                diff += lineDiff(e["old_string"].str(), e["new_string"].str(), &one) + "\n";
                st.added += one.added;
                st.removed += one.removed;
            }
        } else if (tool == "Write") {
            diff = newFileDiff(in["content"].str(), &st);
            card["note"] = "會用以下內容建立或覆寫整個檔案";
        }
        if (!diff.empty()) {
            card["diff"] = util::truncateUtf8(diff, kMaxDiff);
            card["added"] = st.added;
            card["removed"] = st.removed;
        }
        Json options = Json::array();
        options.push(J({{"id", choice::kAllow}, {"label", "允許"}, {"style", "primary"}}));
        if (req["permission_suggestions"].size() > 0) {
            options.push(J({{"id", choice::kAllowSession},
                            {"label", sessionAllowLabel(req["permission_suggestions"])},
                            {"style", "secondary"}}));
        }
        options.push(J({{"id", choice::kDeny}, {"label", "拒絕"}, {"style", "danger"}}));
        card["options"] = options;
    }
    card["agent"] = "claude";
    if (!req["decision_reason"].str().empty()) card["reason"] = req["decision_reason"].str();
    if (!req["description"].str().empty()) card["description"] = req["description"].str();
    if (!req["blocked_path"].str().empty()) card["blockedPath"] = req["blocked_path"].str();
    emit(AgentEvent::Kind::Approval, rid, card);
}

void ClaudeSession::respondApproval(const std::string& id, const std::string& ch, const std::string& feedback) {
    auto it = pending_.find(id);
    if (it == pending_.end()) return;
    Pending p = it->second;
    pending_.erase(it);
    Json resp;
    if (ch == choice::kAllow || ch == choice::kAllowSession) {
        resp = J({{"behavior", "allow"}, {"updatedInput", p.input}});
        if (ch == choice::kAllowSession) {
            Json perms = Json::array();
            for (auto s : p.suggestions.items()) {
                s["destination"] = "session";
                perms.push(s);
            }
            resp["updatedPermissions"] = perms;
        }
    } else {
        std::string message = feedback.empty() ? "使用者拒絕了這個操作。"
                                                : "使用者拒絕了這個操作，並說：" + feedback;
        if (p.tool == "ExitPlanMode") {
            message = feedback.empty() ? "使用者想繼續規劃，請先不要開始實作。"
                                       : "使用者想繼續規劃，並說：" + feedback;
        }
        resp = J({{"behavior", "deny"}, {"message", message}});
        if (ch == choice::kAbort) resp["interrupt"] = true;
    }
    sendControlResponse(id, resp);
    emit(AgentEvent::Kind::Resolved, id, J({{"state", ch}, {"feedback", feedback}}));
    if (p.tool == "ExitPlanMode" && ch == choice::kAllow) {
        // The plan is approved: leave plan mode so Claude can start editing.
        sendControl("set_permission_mode", J({{"mode", "acceptEdits"}}));
        cfg_.permission = runningCfg_.permission = "edit";
        emit(AgentEvent::Kind::SessionInfo, "", J({{"permissionPreset", "edit"}}));
    }
}

void ClaudeSession::respondQuestion(const std::string& id, const Json& answers) {
    auto it = pending_.find(id);
    if (it == pending_.end()) return;
    Pending p = it->second;
    pending_.erase(it);
    Json input = p.input;
    input["answers"] = answers;
    sendControlResponse(id, J({{"behavior", "allow"}, {"updatedInput", input}}));
    emit(AgentEvent::Kind::Resolved, id, J({{"state", "answered"}, {"answers", answers}}));
}

void ClaudeSession::handleResult(const Json& msg) {
    const Json& usage = msg["usage"];
    // Read through a const reference: the non-const operator[] inserts a missing key and may reallocate, which left the
    // earlier references of this same expression dangling.
    const Json& last = lastUsage_;
    long long ctxUsed = last["input_tokens"].i64() + last["cache_creation_input_tokens"].i64() +
                        last["cache_read_input_tokens"].i64() + last["output_tokens"].i64();
    long long ctxWindow = 0;
    for (const auto& mu : msg["modelUsage"].fields()) {
        ctxWindow = std::max<long long>(ctxWindow, mu.second["contextWindow"].i64());
    }
    emit(AgentEvent::Kind::Usage, "",
         J({{"inputTokens", usage["input_tokens"].i64() + usage["cache_creation_input_tokens"].i64() +
                                usage["cache_read_input_tokens"].i64()},
            {"cachedTokens", usage["cache_read_input_tokens"].i64()},
            {"outputTokens", usage["output_tokens"].i64()},
            {"contextUsed", ctxUsed},
            {"contextWindow", ctxWindow > 0 ? ctxWindow : 200000},
            {"costUsd", msg["total_cost_usd"].num()},
            {"durationMs", msg["duration_ms"].i64()},
            {"numTurns", msg["num_turns"].i64()}}));

    std::string status = "completed";
    std::string error;
    if (interrupted_) {
        status = "interrupted";
    } else if (msg["is_error"].boolean() || msg["subtype"].str() != "success") {
        status = "failed";
        error = msg["result"].str();
        if (error.empty()) {
            for (const auto& e : msg["errors"].items()) error += e.str() + "\n";
        }
        if (error.empty()) error = msg["subtype"].str();
        if (msg["subtype"].str() == "error_max_turns") error = "已達單輪工具呼叫上限。";
    }
    if (status == "failed" && isClaudeQuotaError("", error)) quotaExceeded_ = true;
    busy_ = false;
    interrupted_ = false;
    openText_.clear();
    tools_.clear();
    emit(AgentEvent::Kind::TurnDone, "", J({{"status", status}, {"error", util::trim(error)}, {"text", msg["result"].str()}, {"uuid", lastUuid_}, {"quotaExceeded", quotaExceeded_}}));
    if (needsRestart_) {
        proc_.reset();  // the next send starts a fresh process with --resume
        needsRestart_ = false;
    }
}
