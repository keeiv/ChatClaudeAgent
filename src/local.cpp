// The two optional local models (Ollama). They only review, read-only: they read the
// project through local_tools (no writes, no commands), every finding must quote the
// file it cites, and only findings that match are passed on to Claude/GPT, who still
// verify them before acting.
#include <windows.h>

#include <thread>

#include "app.hpp"
#include "hardware.hpp"
#include "humanize.hpp"
#include "local_tools.hpp"
#include "util.hpp"

namespace {

const size_t kMaxDiff = 12 * 1024;
const size_t kMaxRules = 3 * 1024;

bool validModelName(const std::string& n) {
    if (n.empty() || n.size() > 120) return false;
    for (char c : n) {
        if (!(isalnum((unsigned char)c) || c == '.' || c == ':' || c == '-' || c == '_' || c == '/')) return false;
    }
    return true;
}

int localIndex(AgentKind k) { return k == AgentKind::Local1 ? 0 : 1; }
AgentKind localKind(const std::string& s) { return s == "local2" ? AgentKind::Local2 : AgentKind::Local1; }

std::string finalText(const Json& msg) {
    std::string out;
    for (const auto& b : msg["blocks"].items()) {
        if (b["type"].str() == "text") out += (out.empty() ? "" : "\n\n") + b["text"].str();
    }
    return out;
}

const char* kReviewerRole =
    "# 你在 CCA 的工作\n"
    "你是協作群組裡的唯讀審查員。你有三個唯讀工具：read_file（讀檔，結果帶行號）、list_dir（列出資料夾）、"
    "search（搜尋文字）。它們只能讀取專案資料夾；你不能修改檔案，也不能執行指令。\n"
    "CCA 會用程式檢查你的回覆，不符合下列規則就會退回重寫：\n"
    "1. 下結論之前一定要用 read_file 讀過變更的檔案。沒有讀過的檔案不能評論。\n"
    "2. 每條建議以「發現 1：」開頭，並寫「位置：檔案:行號」和「原文：」。原文要從 read_file 的結果逐字複製"
    "（不含行號）；CCA 會拿去和檔案比對，對不上的建議會被刪除。\n"
    "3. 再寫「問題：」「建議：」「信心：高／中／低」。看不出問題就寫「沒有發現問題」，並列出你讀過的檔案。\n"
    "4. 用台灣繁體中文回答，不能有簡體字。不要重寫整份程式，也不要假裝執行過測試。\n"
    "你的建議會轉交給 Claude 和 GPT，他們會再自己查證，決定要不要採納。";

// What the turn under review changed, as project-relative paths the tools can open.
std::vector<std::string> changedPaths(const GroupRun& g, const std::wstring& cwd) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& f : g.turnFiles.items()) {
        if (f["change"].str() == "delete") continue;
        std::string path = f["path"].str();
        size_t arrow = path.find(" → ");
        if (arrow != std::string::npos) path = path.substr(arrow + std::string(" → ").size());
        path = normalizeRelPath(util::displayPath(path, cwd));
        if (!path.empty() && seen.insert(path).second) out.push_back(path);
    }
    return out;
}

}  // namespace

// Claude Code reads CLAUDE.md and Codex reads AGENTS.md by themselves; a local model has no
// tools, so CCA hands it the project's rules. Only the part between the review markers when
// the file has them (local models have a 12k context), else the start of the file.
std::string projectReviewRules(const std::wstring& cwd) {
    if (cwd.empty()) return "";
    std::string text;
    for (const wchar_t* name : {L"AGENTS.md", L"CLAUDE.md"}) {
        if (util::readFile(util::joinPath(cwd, name), text) && !util::trim(text).empty()) break;
        text.clear();
    }
    if (text.empty()) return "";
    const std::string open = "<!-- cca-review:start -->", close = "<!-- cca-review:end -->";
    size_t a = text.find(open);
    size_t b = a == std::string::npos ? std::string::npos : text.find(close, a);
    if (b != std::string::npos) text = text.substr(a + open.size(), b - a - open.size());
    text = util::trim(text);
    if (text.size() > kMaxRules) text = util::truncateUtf8(text, kMaxRules) + "\n…（後面省略）";
    return text;
}

bool App::localEnabled(AgentKind k) const {
    const Json& s = settings_["locals"][agentName(k)];
    return s["enabled"].boolean(true) && !s["model"].str().empty();
}

std::string App::localModel(AgentKind k) const { return settings_["locals"][agentName(k)]["model"].str(); }

LocalSession& App::localFor(Conversation& c, AgentKind k) {
    auto& slot = c.local[localIndex(k)];
    if (!slot) {
        std::string id = c.id;
        slot = std::make_shared<LocalSession>(k, [this, id, k](const AgentEvent& ev) { onAgentEvent(id, k, ev); });
    }
    std::string system = personaPrompt(k);
    system += (system.empty() ? "" : "\n\n") + std::string(kReviewerRole);
    slot->configure(localModel(k), system);
    return *slot;
}

void App::ensureOllama(std::function<void(bool, const std::string&)> then) {
    std::wstring exe = ollama::findExe();
    bool canStart = !exe.empty() && !(ollamaServe_ && ollamaServe_->running());
    if (canStart) {
        // Quick check first; start `ollama serve` only when nothing answers.
        std::string err;
        if (ollama::perform("GET", "/api/version", "", nullptr, nullptr, &err, 800) != 200) {
            ProcessOptions opt;
            opt.exe = exe;
            opt.args = {L"serve"};
            opt.cwd = util::localAppDataDir();
            ollamaServe_ = std::make_unique<ChildProcess>();
            std::string startErr;
            if (!ollamaServe_->start(opt, nullptr, nullptr, nullptr, &startErr)) ollamaServe_.reset();
        }
    }
    std::thread([then, exe] {
        std::string err;
        for (int i = 0; i < 30; ++i) {
            if (ollama::perform("GET", "/api/version", "", nullptr, nullptr, &err, 1000) == 200) {
                postToUi([then] { then(true, ""); });
                return;
            }
            Sleep(500);
        }
        std::string why = exe.empty() ? "這台電腦還沒有安裝 Ollama" : "Ollama 沒有回應";
        postToUi([then, why] { then(false, why); });
    }).detach();
}

void App::sendLocalStatus() {
    hardware_ = detectHardware();  // free memory changes, so read it each time
    Json base = J({{"type", "local"},
                   {"hardware", hardware_},
                   {"recommended", recommendModels(hardware_)},
                   {"slots", settings_["locals"]},
                   {"installedOllama", !ollama::findExe().empty()}});
    if (ollama::findExe().empty()) {
        base["running"] = false;
        send(base);
        return;
    }
    ensureOllama([this, base](bool ok, const std::string& error) mutable {
        if (!ok) {
            base["running"] = false;
            base["error"] = error;
            send(base);
            return;
        }
        auto lines = std::make_shared<std::string>();
        ollama::request(
            "GET", "/api/tags", "", [lines](const std::string& l) { *lines += l; },
            [this, base, lines](int status, const std::string& err) mutable {
                // Still on the worker thread: ask each model what it supports, since a
                // reviewer without tool calls cannot read files.
                Json tags = Json::parse(*lines);
                Json caps = Json::object();
                for (const auto& m : tags["models"].items()) {
                    std::string out, showErr;
                    if (ollama::perform(
                            "POST", "/api/show", J({{"model", m["name"]}}).dump(),
                            [&out](const std::string& l) { out += l + "\n"; }, nullptr, &showErr, 2000) == 200) {
                        caps[m["name"].str()] = Json::parse(out)["capabilities"];
                    }
                }
                postToUi([this, base, tags, caps, status, err]() mutable {
                    Json models = Json::array();
                    for (const auto& m : tags["models"].items()) {
                        const Json& c = caps[m["name"].str()];
                        bool tools = false;
                        for (const auto& x : c.items()) tools = tools || x.str() == "tools";
                        if (c.isArray() && c.size() > 0) localCaps_[m["name"].str()] = c;
                        models.push(J({{"name", m["name"]},
                                       {"sizeGb", std::round(m["size"].num() / 1e8) / 10.0},
                                       {"params", m["details"]["parameter_size"]},
                                       {"quant", m["details"]["quantization_level"]},
                                       {"tools", c.isArray() ? Json(tools) : Json()}}));
                    }
                    base["running"] = status == 200;
                    base["models"] = models;
                    if (!err.empty()) base["error"] = err;
                    send(base);
                });
            });
    });
}

void App::handleLocalPull(const Json& m) {
    std::string model = m["model"].str();
    if (!validModelName(model)) {
        toast("error", "模型名稱不正確");
        return;
    }
    if (pulls_.count(model)) return;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    pulls_[model] = cancel;
    send(J({{"type", "local.pull"}, {"model", model}, {"status", "準備下載"}}));
    ensureOllama([this, model, cancel](bool ok, const std::string& error) {
        if (!ok) {
            pulls_.erase(model);
            send(J({{"type", "local.pull"}, {"model", model}, {"done", true}, {"error", error}}));
            return;
        }
        auto last = std::make_shared<long long>(0);
        auto failure = std::make_shared<std::string>();
        ollama::request(
            "POST", "/api/pull", J({{"model", model}, {"stream", true}}).dump(),
            [this, model, last, failure](const std::string& line) {
                Json j = Json::parse(line);
                if (!j["error"].str().empty()) *failure = j["error"].str();
                long long now = util::nowMs();
                // Progress arrives many times a second; the UI needs a few updates.
                if (now - *last < 250 && j["status"].str() != "success") return;
                *last = now;
                Json msg = J({{"type", "local.pull"}, {"model", model}, {"status", j["status"]},
                              {"completed", j["completed"]}, {"total", j["total"]}});
                postToUi([this, msg] { send(msg); });
            },
            [this, model, cancel, failure](int status, const std::string& err) {
                postToUi([this, model, cancel, failure, status, err] {
                    pulls_.erase(model);
                    std::string e = !failure->empty() ? *failure : (status != 200 ? (err.empty() ? "下載失敗（HTTP " + std::to_string(status) + "）" : err) : "");
                    if (*cancel) e = "已取消";
                    send(J({{"type", "local.pull"}, {"model", model}, {"done", true}, {"error", e}}));
                    if (e.empty()) toast("ok", "已下載 " + model);
                    sendLocalStatus();
                });
            },
            cancel);
    });
}

void App::handleLocalDelete(const Json& m) {
    std::string model = m["model"].str();
    if (!validModelName(model)) return;
    ollama::request("DELETE", "/api/delete", J({{"model", model}}).dump(), nullptr, [this, model](int status, const std::string&) {
        postToUi([this, model, status] {
            if (status == 200) toast("ok", "已刪除 " + model);
            else toast("error", "刪除失敗（HTTP " + std::to_string(status) + "）");
            sendLocalStatus();
        });
    });
}

void App::handleLocalSet(const Json& m) {
    std::string slot = m["slot"].str();
    if (slot != "local1" && slot != "local2") return;
    Json& s = settings_["locals"][slot];
    if (m.has("model")) {
        std::string model = m["model"].str();
        if (model.empty() || validModelName(model)) s["model"] = model;
    }
    if (m.has("enabled")) s["enabled"] = m["enabled"].boolean();
    metaDirty_ = true;
    sendSettings();
    sendLocalStatus();
}

std::string App::localReviewPrompt(const Conversation& c) const {
    const GroupRun& g = c.group;
    std::string p = "【CCA 唯讀審查｜第 " + std::to_string(g.round) + " 輪】\n\n使用者的需求：\n" + g.task + "\n\n";
    std::string rules = projectReviewRules(c.cwd);
    if (!rules.empty()) p += "這個專案的規範（摘自專案的 AGENTS.md），審查時以它為準：\n" + rules + "\n\n";
    p += g.reviewedBy + " 剛完成這一輪。";
    std::string text = util::trim(g.turnText);
    if (!text.empty()) p += "它的說明：\n" + util::truncateUtf8(text, 3000) + "\n\n";
    p += "修改的檔案（審查時一定要用 read_file 讀過，CCA 會檢查你讀了哪些檔案）：\n";
    std::string diffs;
    for (const auto& f : g.turnFiles.items()) {
        p += "- " + f["path"].str() + "（+" + std::to_string(f["added"].i64()) + " −" + std::to_string(f["removed"].i64()) + "）\n";
        if (diffs.size() < kMaxDiff) diffs += "--- " + f["path"].str() + "\n" + f["diff"].str() + "\n";
    }
    p += "\n變更摘要：\n```diff\n" + util::truncateUtf8(diffs, kMaxDiff) + "\n```\n";
    p += "diff 只是摘要。判斷之前，請用 read_file 讀變更檔案的實際內容，需要時用 search 找呼叫端或相關定義。\n\n";
    if (humanizeOn()) p += humanizeReviewNote(settings_["humanize"]["extra"].str()) + "\n\n";
    p += "回覆格式（CCA 會用程式逐條比對檔案，對不上的建議會被刪掉）：\n"
         "發現 1：一句話說明問題\n"
         "位置：src\\example.cpp:42\n"
         "原文：從 read_file 結果逐字複製的那一行（不含行號）\n"
         "問題：為什麼有問題\n"
         "建議：怎麼改\n"
         "信心：高／中／低\n\n"
         "看不出問題就寫「沒有發現問題」，並列出你讀過的檔案。";
    return p + reviewFocus(c, c.group.localActive == "local2" ? AgentKind::Local2 : AgentKind::Local1);
}

void App::localCapabilities(const std::string& model, std::function<void(const Json& caps)> then) {
    if (localCaps_.has(model)) {
        then(localCaps_[model]);
        return;
    }
    std::thread([this, model, then] {
        std::string out, err;
        int status = ollama::perform(
            "POST", "/api/show", J({{"model", model}}).dump(), [&out](const std::string& l) { out += l + "\n"; },
            nullptr, &err, 2000);
        Json caps = status == 200 ? Json::parse(out)["capabilities"] : Json();
        postToUi([this, model, caps, then] {
            if (caps.isArray() && caps.size() > 0) localCaps_[model] = caps;
            then(caps.isArray() ? caps : Json::array());
        });
    }).detach();
}

void App::groupRunLocals(Conversation& c) {
    GroupRun& g = c.group;
    while (!g.localQueue.empty()) {
        AgentKind k = localKind(g.localQueue.front());
        g.localQueue.erase(g.localQueue.begin());
        if (!localEnabled(k)) continue;
        g.localActive = agentName(k);
        c.stateDirty = true;
        std::string convId = c.id;
        auto skip = [this, convId, k](const std::string& why) {
            Conversation* cp = find(convId);
            if (!cp) return;
            pushMessage(*cp,
                        J({{"author", "system"}, {"kind", "info"}, {"text", nameOf(k) + " 這一輪沒有審查：" + why}}));
            cp->group.localActive.clear();
            groupRunLocals(*cp);
        };
        ensureOllama([this, convId, k, skip](bool ok, const std::string& error) {
            Conversation* cp = find(convId);
            if (!cp || !cp->group.active || cp->group.localActive != agentName(k)) return;
            if (cp->group.stopRequested) {
                groupFinish(*cp, "stopped", "已停止協作。");
                return;
            }
            if (!ok) {
                skip(error);
                return;
            }
            std::string model = localModel(k);
            // A reviewer has to read the files itself, so models without tool calls cannot take part.
            localCapabilities(model, [this, convId, k, model, skip](const Json& caps) {
                Conversation* cp = find(convId);
                if (!cp || !cp->group.active || cp->group.localActive != agentName(k)) return;
                if (cp->group.stopRequested) {
                    groupFinish(*cp, "stopped", "已停止協作。");
                    return;
                }
                bool tools = false, think = false;
                for (const auto& x : caps.items()) {
                    tools = tools || x.str() == "tools";
                    think = think || x.str() == "thinking";
                }
                if (caps.size() > 0 && !tools) {
                    skip("模型 " + model +
                         " 不支援工具呼叫，沒辦法自己讀檔。請到 設定 › 本地模型 換成支援工具的模型。");
                    return;
                }
                LocalReviewSetup setup;
                setup.review = true;
                setup.root = cp->cwd;
                setup.changedPaths = changedPaths(cp->group, cp->cwd);
                setup.rules = guardRulesFor(*cp, k);
                setup.maxCorrections = guardOn() ? std::max(0, guardRetries()) : 0;
                setup.think = think;
                LocalSession& s = localFor(*cp, k);
                s.setReview(setup);
                std::string err;
                std::string prompt = localReviewPrompt(*cp);
                if (guardOn()) prompt = guardRuleCard(setup.rules) + "\n" + prompt;
                AgentConfig observed;
                observed.cwd = cp->cwd;
                observed.model = model;
                observed.permission = "readonly";
                captureContext(*cp, agentName(k), observed, prompt);
                if (!s.send(prompt, Json::array(), &err)) skip(err);
            });
        });
        return;
    }
    groupProceed(c);
}

void App::groupLocalDone(Conversation& c, AgentKind k, const Json& msg, const std::string& status) {
    GroupRun& g = c.group;
    g.localActive.clear();
    if (g.stopRequested || status == "interrupted") {
        groupFinish(c, "stopped", "已停止協作。");
        return;
    }
    std::string text = util::trim(msg.has("finalText") ? msg["finalText"].str() : finalText(msg));
    if (status == "completed" && !text.empty()) {
        workflowLocalReply(c.workflow, agentName(k));
        std::set<std::string> read;
        for (const auto& p : msg["readPaths"].items()) read.insert(p.str());
        GuardRules rules = guardRulesFor(c, k);
        LocalReviewCheck check = checkLocalReview(c.cwd, text, read, changedPaths(g, c.cwd), rules);
        if (Json* live = findMsg(c, msg["id"].str())) {
            long long fixes = msg["corrections"].i64();
            if (!check.issues.empty()) {
                (*live)["guard"] = J({{"status", "rejected"},
                                      {"attempt", fixes + 1},
                                      {"issues", guardIssuesJson(check.issues)},
                                      {"removed", check.removed}});
            } else if (fixes > 0) {
                (*live)["guard"] = J({{"status", "passed"}, {"attempt", fixes}});
            }
            markDirty(c, msg["id"].str());
        }
        // Only findings that quote their file correctly reach Claude/GPT.
        if (!util::trim(check.forwarded).empty() &&
            !(check.findings.size() > 0 && check.removed == (int)check.findings.size())) {
            g.localNotes += "【" + nameOf(k) + "（本地模型 " + localModel(k) + "）】\n" +
                            util::truncateUtf8(check.forwarded, 4000) + "\n";
            g.notesNext.push_back(nameOf(k));
        }
        if (check.removed > 0 || !check.issues.empty()) {
            std::string why =
                check.removed > 0 ? std::to_string(check.removed) + " 條建議的引用和檔案對不上，已移除" : "";
            if (!check.issues.empty())
                why += (why.empty() ? "" : "；") + std::string("仍有：") + guardSummary(check.issues);
            pushMessage(
                c,
                J({{"author", "system"},
                   {"kind", "guard"},
                   {"agent", agentName(k)},
                   {"final", true},
                   {"text", nameOf(k) + " 的審查沒有完全通過規範審查（" + why + "）。只有對得上檔案的建議會轉交。"}}));
        }
    }
    groupRunLocals(c);
}

void App::groupProceed(Conversation& c) {
    GroupRun& g = c.group;
    std::string prompt = g.nextPrompt;
    if (!util::trim(g.localNotes).empty()) {
        prompt += "\n\n---\n本地審查員的建議（唯讀、由較小的本地模型產生，可能出錯）：\n\n" + g.localNotes +
                  "這些建議只能當線索，不能直接相信。採納任何一條之前，必須先自己開檔案、搜尋或執行測試取得證據；"
                  "回覆裡要說明你查證了哪幾條、證據是什麼、結論如何。查證後不成立的就說明理由並忽略。";
    }
    g.localNotes.clear();
    AgentKind next = g.nextAgent == "codex"    ? AgentKind::Codex
                     : g.nextAgent == "gemini" ? AgentKind::Gemini
                                               : AgentKind::Claude;
    groupHandOff(c, next, prompt, g.nextPhase);
}
