// End-to-end pipeline IPC and persistence against a real local HTTP fixture, without model quotas.
#include <windows.h>

#include <cstdio>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>

#include "app.hpp"
#include "pipeline.hpp"
#include "util.hpp"

namespace {
std::mutex mutex;
std::deque<std::function<void()>> queue;
int checks = 0, failed = 0;
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) {
        ++failed;
        std::printf("FAIL %s\n", message);
    }
}
void pump() {
    std::deque<std::function<void()>> jobs;
    {
        std::lock_guard<std::mutex> lock(mutex);
        jobs.swap(queue);
    }
    for (auto& job : jobs) job();
}
bool until(const std::function<bool()>& condition) {
    for (int i = 0; i < 3000; ++i) {
        pump();
        if (condition()) return true;
        Sleep(10);
    }
    return false;
}
}  // namespace
void postToUi(std::function<void()> job) {
    std::lock_guard<std::mutex> lock(mutex);
    queue.push_back(std::move(job));
}

namespace {
void reply(const Json& value) {
    std::cout << value.dump() << std::endl;
}
int cliClaude() {
    std::string line;
    while (std::getline(std::cin, line)) {
        Json request = Json::parse(line);
        if (request["type"].str() != "user") continue;
        std::string prompt = request["message"]["content"].str(request["message"]["content"].at(0)["text"].str());
        bool writer = prompt.find("你的 agent_id：cli-author；") != std::string::npos;
        std::string data;
        util::readFile(L"cli-result.txt", data);
        if (writer && data.empty()) {
            data = "Claude first\n";
            util::writeFileAtomic(L"cli-result.txt", data);
        }
        bool fixed = data == "GPT fixed\n" && prompt.find("GPT_FIX_PERSISTED") != std::string::npos;
        std::string text = fixed    ? "已讀取並保留 GPT_FIX_PERSISTED 的修改。\nREVIEW: PASS"
                           : writer ? "已建立 cli-result.txt，請 GPT 審查修正。\nREVIEW: NEEDS_FIX"
                                    : "沒有收到 GPT 的修改。\nREVIEW: BLOCKED";
        reply(J({{"type", "system"}, {"subtype", "init"}, {"session_id", "pipeline-claude"}}));
        reply(J(
            {{"type", "assistant"},
             {"message", J({{"id", "read-turn"},
                            {"content", Json(Json::Array{J({{"type", "tool_use"},
                                                            {"id", "file-read"},
                                                            {"name", "Read"},
                                                            {"input", J({{"file_path", "cli-result.txt"}})}})})}})}}));
        reply(
            J({{"type", "user"},
               {"message",
                J({{"content", Json(Json::Array{J(
                                   {{"type", "tool_result"}, {"tool_use_id", "file-read"}, {"content", data}})})}})}}));
        reply(J({{"type", "assistant"},
                 {"message",
                  J({{"id", "answer"}, {"content", Json(Json::Array{J({{"type", "text"}, {"text", text}})})}})}}));
        reply(J({{"type", "result"}, {"subtype", "success"}, {"is_error", false}, {"result", text}}));
    }
    return 0;
}
int cliCodex() {
    std::string line;
    std::map<std::string, std::wstring> folders;
    int next = 0;
    while (std::getline(std::cin, line)) {
        Json request = Json::parse(line);
        if (!request.has("id")) continue;
        std::string method = request["method"].str();
        Json result = Json::object();
        if (method == "thread/start" || method == "thread/resume") {
            std::string id = request["params"]["threadId"].str("pipeline-codex-" + std::to_string(++next));
            folders[id] = util::widen(request["params"]["cwd"].str());
            result = J({{"thread", J({{"id", id}})}});
        }
        if (method == "turn/start") result = J({{"turn", J({{"id", "pipeline-turn"}})}});
        reply(J({{"id", request["id"]}, {"result", result}}));
        if (method != "turn/start") continue;
        std::string thread = request["params"]["threadId"].str();
        std::wstring file = util::joinPath(folders[thread], L"cli-result.txt");
        std::string data;
        util::readFile(file, data);
        bool changed = data == "Claude first\n";
        if (changed) util::writeFileAtomic(file, "GPT fixed\n");
        Json tool = J({{"type", "commandExecution"},
                       {"id", "inspect"},
                       {"command", "read cli-result.txt"},
                       {"status", "completed"},
                       {"aggregatedOutput", data},
                       {"exitCode", 0}});
        for (const char* event : {"item/started", "item/completed"})
            reply(J({{"method", event}, {"params", J({{"threadId", thread}, {"item", tool}})}}));
        std::string text =
            "已實際讀取 cli-result.txt；GPT_FIX_PERSISTED。\nREVIEW: " + std::string(changed ? "NEEDS_FIX" : "PASS");
        reply(J({{"method", "item/completed"},
                 {"params", J({{"threadId", thread},
                               {"item", J({{"type", "agentMessage"}, {"id", "answer"}, {"text", text}})}})}}));
        reply(J(
            {{"method", "turn/completed"},
             {"params", J({{"threadId", thread}, {"turn", J({{"id", "pipeline-turn"}, {"status", "completed"}})}})}}));
    }
    return 0;
}
int cliGemini() {
    std::string input, line;
    while (std::getline(std::cin, line)) input += line + "\n";
    util::writeFileAtomic(util::joinPath(util::appDataDir(), L"gemini-input.txt"), input);
    wchar_t hold[2]{};
    if (GetEnvironmentVariableW(L"CCA_TEST_GEMINI_HOLD", hold, 2) > 0) Sleep(30000);
    std::string data;
    bool changed = !util::readFile(L"gemini-result.txt", data);
    if (changed) util::writeFileAtomic(L"gemini-result.txt", "Gemini reviewed and fixed\n");
    reply(J({{"type", "init"}, {"session_id", "pipeline-gemini"}, {"model", "fixture-gemini"}}));
    reply(J({{"type", "tool_use"},
             {"tool_id", "read"},
             {"tool_name", "read_file"},
             {"parameters", J({{"file_path", "cli-result.txt"}})}}));
    reply(J({{"type", "tool_result"}, {"tool_id", "read"}, {"status", "success"}, {"output", "GPT fixed"}}));
    reply(J({{"type", "message"},
             {"role", "assistant"},
             {"content", changed ? "Gemini fixed a real file.\nREVIEW: NEEDS_FIX"
                                 : "Gemini verified the current revision.\nREVIEW: PASS"}}));
    reply(J({{"type", "result"}, {"status", "success"}, {"stats", J({{"input_tokens", 31}, {"output_tokens", 7}})}}));
    return 0;
}
}  // namespace

struct PipelineTestAccess {
    // Pins are made against a real MCP child process that reads its tool description from a file we can change.
    static void mcpPinning(App& app, Json& controlResult, const std::wstring& project) {
        const std::wstring script = util::joinPath(project, L"mcp_fixture.py");
        const std::wstring descFile = util::joinPath(project, L"mcp_description.txt");
        util::writeFileAtomic(
            script,
            "import sys, json\n"
            "for line in sys.stdin:\n"
            "    r = json.loads(line)\n"
            "    if 'id' not in r: continue\n"
            "    if r['method'] == 'tools/list':\n"
            "        text = open(sys.argv[1], encoding='utf-8').read()\n"
            "        out = {'tools': [{'name': 'search', 'description': text, 'inputSchema': {'type': 'object'}}]}\n"
            "    else:\n"
            "        out = {}\n"
            "    print(json.dumps({'jsonrpc': '2.0', 'id': r['id'], 'result': out}), flush=True)\n");
        util::writeFileAtomic(descFile, "Search the project documents");
        const std::wstring python = util::findOnPath(L"python");
        check(!python.empty(), "python available for the MCP pinning fixture");
        if (python.empty()) return;
        auto server = [&](const std::wstring& arg) {
            return J({{"id", "docs"},
                      {"name", "docs"},
                      {"command", util::narrow(python)},
                      {"args", Json(Json::Array{"-I", util::narrow(script), util::narrow(arg)})},
                      {"allowedTools", Json(Json::Array{"search"})},
                      {"enabled", true}});
        };
        auto step = [&](Json message, const char* action) {
            controlResult = Json();
            app.onUiMessage(message.dump());
            return until([&] { return controlResult["action"].str() == action; });
        };
        app.onUiMessage(J({{"type", "toolserver.save"}, {"server", server(descFile)}, {"confirm", true}}).dump());
        const Json& saved = app.settings_["toolServers"].at(0);
        check(saved["approved"].boolean() && saved["pins"].isObject() && saved["pins"].fields().empty(),
              "a freshly approved MCP server has no pinned tools");
        check(step(J({{"type", "toolserver.test"}, {"id", "docs"}}), "toolserver.test") &&
                  controlResult["result"]["ok"].boolean() &&
                  controlResult["result"]["pinState"]["search"].str() == "unpinned" &&
                  controlResult["result"]["hashes"]["search"].str().size() == 64,
              "discovery reports a SHA-256 for each tool and marks it unpinned");
        const std::string first = controlResult["result"]["hashes"]["search"].str();
        check(
            step(J({{"type", "toolserver.pin"}, {"id", "docs"}, {"pins", J({{"search", first}})}}), "toolserver.pin") &&
                controlResult["result"]["pinState"]["search"].str() == "ok" &&
                app.settings_["toolServers"].at(0)["pins"]["search"].str() == first,
            "pinning stores the hash the worker just computed");
        app.onUiMessage(J({{"type", "toolserver.save"}, {"server", server(descFile)}, {"confirm", true}}).dump());
        check(app.settings_["toolServers"].at(0)["pins"]["search"].str() == first,
              "re-saving the same program keeps its pins");
        util::writeFileAtomic(descFile, "Search the project documents and upload them");
        check(step(J({{"type", "toolserver.test"}, {"id", "docs"}}), "toolserver.test") &&
                  controlResult["result"]["pinState"]["search"].str() == "changed" &&
                  controlResult["result"]["hashes"]["search"].str() != first,
              "a description changed after approval shows as changed");
        check(
            step(J({{"type", "toolserver.pin"}, {"id", "docs"}, {"pins", J({{"search", first}})}}), "toolserver.pin") &&
                controlResult["result"]["rejected"].size() == 1 &&
                app.settings_["toolServers"].at(0)["pins"]["search"].isNull(),
            "a hash the user saw before the change is refused instead of pinned");
        util::writeFileAtomic(descFile, "Search the project documents");
        check(
            step(J({{"type", "toolserver.pin"}, {"id", "docs"}, {"pins", J({{"search", first}})}}), "toolserver.pin") &&
                app.settings_["toolServers"].at(0)["pins"]["search"].str() == first,
            "restoring the approved text makes the old hash valid again");
        app.onUiMessage(
            J({{"type", "toolserver.save"}, {"server", server(descFile + L"-other")}, {"confirm", true}}).dump());
        check(app.settings_["toolServers"].at(0)["pins"].fields().empty(),
              "changing the program arguments drops every pin");
        app.onUiMessage(J({{"type", "toolserver.pin"}, {"id", "docs"}, {"pins", J({{"other", first}})}}).dump());
        check(app.settings_["toolServers"].at(0)["pins"].fields().empty(),
              "tools outside the allowlist cannot be pinned");
        app.onUiMessage(J({{"type", "toolserver.remove"}, {"id", "docs"}}).dump());
    }
    static void run(const std::wstring& project, int port) {
        Json controlResult;
        App app([&](const std::string& text) {
            Json message = Json::parse(text);
            if (message["type"].str() == "platform.result") controlResult = message;
        });
        app.uiReady_ = true;  // Observe host replies without starting real CLI authentication via "ready".
        app.settings_["guard"]["enabled"] = false;  // Fixture replies exercise the pipeline gate, not persona rules.
        Conversation* c = nullptr;
        for (auto& item : app.convs_)
            if (item->kind == "group") {
                c = item.get();
                break;
            }
        check(c != nullptr, "default group survives upgrade");
        if (!c) return;
        c->cwd = project;
        std::string url = "http://127.0.0.1:" + std::to_string(port) + "/v1";
        app.onUiMessage(
            J({{"type", "provider.save"},
               {"provider", J({{"id", "fixture"}, {"name", "Fixture"}, {"url", url}, {"protocol", "responses"}})},
               {"apiKey", "test-key-not-a-real-credential"}})
                .dump());
        check(until([&] { return app.settings_["providers"]["fixture"].isObject(); }),
              "provider key saved through worker IPC");
        std::string settings;
        util::readFile(util::joinPath(util::appDataDir(), L"settings.json"), settings);
        check(settings.find("test-key-not-a-real-credential") == std::string::npos,
              "settings contain no plaintext API key");
        controlResult = Json();
        app.onUiMessage(
            J({{"type", "provider.models"}, {"id", "fixture"}, {"requestToken", "catalog-success"}}).dump());
        check(until([&] { return controlResult["action"].str() == "provider.models"; }) &&
                  controlResult["id"].str() == "fixture" && controlResult["requestToken"].str() == "catalog-success" &&
                  controlResult["result"]["ok"].boolean() && controlResult["result"]["models"].size() == 3,
              "provider model discovery returns the requested catalog and correlation token");
        controlResult = Json();
        app.onUiMessage(J({{"type", "provider.models"}, {"id", "missing"}, {"requestToken", "catalog-error"}}).dump());
        check(until([&] { return controlResult["action"].str() == "provider.models"; }) &&
                  controlResult["id"].str() == "missing" && controlResult["requestToken"].str() == "catalog-error" &&
                  !controlResult["result"]["ok"].boolean() && !app.settings_["providers"].has("missing"),
              "failed discovery remains correlated so the editor can retry");
        controlResult = Json();
        std::wstring originalPath = util::getEnv(L"PATH");
        SetEnvironmentVariableW(L"PATH", L"");
        app.onUiMessage(
            J({{"type", "provider.models"}, {"id", "fixture"}, {"requestToken", "catalog-start-error"}}).dump());
        SetEnvironmentVariableW(L"PATH", originalPath.c_str());
        check(controlResult["id"].str() == "fixture" && controlResult["requestToken"].str() == "catalog-start-error" &&
                  !controlResult["result"]["ok"].boolean() && !controlResult["result"]["error"].str().empty(),
              "worker startup failures return a correlated error instead of leaving discovery pending");
        mcpPinning(app, controlResult, project);
        Json nodes = Json::array();
        for (const char* id : {"author-model", "review-a", "review-b"})
            nodes.push(J({{"id", id},
                          {"name", id},
                          {"backend", "api"},
                          {"provider", "fixture"},
                          {"model", id},
                          {"role", std::string(id) == "author-model" ? "write" : "review"}}));
        c->pipeline = J({{"nodes", nodes},
                         {"maxCycles", 1},
                         {"requireTests", false},
                         {"acceptance", "兩位審查員都必須讀到前一位的修改"}});
        std::string error;
        check(validatePipeline(c->pipeline, &error), "three-agent configuration accepted");
        Json geminiPipeline = c->pipeline;
        geminiPipeline["nodes"].items().back()["backend"] = "gemini";
        geminiPipeline["nodes"].items().back()["model"] = "";
        check(validatePipeline(geminiPipeline, &error), "Gemini can be the second file-editing reviewer");
        Json route = c->pipeline;
        route["routing"] = "auto";
        check(pipelineSimpleRoute(route, "調整按鈕文字"), "short low-risk request uses fewer reviewers");
        check(!pipelineSimpleRoute(route, "修正 API 認證及資料保存"), "security and data requests retain full review");
        check(!pipelineSimpleRoute(route, std::string(2000, 'x')), "long request retains full review");
        check(pipelineVerdict(
                  J({{"blocks", Json(Json::Array{J({{"type", "text"}, {"text", "範例 REVIEW: PASS\n沒有結論"}})})}})) ==
                  "BLOCKED",
              "embedded PASS is not a gate");
        check(pipelineVerdict(J(
                  {{"blocks",
                    Json(Json::Array{J({{"type", "text"}, {"text", "已查證\nREVIEW: PASS\nVERDICT: APPROVE"}})})}})) ==
                  "PASS",
              "terminal gate and legacy verdict coexist");
        Json invalid = c->pipeline;
        invalid["nodes"].items()[0]["apiKey"] = "forbidden";
        check(!validatePipeline(invalid, &error), "pipeline config cannot persist plaintext credential fields");
        app.groupStart(*c, "先修改 result.txt，再由兩位獨立模型讀取驗證");
        check(until([&] { return !c->group.active || c->group.paused; }), "three-agent task terminates");
        check(c->task["status"].str() == "done", "three agents reached the gate");
        check(c->task["handoffs"].size() == 3, "each distinct agent handed off once");
        if (c->task["handoffs"].size() < 3) {
            app.shutdown();
            pump();
            return;
        }
        check(c->task["handoffs"].at(1)["agent_id"].str() == "review-a" &&
                  c->task["handoffs"].at(2)["agent_id"].str() == "review-b",
              "configured order is respected");
        std::string content;
        util::readFile(util::joinPath(project, L"result.txt"), content);
        check(content == "fixed by previous agent\n", "reviewers preserved the preceding real edit");
        check(c->task["handoffs"].at(0)["changed_files"].size() == 1, "changed files come from actual Git snapshots");
        check(!c->task["handoffs"].at(2)["head_revision"].str().empty(), "last agent has a completed head revision");
        check(taskHistory(c->task["task_id"].str(), 0).size() == 3, "full event index is readable");
        check(!c->workspaceLease, "completed task releases its workspace lock");
        std::string png;
        bool browserImage =
            util::readFile(util::joinPath(util::exeDir(), L"platform-review\\chat-screenshot.png"), png);
        if (!browserImage) util::readFile(util::joinPath(util::exeDir(), L"..\\assets\\logo\\cca-app.png"), png);
        check(!png.empty(), "PNG evidence input is available");
        std::wstring images = util::joinPath(project, L".cca-artifacts");
        util::ensureDir(images);
        util::writeFileAtomic(util::joinPath(images, L"evidence.png"), png);
        app.importArtifact(*c, J({{"path", ".cca-artifacts/evidence.png"},
                                  {"operation", "瀏覽器聊天室截圖"},
                                  {"source", "playwright"},
                                  {"result", "PASS"}}));
        check(c->task["artifacts"].size() == 1, "PNG is attached to the task after WIC decoding");
        if (c->task["artifacts"].empty()) return;
        std::string stored;
        util::readFile(util::joinPath(App::mediaDir(), util::widen(c->task["artifacts"].at(0)["file"].str())), stored);
        check(png == stored, "chat image is a persisted attachment, not just a project path");
        app.importArtifact(*c, J({{"path", ".cca-artifacts/evidence.png"}}));
        check(c->task["artifacts"].size() == 1, "identical screenshot is not duplicated");
        c->task["status"] = "running";
        c->task["cursor"] = 1;
        saveTask(c->task);
        std::string id = c->id;
        app.shutdown();
        pump();
        App restarted([](const std::string&) {});
        c = restarted.find(id);
        check(c && c->task["status"].str() == "interrupted", "restart detects the interrupted task");
        if (!c) return;
        restarted.taskResume(*c);
        check(until([&] { return !c->group.active || c->group.paused; }), "resumed task terminates");
        check(c->task["status"].str() == "done" && c->task["handoffs"].size() == 5,
              "restart resumes remaining reviewers without rerunning author");
        c->task["status"] = "interrupted";
        saveTask(c->task);
        util::writeFileAtomic(util::joinPath(project, L"external.txt"), "manual change\n");
        restarted.taskResume(*c);
        check(until([&] { return c->group.paused; }), "workspace conflict is detected before another agent starts");
        check(c->task["conflict"].boolean(), "conflict requires an explicit user decision");
        Conversation other;
        other.id = "lock-fixture";
        other.kind = "group";
        other.cwd = project;
        other.pipeline = c->pipeline;
        restarted.pipelineStart(other, "不可重疊寫入", false);
        check(!other.group.active && !other.workspaceLease,
              "workspace lock prevents a second task from overwriting the same project");
        restarted.groupStop(*c);
        c->pipeline["requireTests"] = true;
        util::writeFileAtomic(util::joinPath(project, L"result.txt"), "before-again\n");
        restarted.groupStart(*c, "沒有真正執行測試，不得標示完成");
        check(until([&] { return !c->group.active || c->group.paused; }), "missing-test scenario terminates");
        check(c->group.paused && c->task["gate"].str() == "BLOCKED",
              "required test evidence cannot be replaced by a PASS claim");
        check(c->task["blocked_reason"].str().find("測試") != std::string::npos,
              "gate failed for missing tests, not a provider error");
        restarted.groupStop(*c);
        c->pipeline["nodes"].items()[0]["model"] = "broken-model";
        restarted.groupStart(*c, "供應商中斷時保留工作區");
        check(until([&] { return c->group.paused; }), "provider outage pauses the task");
        std::string after;
        util::readFile(util::joinPath(project, L"result.txt"), after);
        check(after == content, "provider outage does not damage workspace");
        check(c->task["handoffs"].size() == 1 && c->task["handoffs"].at(0)["verdict"].str() == "BLOCKED",
              "provider error has a persisted structured handoff");
        check(c->task["handoffs"].at(0)["errors"].size() > 0, "provider errors remain available to the next agent");
        restarted.groupStop(*c);
        c->pipeline["requireTests"] = false;
        c->pipeline["nodes"].items()[0]["model"] = "slow-author";
        util::writeFileAtomic(util::joinPath(project, L"result.txt"), "before pause\n");
        restarted.groupStart(*c, "STEER_REQUIRED：暫停並接續同一位代理");
        check(until([&] { return !c->activeMsg[1].empty(); }), "slow agent starts before pause");
        restarted.onUiMessage(J({{"type", "task.pause"}, {"id", c->id}}).dump());
        check(until([&] { return c->group.paused; }), "active API agent can be paused");
        check(c->task["cursor"].i64() == 0 && c->workspaceLease, "pause preserves cursor and workspace ownership");
        std::string steeredId = c->task["task_id"].str();
        restarted.onUiMessage(
            J({{"type", "conv.send"}, {"id", c->id}, {"text", "STEER_TOKEN：保留既有修改，套用補充再繼續"}}).dump());
        check(
            c->task["task_id"].str() == steeredId && c->task["request"].str().find("STEER_TOKEN") == std::string::npos,
            "steering preserves task identity and immutable original request");
        check(until([&] { return !c->group.active || c->group.paused; }) && c->task["status"].str() == "done",
              "continue resumes the same agent and reaches remaining reviewers");
        long long handoffs = (long long)c->task["handoffs"].size();
        restarted.onUiMessage(J({{"type", "task.rereview"}, {"id", c->id}}).dump());
        check(until([&] { return !c->group.active || c->group.paused; }) && c->task["status"].str() == "done" &&
                  (long long)c->task["handoffs"].size() == handoffs + 2,
              "re-review runs reviewers without rerunning writer");
        c->pipeline["maxCycles"] = 3;
        for (auto& node : c->pipeline["nodes"].items()) node["model"] = "stuck-model";
        restarted.groupStart(*c, "重複問題不得無限循環");
        check(until([&] { return c->group.paused; }) && c->task["handoffs"].size() == 2 &&
                  c->task["blocked_reason"].str().find("重複問題") != std::string::npos,
              "identical findings without changes stop before another paid call");
        restarted.groupStop(*c);
        Json archive = newTask("history-fixture", std::string(24000, 'r'), util::nowMs());
        for (int n = 0; n < 55; ++n) {
            Json handoff = J({{"task_id", archive["task_id"]},
                              {"agent_id", "archive"},
                              {"summary", "decision"},
                              {"issues", Json(Json::Array{"unresolved finding"})}});
            addHandoff(archive, handoff);
            if (!saveTask(archive)) {
                check(false, "archive event persisted");
                break;
            }
        }
        check(archive["request"].str().size() == 24000, "original requirement is not truncated on disk");
        check(archive["handoffs"].size() == 40 && archive["earlier"].i64() == 15, "working memory stays bounded");
        check(taskHistory(archive["task_id"].str(), 0, 50).size() == 50 &&
                  taskHistory(archive["task_id"].str(), 50).size() == 5,
              "compacted handoffs remain retrievable by index");
        check(taskBrief(archive, 2000).size() <= 2000 &&
                  taskBrief(archive, 2000).find("unresolved finding") != std::string::npos,
              "incremental brief retains unresolved findings within budget");
        restarted.settings_["claudePath"] = util::narrow(util::joinPath(util::exeDir(), L"pipeline_test.exe"));
        restarted.settings_["codexPath"] = util::narrow(util::joinPath(util::exeDir(), L"pipeline_test.exe"));
        restarted.claudeStatus_.state = restarted.codexStatus_.state = "ok";
        Json cliNodes = Json::array();
        cliNodes.push(J({{"id", "cli-author"}, {"backend", "claude"}, {"role", "write"}}));
        cliNodes.push(J({{"id", "cli-reviewer"}, {"backend", "codex"}, {"role", "review"}}));
        cliNodes.push(J({{"id", "cli-recheck"}, {"backend", "claude"}, {"role", "verify"}}));
        c->pipeline = J({{"nodes", cliNodes}, {"maxCycles", 2}, {"requireTests", false}});
        restarted.groupStart(*c, "Claude 撰寫，GPT 修正，Claude 接續保留 GPT 的修改");
        check(until([&] { return !c->group.active || c->group.paused; }), "native CLI pipeline terminates");
        check(c->task["status"].str() == "done", "Claude and Codex protocol adapters complete review cycles");
        std::string cliResult;
        util::readFile(util::joinPath(project, L"cli-result.txt"), cliResult);
        check(cliResult == "GPT fixed\n", "Claude keeps GPT's actual correction instead of starting over");
        check(c->pipelineClaude.size() == 2 && c->pipelineCodex.size() == 1,
              "each CLI stage owns an independent session");
        std::string firstCli;
        for (const auto& message : c->messages)
            if (message["agent_id"].str() == "cli-author") {
                firstCli = message["id"].str();
                break;
            }
        restarted.handleRollback(J({{"convId", c->id}, {"msgId", firstCli}, {"files", true}, {"chat", false}}));
        check(!util::fileExists(util::joinPath(project, L"cli-result.txt")) && c->task["status"].str() == "interrupted",
              "file rollback invalidates the completed pipeline gate");
        check(c->pipelineClaude.empty() && c->pipelineCodex.empty() && !c->task["tests_passed"].boolean(),
              "rollback clears future sessions and test approval");
        restarted.handleRollback(J({{"convId", c->id}, {"msgId", firstCli}, {"files", false}, {"chat", true}}));
        check(c->task.isNull(), "chat rollback cannot retain removed task memory");
        restarted.settings_["geminiPath"] = util::narrow(util::joinPath(util::exeDir(), L"pipeline_test.exe"));
        restarted.geminiStatus_.state = "ok";
        restarted.settings_["personas"].push(J({{"id", "gemini-fixture"},
                                                {"agent", "gemini"},
                                                {"name", "Gemini reviewer"},
                                                {"prompt", "GEMINI_PERSONA_MARKER"}}));
        restarted.settings_["persona"]["gemini"] = "gemini-fixture";
        restarted.settings_["profiles"]["gemini"]["name"] = "Gemini reviewer";
        cliNodes.items().back() =
            J({{"id", "gemini-second-reviewer"}, {"name", "Gemini"}, {"backend", "gemini"}, {"role", "review"}});
        c->pipeline = J({{"nodes", cliNodes}, {"maxCycles", 3}, {"requireTests", false}, {"routing", "full"}});
        restarted.groupStart(*c, "Claude writes, Codex reviews, Gemini reviews and edits files");
        check(until([&] { return !c->group.active || c->group.paused; }), "Gemini pipeline terminates");
        check(c->task["status"].str() == "done", "Claude, GPT and Gemini reach consensus on real revisions");
        std::string geminiFile;
        util::readFile(util::joinPath(project, L"gemini-result.txt"), geminiFile);
        check(geminiFile == "Gemini reviewed and fixed\n", "second reviewer changes a real file");
        check(c->task["sessions"]["gemini-second-reviewer"].str() == "pipeline-gemini",
              "Gemini session persists separately in the task");
        check(restarted.usage_["gemini"]["session"]["inputTokens"].i64() >= 31, "Gemini usage is independent of Codex");
        bool geminiAuthor = false, geminiFlow = false;
        for (const auto& message : c->messages) geminiAuthor |= message["author"].str() == "gemini";
        Json flow = restarted.convState(*c)["workflow"];
        for (const auto& node : flow["nodes"].items())
            geminiFlow |= node["agent"].str() == "gemini" && node["id"].str() == "pipeline:gemini-second-reviewer";
        check(geminiAuthor && geminiFlow, "Gemini identity is visible in messages and workflow");
        std::string received;
        check(util::readFile(util::joinPath(util::appDataDir(), L"gemini-input.txt"), received) &&
                  received.find("GEMINI_PERSONA_MARKER") != std::string::npos,
              "selected Gemini persona reaches the real subprocess stdin");
        check(flow["layout"].str() == "pipeline" && flow["nodes"].size() == cliNodes.size() &&
                  flow["nodes"].at(2)["profile"].str() == "Gemini reviewer",
              "configured node order and Gemini display name reach workflow");
        SetEnvironmentVariableW(L"CCA_TEST_GEMINI_HOLD", L"1");
        restarted.groupStart(*c, "interrupt Gemini without changing the GPT message");
        check(until([&] { return c->gemini && c->gemini->busy() && !c->activeMsg[(int)AgentKind::Gemini].empty(); }),
              "Gemini has its own live message slot before interruption");
        const auto geminiMessage = c->activeMsg[(int)AgentKind::Gemini];
        c->activeMsg[(int)AgentKind::Codex] = "codex-sentinel";
        restarted.groupStop(*c);
        check(until([&] { return !c->group.active; }) && c->task["status"].str() == "stopped",
              "stopping Gemini terminates the pipeline and saves stopped status");
        const auto stopped = restarted.findMsg(*c, geminiMessage);
        check(stopped && (*stopped)["status"].str() == "interrupted" &&
                  c->activeMsg[(int)AgentKind::Codex] == "codex-sentinel",
              "interruption closes Gemini's message without touching Codex's slot");
        c->activeMsg[(int)AgentKind::Codex].clear();
        SetEnvironmentVariableW(L"CCA_TEST_GEMINI_HOLD", nullptr);
        {
            // A reviewer without quota is skipped and excused; the remaining review still has to pass.
            restarted.quotaUntil_["gemini"] = util::nowMs() + 3600 * 1000;
            restarted.groupStart(*c, "Gemini has no quota left; Claude writes and GPT reviews");
            check(until([&] { return !c->group.active || c->group.paused; }), "pipeline with an exhausted member ends");
            bool skipped = false;
            for (const auto& message : c->messages)
                skipped |= message["kind"].str() == "skip" && message["agent"].str() == "gemini";
            check(
                skipped && c->task["skipped"]["gemini-second-reviewer"].boolean() && c->task["status"].str() == "done",
                "an exhausted pipeline reviewer is shown as skipped and the others finish");
            // With every member out of quota the task stops at once and records the step it could not run.
            restarted.quotaUntil_["claude"] = restarted.quotaUntil_["codex"] = util::nowMs() + 3600 * 1000;
            restarted.groupStart(*c, "nobody has quota");
            check(!c->group.active && c->task["status"].str() == "interrupted" &&
                      c->task["quota_stop"]["next"].str() == "cli-author:write",
                  "an all-exhausted pipeline force-stops with a resumable record");
            restarted.quotaUntil_.clear();
        }
        Conversation bridge;
        bridge.kind = "group";
        bridge.group.pipeline = true;
        bridge.pipeline = Json::object();
        bridge.pipeline["nodes"] = Json::array();
        bridge.pipeline["nodes"].push(J({{"id", "api-view"}, {"backend", "api"}, {"role", "write"}}));
        bridge.task = J({{"pipeline", bridge.pipeline}, {"cursor", 0}});
        bridge.preparing = "codex";
        bridge.thinking[(int)AgentKind::Codex] = true;
        const auto bridged = restarted.convState(bridge);
        check(bridged["preparing"].str() == "api" && bridged["thinking"]["api"].boolean(),
              "runtime bridge state uses the configured backend identity");
        check(bridged["workflow"]["nodes"].at(0)["status"].str() == "preparing",
              "API pipeline stage exposes its checkpoint state in the shared workflow");
        c->task = newTask(c->id, "storage fault fixture", util::nowMs());
        c->task["pipeline"] = c->pipeline;
        c->group = GroupRun();
        c->group.active = c->group.pipeline = true;
        saveTask(c->task);
        std::wstring taskFile =
            util::joinPath(util::joinPath(util::appDataDir(), L"tasks"), util::widen(c->id + ".json"));
        DeleteFileW(taskFile.c_str());
        CreateDirectoryW(taskFile.c_str(), nullptr);
        restarted.groupFinish(*c, "consensus", "must not complete");
        check(c->group.paused && c->task["status"].str() != "done", "failed task persistence cannot report completion");
        RemoveDirectoryW(taskFile.c_str());
        saveTask(c->task);
        restarted.groupStop(*c);
        restarted.shutdown();
        pump();
    }
};

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--output-format") return cliGemini();
    if (argc > 1 && std::string(argv[1]) == "app-server") return cliCodex();
    if (argc > 1 && std::string(argv[1]) == "-p") return cliClaude();
    std::wstring dir = util::joinPath(util::getEnv(L"TEMP"), util::widen("cca-pipeline-" + util::newId()));
    std::wstring project = util::joinPath(dir, L"project");
    util::ensureDir(dir);
    util::ensureDir(project);
    SetEnvironmentVariableW(L"CCA_DATA_DIR", util::joinPath(dir, L"data").c_str());
    check(util::localAppDataDir() == util::joinPath(util::appDataDir(), L"local"),
          "test cache and snapshots stay inside the isolated data directory");
    util::writeFileAtomic(util::joinPath(project, L"result.txt"), "before\n");
    ChildProcess server;
    ProcessOptions options;
    options.exe = util::findOnPath(L"python");
    options.args = {L"-I", L"-u", util::joinPath(util::exeDir(), L"..\\tests\\provider_fixture.py")};
    int port = 0;
    std::mutex portMutex;
    std::string error;
    bool started = server.start(
        options,
        [&](const std::string& line) {
            std::lock_guard<std::mutex> lock(portMutex);
            port = (int)Json::parse(line)["port"].i64();
        },
        [](const std::string&) {}, [](unsigned long) {}, &error);
    check(started && until([&] {
              std::lock_guard<std::mutex> lock(portMutex);
              return port > 0;
          }),
          "HTTP fixture started");
    if (port) PipelineTestAccess::run(project, port);
    server.terminate();
    std::printf("%d pipeline checks, %d failures\n", checks, failed);
    return failed ? 1 : 0;
}
