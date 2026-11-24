// Real child-process fixtures cover the Cursor stream, account probe, catalogue and permission refusals without model
// usage.
#include <windows.h>

#include <cstdio>
#include <iostream>
#include <mutex>
#include <vector>

#include "app.hpp"
#include "cursor_agent.hpp"
#include "util.hpp"

namespace {
std::mutex mutex;
std::vector<std::function<void()>> jobs;
int failures = 0, checks = 0;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}
void pump() {
    std::vector<std::function<void()>> next;
    {
        std::lock_guard<std::mutex> lock(mutex);
        next.swap(jobs);
    }
    for (auto& job : next) job();
}
void out(const Json& j) {
    std::cout << j.dump() << std::endl;
}

// Emits the documented stream-json shapes: tool calls keyed by their case name, results by their variant.
int fixture(int argc, char** argv) {
    const std::string command = argc > 2 ? argv[2] : "";
    if (command == "status") {
        out(J({{"status", "authenticated"},
               {"isAuthenticated", true},
               {"userInfo", J({{"email", "fixture@example.com"}, {"userId", 1}})}}));
        return 0;
    }
    if (command == "about") {
        std::cout << "Update available\n"
                  << J({{"cliVersion", "fixture-version"}, {"subscriptionTier", "Pro"}}).dump() << std::endl;
        return 0;
    }
    if (command == "models") {
        // Display names carry zero-width spaces and doubled spaces, as the real catalogue does.
        std::cout << "Available models\n\nauto - Auto (default)\n"
                  << "grok-4.7-high - Grok 4.7  High\xE2\x80\x8B\xE2\x80\x8B (current)\n"
                  << "-bad - Starts with a dash\nTip: use --model\n";
        return 0;
    }
    std::string input, line;
    while (std::getline(std::cin, line)) input += line + "\n";
    Json args = Json::array();
    for (int i = 2; i < argc; ++i) args.push(argv[i]);
    util::writeFileAtomic(L"cursor-receipt.json", J({{"args", args},
                                                     {"input", input},
                                                     {"apiKeyPresent", !util::getEnv(L"CURSOR_API_KEY").empty()},
                                                     {"noColor", util::narrow(util::getEnv(L"NO_COLOR"))}})
                                                      .dump());
    const std::string session = "fixture-chat";
    out(J({{"type", "system"},
           {"subtype", "init"},
           {"apiKeySource", "login"},
           {"session_id", session},
           {"model", "Grok 4.7 High"}}));
    auto say = [&](const std::string& text) {
        out(J({{"type", "assistant"},
               {"message",
                J({{"role", "assistant"}, {"content", Json(Json::Array{J({{"type", "text"}, {"text", text}})})}})},
               {"session_id", session}}));
    };
    say("先看檔案。");
    Json shellArgs = J({{"command", "python -m unittest"}});
    Json shellResult = input.find("no-exit") != std::string::npos ? J({{"success", J({{"stdout", "ok"}})}})
                       : input.find("rejected") != std::string::npos
                           ? J({{"rejected", J({{"reason", "needs approval"}})}})
                           : J({{"success", J({{"exitCode", 0}, {"stdout", "OK"}, {"stderr", ""}})}});
    out(J({{"type", "tool_call"},
           {"subtype", "started"},
           {"call_id", "call-1"},
           {"tool_call", J({{"shellToolCall", J({{"args", shellArgs}})}})}}));
    out(J({{"type", "tool_call"},
           {"subtype", "completed"},
           {"call_id", "call-1"},
           {"tool_call", J({{"shellToolCall", J({{"args", shellArgs}, {"result", shellResult}})}})}}));
    out(J({{"type", "tool_call"},
           {"subtype", "started"},
           {"call_id", "call-2"},
           {"tool_call", J({{"readToolCall", J({{"args", J({{"path", "README.md"}})}})}})}}));
    out(J({{"type", "tool_call"},
           {"subtype", "completed"},
           {"call_id", "call-2"},
           {"tool_call", J({{"readToolCall", J({{"args", J({{"path", "README.md"}})},
                                                {"result", J({{"success", J({{"content", "# readme"}})}})}})}})}}));
    out(J({{"type", "tool_call"},
           {"subtype", "started"},
           {"call_id", "call-3"},
           {"tool_call", J({{"futureToolCall", J({{"args", J({{"x", 1}})}})}})}}));
    say("已查證，測試通過。");
    out(J({{"type", "usage"},
           {"subtype", "model_call"},
           {"usage", J({{"inputTokens", 40}, {"outputTokens", 5}, {"cacheReadTokens", 7}, {"cacheWriteTokens", 0}})}}));
    if (input.find("missing-result") != std::string::npos) return 0;
    out(J({{"type", "result"},
           {"subtype", "success"},
           {"is_error", false},
           {"duration_ms", 12},
           {"result", "先看檔案。已查證，測試通過。"},
           {"session_id", session}}));
    return input.find("exit-error") != std::string::npos ? 1 : 0;
}

std::wstring makeDir(const std::wstring& parent, const std::wstring& name) {
    auto dir = util::joinPath(parent, name);
    util::ensureDir(dir);
    return dir;
}
void touch(const std::wstring& path) {
    util::writeFileAtomic(path, "");
}
}  // namespace

struct CursorTestAccess {
    static void run(const std::wstring& root, const std::wstring& exe, const std::wstring& script) {
        App app([](const std::string&) {});
        app.settings_["guard"]["enabled"] = false;
        app.settings_["humanize"]["enabled"] = false;
        app.settings_["cursorPath"] = util::narrow(script);
        app.checkCursor();
        check(app.cursorChecking_, "account check starts");
        auto until = GetTickCount64() + 20000;
        while (app.cursorChecking_ && GetTickCount64() < until) {
            pump();
            Sleep(5);
        }
        pump();
        check(app.cursorStatus_.state == "ok", "status, about and models reach the native state");
        check(app.cursorStatus_.plan == "Pro" && app.cursorStatus_.version == "fixture-version",
              "plan and version come from about, past the update notice");
        check(app.cursorStatus_.email == "fixture@example.com", "account email reported");
        check(app.cursorModels_.size() == 2, "catalogue keeps only well-formed model lines");
        if (app.cursorModels_.size() == 2) {
            check(app.cursorModels_.at(0)["isDefault"].boolean(), "default marker parsed");
            check(app.cursorModels_.at(1)["displayName"].str() == "Grok 4.7 High" &&
                      app.cursorModels_.at(1)["isCurrent"].boolean(),
                  "zero-width characters and markers stripped from the display name");
        }

        Conversation* c = app.find("dm-cursor");
        check(c && c->kind == "cursor", "every install has a Cursor direct message");
        if (!c) return;
        c->cwd = makeDir(root, L"project");
        std::string error;
        c->permission = "edit";
        check(!app.sendToAgent(*c, AgentKind::Cursor, "hello", &error) && error.find("模型") != std::string::npos,
              "no model picked: refused instead of using the CLI default");
        app.applySettings(*c, J({{"cursorModel", "grok-unlisted"}}));
        check(c->cursorModel.empty(), "a model outside the reported catalogue is rejected");
        app.applySettings(*c, J({{"cursorModel", "grok-4.7-high"}}));
        check(c->cursorModel == "grok-4.7-high", "a catalogue model can be pinned");
        c->permission = "ask";
        check(!app.sendToAgent(*c, AgentKind::Cursor, "hello", &error) && error.find("逐項詢問") != std::string::npos,
              "ask permission refused before any checkpoint");
        c->permission = "edit";
        c->daily = true;
        check(!app.sendToAgent(*c, AgentKind::Cursor, "hello", &error) && error.find("日常模式") != std::string::npos,
              "daily mode refused");
        c->daily = false;

        // A full direct-message turn through the host: checkpoint, session, events and the saved chat id.
        c->messages.clear();
        check(app.sendToAgent(*c, AgentKind::Cursor, "review", &error), "direct message starts");
        until = GetTickCount64() + 30000;
        while ((app.anyBusy(*c) || !c->preparing.empty()) && GetTickCount64() < until) {
            pump();
            Sleep(10);
        }
        pump();
        const Json* reply = nullptr;
        for (const auto& m : c->messages)
            if (m["author"].str() == "cursor") reply = &m;
        check(reply != nullptr, "the reply is authored by Cursor");
        if (reply) {
            int texts = 0, tools = 0;
            for (const auto& b : (*reply)["blocks"].items()) {
                texts += b["type"].str() == "text";
                tools += b["type"].str() == "tool";
            }
            check(texts == 2 && tools == 3, "segments stay in order around the tool blocks");
            check((*reply)["resolvedModel"].str() == "Grok 4.7 High", "the model the CLI ran is recorded");
        }
        check(c->cursorSession == "fixture-chat", "chat id saved for the next turn");
        std::string receipt;
        util::readFile(util::joinPath(c->cwd, L"cursor-receipt.json"), receipt);
        check(Json::parse(receipt)["args"].dump().find("grok-4.7-high") != std::string::npos, "pinned model forwarded");

        // Group rotation passes Cursor over when its permission or model cannot be honoured.
        Conversation group;
        group.kind = "group";
        group.cursorMember = true;
        group.cwd = c->cwd;
        group.permission = "edit";
        group.cursorModel = "grok-4.7-high";
        check(app.groupCanRun(group, AgentKind::Cursor), "invited Cursor with a model can review");
        group.permission = "plan";
        check(!app.groupCanRun(group, AgentKind::Cursor), "plan permission skips Cursor");
        auto members = app.groupMembers(group);
        check(members.size() == 3 && members.back() == AgentKind::Cursor, "Cursor joins after the two leads");
        app.shutdown();
        pump();
    }
};

void postToUi(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(mutex);
    jobs.push_back(std::move(fn));
}

int main(int argc, char** argv) {
    if (argc > 1 && util::endsWith(util::toLower(argv[1]), "index.js")) return fixture(argc, argv);
    auto root = util::joinPath(util::getEnv(L"TEMP"), util::widen("cca-cursor-" + util::newId()));
    util::ensureDir(root);
    SetEnvironmentVariableW(L"CCA_DATA_DIR", makeDir(root, L"data").c_str());
    // An isolated Cursor config folder: the host must read the same file the CLI reads.
    auto config = makeDir(root, L"cursor-config");
    SetEnvironmentVariableW(L"CURSOR_CONFIG_DIR", config.c_str());
    auto originalKey = util::getEnv(L"CURSOR_API_KEY");
    SetEnvironmentVariableW(L"CURSOR_API_KEY", L"fixture-must-not-propagate");
    wchar_t exe[32768];
    GetModuleFileNameW(nullptr, exe, 32768);

    // Install resolution: the newest well-formed version folder that holds both node.exe and index.js.
    auto install = makeDir(root, L"cursor-agent");
    auto versions = makeDir(install, L"versions");
    for (const wchar_t* v : {L"2026.11.13-eacade1", L"2026.11.19-f005e9d", L"2026.11.19-12-30-45-abc123",
                             L"2027.1.2-zz", L"2028.01.01-deadbee"}) {
        auto dir = makeDir(versions, v);
        touch(util::joinPath(dir, L"index.js"));
        if (std::wstring(v) != L"2028.01.01-deadbee") touch(util::joinPath(dir, L"node.exe"));
    }
    CursorLaunch found = CursorSession::resolve(install);
    check(found.ok() && found.version == L"2026.11.19-12-30-45-abc123", "newest complete version folder chosen");
    check(CursorSession::resolve(util::joinPath(root, L"nowhere")).node.empty(), "missing install reported");

    auto fake = makeDir(root, L"fake");
    const auto script = util::joinPath(fake, L"index.js");
    touch(script);
    check(CopyFileW(exe, util::joinPath(fake, L"node.exe").c_str(), FALSE), "prepare isolated node.exe");
    CursorLaunch launch = CursorSession::resolve(script);
    check(launch.ok() && launch.node == util::joinPath(fake, L"node.exe"), "a bundle path runs with its own node.exe");

    AgentConfig cfg;
    cfg.cwd = root;
    cfg.permission = "edit";
    cfg.model = "grok-4.7-high";
    cfg.instructions = "fixture-instructions";
    cfg.resumeId = "previous-chat";
    std::vector<AgentEvent> events;
    auto session = std::make_shared<CursorSession>([&](const AgentEvent& e) { events.push_back(e); });
    std::string error;
    auto run = [&](const std::string& prompt) {
        events.clear();
        check(session->send(launch, cfg, prompt, &error), "fixture starts");
        auto until = GetTickCount64() + 15000;
        while (session->busy() && GetTickCount64() < until) {
            pump();
            Sleep(5);
        }
        pump();
        check(!session->busy(), "fixture completes");
        return events.empty() ? "" : events.back().data["status"].str();
    };
    auto shellEnd = [&]() -> Json {
        for (const auto& e : events)
            if (e.kind == AgentEvent::Kind::ToolEnd && e.id == "call-1") return e.data;
        return Json();
    };

    check(run("review") == "completed", "result plus zero exit completes");
    bool usage = false, info = false, unknownTool = false;
    std::vector<std::string> textIds;
    for (const auto& e : events) {
        if (e.kind == AgentEvent::Kind::Usage)
            usage = e.data["inputTokens"].i64() == 40 && e.data["cachedTokens"].i64() == 7;
        if (e.kind == AgentEvent::Kind::SessionInfo) info = e.data["sessionId"].str() == "fixture-chat";
        if (e.kind == AgentEvent::Kind::Text) textIds.push_back(e.id);
        if (e.kind == AgentEvent::Kind::ToolStart && e.id == "call-3") unknownTool = e.data["kind"].str() == "other";
    }
    check(usage && info, "usage summed from model calls and chat id reported");
    check(textIds.size() == 2 && textIds[0] != textIds[1], "each finished segment is its own text block");
    check(unknownTool, "an unknown tool is shown as other, never as a read");
    check(shellEnd()["exitCode"].i64(-1) == 0 && shellEnd()["status"].str() == "ok", "CLI-reported exit code kept");
    std::string receipt;
    util::readFile(util::joinPath(root, L"cursor-receipt.json"), receipt);
    auto r = Json::parse(receipt);
    const std::string args = r["args"].dump();
    check(!r["apiKeyPresent"].boolean(), "API key environment stripped");
    check(r["input"].str().find("fixture-instructions") != std::string::npos, "instructions sent on stdin");
    check(args.find("stream-json") != std::string::npos && args.find("--trust") != std::string::npos,
          "headless stream-json with a trusted workspace");
    check(args.find("--stream-partial-output") == std::string::npos, "partial output (duplicated text) not requested");
    check(args.find("previous-chat") != std::string::npos, "chat resume forwarded");
    check(args.find("--force") == std::string::npos, "edit permission never forces commands");

    check(run("no-exit") == "completed" && !shellEnd().has("exitCode"), "absent exit code is not invented");
    run("rejected");
    check(shellEnd()["status"].str() == "error" && shellEnd()["output"].str().find("拒絕") != std::string::npos,
          "a rejected command is reported as refused");
    check(run("missing-result") == "failed", "missing terminal result cannot pass");
    check(run("exit-error") == "failed", "nonzero process exit cannot pass");

    cfg.permission = "bypass";
    run("review");
    util::readFile(util::joinPath(root, L"cursor-receipt.json"), receipt);
    check(Json::parse(receipt)["args"].dump().find("--force") != std::string::npos, "full access forwards --force");

    cfg.permission = "plan";
    check(!session->send(launch, cfg, "review", &error), "plan refused: headless writes cannot be ruled out");
    cfg.permission = "ask";
    check(!session->send(launch, cfg, "review", &error), "ask refused: headless mode cannot ask");
    cfg.permission = "edit";
    util::writeFileAtomic(util::joinPath(config, L"cli-config.json"), R"({"approvalMode":"unrestricted"})");
    check(!session->send(launch, cfg, "review", &error) && error.find("unrestricted") != std::string::npos,
          "edit refused while Cursor's own config runs everything");
    util::writeFileAtomic(util::joinPath(config, L"cli-config.json"), "{broken");
    check(!session->send(launch, cfg, "review", &error), "unreadable Cursor config refused for edit");
    util::writeFileAtomic(util::joinPath(config, L"cli-config.json"), R"({"approvalMode":"allowlist"})");
    // A repository's .cursor\cli.json replaces the user's allow list; only read entries may pass under edit.
    const auto projectConfig = util::joinPath(makeDir(root, L".cursor"), L"cli.json");
    util::writeFileAtomic(projectConfig, R"json({"permissions":{"allow":["Read(**)","Shell(*)"]}})json");
    check(!session->send(launch, cfg, "review", &error) && error.find("Shell(*)") != std::string::npos,
          "a project allow list that runs commands unasked is refused for edit");
    util::writeFileAtomic(projectConfig, R"json({"permissions":{"allow":["Read(src/**)"]}})json");
    check(run("review") == "completed", "read-only project entries are accepted");
    cfg.permission = "bypass";
    util::writeFileAtomic(projectConfig, R"json({"permissions":{"allow":["Shell(*)"]}})json");
    check(run("review") == "completed", "full access is the user's explicit choice");
    cfg.permission = "edit";
    DeleteFileW(projectConfig.c_str());
    cfg.model.clear();
    check(!session->send(launch, cfg, "review", &error), "empty model refused");
    cfg.model = "--force";
    check(!session->send(launch, cfg, "review", &error), "a model id cannot smuggle a flag");
    cfg.model = "grok-4.7-high";
    session->shutdown();

    CursorTestAccess::run(root, exe, script);
    SetEnvironmentVariableW(L"CURSOR_API_KEY", originalKey.empty() ? nullptr : originalKey.c_str());
    std::printf("%d Cursor checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
