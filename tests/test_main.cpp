// Unit tests for the parts of CCA that do not need a window or a live CLI.
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

#include <windows.h>
#include "activity.hpp"
#include "agent.hpp"
#include "app.hpp"
#include "claude_agent.hpp"
#include "claude_usage.hpp"
#include "cli_render.hpp"
#include "codex_agent.hpp"
#include "diff.hpp"
#include "gitflow.hpp"
#include "guard.hpp"
#include "hardware.hpp"
#include "humanize.hpp"
#include "json.hpp"
#include "local_tools.hpp"
#include "personas.hpp"
#include "process.hpp"
#include "review.hpp"
#include "sandbox_mcp.hpp"
#include "snapshot.hpp"
#include "task_ledger.hpp"
#include "util.hpp"
#include "workflow.hpp"

#include <winsock2.h>
#include <thread>
#pragma comment(lib, "ws2_32.lib")

void postToUi(std::function<void()> fn) { fn(); }

static int g_failed = 0, g_passed = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (cond) {                                                     \
            ++g_passed;                                                 \
        } else {                                                        \
            ++g_failed;                                                 \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                               \
    } while (0)

#define CHECK_EQ(a, b)                                                                      \
    do {                                                                                    \
        auto va = (a);                                                                      \
        auto vb = (b);                                                                      \
        if (va == vb) {                                                                     \
            ++g_passed;                                                                     \
        } else {                                                                            \
            ++g_failed;                                                                     \
            std::printf("FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a, #b);              \
        }                                                                                   \
    } while (0)

static void testJson() {
    std::string err;
    Json j = Json::parse(R"({"a":1,"b":[true,false,null],"c":"x\"y\\z\n","d":-2.5e3,"e":{"f":"中文"}})", &err);
    CHECK(err.empty());
    CHECK_EQ(j["a"].i64(), 1);
    CHECK_EQ(j["b"].size(), (size_t)3);
    CHECK(j["b"].at(0).boolean());
    CHECK(j["b"].at(2).isNull());
    CHECK_EQ(j["c"].str(), std::string("x\"y\\z\n"));
    CHECK_EQ(j["d"].num(), -2500.0);
    CHECK_EQ(j["e"]["f"].str(), std::string("中文"));
    CHECK(j["missing"]["deep"].isNull());

    // Round trip keeps values and escapes control characters.
    Json back = Json::parse(j.dump());
    CHECK(back == j);
    CHECK(Json("a\x01").dump() == "\"a\\u0001\"");

    // \u escapes including a surrogate pair (U+1F600).
    Json u = Json::parse(R"("\u4e2d\ud83d\ude00")");
    CHECK_EQ(u.str(), std::string("中\xF0\x9F\x98\x80"));

    // Big integers survive (Codex request ids, timestamps in ms).
    Json big = Json::parse("1792726804679");
    CHECK_EQ(big.i64(), 1792726804679LL);
    CHECK_EQ(big.dump(), std::string("1792726804679"));

    // Malformed input is rejected with a message.
    CHECK(Json::parse("{\"a\":}", &err).isNull());
    CHECK(!err.empty());
    CHECK(Json::parse("[1,2", &err).isNull());
    CHECK(Json::parse("{} x", &err).isNull());

    Json o = J({{"k", "v"}, {"n", 3}});
    o["k"] = "w";
    CHECK_EQ(o.dump(), std::string(R"({"k":"w","n":3})"));
    o.erase("k");
    CHECK(!o.has("k"));
}

static void testDiff() {
    DiffStat s = countUnifiedDiff("--- a/x\n+++ b/x\n@@ -1,2 +1,3 @@\n a\n-b\n+c\n+d\n");
    CHECK_EQ(s.added, 2);
    CHECK_EQ(s.removed, 1);

    Json hunks = Json::parse(R"([{"oldStart":1,"oldLines":2,"newStart":1,"newLines":2,"lines":[" a","-b","+B"]}])");
    DiffStat hs;
    std::string d = hunksToDiff(hunks, &hs);
    CHECK_EQ(hs.added, 1);
    CHECK_EQ(hs.removed, 1);
    CHECK(d.find("@@ -1,2 +1,2 @@") == 0);

    DiffStat ns;
    newFileDiff("one\ntwo\nthree\n", &ns);
    CHECK_EQ(ns.added, 3);

    DiffStat ls;
    std::string ld = lineDiff("a\nb\nc", "a\nB\nc\nd", &ls);
    CHECK_EQ(ls.added, 2);
    CHECK_EQ(ls.removed, 1);
    CHECK(ld.find(" a\n") != std::string::npos);

    DiffStat cs;
    normalizeCodexDiff("add", "x\ny\n", &cs);
    CHECK_EQ(cs.added, 2);
    normalizeCodexDiff("update", "@@ -1 +1 @@\n-x\n+y\n", &cs);
    CHECK_EQ(cs.added, 1);
    CHECK_EQ(cs.removed, 1);
    normalizeCodexDiff("delete", "x\ny\nz\n", &cs);
    CHECK_EQ(cs.removed, 3);
}

static void testVerdict() {
    std::string rest;
    CHECK(parseVerdict("看起來沒問題。\n\nVERDICT: APPROVE", &rest) == Verdict::Approve);
    CHECK_EQ(rest, std::string("看起來沒問題。"));
    CHECK(parseVerdict("改了兩處\n**VERDICT: CHANGED**\n", &rest) == Verdict::Changed);
    CHECK(parseVerdict("有疑慮\n`VERDICT：ISSUES`", &rest) == Verdict::Issues);
    CHECK(parseVerdict("verdict: approve", &rest) == Verdict::Approve);
    // Only the trailing line counts; a verdict quoted mid-text is ignored.
    CHECK(parseVerdict("規則是 VERDICT: APPROVE\n但我改了檔案", &rest) == Verdict::None);
    CHECK_EQ(rest, std::string("規則是 VERDICT: APPROVE\n但我改了檔案"));
    CHECK(parseVerdict("", &rest) == Verdict::None);
}

static void testCommandLine() {
    CHECK(util::quoteArg(L"plain") == L"plain");
    CHECK(util::quoteArg(L"has space") == L"\"has space\"");
    CHECK(util::quoteArg(L"a\"b") == L"\"a\\\"b\"");
    CHECK(util::quoteArg(L"C:\\dir with space\\") == L"\"C:\\dir with space\\\\\"");
    CHECK(util::quoteArg(L"") == L"\"\"");

    ProcessOptions exe;
    exe.exe = L"C:\\tools\\claude.exe";
    exe.args = {L"-p", L"--model", L"opus"};
    CHECK(buildCommandLine(exe) == L"C:\\tools\\claude.exe -p --model opus");

    ProcessOptions cmd;
    cmd.exe = L"C:\\npm\\codex.cmd";
    cmd.args = {L"app-server"};
    CHECK(buildCommandLine(cmd) == L"cmd.exe /d /s /c \"C:\\npm\\codex.cmd app-server\"");
}

static void testUtil() {
    // A JWT payload with the ChatGPT plan claim, as Codex stores it.
    std::string payload = R"({"https://api.openai.com/auth":{"chatgpt_plan_type":"plus"}})";
    std::string b64;
    {
        static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        int val = 0, bits = -6;
        for (unsigned char c : payload) {
            val = (val << 8) + c;
            bits += 8;
            while (bits >= 0) {
                b64 += tbl[(val >> bits) & 0x3F];
                bits -= 6;
            }
        }
        if (bits > -6) b64 += tbl[((val << 8) >> (bits + 8)) & 0x3F];
    }
    std::string decoded = util::jwtPayload("header." + b64 + ".sig");
    CHECK_EQ(decoded, payload);
    CHECK_EQ(Json::parse(decoded)["https://api.openai.com/auth"]["chatgpt_plan_type"].str(), std::string("plus"));

    CHECK_EQ(util::truncateUtf8("中文字", 4), std::string("中"));
    CHECK_EQ(util::tailUtf8("中文字", 4), std::string("字"));
    CHECK_EQ(util::splitLines("a\r\nb\n\nc").size(), (size_t)4);
    CHECK_EQ(util::displayPath("C:/proj/src/a.cpp", L"C:\\proj"), std::string("src\\a.cpp"));
    CHECK_EQ(util::displayPath("D:\\other\\a.cpp", L"C:\\proj"), std::string("D:\\other\\a.cpp"));
}

static void testProjectFolders() {
    std::wstring home = util::homeDir();
    CHECK(!util::projectFolderProblem(L"C:\\").empty());
    CHECK(!util::projectFolderProblem(home).empty());
    CHECK(!util::projectFolderProblem(home + L"\\").empty());
    CHECK(!util::projectFolderProblem(L"C:\\Users").empty());
    CHECK(!util::projectFolderProblem(L"C:\\Windows\\System32").empty());
    CHECK(!util::projectFolderProblem(L"C:\\Program Files\\Something").empty());
    CHECK(!util::projectFolderProblem(L"").empty());
    CHECK(util::projectFolderProblem(home + L"\\projects\\shop-api").empty());
    CHECK(util::projectFolderProblem(util::exeDir()).empty());
}

static void testPersonas() {
    std::string error;
    Json input = Json::array(), list;
    input.push(J({{"id", "reviewer"},
                  {"name", "審查員"},
                  {"agent", "local"},
                  {"prompt", "說明保持簡潔。"},
                  {"address", "使用者"}}));
    CHECK(parsePersonaPack(input, list, error));
    CHECK_EQ(list.size(), (size_t)1);
    CHECK_EQ(findPersona(Json::array(), "reviewer", list)["name"].str(), std::string("審查員"));
    CHECK(findPersona(Json::array(), "reviewer").isNull());
    CHECK(findPersona(Json::array(), "none").isNull());
    CHECK(findPersona(Json::array(), "missing").isNull());
    std::string text = personaInstructions(list.at(0));
    CHECK(text.find("# 角色設定：審查員") == 0);
    CHECK(text.find("稱呼使用者為「使用者」") != std::string::npos);
    // The role never outranks CCA's protocol or code quality.
    CHECK(text.find("VERDICT") != std::string::npos);
    CHECK(text.find("優先於角色設定") != std::string::npos);

    Json custom = Json::array();
    custom.push(J({{"id", "p-1"}, {"name", "老師"}, {"prompt", "你是耐心的老師。"}}));
    CHECK_EQ(findPersona(custom, "p-1")["name"].str(), std::string("老師"));
    CHECK(personaInstructions(findPersona(custom, "p-1")).find("耐心的老師") != std::string::npos);
    CHECK(personaInstructions(Json()).empty());
    CHECK(personaInstructions(J({{"name", "空"}, {"prompt", "  "}})).empty());
    custom.push(J({{"id", "reviewer"}, {"name", "自訂版本"}, {"prompt", "自訂"}}));
    CHECK_EQ(findPersona(custom, "reviewer", list)["name"].str(), std::string("自訂版本"));
    input.push(input.at(0));
    CHECK(!parsePersonaPack(input, list, error) && list.size() == 0 && !error.empty());
    input.items().pop_back();
    input.at(0)["id"] = "none";
    CHECK(!parsePersonaPack(input, list, error));
    input.at(0)["id"] = "valid";
    input.at(0)["prompt"] = std::string(12001, 'x');
    CHECK(!parsePersonaPack(input, list, error));
    CHECK(!parsePersonaPack(Json::object(), list, error));
}

static void testActivity() {
    Json history = Json::parse(R"({
        "2024-02-27":{"claude":{"input":100}},
        "2024-02-28":{"claude":{"input":2,"output":1,"cached":999}},
        "2024-02-29":{"claude":{"input":10},"api":{"output":5}},
        "2024-03-01":{"local":{"input":-5,"output":2}},
        "2024-02-30":{"claude":{"input":900}},
        "2050-01-01":{"claude":{"input":900}}
    })");
    const std::string before = history.dump();
    Json model = tokenActivity(history, "2024-03-02", 4);
    CHECK_EQ(model["days"].size(), (size_t)4);
    CHECK_EQ(model["days"].at(0)["date"].str(), std::string("2024-02-28"));
    CHECK_EQ(model["days"].at(1)["daily"].i64(), 15LL);
    CHECK_EQ(model["days"].at(0)["cumulative"].i64(), 103LL);
    CHECK_EQ(model["days"].at(3)["weekly"].i64(), 120LL);
    CHECK(!model["days"].at(3)["recorded"].boolean());
    CHECK_EQ(model["windowTotal"].i64(), 20LL);
    CHECK_EQ(model["total"].i64(), 120LL);
    CHECK_EQ(model["activeDays"].i64(), 3LL);
    CHECK_EQ(model["first"].str(), std::string("2024-02-27"));
    CHECK_EQ(history.dump(), before);
    CHECK_EQ(tokenActivity(Json::object(), "2026-01-01")["days"].size(), (size_t)365);
    CHECK_EQ(tokenActivity(Json::object(), "2026-01-01")["total"].i64(), 0LL);
    CHECK_EQ(tokenActivity(history, "2024-02-30").size(), (size_t)0);
    CHECK_EQ(tokenActivity(history, "2024-03-05", 1)["days"].at(0)["weekly"].i64(), 20LL);
}

static void testHumanize() {
    std::string base = humanizeInstructions("");
    CHECK(base.find("# 去 AI 化") == 0);
    CHECK(base.find("Inter") != std::string::npos);
    CHECK(base.find("值得注意的是") != std::string::npos);
    CHECK(base.find("使用者另外的要求") == std::string::npos);
    CHECK_EQ(humanizeInstructions("  \n "), base);
    std::string mine = humanizeInstructions("  主色用墨綠色\n");
    CHECK(mine.find(base) == 0);
    CHECK(mine.find("## 使用者另外的要求\n主色用墨綠色") != std::string::npos);
    CHECK(humanizeInstructions(std::string(20000, 'x')).size() < base.size() + 8100);
    CHECK(humanizeReviewNote().find("去 AI 化") != std::string::npos);
    CHECK_EQ(humanizeReviewNote(" \n "), humanizeReviewNote());
    CHECK(humanizeReviewNote("主色用墨綠色").find("## 使用者另外的要求\n主色用墨綠色") != std::string::npos);
    CHECK(humanizeReviewNote().find("使用者明確指定的風格可以例外") != std::string::npos);
    std::string cjk;
    for (int i = 0; i < 2000; ++i) cjk += "字";
    CHECK(humanizeInstructions(cjk).find(cjk) != std::string::npos);
    CHECK(humanizeReviewNote(cjk).find(cjk) != std::string::npos);
}

// Exercise the app-server handoff without launching a CLI or using quota.
struct CodexThreadTestAccess {
    static void testHumanizeReconfigure() {
        auto server = std::make_shared<CodexServer>(L"");
        int completed = 0;
        auto thread = std::make_shared<CodexThread>(server, [&](const AgentEvent& event) {
            if (event.kind == AgentEvent::Kind::TurnDone) ++completed;
        });
        AgentConfig cfg;
        cfg.resumeId = "test-humanize";
        cfg.instructions = "existing role";
        thread->configure(cfg);
        auto bind = [&] {
            bool bound = false;
            thread->ensureThread([&](bool ok, const std::string&) { bound = ok; });
            CHECK_EQ(server->queued_.size(), (size_t)1);
            if (server->queued_.empty()) return;
            Json request = server->queued_.front();
            server->queued_.pop_front();
            CHECK_EQ(request["method"].str(), std::string("thread/resume"));
            CHECK(request["params"]["developerInstructions"].isString());
            CHECK_EQ(request["params"]["developerInstructions"].str(), cfg.instructions);
            server->handleLine(J({{"id", request["id"]},
                                 {"result", J({{"thread", J({{"id", cfg.resumeId}})}})}}).dump());
            CHECK(bound);
        };
        bind();
        // Enable, edit custom rules, then disable while a response is streaming.
        for (const auto& instructions : {humanizeInstructions(""), humanizeInstructions("主色用墨綠色"), std::string("existing role"), std::string()}) {
            thread->busy_ = true;
            cfg.instructions = instructions;
            thread->configure(cfg);
            int before = completed;
            server->handleLine(J({{"method", "turn/completed"},
                                 {"params", J({{"threadId", cfg.resumeId}, {"turn", J({{"id", "turn"}, {"status", "completed"}})}})}}).dump());
            CHECK_EQ(completed, before + 1);  // The active turn's handler survives.
            CHECK(!thread->busy());
            thread->configure(cfg);  // App configures again before the next send.
            bind();
        }
    }

    // The shared sandbox rides on thread/start and thread/resume, and only when it is switched on.
    static void testSandboxConfig() {
        for (bool on : {true, false}) {
            auto server = std::make_shared<CodexServer>(L"");
            auto thread = std::make_shared<CodexThread>(server, [](const AgentEvent&) {});
            AgentConfig cfg;
            cfg.cwd = L"C:\\Users\\Test\\專案";
            if (on)
                cfg.sandbox = SandboxMcp{L"C:\\Python\\python.exe", L"C:\\cca\\sandbox_mcp.py", L"C:\\cca\\sandbox",
                                         L"C:\\Docker\\docker.exe"};
            thread->configure(cfg);
            thread->ensureThread([](bool, const std::string&) {});
            CHECK_EQ(server->queued_.size(), (size_t)1);
            if (server->queued_.empty()) continue;
            Json params = server->queued_.front()["params"];
            CHECK_EQ(server->queued_.front()["method"].str(), std::string("thread/start"));
            CHECK_EQ(params.has("config"), on);
            if (!on) continue;
            const Json& entry = params["config"]["mcp_servers"][kSandboxServer];
            CHECK_EQ(entry["command"].str(), std::string("C:\\Python\\python.exe"));
            CHECK_EQ(entry["default_tools_approval_mode"].str(), std::string("approve"));
            CHECK_EQ(entry["tool_timeout_sec"].i64(), 150LL);
        }
    }
};

static void testSandboxMcp() {
    AgentConfig cfg;
    cfg.cwd = L"C:\\Users\\Test\\專案 資料夾";
    cfg.sandbox = SandboxMcp{L"C:\\Python\\python.exe", L"C:\\cca\\sandbox_mcp.py", L"C:\\cca\\sandbox",
                             L"C:\\Docker\\docker.exe"};
    CHECK(cfg.sandbox.on());
    CHECK(!AgentConfig().sandbox.on());

    // Both agents are told the same launch line, and paths with backslashes and CJK survive the JSON round trip.
    Json codex = Json::parse(codexSandboxConfig(cfg).dump());
    Json claude = Json::parse(claudeSandboxConfig(cfg).dump());
    const Json& a = codex["mcp_servers"][kSandboxServer]["args"];
    const Json& b = claude["mcpServers"][kSandboxServer]["args"];
    CHECK_EQ(a.size(), (size_t)9);
    CHECK(a == b);
    CHECK_EQ(a.at(0).str(),
             std::string("-I"));  // isolated mode: no PYTHONPATH or script-folder imports from the project
    CHECK_EQ(a.at(2).str(), util::narrow(cfg.sandbox.script));
    CHECK_EQ(a.at(4).str(), util::narrow(cfg.cwd));
    CHECK_EQ(a.at(8).str(), util::narrow(cfg.sandbox.docker));
    CHECK_EQ(claude["mcpServers"][kSandboxServer]["type"].str(), std::string("stdio"));
    CHECK_EQ(claude["mcpServers"][kSandboxServer]["command"].str(), std::string("C:\\Python\\python.exe"));

    std::string note = sandboxInstructions();
    CHECK(note.find("sandbox_run") != std::string::npos);
    CHECK(note.find("mcp__cca-sandbox__sandbox_run") != std::string::npos);

    Json completed = J({{"isolation", "docker"}, {"exit_code", 0}, {"output", "test passed"}});
    Json blocks = Json::array();
    blocks.push(J({{"type", "text"}, {"text", completed.dump()}}));
    CHECK_EQ(sandboxCompletion(blocks)["exitCode"].i64(-1), 0LL);
    CHECK_EQ(sandboxCompletion(completed.dump())["status"].str(), std::string("ok"));
    completed["exit_code"] = 7;
    CHECK_EQ(sandboxCompletion(completed)["status"].str(), std::string("error"));
    completed.erase("exit_code");
    CHECK(sandboxCompletion(completed).isNull());
    CHECK(sandboxCompletion("the model says tests passed").isNull());

    // Whatever this machine has installed, a positive answer must point at real files.
    SandboxMcp found = findSandboxMcp();
    if (found.on()) {
        CHECK(util::fileExists(found.script));
        CHECK(util::fileExists(found.python));
        CHECK(util::fileExists(found.docker));
    }
}

static std::string plain(const tui::Line& l) {
    std::string out;
    for (const auto& sp : l) out += sp.text;
    return out;
}

static void testDataLock() {
    std::wstring before = util::getEnv(L"CCA_DATA_DIR");
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = util::joinPath(tmp, util::widen("cca-lock-" + util::newId()));
    CHECK(util::ensureDir(dir));
    SetEnvironmentVariableW(L"CCA_DATA_DIR", dir.c_str());
    HANDLE first = (HANDLE)util::lockDataDir();
    CHECK(first != nullptr);
    SetEnvironmentVariableW(L"CCA_DATA_DIR", (dir + L"\\.").c_str());
    HANDLE second = (HANDLE)util::lockDataDir();
    CHECK(second == nullptr);
    if (second) CloseHandle(second);
    if (first) CloseHandle(first);
    HANDLE again = (HANDLE)util::lockDataDir();
    CHECK(again != nullptr);
    if (again) CloseHandle(again);
    SetEnvironmentVariableW(L"CCA_DATA_DIR", before.empty() ? nullptr : before.c_str());
    RemoveDirectoryW(dir.c_str());  // this test created one empty directory
}

static void testCliText() {
    using namespace tui;
    CHECK_EQ(charWidth(U'a'), 1);
    CHECK_EQ(charWidth(U'中'), 2);
    CHECK_EQ(charWidth(U'，'), 2);
    CHECK_EQ(charWidth(0x301), 0);      // a combining accent
    CHECK_EQ(charWidth(0x1F600), 2);    // an emoji
    CHECK_EQ(charWidth(U'─'), 1);       // box drawing stays narrow, like Windows Terminal
    CHECK_EQ(charWidth(U'⎿'), 1);
    CHECK_EQ(width(std::string("中文ab")), 6);
    CHECK_EQ(encode(decode("角色甲 ✓ 😀")), std::string("角色甲 ✓ 😀"));

    auto lines = wrap(line("hello world foo"), 11);
    CHECK_EQ(lines.size(), (size_t)2);
    CHECK_EQ(plain(lines[0]), std::string("hello world"));
    CHECK_EQ(plain(lines[1]), std::string("foo"));
    lines = wrap(line("一二三四五六"), 6);
    CHECK_EQ(lines.size(), (size_t)2);
    CHECK_EQ(plain(lines[0]), std::string("一二三"));
    lines = wrap(line("abcdefghij"), 4);  // one word wider than the line is cut
    CHECK_EQ(lines.size(), (size_t)3);
    CHECK_EQ(plain(lines[2]), std::string("ij"));
    lines = wrap(line("a\n\nb"), 10, line("> "), line("  "));
    CHECK_EQ(lines.size(), (size_t)3);
    CHECK_EQ(plain(lines[0]), std::string("> a"));
    CHECK_EQ(plain(lines[1]), std::string("  "));
    CHECK_EQ(wrap(line(""), 10).size(), (size_t)1);
    for (const auto& l : wrap(line("中文和 English 混在一起的一段很長的句子，換行時不能超過寬度"), 13, line("  "), line("  "))) {
        CHECK(width(l) <= 13);
    }
    CHECK_EQ(plain(clip(line("abcdef"), 4)), std::string("abc…"));
    CHECK_EQ(plain(clip(line("abc"), 4)), std::string("abc"));
    CHECK_EQ(width(padTo(line("中"), 5)), 5);
    std::string escaped = ansi(line("text\x1b[2J\x1b]0;title\a\r\n\tend"));
    CHECK(escaped.find("\x1b[2J") == std::string::npos);
    CHECK(escaped.find("\x1b]0;") == std::string::npos);
    CHECK(escaped.find('\a') == std::string::npos);
    CHECK(escaped.find('\r') == std::string::npos && escaped.find('\n') == std::string::npos);
    CHECK(escaped.find("    end") != std::string::npos);

    auto md = markdown("# 標題\n- 項目一\n```\ncode line\n```\n普通 **粗體** 和 `碼`", 40, line("  "));
    CHECK_EQ(md.size(), (size_t)4);
    CHECK_EQ(plain(md[0].line), std::string("  標題"));
    CHECK_EQ(plain(md[1].line), std::string("  • 項目一"));
    CHECK_EQ(plain(md[2].line), std::string("  │ code line"));
    CHECK_EQ(md[2].src, 3);
    CHECK_EQ(plain(md[3].line), std::string("  普通 粗體 和 碼"));
    CHECK(ansi(md[3].line).find("\x1b[0;1m") != std::string::npos);  // the bold part

    Names names{[](const std::string& w) { return w == "claude" ? std::string("角色甲") : w == "codex" ? std::string("角色乙") : w; }};
    CHECK_EQ(withAgentName("Claude 想要執行指令", names), std::string("角色甲 想要執行指令"));
    CHECK_EQ(handoffText(J({{"from", "claude"}, {"to", "codex"}, {"reason", "first"}}), names), std::string("角色甲 把第一版交給 角色乙 審查"));
    CHECK_EQ(withAgentName("Gemini 想要執行指令", names), std::string("gemini 想要執行指令"));
    CHECK_EQ(tokens(12345), std::string("12k"));
    CHECK_EQ(tokens(1234), std::string("1.2k"));
    CHECK_EQ(logo().size(), (size_t)8);
}

static void testCliMessages() {
    using namespace tui;
    Names names{[](const std::string& w) { return w; }};
    // A reply still streaming: finished lines are stable, the line being written is not.
    Json text = J({{"type", "text"}, {"id", "t"}, {"text", "第一行\n第二行還在寫"}});
    Json blocks = Json::array();
    blocks.push(text);
    Json msg = J({{"author", "claude"}, {"streaming", true}, {"blocks", blocks}});
    Rendered r = renderMessage(msg, names, 60);
    CHECK_EQ(r.lines.size(), (size_t)4);  // blank, header, two lines
    CHECK_EQ(r.stable, (size_t)3);
    CHECK(!r.complete);

    // A running command keeps itself and what follows unstable.
    Json running = blocks;
    running.push(J({{"type", "tool"}, {"id", "x"}, {"title", "執行指令"}, {"command", "npm test"}, {"status", "running"}}));
    msg["blocks"] = running;
    r = renderMessage(msg, names, 60);
    CHECK_EQ(r.stable, (size_t)4);  // the text is done once a tool follows it
    CHECK(plain(r.lines.back()).find("npm test") != std::string::npos);

    // A pending approval is drawn as a prompt under the messages, not in them.
    Json asking = blocks;
    asking.push(J({{"type", "approval"}, {"id", "a"}, {"title", "Claude 想要執行指令"}, {"state", "pending"}}));
    msg["blocks"] = asking;
    r = renderMessage(msg, names, 60);
    CHECK_EQ(r.lines.size(), (size_t)4);
    CHECK_EQ(r.stable, (size_t)4);

    // Finished: everything is final, failures show the tail of their output.
    Json done = blocks;
    done.push(J({{"type", "tool"}, {"id", "y"}, {"title", "執行指令"}, {"command", "make"}, {"status", "error"}, {"exitCode", 2},
                 {"output", "line a\nline b\nline c\nerror: boom\n"}}));
    msg["blocks"] = done;
    msg["streaming"] = false;
    msg["verdict"] = "approve";
    msg["usage"] = J({{"inputTokens", 1500}, {"outputTokens", 300}, {"durationMs", 4200}});
    r = renderMessage(msg, names, 60);
    CHECK(r.complete);
    CHECK_EQ(r.stable, r.lines.size());
    bool sawTail = false, sawVerdict = false, sawUsage = false;
    for (const auto& l : r.lines) {
        std::string p = plain(l);
        sawTail |= p.find("error: boom") != std::string::npos;
        sawVerdict |= p.find("結論：同意") != std::string::npos;
        sawUsage |= p.find("輸入 1.5k") != std::string::npos;
        CHECK(width(l) <= 60);
    }
    CHECK(sawTail);
    CHECK(sawVerdict);
    CHECK(sawUsage);

    Json user = J({{"author", "user"}, {"text", "幫我加一個功能"}});
    r = renderMessage(user, names, 60);
    CHECK(r.complete);
    CHECK_EQ(plain(r.lines[1]), std::string("› 幫我加一個功能"));
}

static void testWorkflow() {
    WorkflowRun run;
    Json context = J({{"kind", "group"}, {"lead", "claude"}, {"members", Json(Json::Array{"local1", "local2"})},
                      {"models", J({{"local1", "fixture"}, {"local2", "fixture"}})}});
    Json state = J({{"running", true}, {"typing", Json(Json::Array{"claude"})}, {"thinking", J({{"claude", true}})},
                    {"group", J({{"active", true}, {"current", "claude"}, {"files", Json::object()}})}});
    std::vector<Json> messages = {J({{"id", "c1"}, {"author", "claude"}, {"streaming", true}, {"phase", "write"}, {"blocks", Json::array()}})};
    workflowTurn(run, "claude", "c1", "write", 10000);
    CHECK(run.transfers.empty());  // Thinking alone is never a handoff.
    auto node = [](const Json& view, const std::string& who) {
        for (const auto& n : view["nodes"].items()) if (n["id"].str() == who) return n;
        return Json();
    };
    Json view = workflowView(context, state, run, messages);
    CHECK_EQ(node(view, "claude")["status"].str(), std::string("thinking"));
    CHECK_EQ(view["nodes"].size(), (size_t)4);
    CHECK(tui::workflowAnimating(view, 10100));
    messages[0]["blocks"].push(J({{"type", "tool"}, {"title", "執行測試"}, {"status", "running"}}));
    view = workflowView(context, state, run, messages);
    CHECK_EQ(node(view, "claude")["status"].str(), std::string("tool"));
    messages[0]["blocks"].push(J({{"type", "approval"}, {"state", "pending"}}));
    view = workflowView(context, state, run, messages);
    CHECK_EQ(view["status"].str(), std::string("waitingUser"));
    CHECK(!tui::workflowAnimating(view, 10100));
    messages[0]["blocks"].items().back()["state"] = "allow";
    state["group"]["paused"] = true;
    view = workflowView(context, state, run, messages);
    CHECK_EQ(node(view, "claude")["status"].str(), std::string("paused"));
    CHECK(!tui::workflowAnimating(view, 10100));
    state["group"]["paused"] = false;
    workflowTurn(run, "local1", "l1", "advise", 11000);
    workflowLocalReply(run, "local1");
    workflowTurn(run, "local2", "l2", "advise", 12000);
    workflowLocalReply(run, "local2");
    CHECK_EQ(run.transfers[0]["from"].str(), std::string("claude"));
    CHECK_EQ(run.transfers[1]["from"].str(), std::string("claude"));
    CHECK_EQ(run.transfers[1]["to"].str(), std::string("local2"));
    workflowTurn(run, "codex", "g1", "review", 13000);
    CHECK_EQ(run.transfers.size(), (size_t)5);
    CHECK_EQ(run.transfers[3]["from"].str(), std::string("local1"));
    CHECK_EQ(run.transfers[4]["from"].str(), std::string("local2"));
    CHECK_EQ(run.transfers[4]["to"].str(), std::string("codex"));
    CHECK(run.localReplies.empty());
    messages[0]["streaming"] = false;
    messages[0]["status"] = "completed";
    for (const char* id : {"l1", "l2"}) messages.push_back(J({{"id", id}, {"streaming", false}, {"status", "completed"}}));
    messages.push_back(J({{"id", "g1"}, {"author", "codex"}, {"streaming", true}, {"phase", "review"}}));
    state["typing"] = Json(Json::Array{"codex"});
    state["thinking"] = Json::object();
    view = workflowView(context, state, run, messages);
    tui::Names names{[](const std::string& who) {
        return who == "claude"   ? "角色甲"
               : who == "codex"  ? "角色乙"
               : who == "gemini" ? "Third reviewer"
               : who == "local1" ? "本地甲"
                                 : "本地乙";
    }};
    auto dump = [&](long long time, bool motion) {
        std::string out;
        for (const auto& l : tui::renderWorkflow(view, names, 100, time, false, motion)) out += tui::ansi(l);
        return out;
    };
    CHECK(dump(13100, true) != dump(13600, true));
    CHECK_EQ(dump(13100, false), dump(13600, false));
    // Optional artifacts from the exact renderer under test, with simulated events.
    if (std::wstring out = util::getEnv(L"CCA_TEST_WORKFLOW_OUT"); !out.empty()) {
        util::ensureDir(out);
        std::string recording, snapshots;
        for (int i = 0; i < 2; ++i) {
            recording += "\x1b[H";
            for (const auto& line : tui::renderWorkflow(view, names, 100, i ? 13600 : 13100, false, true)) recording += tui::ansi(line) + "\r\n";
            snapshots += std::string(i ? "next=" : "handoff=") + std::to_string(recording.size()) + "\n";
        }
        CHECK(util::writeFileAtomic(util::joinPath(out, L"four.vt"), recording));
        CHECK(util::writeFileAtomic(util::joinPath(out, L"four.vt.snaps"), snapshots));
        CHECK(util::writeFileAtomic(util::joinPath(out, L"four.json"), view.dump()));
    }
    for (int cols : {24, 40, 68, 80, 100, 120}) for (bool compact : {true, false}) {
        auto lines = tui::renderWorkflow(view, names, cols, 13500, compact, true);
        CHECK(!lines.empty());
        for (const auto& l : lines) CHECK(tui::width(l) <= cols);
    }
    for (int cols : {24, 40}) {
        auto lines = tui::renderWorkflow(view, names, cols, 13500, true, false);
        for (const auto& member : view["nodes"].items()) {
            bool visible = false;
            for (const auto& line : lines) {
                std::string text;
                for (const auto& span : line) text += span.text;
                if (text.find(names.of(member["id"].str())) != std::string::npos &&
                    text.find(member["label"].str()) != std::string::npos) visible = true;
            }
            CHECK(visible);  // A narrow layout must retain every member's name and state.
        }
    }
    state["running"] = false;
    state["typing"] = Json::array();
    messages.back()["streaming"] = false;
    messages.back()["status"] = "failed";
    view = workflowView(context, state, run, messages);
    CHECK_EQ(view["status"].str(), std::string("failed"));
    CHECK(!tui::workflowAnimating(view, 13100));
    run.outcome = "stopped";
    CHECK_EQ(workflowView(context, state, run, messages)["status"].str(), std::string("stopped"));
    run.round = 3; run.files = 2; run.added = 12; run.removed = 4;
    state["group"] = Json::object();
    view = workflowView(context, state, run, messages);
    CHECK_EQ(view["round"].i64(), 3LL);
    CHECK_EQ(view["files"].i64(), 2LL);
    CHECK_EQ(view["added"].i64(), 12LL);
    CHECK_EQ(view["removed"].i64(), 4LL);
    for (int i = 0; i < 30; ++i) workflowTurn(run, i % 2 ? "claude" : "codex", "turn", "review", 14000 + i);
    CHECK_EQ(run.transfers.size(), (size_t)8);
    CHECK(run.turns.size() <= 4);
    WorkflowRun fresh;
    CHECK(workflowView(context, state, fresh, messages)["transfers"].size() == 0);

    Json pipeNodes =
        Json::Array{J({{"id", "a"}, {"backend", "claude"}, {"name", "Writer"}, {"role", "write"}}),
                    J({{"id", "g1"}, {"backend", "gemini"}, {"name", "Security"}, {"role", "review"}}),
                    J({{"id", "g2"}, {"backend", "gemini"}, {"name", "Verification"}, {"role", "verify"}})};
    Json pipeContext =
        J({{"kind", "group"}, {"names", J({{"gemini", "Third reviewer"}})}, {"pipeline", J({{"nodes", pipeNodes}})}});
    Json pipeState = J({{"running", true},
                        {"typing", Json::Array{"gemini"}},
                        {"thinking", J({{"gemini", true}})},
                        {"task", J({{"cursor", 2}})}});
    WorkflowRun pipeRun;
    workflowTurn(pipeRun, "pipeline:g1", "g1-msg", "review", 1000);
    workflowTurn(pipeRun, "pipeline:g2", "g2-msg", "verify", 2000);
    Json pipeView = workflowView(pipeContext, pipeState, pipeRun, {});
    CHECK_EQ(pipeView["layout"].str(), std::string("pipeline"));
    CHECK_EQ(pipeView["nodes"].size(), (size_t)3);
    CHECK_EQ(pipeView["nodes"].at(1)["status"].str(), std::string("waiting"));
    CHECK_EQ(pipeView["nodes"].at(2)["status"].str(), std::string("thinking"));
    CHECK_EQ(pipeView["nodes"].at(2)["profile"].str(), std::string("Third reviewer"));
    CHECK_EQ(pipeView["transfers"].at(0)["to"].str(), std::string("pipeline:g2"));
    auto pipeLines = tui::renderWorkflow(pipeView, names, 80, 2100, false, false);
    std::string pipeText;
    for (const auto& row : pipeLines) pipeText += plain(row) + "\n";
    CHECK(pipeText.find("Security") < pipeText.find("Verification"));
    CHECK(pipeText.find("Third reviewer") != std::string::npos);
    pipeState["group"] = J({{"paused", true}, {"current", "gemini"}});
    pipeView = workflowView(pipeContext, pipeState, pipeRun, {});
    CHECK_EQ(pipeView["nodes"].at(2)["status"].str(), std::string("paused"));
    CHECK_EQ(pipeView["nodes"].at(1)["status"].str(), std::string("waiting"));

    // A direct message draws nothing until the other agent has really joined (a takeover).
    Json dm = J({{"kind", "claude"}, {"members", Json::array()}, {"models", Json::object()}});
    Json dmState = J({{"running", true}, {"typing", Json(Json::Array{"claude"})}, {"group", Json::object()}});
    WorkflowRun solo;
    workflowTurn(solo, "claude", "d1", "write", 20000);
    CHECK_EQ(workflowView(dm, dmState, solo, messages)["nodes"].size(), (size_t)0);
    CHECK(tui::renderWorkflow(workflowView(dm, dmState, solo, messages),
                              tui::Names{[](const std::string& w) { return w; }}, 100, 20000, false, true)
              .empty());
    workflowTurn(solo, "codex", "d2", "write", 21000);
    dmState["typing"] = Json(Json::Array{"codex"});
    Json took = workflowView(dm, dmState, solo, messages);
    CHECK_EQ(took["nodes"].size(), (size_t)2);
    CHECK_EQ(took["transfers"].size(), (size_t)1);
    // The conversation's original provider is not evidence that it joined this run.
    WorkflowRun gptOnly;
    workflowTurn(gptOnly, "codex", "next-request", "write", 22000);
    Json continued = workflowView(dm, dmState, gptOnly, messages);
    CHECK_EQ(continued["nodes"].size(), (size_t)0);
    CHECK_EQ(continued["transfers"].size(), (size_t)0);
    CHECK(tui::renderWorkflow(continued, names, 100, 22000, false, true).empty());
}

static void testProjectRules() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = util::joinPath(tmp, util::widen("cca-rules-" + util::newId()));
    util::ensureDir(dir);
    CHECK(projectReviewRules(dir).empty());
    CHECK(projectReviewRules(L"").empty());
    std::wstring agents = util::joinPath(dir, L"AGENTS.md");
    util::writeFileAtomic(agents, "# 指南\n很長的架構說明\n<!-- cca-review:start -->\n只看這段\n<!-- cca-review:end -->\n其他");
    CHECK_EQ(projectReviewRules(dir), std::string("只看這段"));
    util::writeFileAtomic(agents, "沒有標記的規範\n" + std::string(5000, 'x'));
    std::string head = projectReviewRules(dir);
    CHECK(head.find("沒有標記的規範") == 0);
    CHECK(head.size() < 3 * 1024 + 64);
    DeleteFileW(agents.c_str());
    util::writeFileAtomic(util::joinPath(dir, L"CLAUDE.md"), "只有 CLAUDE.md");
    CHECK_EQ(projectReviewRules(dir), std::string("只有 CLAUDE.md"));
    DeleteFileW(util::joinPath(dir, L"CLAUDE.md").c_str());
    RemoveDirectoryW(dir.c_str());
}

static void testSnapshots() {
    Snapshots snaps;
    if (!snaps.available()) {
        std::printf("skip snapshots: no git\n");
        return;
    }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = util::joinPath(tmp, util::widen("cca-snap-" + util::newId()));
    util::ensureDir(dir);
    std::string conv = "test-" + util::newId();
    auto path = [&](const char* f) { return util::joinPath(dir, util::widen(f)); };
    auto read = [&](const char* f) { std::string s; return util::readFile(path(f), s) ? s : std::string("<missing>"); };
    util::writeFileAtomic(path("a.txt"), "one");
    util::ensureDir(path("node_modules"));
    util::writeFileAtomic(path("node_modules\\dep.js"), "big");
    SnapshotResult s1 = snaps.take(conv, dir, "first");
    CHECK(s1.ok);
    // The agent edits a file and adds another one.
    util::writeFileAtomic(path("a.txt"), "two");
    util::writeFileAtomic(path("b.txt"), "new");
    RestoreResult r = snaps.restore(conv, dir, s1.commit);
    CHECK(r.ok);
    CHECK_EQ(read("a.txt"), std::string("one"));
    CHECK_EQ(read("b.txt"), std::string("<missing>"));
    CHECK_EQ(r.removed.size(), (size_t)1);
    // Excluded dependency folders are never touched.
    CHECK_EQ(read("node_modules\\dep.js"), std::string("big"));
    // Undo the rollback.
    RestoreResult back = snaps.restore(conv, dir, r.backup);
    CHECK(back.ok);
    CHECK_EQ(read("a.txt"), std::string("two"));
    CHECK_EQ(read("b.txt"), std::string("new"));
    CHECK(!snaps.restore(conv, dir, "not-a-hash").ok);
}

static void testReview() {
    // Parsing: files, hunks, changes and counts from a unified diff.
    std::string text =
        "diff --git a/src/a.txt b/src/a.txt\nindex 1..2 100644\n--- a/src/a.txt\n+++ b/src/a.txt\n"
        "@@ -1,3 +1,3 @@\n one\n-two\n+TWO\n three\n@@ -10,2 +10,3 @@ tail\n ten\n eleven\n+twelve\n"
        "diff --git a/new.txt b/new.txt\nnew file mode 100644\nindex 0..3\n--- /dev/null\n+++ b/new.txt\n@@ -0,0 +1 "
        "@@\n+hello\n"
        "diff --git a/img.png b/img.png\nindex 4..5 100644\nBinary files a/img.png and b/img.png differ\n";
    std::vector<ReviewFile> files = parseReviewDiff(text);
    CHECK_EQ(files.size(), (size_t)3);
    CHECK_EQ(files[0].path, std::string("src/a.txt"));
    CHECK_EQ(files[0].hunks.size(), (size_t)2);
    CHECK(files[0].added == 2 && files[0].removed == 1);
    CHECK_EQ(files[1].change, std::string("add"));
    CHECK(files[2].binary);
    std::string patch = reviewHunkPatch(files[0], 1);
    CHECK(patch.find("+++ b/src/a.txt\n@@ -10,2 +10,3 @@ tail\n ten\n eleven\n+twelve\n") != std::string::npos);
    CHECK(patch.find("-two") == std::string::npos);
    CHECK(reviewHunkPatch(files[0], 5).empty());
    bool truncated = false;
    Json js = reviewFilesJson(files, &truncated);
    CHECK(!truncated);
    CHECK_EQ(js.items()[0]["path"].str(), std::string("src\\a.txt"));
    CHECK_EQ(js.items()[2]["change"].str(), std::string("binary"));
    CHECK_EQ(js.items()[0]["hunks"].items()[0]["lines"].size(), (size_t)4);
    CHECK(!Snapshots::safeRelativePath("..\\secret"));
    CHECK(!Snapshots::safeRelativePath("C:\\x"));
    CHECK(!Snapshots::safeRelativePath("-rf"));
    CHECK(!Snapshots::safeRelativePath("src//x"));
    CHECK(Snapshots::safeRelativePath("src\\a.txt"));

    // Against a real shadow repository: diff, revert one hunk, put a new file back to "not there".
    Snapshots snaps;
    if (!snaps.available()) {
        std::printf("skip review: no git\n");
        return;
    }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = util::joinPath(tmp, util::widen("cca-review-" + util::newId()));
    util::ensureDir(dir);
    std::string conv = "test-" + util::newId();
    auto path = [&](const char* f) { return util::joinPath(dir, util::widen(f)); };
    auto read = [&](const char* f) {
        std::string s;
        return util::readFile(path(f), s) ? s : std::string("<missing>");
    };
    std::string before;
    for (int i = 1; i <= 20; ++i) before += "line " + std::to_string(i) + "\n";
    util::writeFileAtomic(path("a.txt"), before);
    SnapshotResult base = snaps.take(conv, dir, "base");
    CHECK(base.ok);
    std::string after =
        util::replaceAll(util::replaceAll(before, "line 2\n", "LINE TWO\n"), "line 19\n", "LINE NINETEEN\n");
    util::writeFileAtomic(path("a.txt"), after);
    util::writeFileAtomic(path("b.txt"), "brand new\n");
    SnapshotResult head = snaps.take(conv, dir, "head");
    std::string out, err;
    CHECK(snaps.diff(conv, dir, base.commit, head.commit, "", &out, &err));
    std::vector<ReviewFile> real = parseReviewDiff(out);
    CHECK_EQ(real.size(), (size_t)2);
    CHECK(real.size() == 2 && real[0].path == "a.txt" && real[0].hunks.size() == 2);
    if (real.size() == 2 && real[0].hunks.size() == 2) {
        CHECK(snaps.reverseApply(conv, dir, reviewHunkPatch(real[0], 0), &err));
        CHECK(read("a.txt").find("line 2\n") != std::string::npos);
        CHECK(read("a.txt").find("LINE NINETEEN") != std::string::npos);
        // The same hunk cannot be reversed twice.
        CHECK(!snaps.reverseApply(conv, dir, reviewHunkPatch(real[0], 0), &err));
    }
    CHECK(snaps.restorePath(conv, dir, base.commit, "b.txt", &err));
    CHECK_EQ(read("b.txt"), std::string("<missing>"));
    CHECK(snaps.restorePath(conv, dir, base.commit, "a.txt", &err));
    CHECK_EQ(read("a.txt"), before);
    CHECK(!snaps.restorePath(conv, dir, base.commit, "..\\a.txt", &err));
}

static void testRecommend() {
    Json gpu12 = J({{"bestVramGb", 12.0}, {"ramGb", 16.0}});
    std::string pick;
    Json r12 = recommendModels(gpu12);
    for (const auto& m : r12.items()) if (m["recommended"].boolean()) pick = m["model"].str();
    CHECK_EQ(pick, std::string("qwen3:14b"));
    Json big = J({{"bestVramGb", 24.0}, {"ramGb", 64.0}});
    pick.clear();
    Json r24 = recommendModels(big);
    for (const auto& m : r24.items()) if (m["recommended"].boolean()) pick = m["model"].str();
    CHECK_EQ(pick, std::string("qwen3-coder:30b"));
    Json cpu = J({{"bestVramGb", 0.0}, {"ramGb", 8.0}});
    pick.clear();
    Json r0 = recommendModels(cpu);
    for (const auto& m : r0.items()) if (m["recommended"].boolean()) pick = m["model"].str();
    CHECK(!pick.empty());
    CHECK_EQ(pick, std::string("qwen3:4b"));
    CHECK(detectHardware()["ramGb"].num() > 0);
}

// Version-control pane rules: matching, status kinds, colors, secret scan and history parsing.
static void testGitflow() {
    using namespace gitflow;
    CHECK(globMatch(".env", ".env") && globMatch(".env", "app/.env") && !globMatch(".env", ".env.example"));
    CHECK(globMatch("*.pem", "certs/server.PEM") && !globMatch("*.pem", "pem/readme.md"));
    CHECK(globMatch("build/", "build/out.exe") && globMatch("build/", "sub/build/a.o") &&
          !globMatch("build/", "build"));
    CHECK(globMatch("docs/private", "docs/private/notes.md") && !globMatch("docs/private", "src/docs/private/x"));
    CHECK(globMatch("src/**/*.gen.cpp", "src/a/b/c.gen.cpp") && !globMatch("src/*.gen.cpp", "src/a/c.gen.cpp"));
    CHECK(validRule("secrets/") && !validRule("../outside") && !validRule("C:/x") && !validRule(""));
    CHECK(authProblem("fatal: Authentication failed for 'https://github.com/a/b.git/'") == "auth");
    CHECK(authProblem("fatal: could not read Username for 'https://x': terminal prompts disabled") == "auth");
    CHECK(authProblem("git@github.com: Permission denied (publickey).\nfatal: Could not read from remote") == "auth");
    CHECK(authProblem("remote: Repository not found.\nfatal: repository 'https://github.com/a/b.git/' not found") ==
          "auth");
    CHECK(authProblem("fatal: unable to access 'https://x/': Could not resolve host: x") == "network" &&
          authProblem("fatal: not a git repository") == "");
    Json host = remoteHost("https://user:token@GitHub.com:443/a/b.git");
    CHECK(host["host"].str() == "github.com" && !host["ssh"].boolean());
    host = remoteHost("git@gitlab.example.com:team/app.git");
    CHECK(host["host"].str() == "gitlab.example.com" && host["ssh"].boolean());
    host = remoteHost("ssh://git@host.example:2222/a.git");
    CHECK(host["host"].str() == "host.example" && host["ssh"].boolean());
    CHECK(remoteHost("C:\\repos\\remote.git")["host"].str().empty() &&
          remoteHost("file:///C:/repos/r.git")["host"].str().empty());
    const char nul = '\0';
    std::string raw = std::string(" M src/a.cpp") + nul + "?? new file.txt" + nul + "A  added.h" + nul + " D gone.txt" +
                      nul + "R  moved.cpp" + nul + "old.cpp" + nul + "UU clash.c" + nul;
    Json files = parseStatus(raw);
    CHECK_EQ(files.size(), (size_t)6);
    CHECK_EQ(files.at(1)["path"].str(), std::string("new file.txt"));
    CHECK_EQ(files.at(4)["from"].str(), std::string("old.cpp"));
    CHECK_EQ(files.at(5)["kind"].str(), std::string("conflict"));
    Json more = parseStatus(std::string("?? .env") + nul + "?? tool.exe" + nul + "?? keys/id_rsa" + nul + "?? big.dat" +
                            nul + "?? node_modules/x/index.js" + nul);
    for (const auto& f : more.items()) files.push(f);
    Json tones = classify(files, Json(Json::Array{"*.exe"}), J({{"big.dat", 6LL * 1024 * 1024}}));
    auto tone = [&](size_t i) { return tones.at(i)["tone"].str(); };
    CHECK_EQ(tone(0), std::string("changed"));                       // yellow: modified
    CHECK_EQ(tone(1), std::string("new"));                           // green: untracked
    CHECK_EQ(tone(2), std::string("new"));                           // green: added
    CHECK_EQ(tone(3), std::string("changed"));                       // a deletion is a change, not a risk
    CHECK_EQ(tone(5), std::string("risk"));                          // conflicts are never preselected
    CHECK(tone(6) == "risk" && !tones.at(6)["selected"].boolean());  // .env
    CHECK(tones.at(7)["locked"].boolean() && tones.at(7)["rule"].str() == "*.exe");  // never-commit rule
    CHECK(tone(8) == "risk" && tone(9) == "risk" && tone(10) == "risk");             // key, size, dependency
    CHECK(tones.at(0)["selected"].boolean() && tones.at(1)["selected"].boolean());
    const std::string diff =
        "+++ b/config.js\n@@ -1,0 +1,3 @@\n+const name = 'ok';\n+const key = \"sk-abcdefghijklmnopqrstuvwxyz012345\";\n"
        "+password = \"hunter2hunter2\"\n+++ b/clean.txt\n@@ -0,0 +1 @@\n+hello\n";
    Json hits = secretHits(diff);
    CHECK_EQ(hits.size(), (size_t)2);
    CHECK(hits.at(0)["path"].str() == "config.js" && hits.at(0)["line"].i64() == 2);
    CHECK(hits.dump().find("abcdefghij") == std::string::npos);  // the secret itself is never echoed
    const std::string us = "\x1f", rs = "\x1e";
    const std::string log = "aaaaaaaa1" + us + "bbbbbbbb2 cccccccc3" + us + "Finn" + us + "1700000000" + us +
                            "HEAD -> main, origin/main" + us + "Merge work" + us + "task-42" + rs + "\n" + "bbbbbbbb2" +
                            us + us + "Finn" + us + "1690000000" + us + us + "Initial" + us + rs;
    Json commits = parseLog(log);
    CHECK_EQ(commits.size(), (size_t)2);
    CHECK_EQ(commits.at(0)["parents"].size(), (size_t)2);
    CHECK_EQ(commits.at(0)["refs"].size(), (size_t)2);
    CHECK_EQ(commits.at(0)["task"].str(), std::string("task-42"));
    CHECK(commits.at(1)["parents"].size() == 0 && commits.at(1)["ts"].i64() == 1690000000000LL);
}

static void testPresets() {
    CHECK_EQ(ClaudeSession::permissionMode("plan"), std::string("plan"));
    CHECK_EQ(ClaudeSession::permissionMode("ask"), std::string("default"));
    CHECK_EQ(ClaudeSession::permissionMode("edit"), std::string("acceptEdits"));
    CHECK_EQ(ClaudeSession::permissionMode("bypass"), std::string("bypassPermissions"));
    CHECK_EQ(CodexThread::sandboxMode("ask"), std::string("read-only"));
    CHECK_EQ(CodexThread::sandboxMode("edit"), std::string("workspace-write"));
    CHECK_EQ(CodexThread::sandboxMode("bypass"), std::string("danger-full-access"));
    CHECK_EQ(CodexThread::approvalPolicy("bypass"), std::string("never"));
    CHECK_EQ(CodexThread::approvalPolicy("edit"), std::string("on-request"));
    CHECK_EQ(CodexThread::sandboxPolicy("edit")["type"].str(), std::string("workspaceWrite"));
    // Every variable a parent Claude session sets for its children is scrubbed.
    bool hasApiKey = false;
    for (const auto& v : ClaudeSession::envToDrop()) hasApiKey |= v == L"ANTHROPIC_API_KEY";
    CHECK(hasApiKey);
}

static Json toolBlock(const char* kind) {
    Json blocks = Json::array();
    blocks.push(J({{"type", "tool"}, {"kind", kind}, {"status", "ok"}}));
    return blocks;
}

static bool hasIssue(const std::vector<GuardIssue>& issues, const std::string& code) {
    for (const auto& i : issues) {
        if (i.code == code) return true;
    }
    return false;
}

static void testGuard() {
    GuardRules dm;
    Json none = Json::array();
    CHECK(guardCheck(dm, "這個函式沒有問題，邊界情況也處理好了。", none).empty());
    // Simplified Chinese, but code, paths and identifiers in it do not count.
    CHECK(hasIssue(guardCheck(dm, "这个函数没有问题。", none), "simplified"));
    CHECK(guardCheck(dm, "這段沒問題：\n```js\nconst 这个 = 1; // 简体注释\n```\n路徑 `src/这.js` 也可以。", none)
              .empty());
    CHECK(guardSimplified("這裡有個这").size() == 1);
    CHECK(!hasIssue(guardCheck(dm, "這裡有一個「这」字被引用。", none), "simplified"));
    // English replies.
    CHECK(hasIssue(
        guardCheck(dm, "The function looks fine and the edge cases are handled correctly in every branch.", none),
        "language"));
    CHECK(guardCheck(dm, "已經修好 `parseVerdict`，Claude Code 和 Codex app-server 都能讀到 VERDICT。", none).empty());

    // Persona: the address in every reply, no stepping out of character.
    GuardRules leva;
    leva.personaName = "角色甲";
    leva.address = "指揮官";
    CHECK(hasIssue(guardCheck(leva, "函式沒有問題。", none), "address"));
    CHECK(guardCheck(leva, "指揮官，函式沒有問題。", none).empty());
    CHECK(hasIssue(guardCheck(leva, "指揮官，作為一個 AI 語言模型，我無法保證。", none), "persona"));
    CHECK(hasIssue(guardCheck(leva, "指揮官，As an AI, I cannot do that. 不過我看過了。", none), "persona"));

    // Group protocol and evidence.
    GuardRules review;
    review.requireVerdict = review.requireEvidence = true;
    CHECK(hasIssue(guardCheck(review, "看過了，沒問題。", none), "verdict"));
    CHECK(hasIssue(guardCheck(review, "看過了，沒問題。\nVERDICT: APPROVE", none), "no-evidence"));
    CHECK(guardCheck(review, "看過了，沒問題。\nVERDICT: APPROVE", toolBlock("read")).empty());
    review.changedFiles = 2;
    CHECK(hasIssue(guardCheck(review, "改好了。\nVERDICT: APPROVE", toolBlock("read")), "approve-changed"));
    CHECK(guardCheck(review, "改了兩個檔案。\nVERDICT: CHANGED", toolBlock("edit")).empty());

    // Test claims need a command in the same turn; denials and failures are not claims.
    CHECK(guardSaysTestsPassed("測試全部通過。"));
    CHECK(guardSaysTestsPassed("155 passed, 0 failed"));
    CHECK(!guardSaysTestsPassed("還沒有確認測試通過。"));
    CHECK(!guardSaysTestsPassed("測試沒有通過，還有兩個失敗。"));
    CHECK(!guardSaysTestsPassed("我改了兩個函式。"));
    CHECK(hasIssue(guardCheck(dm, "改好了，測試都通過。", none), "test-claim"));
    CHECK(guardCheck(dm, "改好了，測試都通過。", toolBlock("command")).empty());
    GuardRules daily;
    daily.checkTestClaims = false;
    CHECK(guardCheck(daily, "測試都通過。", none).empty());

    // Local notes must be answered by name, with evidence.
    GuardRules notes;
    notes.localNames = {"本地甲", "本地乙"};
    CHECK(hasIssue(guardCheck(notes, "本地甲說的對，已經修好。", toolBlock("read")), "local-notes"));
    CHECK(hasIssue(guardCheck(notes, "本地甲和本地乙的建議我都看過了。", none), "local-notes"));
    CHECK(guardCheck(notes, "本地甲的建議：讀了 a.cpp 第 3 行，成立。本地乙的建議：不成立。", toolBlock("read")).empty());

    // The rule card and the send-back carry the rules that apply.
    std::string card = guardRuleCard(leva);
    CHECK(card.find("繁體中文") != std::string::npos);
    CHECK(card.find("指揮官") != std::string::npos);
    CHECK(card.find("VERDICT") == std::string::npos);
    CHECK(guardRuleCard(review).find("VERDICT") != std::string::npos);
    auto issues = guardCheck(leva, "函式沒有問題。", none);
    std::string back = guardCorrection(leva, issues, 2, 2);
    CHECK(back.find("第 2/2 次退回") != std::string::npos);
    CHECK(back.find("最後一次") != std::string::npos);
    CHECK(guardSummary(issues) == "沒有使用角色稱呼");
    CHECK(guardIssuesJson(issues).size() == 1);
}

static void testLocalTools() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring root = util::joinPath(tmp, L"cca-local-tools-" + util::widen(util::randomHex(4)));
    util::ensureDir(util::joinPath(root, L"src"));
    util::writeFileAtomic(util::joinPath(root, L"src\\cart.ts"),
                          "export function total(items) {\n  let sum = 0;\n  for (const i of items) sum += i.price;\n"
                          "  return sum - discount;\n}\n");
    std::wstring outside = util::joinPath(tmp, L"cca-outside-secret.txt");
    util::writeFileAtomic(outside, "secret\n");

    std::wstring full;
    std::string err;
    CHECK(resolveInside(root, "src/cart.ts", &full, &err));
    CHECK(!resolveInside(root, "..\\cca-outside-secret.txt", &full, &err));
    CHECK(!resolveInside(root, util::narrow(outside), &full, &err));
    CHECK(!resolveInside(root, "src\\cart.ts:stream", &full, &err));
    CHECK(!resolveInside(root, ".git\\config", &full, &err));
    CHECK(!resolveInside(root, "src\\.GIT\\config", &full, &err));
    // The same names agent_runtime.py denies: secrets, credentials and tool settings.
    util::writeFileAtomic(util::joinPath(root, L".env.local"), "TOKEN=1\n");
    util::ensureDir(util::joinPath(root, L"credentials"));
    util::writeFileAtomic(util::joinPath(root, L"credentials\\key.txt"), "secret-token\n");
    for (const char* path :
         {".env.local", "credentials\\key.txt", ".npmrc", ".claude\\settings.json", ".codex\\config.toml"}) {
        CHECK(!resolveInside(root, path, &full, &err));
    }
    CHECK(runLocalTool(root, "list_dir", J({{"path", "."}})).output.find("credentials") == std::string::npos);
    CHECK(runLocalTool(root, "list_dir", J({{"path", "."}})).output.find(".env") == std::string::npos);
    CHECK(runLocalTool(root, "search", J({{"text", "secret-token"}})).output.find("key.txt") == std::string::npos);
    DeleteFileW(util::joinPath(root, L".env.local").c_str());
    DeleteFileW(util::joinPath(root, L"credentials\\key.txt").c_str());
    RemoveDirectoryW(util::joinPath(root, L"credentials").c_str());

    // Junctions: one leading out of the project is refused anywhere along the path, one inside is fine.
    std::wstring away = util::joinPath(tmp, L"cca-outside-" + util::widen(util::randomHex(4)));
    util::ensureDir(away);
    util::writeFileAtomic(util::joinPath(away, L"secret.txt"), "secret-in-junction\n");
    std::wstring outLink = util::joinPath(root, L"outlink"), inLink = util::joinPath(root, L"inlink");
    _wsystem((L"cmd /c mklink /J \"" + outLink + L"\" \"" + away + L"\" >nul").c_str());
    _wsystem((L"cmd /c mklink /J \"" + inLink + L"\" \"" + util::joinPath(root, L"src") + L"\" >nul").c_str());
    CHECK(util::dirExists(outLink) && util::dirExists(inLink));
    CHECK(!resolveInside(root, "outlink\\secret.txt", &full, &err));
    CHECK(!runLocalTool(root, "read_file", J({{"path", "outlink/secret.txt"}})).ok);
    CHECK(runLocalTool(root, "search", J({{"text", "secret-in-junction"}})).output.find("outlink") ==
          std::string::npos);
    CHECK(resolveInside(root, "inlink\\cart.ts", &full, &err));
    CHECK(!resolveInside(root, "src\\missing.ts", &full, &err));
    // The checkout itself (on this machine a OneDrive folder) stays readable after link resolution.
    wchar_t here[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, here);
    if (util::fileExists(util::joinPath(here, L"AGENTS.md"))) {
        CHECK(runLocalTool(here, "read_file", J({{"path", "AGENTS.md"}, {"end_line", 3}})).ok);
    }
    RemoveDirectoryW(outLink.c_str());
    RemoveDirectoryW(inLink.c_str());
    DeleteFileW(util::joinPath(away, L"secret.txt").c_str());
    RemoveDirectoryW(away.c_str());
    CHECK_EQ(normalizeRelPath("./SRC/Cart.ts"), std::string("src\\cart.ts"));

    LocalToolResult r =
        runLocalTool(root, "read_file", J({{"path", "src/cart.ts"}, {"start_line", 2}, {"end_line", 3}}));
    CHECK(r.ok);
    CHECK_EQ(r.path, std::string("src\\cart.ts"));
    CHECK(r.output.find("2|   let sum = 0;") != std::string::npos);
    CHECK(r.output.find("1| export") == std::string::npos);
    CHECK(!runLocalTool(root, "read_file", J({{"path", "../cca-outside-secret.txt"}})).ok);
    CHECK(runLocalTool(root, "search", J({{"text", "DISCOUNT"}})).output.find("cart.ts:4:") != std::string::npos);
    CHECK(runLocalTool(root, "list_dir", J({{"path", "."}})).output.find("src\\") != std::string::npos);
    CHECK(!runLocalTool(root, "write_file", J({{"path", "x"}})).ok);
    CHECK(localToolSpecs().size() == 3);

    GuardRules rules;
    std::set<std::string> read{"src\\cart.ts"};
    std::vector<std::string> changed{"src\\cart.ts"};
    std::string good =
        "發現 1：discount 沒有定義\n位置：src/cart.ts:4\n原文：`return sum - discount;`\n問題：會丟 ReferenceError\n"
        "建議：改成參數\n信心：高\n";
    LocalReviewCheck ok = checkLocalReview(root, good, read, changed, rules);
    CHECK(ok.issues.empty());
    CHECK(ok.findings.size() == 1 && ok.findings[0].verified);
    CHECK(ok.forwarded.find("discount 沒有定義") != std::string::npos);

    // A quote that is not in the file, a line that does not exist, and a file never read.
    std::string bad = good +
                      "\n發現 2：迴圈寫錯\n位置：src/cart.ts:3\n原文：for (let i = 0; i < n; i++)\n"
                      "\n發現 3：超出範圍\n位置：src/cart.ts:99\n原文：x\n";
    LocalReviewCheck mixed = checkLocalReview(root, bad, read, changed, rules);
    CHECK(mixed.removed == 2);
    CHECK(hasIssue(mixed.issues, "local-cite"));
    CHECK(mixed.forwarded.find("迴圈寫錯") == std::string::npos);
    CHECK(mixed.forwarded.find("已移除") != std::string::npos);
    CHECK(hasIssue(checkLocalReview(root, good, {}, changed, rules).issues, "local-unread"));
    std::string worded =
        "### 發現 1：discount 沒有定義\n- **位置：** `src\\cart.ts`，第 4 行\n- **原文：**\n```ts\nreturn sum - "
        "discount;\n```\n";
    LocalReviewCheck words = checkLocalReview(root, worded, read, changed, rules);
    CHECK(words.issues.empty());
    CHECK(words.findings.size() == 1 && words.findings[0].line == 4 && words.findings[0].verified);
    // Title and location on one line, and a quote copied with read_file's line numbers.
    std::string numbered =
        "發現 1：位置：src\\cart.ts:2–3\n原文：\n2|   let sum = 0;\n3|   for (const i of items) sum += i.price;\n"
        "問題：沒有處理空陣列\n信心：低\n";
    LocalReviewCheck nums = checkLocalReview(root, numbered, read, changed, rules);
    CHECK(nums.findings.size() == 1 && nums.findings[0].line == 2 && nums.findings[0].lineEnd == 3);
    CHECK(nums.findings.size() == 1 && nums.findings[0].verified);
    CHECK(!hasIssue(nums.issues, "local-cite"));

    // A finding that starts after the form of address is still its own finding, checked on its own.
    std::string addressed =
        good + "\n指揮官，發現 2：亂猜的迴圈\n位置：src/cart.ts:3\n原文：for (let i = 0; i < n; i++)\n";
    LocalReviewCheck split = checkLocalReview(root, addressed, read, changed, rules);
    CHECK(split.findings.size() == 2);
    CHECK(split.removed == 1);
    CHECK(split.forwarded.find("亂猜的迴圈") == std::string::npos);

    // Tool calls written into the text are run; what followed them was written blind and is dropped.
    Json textCalls = Json::array();
    std::string ahead;
    CHECK(
        extractTextToolCalls("指揮官，我先讀檔。\n\n{\"name\": \"read_file\", \"parameters\": {\"path\": "
                             "\"src\\\\cart.ts\"}}\n\n發現 1：亂猜的",
                             &textCalls, &ahead));
    CHECK(textCalls.size() == 1);
    CHECK_EQ(textCalls.items()[0]["function"]["name"].str(), std::string("read_file"));
    CHECK_EQ(textCalls.items()[0]["function"]["arguments"]["path"].str(), std::string("src\\cart.ts"));
    CHECK_EQ(ahead, std::string("指揮官，我先讀檔。"));
    Json none2 = Json::array();
    CHECK(!extractTextToolCalls("設定是 {\"name\": \"x\"}，沒有工具。", &none2, &ahead));
    CHECK(!extractTextToolCalls("const o = { a: 1 };", &none2, &ahead));
    CHECK(hasIssue(checkLocalReview(root, "看起來都還好。", read, changed, rules).issues, "local-format"));
    LocalReviewCheck clean = checkLocalReview(root, "沒有發現問題。讀過 src\\cart.ts。", read, changed, rules);
    CHECK(clean.issues.empty());
    CHECK(clean.forwarded.find("沒有發現問題") != std::string::npos);
    CHECK(hasIssue(checkLocalReview(root, "没有发现问题。", read, changed, rules).issues, "simplified"));

    DeleteFileW(util::joinPath(root, L"src\\cart.ts").c_str());
    RemoveDirectoryW(util::joinPath(root, L"src").c_str());
    RemoveDirectoryW(root.c_str());
    DeleteFileW(outside.c_str());
}

static void testClaudeUsage() {
    CHECK_EQ(parseIsoTime("2026-10-24T05:00:00Z"), 1792818000LL);
    CHECK_EQ(parseIsoTime("2026-10-24T13:00:00+08:00"), 1792818000LL);
    CHECK_EQ(parseIsoTime("2026-10-24T05:00:00.123456+00:00"), 1792818000LL);
    CHECK_EQ(parseIsoTime("not a date"), 0LL);
    CHECK_EQ(parseIsoTime("2026-02-30T05:00:00Z"), 0LL);
    CHECK_EQ(parseIsoTime("2026-10-24T25:00:00Z"), 0LL);
    CHECK_EQ(parseIsoTime("2026-10-24T05:00:00+80:00"), 0LL);
    CHECK_EQ(parseIsoTime("2026-10-24"), 0LL);
    Json body = Json::parse(R"({"five_hour":{"utilization":23.0,"resets_at":"2026-10-24T05:00:00Z"},
        "seven_day":{"utilization":100,"resets_at":"2026-10-30T00:00:00Z"},"seven_day_opus":null,
        "extra_usage":{"is_enabled":false}})");
    Json l = parseClaudeUsage(body);
    CHECK(l["five_hour"]["utilization"].num() > 0.229 && l["five_hour"]["utilization"].num() < 0.231);
    CHECK_EQ(l["five_hour"]["status"].str(), std::string("allowed"));
    CHECK_EQ(l["five_hour"]["resetsAt"].i64(), 1792818000LL);
    CHECK_EQ(l["seven_day"]["status"].str(), std::string("rejected"));
    CHECK(!l.has("seven_day_opus") && !l.has("extra_usage"));  // nulls and non-window objects are skipped
    Json arr =
        parseClaudeUsage(Json::parse(R"({"limits":[{"type":"five_hour","utilization":85,"resets_at":1792818000}]})"));
    CHECK_EQ(arr["five_hour"]["status"].str(), std::string("allowed_warning"));

    // The real request path against a local stand-in for the usage service.
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(srv, (sockaddr*)&addr, sizeof addr);
    int len = sizeof addr;
    getsockname(srv, (sockaddr*)&addr, &len);
    listen(srv, 1);
    std::string request;
    std::thread server([&] {
        SOCKET s = accept(srv, nullptr, nullptr);
        char buf[4096];
        int n = recv(s, buf, sizeof buf, 0);
        if (n > 0) request.assign(buf, n);
        std::string reply = R"({"five_hour":{"utilization":42.5,"resets_at":"2026-10-24T05:00:00Z"}})";
        std::string resp =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(reply.size()) +
            "\r\nConnection: close\r\n\r\n" + reply;
        send(s, resp.data(), (int)resp.size(), 0);
        closesocket(s);
    });
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring cred = util::joinPath(tmp, util::widen("cca-cred-" + util::newId() + ".json"));
    util::writeFileAtomic(
        cred,
        J({{"claudeAiOauth", J({{"accessToken", "fake-token-123"}, {"expiresAt", util::nowMs() + 3600000}})}}).dump());
    SetEnvironmentVariableW(L"CCA_CLAUDE_CREDENTIALS", cred.c_str());
    std::wstring url = L"http://127.0.0.1:" + std::to_wstring(ntohs(addr.sin_port)) + L"/api/oauth/usage";
    SetEnvironmentVariableW(L"CCA_CLAUDE_USAGE_URL", url.c_str());
    ClaudeUsageResult r = fetchClaudeUsage();
    server.join();
    closesocket(srv);
    CHECK(r.ok);
    CHECK(r.limits["five_hour"]["utilization"].num() > 0.42 && r.limits["five_hour"]["utilization"].num() < 0.43);
    CHECK(request.find("Authorization: Bearer fake-token-123") != std::string::npos);
    CHECK(request.find("anthropic-beta: oauth-2025-04-20") != std::string::npos);
    CHECK(r.error.find("fake-token") == std::string::npos);
    // An expired sign-in is left for Claude Code to refresh; no request is made.
    util::writeFileAtomic(cred, J({{"claudeAiOauth", J({{"accessToken", "old"}, {"expiresAt", 1000}})}}).dump());
    ClaudeUsageResult expired = fetchClaudeUsage();
    CHECK(!expired.ok && expired.error.find("過期") != std::string::npos);
    // Plain http anywhere but loopback is refused.
    SetEnvironmentVariableW(L"CCA_CLAUDE_USAGE_URL", L"http://example.com/usage");
    util::writeFileAtomic(
        cred, J({{"claudeAiOauth", J({{"accessToken", "fake"}, {"expiresAt", util::nowMs() + 3600000}})}}).dump());
    CHECK(fetchClaudeUsage().error.find("HTTPS") != std::string::npos);
    SetEnvironmentVariableW(L"CCA_CLAUDE_USAGE_URL", nullptr);
    SetEnvironmentVariableW(L"CCA_CLAUDE_CREDENTIALS", nullptr);
    DeleteFileW(cred.c_str());
    WSACleanup();
}

static void testTaskLedger() {
    Json task = newTask("conv-ledger-" + util::newId(), "加上登入頁，驗收：測試全過", 1000);
    CHECK_EQ(task["status"].str(), std::string("running"));
    Json blocks = Json::array();
    blocks.push(J({{"type", "text"}, {"text", "寫好了 login.js"}}));
    blocks.push(J({{"type", "tool"}, {"command", "npm test"}, {"status", "ok"}, {"exitCode", 0}}));
    blocks.push(J({{"type", "tool"}, {"command", "ls"}, {"status", "ok"}, {"exitCode", 0}}));
    Json msg = J({{"id", "m1"}, {"snapshot", "aaaaaaaa1111"}, {"blocks", blocks}});
    Json files = Json::array();
    files.push(J({{"path", "login.js"}, {"change", "add"}, {"added", 30}, {"removed", 0}, {"diff", "+..."}}));
    Json h = handoffFromTurn(task, msg, "claude", "write", files, "write", 2000);
    for (const char* key : {"task_id", "agent_id", "role", "base_revision", "head_revision", "changed_files",
                            "diff_reference", "summary", "issues", "test_results", "next_action"}) {
        CHECK(h.has(key));
    }
    CHECK(!h["changed_files"].at(0).has("diff"));  // the record points at the diff instead of copying it
    CHECK(h["test_results"].at(0)["test"].boolean());
    CHECK(!h["test_results"].at(1)["test"].boolean());
    addHandoff(task, h);
    setNextAction(task, "codex:review");
    setHeadRevision(task, "bbbbbbbb2222");
    CHECK_EQ(task["handoffs"].at(0)["diff_reference"].str(), std::string("snapshot:aaaaaaaa1111..bbbbbbbb2222"));
    setNextAction(task, "ignored");  // already decided
    CHECK_EQ(task["handoffs"].at(0)["next_action"].str(), std::string("codex:review"));
    std::string brief = taskBrief(task, 4000);
    CHECK(brief.find("login.js(+30−0)") != std::string::npos);
    CHECK(brief.find("測試：1 次，失敗 0") != std::string::npos);
    CHECK(brief.find("codex:review") != std::string::npos);

    // Many rounds: the record is capped and the brief stays within its budget.
    for (int i = 0; i < 60; ++i) {
        Json more = handoffFromTurn(task, msg, i % 2 ? "claude" : "codex", "review", files, "changed", 3000 + i);
        addHandoff(task, more);
    }
    CHECK_EQ(task["handoffs"].size(), (size_t)40);
    CHECK_EQ(task["earlier"].i64(), 21LL);
    CHECK_EQ(task["handoffs"].items().back()["seq"].i64(), 61LL);
    std::string capped = taskBrief(task, 1500);
    CHECK(capped.size() <= 1500 + 2100);  // header (request) plus the budget
    CHECK(capped.find("較早的") != std::string::npos);
    Json view = taskView(task);
    CHECK_EQ(view["handoffs"].size(), (size_t)10);
    CHECK_EQ(view["count"].i64(), 61LL);

    // Saved and read back the way a restart does.
    task["status"] = "paused";
    CHECK(saveTask(task));
    Json back = loadTask(task["conv_id"].str());
    CHECK_EQ(back["task_id"].str(), task["task_id"].str());
    CHECK_EQ(back["handoffs"].size(), (size_t)40);
    deleteTask(task["conv_id"].str());
    CHECK(loadTask(task["conv_id"].str()).isNull());
}

int main() {
    testClaudeUsage();
    testTaskLedger();
    testJson();
    testDiff();
    testVerdict();
    testCommandLine();
    testUtil();
    testProjectFolders();
    testPersonas();
    testActivity();
    testHumanize();
    testGuard();
    testLocalTools();
    testDataLock();
    testCliText();
    testCliMessages();
    testWorkflow();
    CodexThreadTestAccess::testHumanizeReconfigure();
    CodexThreadTestAccess::testSandboxConfig();
    testSandboxMcp();
    testProjectRules();
    testSnapshots();
    testReview();
    testRecommend();
    testPresets();
    testGitflow();
    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
