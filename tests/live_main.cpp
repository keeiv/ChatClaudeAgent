// Live end-to-end check against the real CLIs (uses a little subscription quota).
// Drives App exactly like the UI would and prints what the UI would receive.
//   build.bat live            then   build\live.exe codex|claude|group
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <set>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "app.hpp"
#include "local_tools.hpp"
#include "util.hpp"

static std::mutex g_mu;
static std::condition_variable g_cv;
static std::deque<std::function<void()>> g_q;

void postToUi(std::function<void()> fn) {
    std::lock_guard<std::mutex> l(g_mu);
    g_q.push_back(std::move(fn));
    g_cv.notify_one();
}

static Json g_auth;
static Json g_lastMsg;
static Json g_state;
static std::vector<Json> g_msgs;

static void pump(App& app, int ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    auto nextTick = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < end) {
        std::deque<std::function<void()>> q;
        {
            std::unique_lock<std::mutex> l(g_mu);
            g_cv.wait_for(l, std::chrono::milliseconds(20), [] { return !g_q.empty(); });
            q.swap(g_q);
        }
        for (auto& f : q) f();
        if (std::chrono::steady_clock::now() >= nextTick) {
            app.tick();
            nextTick += std::chrono::milliseconds(50);
        }
    }
}

template <class Pred>
static bool pumpUntil(App& app, int timeoutMs, Pred done) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        pump(app, 100);
        if (done()) return true;
    }
    return false;
}

static void ui(App& app, Json m) { app.onUiMessage(m.dump()); }

static void printMsg(const Json& m) {
    std::printf("  [%s] status=%s verdict=%s phase=%s round=%lld\n", m["author"].str().c_str(), m["status"].str().c_str(),
                m["verdict"].str().c_str(), m["phase"].str().c_str(), (long long)m["round"].i64());
    for (const auto& b : m["blocks"].items()) {
        std::string t = b["type"].str();
        if (t == "text") std::printf("    text: %s\n", util::truncateUtf8(util::replaceAll(b["text"].str(), "\n", " / "), 300).c_str());
        else if (t == "tool") {
            std::printf("    tool: %s %s %s [%s] exit=%lld\n", b["title"].str().c_str(), b["command"].str().c_str(), b["path"].str().c_str(), b["status"].str().c_str(), (long long)b["exitCode"].i64(-1));
            if (b["status"].str() != "ok" && !b["output"].str().empty()) std::printf("      output: %s\n", util::truncateUtf8(util::replaceAll(b["output"].str(), "\n", " / "), 600).c_str());
        }
        else if (t == "file") std::printf("    file: %s %s +%lld -%lld\n", b["change"].str().c_str(), b["path"].str().c_str(), (long long)b["added"].i64(), (long long)b["removed"].i64());
        else if (t == "approval") std::printf("    approval: %s | %s %s [%s]\n", b["title"].str().c_str(), b["command"].str().c_str(), b["path"].str().c_str(), b["state"].str().c_str());
        else std::printf("    %s: %s\n", t.c_str(), util::truncateUtf8(b.dump(), 200).c_str());
    }
    if (m["usage"].isObject()) std::printf("    usage: %s\n", m["usage"].dump().c_str());
    if (m["author"].str() == "system") std::printf("    system: %s %s\n", m["kind"].str().c_str(), m["text"].str().c_str());
}

static const Json* lastFrom(const std::string& convId, const std::string& author) {
    for (auto it = g_msgs.rbegin(); it != g_msgs.rend(); ++it) {
        if ((*it)["convId"].str() == convId && (*it)["msg"]["author"].str() == author) return &(*it)["msg"];
    }
    return nullptr;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::string mode = argc > 1 ? argv[1] : "codex";
    bool fakeClaude = false;
    std::string localModelArg;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--fake-claude") fakeClaude = true;
        else if (a == "--local" && i + 1 < argc) localModelArg = argv[++i];
    }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring root = util::joinPath(tmp, util::widen("cca-live-" + util::newId()));
    std::wstring data = util::joinPath(root, L"data");
    std::wstring proj = util::joinPath(root, L"proj");
    util::ensureDir(data);
    util::ensureDir(proj);
    SetEnvironmentVariableW(L"CCA_DATA_DIR", data.c_str());
    std::printf("project folder: %s\n", util::narrow(proj).c_str());
    if (fakeClaude) {
        // tests\fake_claude.cmd sits next to build\ in the source tree.
        std::wstring fake = util::joinPath(util::exeDir(), L"..\\tests\\fake_claude.cmd");
        util::writeFileAtomic(util::joinPath(data, L"settings.json"), J({{"claudePath", util::narrow(fake)}}).dump());
        std::printf("using fake claude: %s\n", util::narrow(fake).c_str());
    }

    App app([](const std::string& json) {
        Json m = Json::parse(json);
        std::string t = m["type"].str();
        if (t == "auth") g_auth = m;
        else if (t == "msg") {
            g_msgs.push_back(m);
            g_lastMsg = m["msg"];
        } else if (t == "convState") g_state = m["state"];
        else if (t == "toast") std::printf("  toast[%s]: %s\n", m["level"].str().c_str(), m["text"].str().c_str());
        else if (t == "models") std::printf("  models: claude=%zu codex=%zu\n", m["models"]["claude"].size(), m["models"]["codex"].size());
        else if (t == "local") {
            std::printf("  local: running=%d installedOllama=%d error=%s\n  hardware=%s\n", m["running"].boolean(),
                        m["installedOllama"].boolean(), m["error"].str().c_str(), m["hardware"].dump().c_str());
            for (const auto& r : m["recommended"].items())
                std::printf("    %s %s %.1fGB fit=%s%s\n", r["recommended"].boolean() ? "*" : " ", r["model"].str().c_str(),
                            r["sizeGb"].num(), r["fit"].str().c_str(), "");
            std::printf("  installed models: %s\n", m["models"].dump().c_str());
        }
    });
    app.windowAction = [](const std::string&) {};

    ui(app, J({{"type", "ready"}}));
    auto settled = [](const Json& a) { std::string s = a["state"].str(); return !s.empty() && s != "checking"; };
    bool authed = pumpUntil(app, 60000, [&] {
        if (mode == "codex") return settled(g_auth["codex"]);
        if (mode == "claude") return settled(g_auth["claude"]);
        return settled(g_auth["claude"]) && settled(g_auth["codex"]);
    });
    std::printf("auth: claude=%s (%s) codex=%s (%s)\n", g_auth["claude"]["state"].str().c_str(), g_auth["claude"]["planLabel"].str().c_str(),
                g_auth["codex"]["state"].str().c_str(), g_auth["codex"]["planLabel"].str().c_str());
    if (!authed) { std::printf("FAIL auth timeout\n"); return 1; }

    if (mode == "localreview") {
        // One read-only review through LocalSession with the file tools and the citation check,
        // on a throwaway project with a planted bug. Uses only the local model, no subscription.
        std::string model = localModelArg.empty() ? "llama3.1:8b" : localModelArg;
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        std::wstring root = util::joinPath(tmp, L"cca-localreview-" + util::widen(util::randomHex(4)));
        util::ensureDir(util::joinPath(root, L"src"));
        util::writeFileAtomic(util::joinPath(root, L"src\\discount.js"),
                              "// Applies a discount code to a cart total.\n"
                              "function applyDiscount(total, code) {\n"
                              "  if (code.type === 'percent') {\n"
                              "    return total - total * code.value / 100;\n"
                              "  }\n"
                              "  if (code.type === 'fixed') {\n"
                              "    return total - code.value;\n"
                              "  }\n"
                              "  return total;\n"
                              "}\n\n"
                              "module.exports = { applyDiscount };\n");
        std::string text, status, toolLog;
        Json readPaths = Json::array();
        bool done = false;
        auto session = std::make_shared<LocalSession>(AgentKind::Local1, [&](const AgentEvent& ev) {
            if (ev.kind == AgentEvent::Kind::ToolStart)
                toolLog += "  tool " + ev.data["tool"].str() + " " + ev.data["detail"].str() + "\n";
            if (ev.kind == AgentEvent::Kind::Notice) toolLog += "  notice " + ev.data["text"].str() + "\n";
            if (ev.kind == AgentEvent::Kind::TurnDone) {
                status = ev.data["status"].str() + " " + ev.data["error"].str();
                text = ev.data["text"].str();
                readPaths = ev.data["readPaths"];
                done = true;
            }
        });
        GuardRules rules;
        rules.personaName = "本地甲";
        rules.address = "指揮官";
        LocalReviewSetup setup;
        setup.review = true;
        setup.root = root;
        setup.changedPaths = {"src\\discount.js"};
        setup.rules = rules;
        setup.think = localModelArg.find("qwen3") != std::string::npos;
        session->configure(
            model,
            "你現在扮演「本地甲」，稱呼使用者為「指揮官」。你是唯讀審查員，只能用工具讀檔並提出建議。"
            "每條建議以「發現 1：」開頭，寫「位置：檔案:行號」和「原文：」（從 read_file 結果逐字複製），"
            "再寫問題、建議、信心。用台灣繁體中文回答。");
        session->setReview(setup);
        ui(app, J({{"type", "local.status"}}));  // starts Ollama when it is not running
        pump(app, 20000);
        std::string err;
        session->send(guardRuleCard(rules) +
                          "\n【CCA 唯讀審查】Claude 新增了 src\\discount.js（折扣碼）。請用 read_file 讀這個檔案，"
                          "檢查固定金額和百分比折扣的邊界情況，照格式回報。",
                      Json::array(), &err);
        pumpUntil(app, 600000, [&] { return done; });
        std::set<std::string> read;
        for (const auto& p : readPaths.items()) read.insert(p.str());
        LocalReviewCheck check = checkLocalReview(root, text, read, setup.changedPaths, rules);
        int verified = 0;
        for (const auto& f : check.findings) verified += f.verified ? 1 : 0;
        std::printf(
            "model=%s status=%s\n%s--- final ---\n%s\n--- check: findings=%d verified=%d removed=%d issues=%s\n",
            model.c_str(), status.c_str(), toolLog.c_str(), text.c_str(), (int)check.findings.size(), verified,
            check.removed, guardSummary(check.issues).c_str());
        app.shutdown();
        return status.rfind("completed", 0) == 0 && !read.empty() ? 0 : 1;
    }
    if (mode == "ollama") {
        // Hardware, recommendations and Ollama status, as the settings page gets them.
        Json local;
        App* appPtr = &app;
        (void)appPtr;
        ui(app, J({{"type", "local.status"}}));
        // The App's send callback above only records a few types; poll the raw output instead.
        std::printf("(see local status below)\n");
        pump(app, 60000);
        app.shutdown();
        return 0;
    }
    if (mode == "persona") {
        // Daily mode + persona + custom names, checked through what the (fake) CLI was started with.
        ui(app, J({{"type", "profile.set"}, {"who", "claude"}, {"name", "撰寫測試員"}}));
        ui(app, J({{"type", "profile.set"}, {"who", "codex"}, {"name", "審查測試員"}}));
        ui(app, J({{"type", "persona.save"},
                   {"assignTo", "claude"},
                   {"persona", J({{"id", "test-writer"},
                                  {"name", "撰寫測試員"},
                                  {"agent", "claude"},
                                  {"prompt", "以簡潔的繁體中文說明。"}})}}));
        ui(app, J({{"type", "conv.settings"}, {"id", "dm-claude"}, {"settings", J({{"daily", true}})}}));
        pump(app, 300);
        size_t before = g_msgs.size();
        ui(app, J({{"type", "conv.send"}, {"id", "dm-claude"}, {"text", "回報設定"}}));
        bool started = false;
        pumpUntil(app, 60000, [&] {
            if (g_state["id"].str() == "dm-claude" && g_state["running"].boolean()) started = true;
            return started && !g_state["running"].boolean();
        });
        std::string reply;
        for (size_t i = before; i < g_msgs.size(); ++i) {
            if (g_msgs[i]["msg"]["author"].str() == "claude") {
                reply.clear();
                for (const auto& b : g_msgs[i]["msg"]["blocks"].items()) if (b["type"].str() == "text") reply += b["text"].str();
            }
        }
        std::printf("%s\n", reply.c_str());
        int bad = 0;
        auto expect = [&](const char* what, bool ok) { std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what); bad += !ok; };
        expect("daily mode uses haiku", reply.find("model=haiku") != std::string::npos);
        expect("daily mode asks before edits", reply.find("permission=default") != std::string::npos);
        expect("daily chats run without a project folder", reply.find("CCA\\daily") != std::string::npos);
        expect("custom persona is in the system prompt", reply.find("# 角色設定：撰寫測試員") != std::string::npos);
        expect("persona rules are attached", reply.find("扮演規則") != std::string::npos);
        expect("daily note is attached", reply.find("# 日常模式") != std::string::npos);
        app.shutdown();
        std::printf("\n%s\n", bad ? "LIVE TEST FAILED" : "LIVE TEST OK");
        return bad ? 1 : 0;
    }
    std::string convId = mode == "codex" ? "dm-codex" : mode == "claude" ? "dm-claude" : "";
    if (mode == "group") {
        if (!g_auth["ok"].boolean()) { std::printf("FAIL group needs both subscriptions\n"); return 1; }
        ui(app, J({{"type", "conv.create"}, {"name", "live"}, {"cwd", util::narrow(proj)}}));
        pump(app, 300);
        // The created group is the newest one; find it from saved meta.
        std::string text;
        util::readFile(util::joinPath(data, L"conversations.json"), text);
        Json list = Json::parse(text);
        for (const auto& c : list.items()) if (c["name"].str() == "live") convId = c["id"].str();
    }
    // Groups use "ask" so GPT's commands run after approval even where Codex's sandbox cannot start.
    Json settings = J({{"cwd", util::narrow(proj)}, {"permission", mode == "group" ? "ask" : "edit"}});
    if (mode != "claude") { settings["codexModel"] = "gpt-6-luna"; settings["codexEffort"] = "low"; }
    if (mode != "codex") { settings["claudeModel"] = "haiku"; }
    if (mode == "group") settings["maxRounds"] = 4;
    if (mode == "group" && !localModelArg.empty()) {
        // Invite local reviewer 1 (本地甲) with the given Ollama model.
        ui(app, J({{"type", "local.set"}, {"slot", "local1"}, {"model", localModelArg}, {"enabled", true}}));
        settings["localMembers"] = Json(Json::Array{Json("local1")});
        std::printf("local reviewer: %s\n", localModelArg.c_str());
    }
    ui(app, J({{"type", "conv.settings"}, {"id", convId}, {"settings", settings}}));
    pump(app, 200);

    auto runTurn = [&](const std::string& text, int timeoutMs, std::function<void(const Json&)> onApproval) {
        size_t before = g_msgs.size();
        ui(app, J({{"type", "conv.send"}, {"id", convId}, {"text", text}}));
        bool started = false;
        bool ok = pumpUntil(app, timeoutMs, [&] {
            if (g_state["id"].str() == convId && g_state["running"].boolean()) started = true;
            for (size_t i = before; i < g_msgs.size(); ++i) {
                for (const auto& b : g_msgs[i]["msg"]["blocks"].items()) {
                    if ((b["type"].str() == "approval") && b["state"].str() == "pending" && onApproval) {
                        Json card = b;
                        card["msgId"] = g_msgs[i]["msg"]["id"];
                        onApproval(card);
                    }
                }
            }
            return started && g_state["id"].str() == convId && !g_state["running"].boolean();
        });
        return ok;
    };

    int failures = 0;
    if (mode == "codex" || mode == "claude") {
        std::string who = mode == "codex" ? "codex" : "claude";
        std::printf("\n== turn 1: create a file (permission: edit)\n");
        bool ok = runTurn("在目前資料夾建立 hello.txt，內容只有一行：hi。完成後用一句話回覆。", 240000, nullptr);
        if (const Json* m = lastFrom(convId, who)) printMsg(*m);
        std::string content;
        bool exists = util::readFile(util::joinPath(proj, L"hello.txt"), content);
        std::printf("  hello.txt exists=%d content=%s\n", exists, util::trim(content).c_str());
        if (!ok || !exists) { std::printf("FAIL turn 1\n"); ++failures; }

        std::printf("\n== turn 2: edit with permission ask (expect an approval card)\n");
        ui(app, J({{"type", "conv.settings"}, {"id", convId}, {"settings", J({{"permission", "ask"}})}}));
        pump(app, 200);
        std::set<std::string> answered;
        bool sawApproval = false;
        ok = runTurn("把 hello.txt 的內容改成 hello world。完成後用一句話回覆。", 240000, [&](const Json& card) {
            if (answered.count(card["id"].str())) return;
            answered.insert(card["id"].str());
            sawApproval = true;
            std::printf("  -> approving: %s\n", card["title"].str().c_str());
            ui(app, J({{"type", "approval"}, {"convId", convId}, {"msgId", card["msgId"]}, {"blockId", card["id"]}, {"choice", "allow"}}));
        });
        if (const Json* m = lastFrom(convId, who)) printMsg(*m);
        util::readFile(util::joinPath(proj, L"hello.txt"), content);
        std::printf("  hello.txt now: %s  (approval seen: %d)\n", util::trim(content).c_str(), sawApproval);
        if (!ok || util::trim(content) != "hello world") { std::printf("FAIL turn 2\n"); ++failures; }
        if (!sawApproval) std::printf("NOTE no approval card was shown in ask mode\n");
    } else {
        std::printf("\n== group: write -> review -> recheck\n");
        std::set<std::string> answered;
        bool ok = runTurn("寫一個 Python 函式 is_prime(n) 放在 prime.py，並附上 3 個 assert 測試放在 test_prime.py。保持簡短。", 900000,
                          [&](const Json& card) {
                              if (answered.count(card["id"].str())) return;
                              answered.insert(card["id"].str());
                              std::printf("  -> approving (%s): %s %s\n", card["agent"].str().c_str(), card["title"].str().c_str(),
                                          util::truncateUtf8(card["command"].str(), 120).c_str());
                              ui(app, J({{"type", "approval"}, {"convId", convId}, {"msgId", card["msgId"]},
                                         {"blockId", card["id"]}, {"choice", "allow"}}));
                          });
        // Print the final version of every message in order.
        std::vector<std::string> order;
        for (const auto& m : g_msgs) {
            if (m["convId"].str() != convId) continue;
            std::string id = m["msg"]["id"].str();
            if (std::find(order.begin(), order.end(), id) == order.end()) order.push_back(id);
        }
        for (const auto& id : order) {
            for (auto it = g_msgs.rbegin(); it != g_msgs.rend(); ++it) {
                if ((*it)["msg"]["id"].str() == id) { printMsg((*it)["msg"]); break; }
            }
        }
        if (!ok) { std::printf("FAIL group did not finish\n"); ++failures; }
    }
    app.shutdown();
    std::printf("\n%s\n", failures ? "LIVE TEST FAILED" : "LIVE TEST OK");
    return failures ? 1 : 0;
}
