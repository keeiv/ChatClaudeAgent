// Opt-in smoke test of the actual ClaudeSession adapter and shared Docker MCP server.
// --connect-only checks CLI/MCP loading without inference; --run uses subscription quota.
#include <windows.h>

#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include "claude_agent.hpp"
#include "sandbox_mcp.hpp"
#include "util.hpp"

namespace {
std::mutex queueMutex;
std::deque<std::function<void()>> callbacks;

void pump() {
    std::deque<std::function<void()>> ready;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        ready.swap(callbacks);
    }
    for (auto& fn : ready) fn();
    Sleep(20);
}
}  // namespace

void postToUi(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(queueMutex);
    callbacks.push_back(std::move(fn));
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 3 || (std::wstring(argv[2]) != L"--run" && std::wstring(argv[2]) != L"--connect-only")) {
        std::puts("Usage: sandbox_live.exe <claude.exe> --connect-only|--run [image]");
        return 2;
    }
    bool live = std::wstring(argv[2]) == L"--run";
    std::string image = argc > 3 ? util::narrow(argv[3]) : "busybox:1.36";
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::wstring root = util::joinPath(temp, util::widen("cca-sandbox-live-" + util::newId()));
    std::wstring data = util::joinPath(root, L"data"), project = util::joinPath(root, L"project");
    util::ensureDir(util::joinPath(project, L"src"));
    util::ensureDir(data);
    SetEnvironmentVariableW(L"CCA_DATA_DIR", data.c_str());
    std::string token = "CCA_PROBE_" + util::newId();
    util::writeFileAtomic(util::joinPath(project, L"src\\probe.txt"), token + "\n");
    util::writeFileAtomic(util::joinPath(project, L".env"), "TEST_SECRET=fixture-only\n");
    AgentConfig config;
    config.cwd = project;
    config.model = "haiku";
    config.effort = "low";
    config.permission = "edit";
    config.sandbox = findSandboxMcp();
    if (!config.sandbox.on()) {
        std::puts("BLOCKED: Python, Docker or sandbox script unavailable");
        return 3;
    }
    bool connected = false;
    std::string error;
    bool initialized = false, done = false, turnCompleted = false;
    bool statusOk = false, runOk = false, unexpected = false;
    Json result = Json::object();
    std::map<std::string, std::string> tools;
    std::shared_ptr<ClaudeSession> session;
    session = std::make_shared<ClaudeSession>(argv[1], [&](const AgentEvent& ev) {
        using K = AgentEvent::Kind;
        if (ev.kind == K::SessionInfo) {
            if (ev.data.has("version")) initialized = true;
            for (const auto& server : ev.data["mcpServers"].items())
                if (server["name"].str() == kSandboxServer && server["status"].str() == "connected") {
                    bool status = false, run = false;
                    for (const auto& tool : server["tools"].items()) {
                        status |= tool.str() == "sandbox_status";
                        run |= tool.str() == "sandbox_run";
                    }
                    // The per-turn init frame repeats status without tool descriptions.
                    connected |= status && run;
                }
        }
        if (ev.kind == K::ToolStart) {
            std::string name = ev.data["tool"].str();
            tools[ev.id] = name;
            std::printf("tool: %s\n", name.c_str());
            // Claude may first resolve deferred MCP schemas; ToolSearch executes no project code.
            if (name != "ToolSearch" && name != "mcp__cca-sandbox__sandbox_status" &&
                name != "mcp__cca-sandbox__sandbox_run")
                unexpected = true;
        }
        if (ev.kind == K::Approval) {
            unexpected = true;
            session->respondApproval(ev.id, choice::kDeny, "Sandbox test only; no host tool permission");
        }
        if (ev.kind == K::ToolEnd) {
            const std::string& name = tools[ev.id];
            if (name == "mcp__cca-sandbox__sandbox_status") statusOk = ev.data["status"].str() == "ok";
            if (name == "mcp__cca-sandbox__sandbox_run") {
                result = ev.data;
                const std::string text = ev.data["output"].str();
                runOk = ev.data["status"].str() == "ok" && ev.data["exitCode"].i64(-1) == 0 &&
                        text.find(token) != std::string::npos && text.find("uid=65534") != std::string::npos &&
                        text.find("env=hidden") != std::string::npos;
            }
        }
        if (ev.kind == K::TurnDone) {
            done = true;
            turnCompleted = ev.data["status"].str() == "completed";
            if (!turnCompleted) std::printf("turn failed: %s\n", ev.data["error"].str().c_str());
        }
        if (ev.kind == K::Exited && !done) {
            done = true;
            std::printf("CLI exited: %s\n", ev.data.dump().c_str());
        }
    });
    config.instructions = sandboxInstructions() + "\n這是隔離工具驗證，只能使用這兩個 MCP 工具，不使用其他工具。";
    session->configure(config);
    bool started = session->start(&error);
    ULONGLONG deadline = GetTickCount64() + 45000;
    ULONGLONG nextStatus = 0;
    while (started && !connected && !done && GetTickCount64() < deadline) {
        pump();
        if (initialized && GetTickCount64() >= nextStatus) {
            session->requestMcpStatus();
            nextStatus = GetTickCount64() + 1000;
        }
    }
    std::printf("CCA adapter initialize: %s\n", initialized ? "PASS" : "FAIL");
    std::printf("CCA session MCP load: %s\n", connected ? "PASS" : "FAIL");
    if (!connected || !live) {
        session->shutdown();
        pump();
        return connected ? 0 : 1;
    }
    std::string script =
        "echo uid=$(id -u); cat /input/src/probe.txt; "
        "test ! -e /input/.env && echo env=hidden; echo evidence > /output/proof.txt";
    Json args = J({{"image", image}, {"command", Json::Array{"sh", "-c", script}}});
    std::string prompt =
        "指揮官要求一次沙盒驗證。先呼叫 sandbox_status，再呼叫 sandbox_run，參數如下。"
        "不得使用其他工具、不得改主機檔案。兩次工具結束後簡短回報實際結果，不要重試。\n" +
        args.dump();
    started = session->send(prompt, &error);
    deadline = GetTickCount64() + 180000;
    while (started && !done && GetTickCount64() < deadline) pump();
    session->shutdown();
    pump();
    std::string unchanged;
    bool sourceOk = util::readFile(util::joinPath(project, L"src\\probe.txt"), unchanged) && unchanged == token + "\n";
    // A successful tool is insufficient when the surrounding turn failed or the CLI crashed.
    bool ok =
        started && initialized && connected && done && turnCompleted && statusOk && runOk && !unexpected && sourceOk;
    std::printf("Claude -> CCA MCP -> Docker: %s\n%s\n", ok ? "PASS" : "FAIL", result.dump().c_str());
    util::writeFileAtomic(util::joinPath(root, L"result.json"), J({{"ok", ok},
                                                                   {"connected", connected},
                                                                   {"initialized", initialized},
                                                                   {"turn_completed", turnCompleted},
                                                                   {"status_ok", statusOk},
                                                                   {"run_ok", runOk},
                                                                   {"unexpected_tool", unexpected},
                                                                   {"source_unchanged", sourceOk},
                                                                   {"result", result}})
                                                                    .dump());
    std::printf("Evidence: %s\n", util::narrow(root).c_str());
    return ok ? 0 : 1;
}
