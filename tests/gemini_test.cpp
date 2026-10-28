// Real child-process fixtures cover Gemini streaming, exit failures and permission boundaries without model usage.
#include <windows.h>

#include <cstdio>
#include <iostream>
#include <mutex>
#include <vector>

#include "app.hpp"
#include "gemini_agent.hpp"
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
int fixture(int argc, char** argv) {
    std::string input, line;
    while (std::getline(std::cin, line)) input += line + "\n";
    Json args = Json::array();
    for (int i = 1; i < argc; ++i) args.push(argv[i]);
    util::writeFileAtomic(L"receipt.json",
                          J({{"args", args},
                             {"input", input},
                             {"apiKeyPresent", !util::getEnv(L"GEMINI_API_KEY").empty()},
                             {"accountSettings", util::narrow(util::getEnv(L"GEMINI_CLI_SYSTEM_SETTINGS_PATH"))}})
                              .dump());
    auto out = [](Json j) { std::cout << j.dump() << std::endl; };
    out(J({{"type", "init"}, {"session_id", "fixture-session"}, {"model", "fixture-model"}}));
    out(J({{"type", "tool_use"},
           {"tool_id", "read-1"},
           {"tool_name", "read_file"},
           {"parameters", J({{"file_path", "result.txt"}})}}));
    out(J({{"type", "tool_result"}, {"tool_id", "read-1"}, {"status", "success"}, {"output", "fixture"}}));
    out(J({{"type", "message"}, {"role", "assistant"}, {"delta", true}, {"content", "已查證"}}));
    if (input.find("missing-result") != std::string::npos) return 0;
    out(J({{"type", "result"},
           {"status", "success"},
           {"stats", J({{"input_tokens", 30}, {"output_tokens", 4}, {"cached", 6}})}}));
    return input.find("exit-error") != std::string::npos ? 1 : 0;
}
}  // namespace

struct GeminiTestAccess {
    static void run(const std::wstring& root, const std::wstring& exe) {
        const auto oldPath = util::getEnv(L"PATH");
        SetEnvironmentVariableW(L"PATH", L"");
        App app([](const std::string&) {});
        const auto node = util::joinPath(root, L"node.exe");
        check(CopyFileW(exe.c_str(), node.c_str(), FALSE), "prepare isolated account probe");
        SetEnvironmentVariableW(L"PATH", root.c_str());
        app.settings_["geminiPath"] = util::narrow(exe);
        app.geminiLogin_ = std::make_unique<ChildProcess>();
        ProcessOptions opt;
        opt.exe = exe;
        opt.args = {L"--wait"};
        opt.cwd = root;
        std::string error;
        check(app.geminiLogin_->start(opt, {}, {}, {}, &error), "login fixture stays running");
        app.checkGemini();
        check(app.geminiChecking_, "account check allowed with login window open");
        auto until = GetTickCount64() + 10000;
        while (app.geminiChecking_ && GetTickCount64() < until) {
            pump();
            Sleep(5);
        }
        pump();
        check(app.geminiStatus_.state == "ok", "probe result reaches native auth state");
        check(app.geminiStatus_.version == "fixture", "probe version reaches native state");
        app.geminiLogin_->terminate();
        app.shutdown();
        pump();
        SetEnvironmentVariableW(L"PATH", oldPath.c_str());
    }
};

void postToUi(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(mutex);
    jobs.push_back(std::move(fn));
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--wait") {
        Sleep(30000);
        return 0;
    }
    if (argc > 1 && std::string(argv[1]).find("gemini_account.cjs") != std::string::npos) {
        std::cout << "CCA_GEMINI_STATUS:"
                  << J({{"state", "ok"}, {"version", "fixture"}, {"planLabel", "Fixture"}}).dump() << std::endl;
        return 0;
    }
    if (argc > 1 && std::string(argv[1]) == "--output-format") return fixture(argc, argv);
    auto root = util::joinPath(util::getEnv(L"TEMP"), util::widen("cca-gemini-" + util::newId()));
    util::ensureDir(root);
    SetEnvironmentVariableW(L"CCA_DATA_DIR", root.c_str());
    auto originalKey = util::getEnv(L"GEMINI_API_KEY");
    SetEnvironmentVariableW(L"GEMINI_API_KEY", L"fixture-must-not-propagate");
    wchar_t exe[32768];
    GetModuleFileNameW(nullptr, exe, 32768);
    AgentConfig cfg;
    cfg.cwd = root;
    cfg.permission = "edit";
    cfg.instructions = "fixture-instructions";
    cfg.resumeId = "previous-session";
    std::vector<AgentEvent> events;
    auto session = std::make_shared<GeminiSession>([&](const AgentEvent& e) { events.push_back(e); });
    std::string error;
    auto run = [&](const std::string& prompt) {
        events.clear();
        check(session->send(exe, cfg, prompt, &error), "fixture starts");
        auto until = GetTickCount64() + 10000;
        while (session->busy() && GetTickCount64() < until) {
            pump();
            Sleep(5);
        }
        pump();
        check(!session->busy(), "fixture completes");
        return events.empty() ? "" : events.back().data["status"].str();
    };
    check(run("review") == "completed", "result plus zero exit completes");
    bool usage = false, tool = false, info = false;
    for (const auto& e : events) {
        if (e.kind == AgentEvent::Kind::Usage) usage = e.data["inputTokens"].i64() == 30;
        if (e.kind == AgentEvent::Kind::SessionInfo) info = e.data["sessionId"].str() == "fixture-session";
        if (e.kind == AgentEvent::Kind::ToolEnd) tool = !e.data.has("exitCode");
    }
    check(usage && tool && info, "usage and session retained without fabricated test exit code");
    std::string receipt;
    util::readFile(util::joinPath(root, L"receipt.json"), receipt);
    auto r = Json::parse(receipt);
    check(!r["apiKeyPresent"].boolean(), "API billing environment stripped");
    check(r["input"].str().find("fixture-instructions") != std::string::npos, "instructions use stdin");
    check(r["args"].dump().find("previous-session") != std::string::npos, "session resume forwarded");
    check(r["args"].dump().find("auto_edit") != std::string::npos, "edit permission remains auto_edit");
    check(!r["accountSettings"].str().empty(), "Google account authentication enforced");
    check(run("missing-result") == "failed", "missing terminal result cannot pass");
    check(run("exit-error") == "failed", "nonzero process exit cannot pass");
    cfg.permission = "plan";
    check(!session->send(exe, cfg, "review", &error), "unsafe headless plan transition blocked");
    session->shutdown();
    GeminiTestAccess::run(root, exe);
    SetEnvironmentVariableW(L"GEMINI_API_KEY", originalKey.empty() ? nullptr : originalKey.c_str());
    std::printf("%d Gemini checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
