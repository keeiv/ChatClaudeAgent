// Native routing, real fixture transport, theme persistence and terminal palette checks.
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <vector>

#include "app.hpp"
#include "cli_render.hpp"
#include "routing.hpp"
#include "themes.hpp"
#include "util.hpp"
#include "version.hpp"

namespace {
std::mutex uiMutex;
std::vector<std::function<void()>> jobs;
int checks = 0, failures = 0;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}
void pump() {
    std::vector<std::function<void()>> pending;
    {
        std::lock_guard<std::mutex> lock(uiMutex);
        pending.swap(jobs);
    }
    for (auto& job : pending) job();
}
Json models() {
    Json out = Json::array();
    for (const auto family : {"haiku", "sonnet", "opus"}) {
        out.push(J({{"value", std::string("claude-") + family + "-test"},
                    {"displayName", family},
                    {"supportedEffortLevels", Json(Json::Array{"low", "medium", "high"})}}));
    }
    return out;
}
int fakeCodex() {
    auto out = [](const Json& message) { std::cout << message.dump() << std::endl; };
    std::string line;
    while (std::getline(std::cin, line)) {
        const auto message = Json::parse(line);
        if (!message.has("id")) continue;
        const auto method = message["method"].str();
        Json result = Json::object();
        if (method == "thread/start" || method == "thread/resume")
            result = J({{"thread", J({{"id", "appearance-codex"}})}});
        if (method == "turn/start") result = J({{"turn", J({{"id", "fixture-turn"}})}});
        out(J({{"id", message["id"]}, {"result", result}}));
        if (method != "turn/start") continue;
        const auto answer =
            "model=" + message["params"]["model"].str() + " effort=" + message["params"]["effort"].str();
        out(J({{"method", "item/completed"},
               {"params", J({{"threadId", "appearance-codex"},
                             {"item", J({{"type", "agentMessage"}, {"id", "answer"}, {"text", answer}})}})}}));
        out(J({{"method", "turn/completed"},
               {"params", J({{"threadId", "appearance-codex"},
                             {"turn", J({{"id", "fixture-turn"}, {"status", "completed"}})}})}}));
    }
    return 0;
}
}  // namespace

void postToUi(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(uiMutex);
    jobs.push_back(std::move(fn));
}

struct AppearanceTestAccess {
    static void run(const std::wstring& root) {
        Json response;
        auto path = util::getEnv(L"PATH");
        SetEnvironmentVariableW(L"PATH", L"");
        App app([&](const std::string& raw) { response = Json::parse(raw); });
        SetEnvironmentVariableW(L"PATH", path.c_str());
        app.uiReady_ = true;
        app.settings_["guard"]["enabled"] = false;
        app.settings_["sandboxShare"] = false;
        auto c = app.find("dm-claude");
        c->cwd = util::joinPath(root, L"project");
        util::ensureDir(c->cwd);
        const auto linkFile = util::joinPath(c->cwd, L"link-test.exe");
        util::writeFileAtomic(linkFile, "fixture, never executable");
        std::wstring revealed;
        app.openExternal = [&](const std::wstring& folder) { revealed = folder; };
        app.onUiMessage(J({{"type", "file.reveal"}, {"target", util::narrow(linkFile)}}).dump());
        check(revealed == c->cwd, "executable link reveals containing folder only");
        revealed.clear();
        for (const auto target :
             {"https://example.test/a.exe", "\\\\server\\share\\a.exe", "C:\\a.exe:stream", "C:\\missing-file.exe"}) {
            app.onUiMessage(J({{"type", "file.reveal"}, {"target", target}}).dump());
            check(revealed.empty(), "invalid or missing file link has no shell action");
        }
        app.claudeModels_ = models();
        auto act = [&](Json message) {
            message["id"] = c->id;
            message["agent"] = "claude";
            app.onUiMessage(message.dump());
            return response["ok"].boolean();
        };
        check(c->adaptive["claude"]["model"].boolean(), "fresh conversation enables automatic model selection");
        check(act(J({{"type", "theme.set"}, {"theme", "claude"}})), "save theme");
        check(app.settings_["theme"].str() == "claude", "theme persisted in settings");
        check(!act(J({{"type", "theme.set"}, {"theme", "../../unknown"}})) && app.settings_["theme"].str() == "claude",
              "reject untrusted theme ID");
        HANDLE locked = CreateFileW(util::joinPath(util::appDataDir(), L"settings.json").c_str(), GENERIC_READ, 0,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        check(locked != INVALID_HANDLE_VALUE, "lock persistence fixture");
        check(!act(J({{"type", "theme.set"}, {"theme", "gemini"}})) && app.settings_["theme"].str() == "claude",
              "failed theme save restores memory");
        CloseHandle(locked);
        const auto previous = c->adaptive;
        locked = CreateFileW(util::joinPath(util::appDataDir(), L"conversations.json").c_str(), GENERIC_READ, 0,
                             nullptr, OPEN_EXISTING, 0, nullptr);
        check(!act(J({{"type", "routing.set"}, {"model", false}})) && c->adaptive == previous,
              "failed routing save preserves preferences");
        CloseHandle(locked);
        c->preparing = "fixture";
        check(!act(J({{"type", "routing.set"}, {"model", false}})) && c->adaptive == previous,
              "cannot change routing during preparation");
        c->preparing.clear();
        check(act(J({{"type", "routing.manual"},
                     {"settings", J({{"claudeModel", "claude-sonnet-test"}, {"claudeEffort", "medium"}})}})),
              "manual selection acknowledged");
        check(!c->adaptive["claude"]["model"].boolean() && !c->adaptive["claude"]["effort"].boolean(),
              "explicit manual controls pin both dimensions");
        check(act(J({{"type", "routing.set"}, {"model", true}, {"effort", true}})), "re-enable automatic controls");
        AgentConfig config = app.configFor(*c, AgentKind::Claude, false);
        app.routeConfig(*c, AgentKind::Claude, config, "你好");
        check(config.model == "claude-haiku-test" && config.effort == "low", "routing applied to transport config");
        check(c->claudeModel == "claude-sonnet-test" && c->claudeEffort == "medium", "saved manual fallback retained");
        app.routeConfig(*c, AgentKind::Claude, config, "修復並發造成的資料遺失", true, true);
        check(config.model == "claude-haiku-test" && config.effort == "low", "fixed pipeline fields remain fixed");
        auto exe =
            util::joinPath(std::filesystem::path(util::exeDir()).parent_path().wstring(), L"tests\\fake_claude.cmd");
        SetEnvironmentVariableW(L"CCA_TEST_ADAPTIVE", L"1");
        app.claudeStatus_.state = "ok";
        app.claudeStatus_.exe = exe;
        app.settings_["claudePath"] = util::narrow(exe);
        auto run = [&](const std::string& request, const std::string& expected) {
            size_t before = c->messages.size();
            app.onUiMessage(J({{"type", "conv.send"}, {"id", c->id}, {"text", request}}).dump());
            const auto deadline = GetTickCount64() + 12000;
            while (GetTickCount64() < deadline) {
                pump();
                app.tick();
                if (!app.anyBusy(*c) && c->messages.size() > before + 1) break;
                Sleep(10);
            }
            bool received = false, receipt = false;
            for (size_t i = before; i < c->messages.size(); ++i) {
                const auto& message = c->messages[i];
                for (const auto& block : message["blocks"].items())
                    received |= block["text"].str().find(expected) != std::string::npos;
                receipt |= message["routing"]["model"].str() == expected.substr(6, expected.find(" effort=") - 6);
            }
            check(received, "real fixture received selected --model and --effort");
            check(receipt, "assistant message carries the same selection receipt");
        };
        run("回報設定", "model=claude-haiku-test effort=low");
        run("回報設定，深入分析並修復並發造成的資料遺失", "model=claude-opus-test effort=high");
        run("繼續", "model=claude-opus-test effort=high");
        check(c->desk["contexts"].items().back()["routing"]["inherited"].boolean(),
              "real followup transport retains prior request profile");
        check(c->desk["contexts"].items().back()["routing"]["model"].str() == "claude-opus-test",
              "context source records actual automatic decision");
        check(act(J({{"type", "routing.manual"},
                     {"settings", J({{"claudeModel", "claude-sonnet-test"}, {"claudeEffort", "medium"}})}})),
              "pin after automatic turns");
        run("回報設定", "model=claude-sonnet-test effort=medium");
        c = app.find("dm-codex");
        c->cwd = util::joinPath(root, L"project");
        app.codexStatus_.state = "ok";
        app.codexStatus_.exe = util::joinPath(util::exeDir(), L"appearance_test.exe");
        app.settings_["codexPath"] = util::narrow(app.codexStatus_.exe);
        app.codexModels_ = Json::array();
        for (const auto suffix : {"luna", "sol", "astra"})
            app.codexModels_.push(J({{"value", std::string("gpt-6-") + suffix},
                                     {"supportedEffortLevels", Json(Json::Array{"low", "medium", "high"})}}));
        run("回報設定", "model=gpt-6-luna effort=low");
        run("回報設定並檢查資料庫遷移的併發安全", "model=gpt-6-astra effort=high");
        app.shutdown();
        pump();
        SetEnvironmentVariableW(L"CCA_TEST_ADAPTIVE", nullptr);
        App reload([](const std::string&) {});
        check(reload.settings_["theme"].str() == "claude", "theme survives restart");
        check(!reload.find("dm-claude")->adaptive["claude"]["model"].boolean(), "manual preference survives restart");
    }
};

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "app-server") return fakeCodex();
    check(std::string(CCA_VERSION) == "1.0.2-oss", "the ordinary line continues with 1.0.2-oss");
    check(themeCatalogue().size() >= 16, "the catalogue holds the sixteen ordinary themes");
    for (const auto& theme : themeCatalogue().items()) {
        check(!themeById(theme["id"].str()).isNull(), "catalogue theme roundtrip");
        check(themeColor(theme, "text") != 0 && themeColor(theme, "brand") != 0, "native theme colors parse");
    }
    check(analyzeTask("你好")["level"].i64() == 0, "short query is lightweight");
    check(analyzeTask("修復一個函式")["level"].i64() == 1, "routine implementation");
    check(analyzeTask("檢查整個專案的架構")["level"].i64() == 2, "broad architecture request");
    check(analyzeTask("修復並發造成的資料遺失")["level"].i64() == 2, "data integrity request");
    check(analyzeTask("什麼是資料庫遷移")["level"].i64() == 0, "short definition does not force a deep model");
    check(analyzeTask(std::string(6500, 'a'))["level"].i64() == 1, "length alone does not imply a hard task");
    const std::vector<std::pair<std::string, int>> taskCases{
        {"修改 README 裡的錯字", 0},
        {"不要重構，只修改這句話的標點", 0},
        {"請把文案中的「資料遺失」改為「內容已清除」", 0},
        {"What is authentication?", 0},
        {"什麼是演算法複雜度", 0},
        {"Please proofread this paragraph", 0},
        {"新增一個設定選項", 1},
        {"修復一個函式", 1},
        {"implement a small button", 1},
        {"這個幫我弄一下", 1},
        {"繼續", 1},
        {"x" + std::string(10000, 'x'), 1},
        {"掃描整個專案", 2},
        {"找出兩個人資料被清零的原因", 2},
        {"修復正式環境的間歇崩潰", 2},
        {"Review all services for race condition and transaction consistency", 2},
        {"證明此演算法在所有輸入下都正確", 2},
        {"請比較互斥鎖與無鎖結構的取捨並證明", 2},
        {"請修改錯字\n> security audit concurrency database migration", 0},
        {"只修改文案\n~~~\nrefactor architecture SQL injection\n~~~", 0},
        {"更新 a.cpp b.cpp c.cpp，新增測試但保留原 API", 1}};
    for (const auto& [request, level] : taskCases) {
        const auto profile = analyzeTask(request);
        check(profile["level"].i64() == level, ("profile: " + request).c_str());
        check(profile["analysisVersion"].i64() == 2 && !profile["factors"].empty(), "profile exposes evidence factors");
    }
    check(analyzeTask("繼續", "修復並發造成的資料遺失")["level"].i64() == 2, "followup retains prior risk");
    check(analyzeTask("繼續", "修改錯字")["level"].i64() == 0, "followup does not inflate lightweight work");
    check(analyzeTask("修改錯字", "修復資料遺失")["level"].i64() == 0, "new request does not inherit unrelated risk");
    check(analyzeTask("全專案重構，正式環境，保留原 API，需跨平台與測試，證明併發資料一致性")["targetEffort"].str() ==
              "xhigh",
          "combined constraints select a higher supported effort target");
    const auto catalogue = models();
    for (const auto& request : {"你好", "修復一個函式", "修復並發造成的資料遺失"}) {
        auto result = chooseRoute(request, catalogue, "", "", true, true);
        const auto model = result["model"].str();
        bool exists = false;
        for (const auto& item : catalogue.items()) exists |= model == item["value"].str();
        check(exists, "automatic ID exists in backend catalogue");
        check(result["effort"].str() == (result["level"].i64() == 0   ? "low"
                                         : result["level"].i64() == 1 ? "medium"
                                                                      : "high"),
              "effort follows task level");
    }
    auto manual = chooseRoute("修復資料遺失", catalogue, "chosen-model", "medium", false, false);
    check(manual["model"].str() == "chosen-model" && manual["effort"].str() == "medium", "manual pins are respected");
    auto missing = chooseRoute("你好", Json::array(), "legacy-model", "", true, true);
    check(missing["model"].str() == "legacy-model" && missing["effort"].str().empty(),
          "missing catalogue never invents parameters");
    auto blocked = chooseRoute("你好", catalogue, "", "ultra", true, false);
    check(blocked["blocked"].boolean(), "incompatible manual effort blocks automatic candidate selection");
    Json generation =
        Json(Json::Array{J({{"value", "gpt-6-sol"}}), J({{"value", "gpt-6.1-sol"}}), J({{"value", "gpt-image-99"}})});
    check(chooseRoute("修復一個函式", generation, "", "", true, true)["model"].str() == "gpt-6.1-sol",
          "same family chooses newer numeric generation and excludes image models");
    tui::setTheme("claude");
    auto ansi = tui::ansi(tui::line("hello\x1b]52;ignored"));
    check(ansi.find("48;2;250;249;245") != std::string::npos, "light terminal includes paper background");
    check(ansi.find("\x1b]52") == std::string::npos, "themed terminal still strips control injection");
    tui::setTheme("cca");
    check(tui::color::kBrand == 0xE9EDF1, "original terminal palette can be restored");
    const auto rendered =
        tui::renderMessage(J({{"author", "claude"},
                              {"status", "done"},
                              {"routing", J({{"autoModel", true}, {"reason", "fixture selection reason"}})}}),
                           tui::Names{[](const std::string& who) { return who; }}, 100);
    bool routeVisible = false;
    for (const auto& line : rendered.lines)
        for (const auto& span : line) routeVisible |= span.text.find("fixture selection reason") != std::string::npos;
    check(routeVisible, "terminal displays the recorded selection reason");
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    auto root = util::joinPath(temp, util::widen("cca-appearance-" + util::newId()));
    util::ensureDir(root);
    SetEnvironmentVariableW(L"CCA_DATA_DIR", root.c_str());
    AppearanceTestAccess::run(root);
    std::printf("%d appearance/routing checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
