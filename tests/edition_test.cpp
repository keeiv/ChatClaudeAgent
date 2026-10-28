// Editions and CCA plugins in isolated data: updater choices with real executables, the ordinary-edition vault in
// both directions, and plugin validation, pinning, themes, modes and commands.
#include <windows.h>

#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>

#include "app.hpp"
#include "extensions.hpp"
#include "themes.hpp"
#include "updater.hpp"
#include "util.hpp"

namespace {
std::mutex uiMutex;
std::deque<std::function<void()>> uiQueue;
int checks = 0, failures = 0;
void check(bool ok, const char* name) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", name);
    }
}
std::wstring exeDir() {
    wchar_t path[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, path, MAX_PATH * 4);
    std::wstring dir = path;
    return dir.substr(0, dir.find_last_of(L"\\/"));
}
bool copy(const std::wstring& from, const std::wstring& to) {
    return CopyFileW(from.c_str(), to.c_str(), FALSE) != 0;
}
const Json* editionChoice(const Json& result, const std::string& edition) {
    for (const auto& c : result["choices"].items())
        if (c["edition"].str() == edition) return &c;
    return nullptr;
}
std::string plugin(const std::string& id, const Json& extra) {
    Json m = J({{"schema", 1}, {"id", id}, {"name", "測試 " + id}, {"version", "1.0.0"}, {"author", "CCA"}});
    for (const auto& [k, v] : extra.fields()) m[k] = v;
    return m.dump();
}
Json palette(const char* text, const char* chat) {
    return J({{"rail", "#050405"},
              {"side", "#0a0809"},
              {"chat", chat},
              {"panel", "#0b0809"},
              {"input", "#181112"},
              {"hover", "#1f1517"},
              {"selected", "#2f1115"},
              {"line", "#3a2327"},
              {"text", text},
              {"muted", "#ad9d9f"},
              {"signal", "#ff8a3d"},
              {"brand", "#d0602a"},
              {"accents", Json(Json::Array{"#ff8a3d"})}});
}
}  // namespace

void postToUi(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(uiMutex);
    uiQueue.push_back(std::move(fn));
}

struct EditionTestAccess {


    static void plugins(const std::wstring& root) {
        const auto data = util::joinPath(root, L"plugin-data");
        util::ensureDir(data);
        SetEnvironmentVariableW(L"CCA_DATA_DIR", data.c_str());
        const auto dir = extensions::folder();
        auto write = [&](const std::wstring& folder, const std::string& text) {
            util::ensureDir(util::joinPath(dir, folder));
            util::writeFileAtomic(util::joinPath(util::joinPath(dir, folder), L"plugin.json"), text);
        };
        Json theme = J({{"id", "ember"},
                        {"name", "餘燼"},
                        {"scheme", "dark"},
                        {"shape", "slash"},
                        {"base", "hunt"},
                        {"palette", palette("#ece4e4", "#0f0b0c")}});
        Json mode = J({{"id", "strict"},
                       {"name", "嚴格審查"},
                       {"scope", "group"},
                       {"settings", J({{"permission", "ask"}, {"maxRounds", 9}})},
                       {"instructions", "MODE_RULE：每項修改附上重現步驟。"}});
        Json command =
            J({{"name", "sec"}, {"description", "安全審查"}, {"template", "SEC: {{input}}"}, {"scope", "any"}});
        Json reserved = J({{"name", "help"}, {"description", "x"}, {"template", "shadow"}});
        const std::string good = plugin("night-shift", J({{"themes", Json(Json::Array{theme})},
                                                          {"modes", Json(Json::Array{mode})},
                                                          {"commands", Json(Json::Array{command, reserved})}}));
        write(L"night-shift", good);
        Json bypass = mode;
        bypass["settings"]["permission"] = "bypass";
        Json dim = theme;
        dim["palette"] = palette("#222222", "#1a1a1a");
        Json script = theme;
        script["id"] = "script";
        script["palette"]["display"] = "x; background: url(http://example.invalid)";
        write(L"bad",
              plugin("bad", J({{"modes", Json(Json::Array{bypass})}, {"themes", Json(Json::Array{dim, script})}})));
        write(L"not-a-plugin-dir", "{");
        App app([](const std::string&) {});
        const Json* nightShift = nullptr;
        const Json* bad = nullptr;
        for (const auto& p : app.extensions_.items()) {
            if (p["id"].str() == "night-shift") nightShift = &p;
            if (p["id"].str() == "bad") bad = &p;
        }
        check(nightShift && (*nightShift)["errors"].empty() && !(*nightShift)["enabled"].boolean(),
              "a valid plugin is found and starts disabled");
        std::string reasons = bad ? (*bad)["errors"].dump() : "";
        check(bad && reasons.find("bypass") != std::string::npos && reasons.find("4.5") != std::string::npos &&
                  reasons.find("字型") != std::string::npos,
              "bypass permission, unreadable colors and CSS in font stacks are rejected");
        check(app.extensions_.size() == 3, "a broken plugin.json is listed with its error, not skipped");
        const std::string hash = nightShift ? (*nightShift)["hash"].str() : "";
        app.onUiMessage(
            J({{"type", "extensions.enable"}, {"id", "night-shift"}, {"hash", std::string(64, '0')}}).dump());
        check(!app.settings_["extensions"]["enabled"].has("night-shift"), "enabling needs the hash the user reviewed");
        app.onUiMessage(
            J({{"type", "extensions.enable"}, {"id", "bad"}, {"hash", bad ? (*bad)["hash"].str() : ""}}).dump());
        check(!app.settings_["extensions"]["enabled"].has("bad"), "a plugin with errors cannot be enabled");
        app.onUiMessage(J({{"type", "extensions.enable"}, {"id", "night-shift"}, {"hash", hash}}).dump());
        check(app.settings_["extensions"]["enabled"]["night-shift"].str() == hash, "enabling pins the reviewed file");
        Json ember = themeById("p.night-shift.ember");
        check(!ember.isNull() && ember["tokens"]["signal"].str() == "#ff8a3d" && ember["base"].str() == "hunt" &&
                  ember["tokens"]["signal-rgb"].str() == "255, 138, 61",
              "an enabled plugin theme is built from CCA's tokens and its palette");
        app.onUiMessage(J({{"type", "theme.set"}, {"theme", "p.night-shift.ember"}, {"requestToken", "t"}}).dump());
        check(app.settings_["theme"].str() == "p.night-shift.ember", "a plugin theme can be applied");
        Conversation* group = nullptr;
        for (auto& c : app.convs_)
            if (c->kind == "group") group = c.get();
        Conversation* dm = app.find("dm-claude");
        app.onUiMessage(J({{"type", "extensions.mode"}, {"id", dm->id}, {"mode", "night-shift/strict"}}).dump());
        check(dm->mode.empty(), "a group-only mode is refused in a direct message");
        app.onUiMessage(J({{"type", "extensions.mode"}, {"id", group->id}, {"mode", "night-shift/strict"}}).dump());
        check(group->mode == "night-shift/strict" && group->permission == "ask" && group->maxRounds == 9,
              "a mode applies its allowlisted settings");
        check(app.configFor(*group, AgentKind::Claude, false).instructions.find("MODE_RULE") != std::string::npos,
              "the mode's instructions reach the agent with their source named");
        std::string text = "/sec 登入流程";
        app.expandExtensionCommand(*group, text);
        check(text == "SEC: 登入流程", "a plugin command expands its template");
        std::string shadow = "/help me";
        app.expandExtensionCommand(*group, shadow);
        check(shadow == "/help me", "a plugin cannot take over a built-in command name");
        std::string path = "/usr/local/bin 是什麼";
        app.expandExtensionCommand(*group, path);
        check(path == "/usr/local/bin 是什麼", "ordinary text that starts with a slash is untouched");
        // Editing plugin.json after approval turns the plugin off until it is reviewed again.
        write(L"night-shift", good + " ");
        app.loadExtensions();
        bool changed = false;
        for (const auto& p : app.extensions_.items())
            if (p["id"].str() == "night-shift") changed = p["changed"].boolean() && !p["enabled"].boolean();
        check(changed && themeById("p.night-shift.ember").isNull(),
              "a changed plugin is disabled and its theme removed");
        check(app.extensionModeInstructions(*group).empty(), "a disabled plugin's mode stops adding instructions");
        app.shutdown();
        App restarted([](const std::string&) {});
        check(restarted.settings_["theme"].str() == "p.night-shift.ember",
              "the plugin theme choice survives a restart");
        restarted.shutdown();
    }
};

int main() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const auto root = util::joinPath(tmp, util::widen("cca-edition-" + util::newId()));
    util::ensureDir(root);
    SetEnvironmentVariableW(L"CCA_DATA_DIR", util::joinPath(root, L"updater-data").c_str());
    EditionTestAccess::plugins(root);
    std::printf("%d edition checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
