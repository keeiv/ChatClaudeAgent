// Real child-process protocol tests, without network calls or subscription quota.
#include "takeover.hpp"
#include <windows.h>
#include <cstdio>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include "app.hpp"
#include "editions.hpp"
#include "routing.hpp"
#include "themes.hpp"
#include "util.hpp"

static std::mutex mu;
static std::deque<std::function<void()>> queue;
void postToUi(std::function<void()> fn) { std::lock_guard<std::mutex> lock(mu); queue.push_back(std::move(fn)); }
static void pump() {
    std::deque<std::function<void()>> q;
    { std::lock_guard<std::mutex> lock(mu); q.swap(queue); }
    for (auto& fn : q) fn();
}
static int failures = 0, checks = 0;
static void check(bool ok, const char* name) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", name); } }
static void out(Json j) { std::cout << j.dump() << std::endl; }

static std::string opinionFixture(bool summary) {
    return summary ? J({{"consensus", Json(Json::Array{"以實際測試驗證"})},
                        {"disagreements", Json(Json::Array{"效能仍需量測"})},
                        {"plan",
                         J({{"title", "測試計畫"},
                            {"steps", Json(Json::Array{J({{"text", "增加邊界測試"}, {"acceptance", "測試通過"}})})}})}})
                         .dump()
                   : "獨立意見：檢查空輸入，保留現有資料；以測試取得證據。";
}

static int fakeClaude(int argc, char** argv) {
    std::string line;
    while (std::getline(std::cin, line)) {
        Json m = Json::parse(line);
        if (m["type"].str() == "control_request" && m["request"]["subtype"].str() == "interrupt") {
            if (util::getEnv(L"CCA_TEST_FAILURE") == L"stop") out(J({{"type", "rate_limit_event"}, {"rate_limit_info", J({{"status", "rejected"}})}}));
            out(J({{"type", "result"}, {"subtype", "success"}, {"is_error", false}, {"result", "interrupted"}}));
            continue;
        }
        if (m["type"].str() != "user") continue;
        out(J({{"type", "system"}, {"subtype", "init"}, {"session_id", "fake-claude"}}));
        if (util::getEnv(L"CCA_TEST_GUARD") == L"1") {
            // Succeeds every turn; says whether the prompt carried the guard's revert note (raw or \u-escaped).
            bool noticed = line.find("CCA 測試守門") != std::string::npos ||
                           line.find("CCA \\u6e2c\\u8a66\\u5b88\\u9580") != std::string::npos;
            std::string answer = noticed ? "SAW_REVERT_NOTICE\n\nVERDICT: APPROVE" : "已處理。\n\nVERDICT: APPROVE";
            out(J({{"type", "assistant"},
                   {"message",
                    J({{"id", "guard"}, {"content", Json(Json::Array{J({{"type", "text"}, {"text", answer}})})}})}}));
            out(J({{"type", "result"}, {"subtype", "success"}, {"is_error", false}, {"result", answer}}));
            continue;
        }
        if (util::getEnv(L"CCA_TEST_OPINION") == L"1") {
            bool readonly = false;
            for (int i = 1; i + 1 < argc; ++i)
                if (std::string(argv[i]) == "--permission-mode") readonly = std::string(argv[i + 1]) == "plan";
            std::string answer = readonly ? opinionFixture(false) : "READONLY_MISSING";
            out(J({{"type", "assistant"},
                   {"message",
                    J({{"id", "opinion"}, {"content", Json(Json::Array{J({{"type", "text"}, {"text", answer}})})}})}}));
            out(J({{"type", "result"}, {"subtype", "success"}, {"is_error", false}, {"result", answer}}));
            continue;
        }
        out(J({{"type", "assistant"}, {"message", J({{"id", "partial"}, {"content", Json(Json::Array{
            J({{"type", "text"}, {"text", "STEP_A_DONE; STEP_B_PENDING"}})})}})}}));
        std::string mode = util::narrow(util::getEnv(L"CCA_TEST_FAILURE"));
        if (mode == "wait" || mode == "stop") continue;
        if (mode != "network") out(J({{"type", "rate_limit_event"}, {"rate_limit_info", J({{"status", "rejected"}, {"resetsAt", util::nowMs()/1000 + 3600}, {"rateLimitType", "five_hour"}})}}));
        out(J({{"type", "result"}, {"subtype", "error_during_execution"}, {"is_error", true},
               {"result", mode == "network" ? "Connection timed out" : "You've hit your limit"}}));
    }
    return 0;
}

static int fakeCodex() {
    std::string line;
    int turns = 0;
    bool readonly = false;
    while (std::getline(std::cin, line)) {
        Json m = Json::parse(line);
        std::string method = m["method"].str();
        if (method == "thread/start") readonly = m["params"]["sandbox"].str() == "read-only";
        if (!m.has("id")) continue;
        Json result = Json::object();
        if (method == "account/read") result = J({{"account", J({{"type", "chatgpt"}, {"planType", "plus"}, {"email", "fixture@example.test"}})}});
        if (method == "thread/start" || method == "thread/resume") result = J({{"thread", J({{"id", "fake-codex"}})}});
        if (method == "turn/start") result = J({{"turn", J({{"id", "fake-turn"}})}});
        out(J({{"id", m["id"]}, {"result", result}}));
        if (method == "turn/start") {
            if (util::getEnv(L"CCA_TEST_FLOW") == L"1") Sleep(1800);
            if (++turns == 1 && util::getEnv(L"CCA_TEST_WRITE") == L"1") {
                out(J({{"method", "item/completed"}, {"params", J({{"threadId", "fake-codex"},
                    {"item", J({{"type", "fileChange"}, {"id", "edit"}, {"status", "completed"},
                        {"changes", Json(Json::Array{J({{"path", "step.cpp"}, {"kind", J({{"type", "add"}})}, {"diff", "STEP_B_DONE\n"}})})}})}})}}));
            }
            if (util::getEnv(L"CCA_TEST_CODEX_QUOTA") == L"1") {
                out(J({{"method", "error"},
                       {"params", J({{"threadId", "fake-codex"},
                                     {"willRetry", false},
                                     {"error", J({{"message", "You've hit your usage limit"},
                                                  {"codexErrorInfo", "usageLimitExceeded"}})}})}}));
                out(J({{"method", "turn/completed"},
                       {"params", J({{"threadId", "fake-codex"},
                                     {"turn", J({{"id", "fake-turn"},
                                                 {"status", "failed"},
                                                 {"error", J({{"message", "usage limit"}})}})}})}}));
                continue;
            }
            if (util::getEnv(L"CCA_TEST_GUARD") == L"1") {
                // The first review really edits the file named by CCA_TEST_GUARD_FILE; later turns only approve.
                bool edit = turns == 1;
                if (edit) {
                    util::writeFileAtomic(util::getEnv(L"CCA_TEST_GUARD_FILE"), "BAD\n");
                    out(J({{"method", "item/completed"},
                           {"params",
                            J({{"threadId", "fake-codex"},
                               {"item", J({{"type", "fileChange"},
                                           {"id", "edit"},
                                           {"status", "completed"},
                                           {"changes", Json(Json::Array{J({{"path", "state.txt"},
                                                                           {"kind", J({{"type", "update"}})},
                                                                           {"diff", "-GOOD\n+BAD\n"}})})}})}})}}));
                }
                std::string answer =
                    edit ? "我把 state.txt 改成 BAD。\n\nVERDICT: CHANGED" : "沒有其他意見。\n\nVERDICT: APPROVE";
                out(J({{"method", "item/completed"},
                       {"params", J({{"threadId", "fake-codex"},
                                     {"item", J({{"type", "agentMessage"}, {"id", "answer"}, {"text", answer}})}})}}));
                out(J({{"method", "turn/completed"},
                       {"params", J({{"threadId", "fake-codex"},
                                     {"turn", J({{"id", "fake-turn"}, {"status", "completed"}})}})}}));
                continue;
            }
            std::string input = m["params"]["input"].at(0)["text"].str();
            bool context = input.find("STEP_A_DONE; STEP_B_PENDING") != std::string::npos && input.find("ORIGINAL_TASK") != std::string::npos;
            std::string answer = context ? "CONTEXT_OK; STEP_B_DONE" : "CONTEXT_MISSING";
            if (util::getEnv(L"CCA_TEST_CLI_FIXTURE") == L"1") answer = "模擬後端已收到審查提示。\n\nVERDICT: APPROVE";
            if (util::getEnv(L"CCA_TEST_OPINION") == L"1") {
                readonly &= m["params"]["sandboxPolicy"]["type"].str() == "readOnly";
                answer = readonly ? opinionFixture(input.find("意見整理") != std::string::npos) : "READONLY_MISSING";
            }
            out(J({{"method", "item/completed"}, {"params", J({{"threadId", "fake-codex"},
                {"item", J({{"type", "agentMessage"}, {"id", "answer"}, {"text", answer}})}})}}));
            out(J({{"method", "turn/completed"}, {"params", J({{"threadId", "fake-codex"}, {"turn", J({{"id", "fake-turn"}, {"status", "completed"}})}})}}));
        }
    }
    return 0;
}

// Gemini CLI headless stream: one prompt on stdin, newline-delimited events on stdout.
static int fakeGemini() {
    std::string all, line;
    while (std::getline(std::cin, line)) all += line + "\n";
    out(J({{"type", "init"}, {"session_id", "fake-gemini"}, {"model", "gemini-fixture"}}));
    if (util::getEnv(L"CCA_TEST_GEMINI_QUOTA") == L"1") {
        out(J({{"type", "error"}, {"severity", "error"}, {"message", "Quota exceeded (RESOURCE_EXHAUSTED)"}}));
        out(J({{"type", "result"}, {"status", "error"}, {"error", J({{"message", "RESOURCE_EXHAUSTED"}})}}));
        return 1;
    }
    std::string answer =
        all.find("第三位審查者") != std::string::npos ? "GEMINI_REVIEW_OK\n\nVERDICT: APPROVE" : "GEMINI_DM_OK";
    out(J({{"type", "message"}, {"role", "assistant"}, {"content", answer}}));
    out(J({{"type", "result"}, {"status", "success"}, {"stats", J({{"input_tokens", 1}, {"output_tokens", 1}})}}));
    return 0;
}

struct TakeoverTestAccess {
    // Gemini as a normal member: a DM, a three-member group, and every member running out of quota.
    static void geminiMember(const std::wstring& root, const std::wstring& exe) {
        auto fixture = [&](const wchar_t* label) {
            std::wstring data = util::joinPath(root, label);
            util::ensureDir(data);
            SetEnvironmentVariableW(L"CCA_DATA_DIR", data.c_str());
            SetEnvironmentVariableW(L"CCA_TEST_FAILURE", L"");
            SetEnvironmentVariableW(L"CCA_TEST_WRITE", L"0");
        };
        auto setup = [&](App& app) {
            app.claudeStatus_.state = app.codexStatus_.state = app.geminiStatus_.state = "ok";
            app.claudeStatus_.exe = app.codexStatus_.exe = exe;
            app.settings_["claudePath"] = util::narrow(exe);
            app.settings_["codexPath"] = util::narrow(exe);
            app.settings_["geminiPath"] = util::narrow(exe);
            app.settings_["guard"]["enabled"] = false;
        };
        auto settle = [&](App& app, Conversation* c) {
            for (int i = 0; i < 1500; ++i) {
                pump();
                if (!app.anyBusy(*c) && (!c->group.active || c->group.paused)) break;
                Sleep(10);
            }
        };
        auto count = [](Conversation* c, const char* author, const char* kind = "") {
            int n = 0;
            for (const auto& m : c->messages) n += m["author"].str() == author && (!*kind || m["kind"].str() == kind);
            return n;
        };
        std::string path = util::narrow(util::getEnv(L"PATH"));
        {
            fixture(L"gemini-dm");
            SetEnvironmentVariableW(L"PATH", L"");
            App app([](const std::string&) {});
            SetEnvironmentVariableW(L"PATH", util::widen(path).c_str());
            setup(app);
            Conversation* c = app.find("dm-gemini");
            check(c && c->kind == "gemini", "every install has a Gemini direct message");
            c->cwd = root;
            app.onUiMessage(J({{"type", "conv.send"}, {"id", c->id}, {"text", "hello"}}).dump());
            settle(app, c);
            bool answered = false;
            for (const auto& m : c->messages)
                for (const auto& b : m["blocks"].items())
                    answered |=
                        m["author"].str() == "gemini" && b["text"].str().find("GEMINI_DM_OK") != std::string::npos;
            check(answered && c->geminiSession == "fake-gemini", "Gemini DM answers and keeps its session");
            app.shutdown();
            pump();
        }
        {
            // Claude (fixture) is out of quota: it is skipped, GPT writes, Gemini reviews, and the group agrees.
            fixture(L"gemini-group");
            SetEnvironmentVariableW(L"PATH", L"");
            App app([](const std::string&) {});
            SetEnvironmentVariableW(L"PATH", util::widen(path).c_str());
            setup(app);
            Conversation* c = nullptr;
            for (auto& p : app.convs_)
                if (p->kind == "group") c = p.get();
            c->cwd = root;
            app.onUiMessage(
                J({{"type", "conv.settings"}, {"id", c->id}, {"settings", J({{"geminiMember", true}})}}).dump());
            check(c->geminiMember && app.groupMembers(*c).size() == 3, "Gemini joins the ordinary group rotation");
            app.onUiMessage(J({{"type", "conv.send"},
                               {"id", c->id},
                               {"text", "ORIGINAL_TASK: finish both steps; preserve existing work"}})
                                .dump());
            settle(app, c);
            bool reviewed = false, consensus = false;
            for (const auto& m : c->messages) {
                for (const auto& b : m["blocks"].items())
                    reviewed |=
                        m["author"].str() == "gemini" && b["text"].str().find("GEMINI_REVIEW_OK") != std::string::npos;
                consensus |= m["kind"].str() == "summary" && m["outcome"].str() == "consensus";
            }
            check(count(c, "system", "skip") >= 1 && reviewed && consensus && !c->group.active,
                  "an exhausted member is shown as skipped and the rest reach consensus");
            app.shutdown();
            pump();
        }
        {
            fixture(L"gemini-all-exhausted");
            SetEnvironmentVariableW(L"CCA_TEST_CODEX_QUOTA", L"1");
            SetEnvironmentVariableW(L"CCA_TEST_GEMINI_QUOTA", L"1");
            SetEnvironmentVariableW(L"PATH", L"");
            App app([](const std::string&) {});
            SetEnvironmentVariableW(L"PATH", util::widen(path).c_str());
            setup(app);
            Conversation* c = nullptr;
            for (auto& p : app.convs_)
                if (p->kind == "group") c = p.get();
            c->cwd = root;
            c->geminiMember = true;
            app.onUiMessage(J({{"type", "conv.send"},
                               {"id", c->id},
                               {"text", "ORIGINAL_TASK: finish both steps; preserve existing work"}})
                                .dump());
            settle(app, c);
            std::string outcome;
            for (const auto& m : c->messages)
                if (m["kind"].str() == "summary") outcome = m["outcome"].str();
            if (outcome != "quota")
                for (const auto& m : c->messages) std::printf("  %s\n", m.dump().substr(0, 600).c_str());
            check(count(c, "system", "skip") == 3, "each member that runs out of quota is shown as skipped once");
            check(outcome == "quota" && !c->group.active, "no quota left anywhere force-stops the work");
            Json saved = loadTask(c->id);
            check(saved["status"].str() == "interrupted" && saved["quota_stop"]["next"].str() == "gemini:write" &&
                      saved["handoffs"].size() == 0,
                  "the stop records exactly which step was not done");
            SetEnvironmentVariableW(L"CCA_TEST_CODEX_QUOTA", nullptr);
            SetEnvironmentVariableW(L"CCA_TEST_GEMINI_QUOTA", nullptr);
            app.quotaUntil_.clear();
            app.settings_["claudeQuota"]["blocked"] = false;
            c->lead = "codex";
            app.onUiMessage(J({{"type", "task.resume"}, {"id", c->id}}).dump());
            check(c->group.active && c->group.lastPrompt.find("額度用完") != std::string::npos,
                  "a quota stop resumes from its record once quota returns");
            settle(app, c);
            app.shutdown();
            pump();
        }
    }

    static void run(const std::wstring& root, const std::wstring& exe) {
        geminiMember(root, exe);
        {
            SetEnvironmentVariableW(L"CCA_DATA_DIR", util::joinPath(root, L"opinion-protocol").c_str());
            SetEnvironmentVariableW(L"CCA_TEST_OPINION", L"1");
            SetEnvironmentVariableW(L"CCA_TEST_FAILURE", nullptr);
            App app([](const std::string&) {});
            app.uiReady_ = true;
            app.settings_["guard"]["enabled"] = false;
            app.claudeStatus_.state = app.codexStatus_.state = "ok";
            app.claudeStatus_.exe = app.codexStatus_.exe = exe;
            app.settings_["claudePath"] = util::narrow(exe);
            app.settings_["codexPath"] = util::narrow(exe);
            auto c = app.find("dm-claude");
            c->cwd = root;
            c->claudeSession = "normal-claude";
            c->codexThread = "normal-codex";
            app.startOpinion(*c,
                             J({{"question", "如何改善輸入驗證？"}, {"agents", Json(Json::Array{"claude", "codex"})}}));
            for (int i = 0; i < 2000; ++i) {
                pump();
                app.tick();
                auto state = c->collaboration["opinion"]["status"].str();
                if (state == "done" || state == "failed") break;
                Sleep(10);
            }
            const auto& op = c->collaboration["opinion"];
            check(op["status"].str() == "done", "independent real child processes finish and synthesize opinions");
            check(op["results"]["claude"]["text"].str().find("獨立意見") != std::string::npos &&
                      op["results"]["codex"]["text"].str().find("獨立意見") != std::string::npos,
                  "both transports use read-only permission settings");
            check(op["summary"]["consensus"].size() == 1 && op["summary"]["disagreements"].size() == 1 &&
                      op["proposedPlan"]["steps"].size() == 1,
                  "synthesis yields validated consensus, disagreement and proposal");
            check(c->claudeSession == "normal-claude" && c->codexThread == "normal-codex",
                  "opinion sessions do not replace normal conversation memory");
            app.onUiMessage(J({{"type", "opinion.plan"}, {"id", c->id}}).dump());
            check(c->collaboration["plan"]["status"].str() == "draft" && !app.anyBusy(*c),
                  "adopting model advice creates a draft without running it");
            SetEnvironmentVariableW(L"CCA_TEST_OPINION", nullptr);
            c->lead = "codex";
            app.onUiMessage(J({{"type", "plan.approve"}, {"id", c->id}, {"revision", 1}}).dump());
            app.onUiMessage(J({{"type", "plan.execute"}, {"id", c->id}, {"revision", 1}}).dump());
            for (int i = 0; i < 2000; ++i) {
                pump();
                app.tick();
                if (c->collaboration["plan"]["status"].str() != "running") break;
                Sleep(10);
            }
            check(c->collaboration["plan"]["status"].str() == "awaiting_acceptance",
                  "approved plan executes via normal transport and finishes awaiting acceptance");
            app.onUiMessage(
                J({{"type", "plan.accept"}, {"id", c->id}, {"revision", 1}, {"step", "step-1"}, {"accepted", true}})
                    .dump());
            check(c->collaboration["plan"]["status"].str() == "accepted",
                  "only explicit acceptance completes an executed plan");
            app.shutdown();
            pump();
            SetEnvironmentVariableW(L"CCA_TEST_OPINION", nullptr);
        }
        {
            SetEnvironmentVariableW(L"CCA_DATA_DIR", util::joinPath(root, L"delivery-order").c_str());
            std::vector<std::string> delivered;
            App app([&](const std::string& s) {
                Json event = Json::parse(s);
                if (event["type"].str() == "msg") delivered.push_back(event["msg"]["id"].str());
            });
            app.uiReady_ = true;
            Conversation* c = app.find("dm-claude");
            app.pushMessage(*c, J({{"id", "z-reply"}, {"author", "claude"}, {"ts", 1}}));
            app.pushMessage(*c, J({{"id", "a-summary"}, {"author", "system"}, {"ts", 1}}));
            app.tick();
            check(delivered == std::vector<std::string>{"z-reply", "a-summary"}, "message delivery preserves conversation order, not sorted IDs");
            app.onUiMessage(J({{"type", "visual.set"}, {"mode", "compact"}, {"motion", false}}).dump());
            check(app.settings_["visual"]["mode"].str() == "compact" && !app.settings_["visual"]["motion"].boolean(), "visual controls update shared settings");
            app.onUiMessage(J({{"type", "visual.set"}, {"mode", "invalid"}, {"motion", "false"}}).dump());
            check(app.settings_["visual"]["mode"].str() == "compact" && !app.settings_["visual"]["motion"].boolean(), "malformed visual settings are ignored");
            auto event = [&](AgentEvent::Kind kind, const char* id, const Json& data) {
                app.onAgentEvent(c->id, AgentKind::Claude, AgentEvent{kind, id, data});
            };
            event(AgentEvent::Kind::TurnStarted, "", Json::object());
            check(!c->workflow.id.empty() && c->workflow.owner == "claude", "backend turn starts real workflow tracking");
            c->stateDirty = false;
            event(AgentEvent::Kind::TextDelta, "text", J({{"delta", "hello"}}));
            check(!c->stateDirty, "ordinary text chunks do not resend workflow state");
            event(AgentEvent::Kind::ToolStart, "tool", J({{"title", "test"}}));
            check(c->stateDirty, "tool start publishes workflow state");
            c->stateDirty = false;
            event(AgentEvent::Kind::ToolOutput, "tool", J({{"delta", "output"}}));
            check(!c->stateDirty, "tool output does not resend unchanged workflow state");
            event(AgentEvent::Kind::Approval, "approval", Json::object());
            check(c->stateDirty, "approval publishes waiting state");
            c->stateDirty = false;
            event(AgentEvent::Kind::Resolved, "approval", J({{"state", "allow"}}));
            check(c->stateDirty, "approval resolution publishes active state");
            app.resetConversation(*c);
            check(c->workflow.id.empty() && c->workflow.transfers.empty(), "clearing conversation releases workflow state");
        }
        {
            // CCA closed while GPT was about to review Claude's work; a new App resumes from the saved task.
            std::wstring data = util::joinPath(root, L"restart-resume");
            util::ensureDir(data);
            SetEnvironmentVariableW(L"CCA_DATA_DIR", data.c_str());
            SetEnvironmentVariableW(L"CCA_TEST_FAILURE", L"");
            SetEnvironmentVariableW(L"CCA_TEST_WRITE", L"0");
            std::string groupId;
            {
                App first([](const std::string&) {});
                for (auto& p : first.convs_)
                    if (p->kind == "group") groupId = p->id;
                Json task = newTask(groupId, "ORIGINAL_TASK: finish both steps; preserve existing work", 1000);
                Json msg = J(
                    {{"id", "claude-turn"},
                     {"snapshot", "aaaa1111"},
                     {"blocks",
                      Json(Json::Array{
                          J({{"type", "text"}, {"text", "STEP_A_DONE; STEP_B_PENDING"}}),
                          J({{"type", "tool"}, {"command", "build.bat test"}, {"status", "ok"}, {"exitCode", 0}})})}});
                addHandoff(task, handoffFromTurn(task, msg, "claude", "write", Json::array(), "write", 2000));
                setNextAction(task, "codex:review");
                saveTask(task);  // status "running", as if CCA was killed mid-task
            }
            std::string path = util::narrow(util::getEnv(L"PATH"));
            SetEnvironmentVariableW(L"PATH", L"");
            App app([](const std::string&) {});
            SetEnvironmentVariableW(L"PATH", util::widen(path).c_str());
            Conversation* c = app.find(groupId);
            check(c && c->task["status"].str() == "interrupted",
                  "a task running at shutdown is marked interrupted on restart");
            check(c && app.convState(*c)["task"]["handoffs"].size() == 1, "restored task is published to the UI");
            app.claudeStatus_.state = app.codexStatus_.state = "ok";
            app.claudeStatus_.exe = app.codexStatus_.exe = exe;
            app.settings_["claudePath"] = util::narrow(exe);
            app.settings_["codexPath"] = util::narrow(exe);
            app.settings_["guard"]["enabled"] = false;
            c->cwd = root;
            app.onUiMessage(J({{"type", "task.resume"}, {"id", c->id}}).dump());
            check(c->group.active && c->group.current == "codex", "resume hands the task to the saved next agent");
            check(c->group.lastPrompt.find("【CCA 任務接續】") != std::string::npos &&
                      c->group.lastPrompt.find("build.bat test") != std::string::npos,
                  "resume prompt carries the structured handoff, not the whole history");
            for (int i = 0; i < 1000 && (app.anyBusy(*c) || c->group.active) && !c->group.paused; ++i) {
                pump();
                Sleep(10);
            }
            bool context = false;
            for (const auto& m : c->messages)
                for (const auto& b : m["blocks"].items())
                    context |= b["text"].str().find("CONTEXT_OK") != std::string::npos;
            check(context, "GPT continues from Claude's saved progress after the restart");
            const auto& hs = c->task["handoffs"].items();
            check(hs.size() == 2 && hs.back()["agent_id"].str() == "codex" && hs.back()["seq"].i64() == 2,
                  "the resumed turn is appended to the same task record");
            Json saved = loadTask(groupId);
            check(saved["handoffs"].size() == 2 && saved["task_id"].str() == c->task["task_id"].str(),
                  "the task record on disk follows the resumed task");
            app.onUiMessage(J({{"type", "task.resume"}, {"id", c->id}}).dump());
            check(saved["status"].str() != "interrupted" && !c->group.active,
                  "a finished task cannot be resumed again");
        }
        {
            // Reviewer regression guard against real child processes: GPT's review edits state.txt to BAD.
            auto guardRun = [&](const char* label, const std::string& command, bool enabled, const char* expectStatus,
                                const char* expectFile, bool expectNotice) {
                std::wstring data = util::joinPath(root, util::widen(std::string("guard-data-") + label));
                std::wstring project = util::joinPath(root, util::widen(std::string("guard-project-") + label));
                util::ensureDir(data);
                util::ensureDir(project);
                const std::wstring state = util::joinPath(project, L"state.txt");
                util::writeFileAtomic(state, "GOOD\n");
                SetEnvironmentVariableW(L"CCA_DATA_DIR", data.c_str());
                SetEnvironmentVariableW(L"CCA_TEST_FAILURE", L"");
                SetEnvironmentVariableW(L"CCA_TEST_WRITE", L"0");
                SetEnvironmentVariableW(L"CCA_TEST_GUARD", L"1");
                SetEnvironmentVariableW(L"CCA_TEST_GUARD_FILE", state.c_str());
                App app([](const std::string&) {});
                app.claudeStatus_.state = app.codexStatus_.state = "ok";
                app.claudeStatus_.exe = app.codexStatus_.exe = exe;
                app.settings_["claudePath"] = util::narrow(exe);
                app.settings_["codexPath"] = util::narrow(exe);
                app.settings_["guard"]["enabled"] = false;
                Conversation* c = nullptr;
                for (auto& p : app.convs_)
                    if (p->kind == "group") c = p.get();
                check(c != nullptr, "regression guard fixture has a group");
                if (!c) return;
                c->cwd = project;
                app.onUiMessage(J({{"type", "conv.settings"},
                                   {"id", c->id},
                                   {"settings", J({{"testCommand", command}, {"regressionGuard", enabled}})}})
                                    .dump());
                check(c->testCommand == command && c->regressionGuard == enabled, "test guard settings are stored");
                app.onUiMessage(J({{"type", "conv.send"}, {"id", c->id}, {"text", "把 state.txt 保持為 GOOD"}}).dump());
                for (int i = 0; i < 3000 && (c->group.active || app.anyBusy(*c)); ++i) {
                    pump();
                    Sleep(10);
                }
                std::string file;
                util::readFile(state, file);
                std::string status;
                int regressions = 0;
                bool noticed = false;
                for (const auto& m : c->messages) {
                    if (m["kind"].str() == "regression") {
                        status = m["status"].str();
                        ++regressions;
                    }
                    for (const auto& b : m["blocks"].items())
                        noticed |= b["text"].str().find("SAW_REVERT_NOTICE") != std::string::npos;
                }
                std::string name(label);
                check(!c->group.active && c->task["status"].str() == "done",
                      (name + ": the task still finishes").c_str());
                check(file == expectFile, (name + ": the file ends up as expected").c_str());
                check(enabled ? regressions == 1 && status == expectStatus : regressions == 0,
                      (name + ": the guard reports " + expectStatus).c_str());
                check(noticed == expectNotice,
                      (name + ": the revert note reaches the next member only when reverted").c_str());
                check(enabled ? c->task["regression_guard"].size() == 1 &&
                                    c->task["regression_guard"].at(0)["status"].str() == expectStatus
                              : c->task["regression_guard"].isNull(),
                      (name + ": the ledger records the guard result").c_str());
                app.shutdown();
                pump();
            };
            guardRun("revert", "findstr /B GOOD state.txt >nul", true, "reverted", "GOOD\n", true);
            guardRun("baseline", "exit /b 1", true, "baseline-red", "BAD\n", false);
            guardRun("passed", "exit /b 0", true, "passed", "BAD\n", false);
            guardRun("off", "findstr /B GOOD state.txt >nul", false, "", "BAD\n", false);
            SetEnvironmentVariableW(L"CCA_TEST_GUARD", nullptr);
            SetEnvironmentVariableW(L"CCA_TEST_GUARD_FILE", nullptr);
        }
        check(isClaudeQuotaError("rate_limit", ""), "typed quota error");
        check(isClaudeQuotaError("", "You've hit your limit"), "provider quota text");
        check(!isClaudeQuotaError("", "HTTP 429 transient overload"), "transient throttling is not depletion");
        check(!isClaudeQuotaRejected(J({{"status", "allowed_warning"}})), "warning does not switch models");
        auto scenario = [&](const char* label, const char* mode, bool fallback, bool group, bool stop) {
            std::wstring data = util::joinPath(root, util::widen(label));
            util::ensureDir(data);
            SetEnvironmentVariableW(L"CCA_DATA_DIR", data.c_str());
            SetEnvironmentVariableW(L"CCA_TEST_FAILURE", util::widen(mode).c_str());
            SetEnvironmentVariableW(L"CCA_TEST_WRITE", std::string(label) == "group-selfcheck" ? L"1" : L"0");
            std::string path = util::narrow(util::getEnv(L"PATH"));
            SetEnvironmentVariableW(L"PATH", L""); // No checkpoint subprocess is needed in a protocol fixture.
            App app([](const std::string&) {});
            SetEnvironmentVariableW(L"PATH", util::widen(path).c_str());
            app.claudeStatus_.state = app.codexStatus_.state = "ok";
            app.claudeStatus_.exe = app.codexStatus_.exe = exe;
            app.settings_["claudePath"] = util::narrow(exe);
            app.settings_["codexPath"] = util::narrow(exe);
            // These fixtures test the takeover protocol; their terse English replies would be sent back by
            // the compliance checks, which have their own scenario below.
            app.settings_["guard"]["enabled"] = false;
            Conversation* c = app.find("dm-claude");
            if (group) for (auto& p : app.convs_) if (p->kind == "group") c = p.get();
            c->cwd = root;
            c->autoFallback = fallback;
            if (std::string(mode) == "gpt-only") {
                app.claudeStatus_.state = "loggedOut";
                app.onUiMessage(J({{"type", "conv.settings"}, {"id", c->id}, {"settings", J({{"lead", "codex"}})}}).dump());
                c->messages.push_back(J({{"author", "claude"}, {"blocks", Json(Json::Array{J({{"type", "text"}, {"text", "STEP_A_DONE; STEP_B_PENDING"}})})}}));
            }
            if (!group) c->daily = true;
            app.onUiMessage(J({{"type", "conv.send"}, {"id", c->id}, {"text", "ORIGINAL_TASK: finish both steps; preserve existing work"}}).dump());
            if (std::string(label) == "gpt-unavailable") app.codexStatus_.state = "loggedOut";
            bool stopped = false;
            bool manual = std::string(mode) == "wait";
            for (int i = 0; i < 1000; ++i) {
                pump();
                bool partial = false;
                for (const auto& message : c->messages) for (const auto& block : message["blocks"].items())
                    if (block["text"].str().find("STEP_A_DONE") != std::string::npos) partial = true;
                if ((stop || manual) && !stopped && partial && c->claude && c->claude->busy()) {
                    app.onUiMessage(J({{"type", stop ? "conv.stop" : "conv.takeover"}, {"id", c->id}}).dump());
                    stopped = true;
                }
                if (!app.anyBusy(*c) && (!c->group.active || c->group.paused)) break;
                Sleep(10);
            }
            bool codex = false, context = false;
            for (const auto& m : c->messages) if (m["author"].str() == "codex") {
                codex = true;
                for (const auto& b : m["blocks"].items()) if (b["text"].str().find("CONTEXT_OK") != std::string::npos) context = true;
            }
            if (std::string(label) == "gpt-unavailable") {
                check(!codex && c->group.paused && c->group.pendingAgent == "codex", "unavailable GPT pauses safely");
                check(c->group.pendingPrompt.find("STEP_A_DONE") != std::string::npos, "failed dispatch retains handover for retry");
            }
            else if (stop) check(stopped && !codex, "user stop cancels automatic takeover");
            else if (fallback && std::string(mode) != "network") {
                check(codex && context, group ? "group quota takeover retains progress and original task" : "DM quota takeover retains progress and original task");
                check(c->lead == "codex", "takeover persists GPT leadership");
                check(manual || std::string(mode) == "gpt-only" ? !app.claudeLimited() : app.claudeLimited(), "manual switching and quota tracking are separate");
                if (group) check(!c->group.active, "solo GPT finishes without handing back to depleted Claude");
                else if (std::string(mode) == "gpt-only")
                    check(app.convState(*c)["workflow"]["nodes"].size() == 0, "GPT-only DM has no phantom Claude workflow node");
            } else check(!codex, "disabled fallback or ordinary network error does not switch");
            if (std::string(label) == "group-selfcheck") {
                int replies = 0;
                for (const auto& message : c->messages) replies += message["author"].str() == "codex";
                check(replies == 2 && !c->group.active, "GPT checks file changes once and finishes");
            }
            std::string memo = app.handoverPrompt(*c, "ORIGINAL_TASK");
            check(memo.find("ORIGINAL_TASK") != std::string::npos, "handover contains original request");
            if (group) {
                c->lead = "codex";
                std::string role = app.groupInstructions(*c, AgentKind::Codex);
                check(role.find("主要撰寫者") != std::string::npos || role.find("由你接續實作") != std::string::npos, "GPT can be primary writer or sole implementer");
                check(app.groupInstructions(*c, AgentKind::Claude).find("審查者") != std::string::npos, "Claude can review GPT");
            }
            std::string id = c->id;
            std::string expectedLead = c->lead;
            c->messages.push_back(J({{"author", "claude"}, {"blocks", Json(Json::Array{
                J({{"type", "tool"}, {"command", "npm test"}, {"status", "interrupted"}, {"output", "TEST_NOT_FINISHED"}}),
                J({{"type", "file"}, {"path", "work.cpp"}, {"diff", "+STEP_A"}}),
                J({{"type", "todo"}, {"items", Json(Json::Array{J({{"content", "STEP_B"}, {"status", "pending"}})})}})
            })}}));
            memo = app.handoverPrompt(*c, "ORIGINAL_TASK");
            check(memo.find("TEST_NOT_FINISHED") != std::string::npos && memo.find("work.cpp") != std::string::npos && memo.find("STEP_B") != std::string::npos, "handover includes command status, file changes and outstanding todo");
            app.shutdown();
            pump();
            App reloaded([](const std::string&) {});
            check(reloaded.find(id)->lead == expectedLead && reloaded.find(id)->autoFallback == fallback, "lead and fallback survive restart");
            reloaded.shutdown();
            pump();
        };
        scenario("dm-quota", "quota", true, false, false);
        scenario("group-quota", "quota", true, true, false);
        scenario("fallback-off", "quota", false, false, false);
        scenario("network-error", "network", true, false, false);
        scenario("user-stop", "stop", true, false, true);
        scenario("manual-takeover", "wait", true, false, false);
        scenario("group-selfcheck", "quota", true, true, false);
        scenario("gpt-unavailable", "quota", true, true, false);
        scenario("gpt-primary-no-claude", "gpt-only", true, true, false);
        scenario("dm-gpt-primary-no-claude", "gpt-only", true, false, false);

        {
            // Compliance checks with a real (fixture) backend: GPT's reply lacks the persona's form of
            // address every time, so it goes back twice and then stays, marked, for the user to judge.
            std::wstring data = util::joinPath(root, L"guard");
            util::ensureDir(data);
            SetEnvironmentVariableW(L"CCA_DATA_DIR", data.c_str());
            SetEnvironmentVariableW(L"CCA_TEST_FAILURE", L"");
            SetEnvironmentVariableW(L"CCA_TEST_WRITE", L"0");
            std::string path = util::narrow(util::getEnv(L"PATH"));
            SetEnvironmentVariableW(L"PATH", L"");
            App app([](const std::string&) {});
            SetEnvironmentVariableW(L"PATH", util::widen(path).c_str());
            app.claudeStatus_.state = app.codexStatus_.state = "ok";
            app.claudeStatus_.exe = app.codexStatus_.exe = exe;
            app.settings_["claudePath"] = util::narrow(exe);
            app.settings_["codexPath"] = util::narrow(exe);
            check(app.guardOn() && app.guardRetries() == 2, "compliance checks are on by default with two send-backs");
            // Personas are user-owned; the retry test supplies its own required address explicitly.
            app.settings_["personas"] = Json(Json::Array{J({{"id", "guard-fixture"},
                                                            {"name", "測試審查員"},
                                                            {"address", "測試指揮"},
                                                            {"prompt", "每次回覆稱呼測試指揮。"}})});
            app.settings_["persona"]["codex"] = "guard-fixture";
            Conversation* c = app.find("dm-codex");
            c->cwd = root;
            app.onUiMessage(J({{"type", "conv.send"}, {"id", c->id}, {"text", "幫我看一下這個專案"}}).dump());
            auto count = [&](const char* author, const char* kind) {
                int n = 0;
                for (const auto& m : c->messages)
                    n += m["author"].str() == author && (!kind || m["kind"].str() == kind);
                return n;
            };
            for (int i = 0; i < 1500; ++i) {
                pump();
                if (count("codex", nullptr) >= 3 && !app.anyBusy(*c)) break;
                Sleep(10);
            }
            check(count("codex", nullptr) == 3, "a failing reply is sent back at most twice");
            int sendBacks = 0, finals = 0, rejected = 0;
            for (const auto& m : c->messages) {
                if (m["kind"].str() == "guard") (m["final"].boolean() ? finals : sendBacks)++;
                if (m["author"].str() == "codex" && m["guard"]["status"].str() == "rejected") ++rejected;
            }
            check(sendBacks == 2 && finals == 1, "two send-back notices, then one final notice");
            check(rejected == 3, "every failing reply is marked");
            check(c->guardTry[(int)AgentKind::Codex] == 0, "the send-back count resets after giving up");
            app.shutdown();
            pump();
        }

        {
            // The review pane end to end through App: open, revert one hunk, undo through the rollback message.
            std::wstring data = util::joinPath(root, L"review");
            std::wstring project = util::joinPath(root, L"review-project");
            util::ensureDir(data);
            util::ensureDir(project);
            SetEnvironmentVariableW(L"CCA_DATA_DIR", data.c_str());
            std::vector<Json> got;
            App app([&](const std::string& s) {
                Json e = Json::parse(s);
                std::string t = e["type"].str();
                if (t == "review" || t == "review.reverted") got.push_back(e);
            });
            app.uiReady_ = true;
            Conversation* c = app.find("dm-claude");
            c->cwd = project;
            std::string before;
            for (int i = 1; i <= 20; ++i) before += "line " + std::to_string(i) + "\n";
            std::wstring file = util::joinPath(project, L"a.txt");
            util::writeFileAtomic(file, before);
            if (!app.snapshots_.available()) {
                std::printf("skip review: no git\n");
            } else {
                SnapshotResult base = app.snapshots_.take(c->id, project, "fixture");
                app.pushMessage(*c, J({{"author", "user"}, {"text", "改兩行"}}));
                app.pushMessage(*c, J({{"author", "claude"}, {"snapshot", base.commit}, {"blocks", Json::array()}}));
                std::string after =
                    util::replaceAll(util::replaceAll(before, "line 2\n", "LINE 2\n"), "line 19\n", "LINE 19\n");
                util::writeFileAtomic(file, after);
                auto wait = [&](const char* type) {
                    for (int i = 0; i < 1500; ++i) {
                        pump();
                        for (const auto& e : got) {
                            if (e["type"].str() == type) return e;
                        }
                        Sleep(10);
                    }
                    return Json();
                };
                app.onUiMessage(J({{"type", "review.open"}, {"id", c->id}, {"scope", "task"}}).dump());
                Json review = wait("review");
                check(review["error"].str().empty() && review["files"].size() == 1,
                      "review lists the changed file since the task checkpoint");
                const Json& f = review["files"].items()[0];
                check(f["path"].str() == "a.txt" && f["hunks"].size() == 2, "review splits the change into hunks");
                got.clear();
                app.onUiMessage(J({{"type", "review.revert"},
                                   {"id", c->id},
                                   {"base", review["base"]},
                                   {"head", review["head"]},
                                   {"requestToken", "revert-original"},
                                   {"path", "a.txt"},
                                   {"hunk", 0},
                                   {"header", f["hunks"].items()[0]["header"]}})
                                    .dump());
                Json reverted = wait("review.reverted");
                check(reverted["requestToken"].str() == "revert-original", "revert echoes its operation token");
                std::string now;
                util::readFile(file, now);
                check(reverted["ok"].boolean() && now.find("line 2\n") != std::string::npos &&
                          now.find("LINE 19") != std::string::npos,
                      "reverting one hunk keeps the other change");
                check(!c->messages.empty() && c->messages.back()["kind"].str() == "rollback" &&
                          !c->messages.back()["backup"].str().empty(),
                      "a revert leaves an undoable rollback message");
                app.onUiMessage(
                    J({{"type", "rollback.undo"}, {"convId", c->id}, {"backup", reverted["backup"]}}).dump());
                for (int i = 0; i < 500; ++i) {
                    pump();
                    util::readFile(file, now);
                    if (now == after) break;
                    Sleep(10);
                }
                check(now == after, "undo brings the reverted hunk back");
                got.clear();
                app.onUiMessage(
                    J({{"type", "review.open"}, {"id", c->id}, {"scope", "task"}, {"requestToken", "latest-review"}})
                        .dump());
                Json freshReview = wait("review");
                check(freshReview["requestToken"].str() == "latest-review" && !freshReview["head"].str().empty(),
                      "review echoes its request token and the reviewed head");
                const std::string newer = util::replaceAll(after, "LINE 2", "UNREVIEWED 2");
                util::writeFileAtomic(file, newer);
                for (long long hunk : {-1LL, 0LL}) {
                    got.clear();
                    app.onUiMessage(J({{"type", "review.revert"},
                                       {"id", c->id},
                                       {"base", freshReview["base"]},
                                       {"head", freshReview["head"]},
                                       {"path", "a.txt"},
                                       {"hunk", hunk}})
                                        .dump());
                    Json refused = wait("review.reverted");
                    util::readFile(file, now);
                    check(!refused["ok"].boolean() && now == newer,
                          "file and hunk reverts preserve edits made after the review opened");
                }
                got.clear();
                app.onUiMessage(
                    J({{"type", "review.revert"}, {"id", c->id}, {"base", review["base"]}, {"path", "..\\outside.txt"}})
                        .dump());
                check(wait("review.reverted")["ok"].boolean() == false, "paths outside the project are refused");
            }
            app.shutdown();
            pump();
        }
    }
};

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--version") { std::cout << "9.9.9 fixture\n"; return 0; }
    if (argc > 1 && std::string(argv[1]) == "auth") {
        out(J({{"loggedIn", true}, {"authMethod", "claude.ai"}, {"apiProvider", "firstParty"}, {"subscriptionType", "max"}}));
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "app-server") return fakeCodex();
    if (argc > 1 && std::string(argv[1]) == "-p") return fakeClaude(argc, argv);
    if (argc > 1 && std::string(argv[1]) == "--output-format") return fakeGemini();
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring root = util::joinPath(tmp, util::widen("cca-takeover-" + util::newId()));
    util::ensureDir(root);
    wchar_t exe[MAX_PATH * 4]; GetModuleFileNameW(nullptr, exe, MAX_PATH * 4);
    TakeoverTestAccess::run(root, exe);
    std::printf("%d takeover checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
