// CCA plugins are data, never code: a plugin.json may add color themes built on CCA's own devices, named presets
// of conversation settings with extra instructions, and prompt commands. Enabling pins the file's SHA-256.
#include "extensions.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <set>

#include "app.hpp"
#include "editions.hpp"
#include "themes.hpp"
#include "updater.hpp"
#include "util.hpp"

namespace {
constexpr size_t kMaxManifest = 64 * 1024;
constexpr int kMaxPlugins = 32;
const char* const kShapes[] = {"cut", "soft",     "paper",  "round",  "editor",  "sticker",
                               "sky", "tactical", "signal", "ribbon", "pumpkin", "slash"};
const char* const kPalette[] = {"rail",     "side", "chat", "panel", "input",  "hover",
                                "selected", "line", "text", "muted", "signal", "brand"};

bool slug(const std::string& s, size_t max) {
    if (s.size() < 2 || s.size() > max || s.front() == '-' || s.back() == '-') return false;
    return std::all_of(s.begin(), s.end(),
                       [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}
bool hexColor(const Json& v) {
    const auto s = v.str();
    return v.isString() && s.size() == 7 && s[0] == '#' &&
           std::all_of(s.begin() + 1, s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}
// Plain display text: bounded, no control characters except line breaks in long fields.
bool text(const Json& v, size_t max, bool lines = false) {
    if (v.isNull()) return true;  // optional fields may be left out; required ones are checked by the caller
    const std::string s = v.str();
    if (!v.isString() || s.size() > max) return false;
    return std::none_of(s.begin(), s.end(), [lines](unsigned char c) {
        return (c < 0x20 && !(lines && (c == '\n' || c == '\t'))) || c == 0x7f;
    });
}
// A font stack names installed fonts only: no url(), no semicolons, nothing that leaves the property value.
bool fontStack(const Json& v) {
    const std::string s = v.str();
    if (!v.isString() || s.empty() || s.size() > 160) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isalnum(c) || c == ' ' || c == ',' || c == '"' || c == '-' || c >= 0x80;
    });
}
std::string rgbOf(const std::string& hex) {
    auto part = [&](int at) { return std::to_string(std::stoi(hex.substr(at, 2), nullptr, 16)); };
    return part(1) + ", " + part(3) + ", " + part(5);
}
double luminance(const std::string& hex) {
    double sum = 0;
    const double weights[] = {.2126, .7152, .0722};
    for (int i = 0; i < 3; ++i) {
        double v = std::stoi(hex.substr(1 + i * 2, 2), nullptr, 16) / 255.0;
        v = v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4);
        sum += v * weights[i];
    }
    return sum;
}
template <typename Visit>
void eachFolder(const std::wstring& dir, Visit visit) {
    WIN32_FIND_DATAW entry{};
    HANDLE find =
        FindFirstFileExW(util::joinPath(dir, L"*").c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) return;
    int seen = 0;
    do {
        const std::wstring name = entry.cFileName;
        if (name == L"." || name == L".." || !(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            continue;
        visit(name);
    } while (++seen < kMaxPlugins * 4 && FindNextFileW(find, &entry));
    FindClose(find);
}
}  // namespace

namespace extensions {
std::wstring folder() {
    const auto dir = util::joinPath(util::appDataDir(), L"plugins");
    util::ensureDir(dir);
    return dir;
}
double contrast(const std::string& a, const std::string& b) {
    if (!hexColor(Json(a)) || !hexColor(Json(b))) return 0;
    const double x = luminance(a), y = luminance(b);
    return (std::max(x, y) + .05) / (std::min(x, y) + .05);
}
// The same mapping tools/make_themes.py applies to the built-in catalogue, starting from CCA's native tokens.
Json themeFromPalette(const std::string& id, const Json& theme, const std::string& pluginName) {
    Json tokens = themeById("cca")["tokens"];
    const Json& p = theme["palette"];
    const bool light = theme["scheme"].str() == "light";
    auto c = [&](const char* key) { return p[key].str(); };
    for (const char* key : {"rail", "side", "chat", "panel", "input", "hover", "selected"})
        tokens[std::string("bg-") + key] = c(key);
    const std::pair<const char*, const char*> same[] = {
        {"bg-float", "chat"},        {"bg-embed", "panel"},   {"bg-side-hover", "hover"}, {"bg-code", "side"},
        {"border-code", "line"},     {"divider", "line"},     {"line", "line"},           {"text", "text"},
        {"header", "text"},          {"text-muted", "muted"}, {"text-faint", "muted"},    {"header-2", "muted"},
        {"icon", "muted"},           {"icon-hover", "text"},  {"brand", "brand"},         {"brand-hover", "text"},
        {"signal", "signal"},        {"link", "signal"},      {"scroll-thumb", "line"},   {"gray-btn", "selected"},
        {"gray-btn-hover", "hover"}, {"badge-ink", "text"},   {"commander", "signal"}};
    for (const auto& [token, key] : same) tokens[token] = c(key);
    tokens["brand-ink"] = light ? std::string("#ffffff") : c("chat");
    tokens["signal-rgb"] = rgbOf(c("signal"));
    tokens["signal-soft"] = "rgba(" + rgbOf(c("signal")) + ", .12)";
    tokens["brand-soft"] = "rgba(" + rgbOf(c("brand")) + ", .08)";
    tokens["dot-grid"] = "transparent";
    for (size_t i = 0; i < p["accents"].size() && i < 6; ++i)
        tokens["theme-a" + std::to_string(i + 1)] = p["accents"].at(i);
    if (p.has("display")) tokens["theme-display"] = p["display"];
    if (p.has("reading")) tokens["theme-reading"] = p["reading"];
    if (p.has("ink")) tokens["brand-ink"] = p["ink"];
    if (light) {
        // Member and status colors readable on paper, as the built-in light themes use.
        const std::pair<const char*, const char*> paper[] = {{"gemini", "#245db1"},
                                                             {"gemini-name", "#245db1"},
                                                             {"gemini-rgb", "36, 93, 177"},
                                                             {"claude", "#99482f"},
                                                             {"claude-name", "#99482f"},
                                                             {"codex", "#146948"},
                                                             {"codex-name", "#146948"},
                                                             {"local1", "#ad3975"},
                                                             {"local1-name", "#9a2864"},
                                                             {"local2", "#7258a7"},
                                                             {"local2-name", "#64439b"},
                                                             {"green", "#277743"},
                                                             {"red", "#b73541"},
                                                             {"yellow", "#89601b"},
                                                             {"success-text", "#246b38"},
                                                             {"danger-text", "#a62633"},
                                                             {"diff-add-text", "#226734"},
                                                             {"diff-remove-text", "#a62633"},
                                                             {"yellow-rgb", "137, 96, 27"},
                                                             {"green-rgb", "39, 119, 67"},
                                                             {"red-rgb", "183, 53, 65"},
                                                             {"claude-rgb", "153, 72, 47"},
                                                             {"codex-rgb", "20, 105, 72"},
                                                             {"local1-rgb", "173, 57, 117"},
                                                             {"local2-rgb", "114, 88, 167"},
                                                             {"badge-copper", "#8d6033"},
                                                             {"badge-teal", "#286c65"},
                                                             {"badge-steel", "#506a82"},
                                                             {"badge-blue", "#38648a"}};
        for (const auto& [token, value] : paper) tokens[token] = value;
    }
    return J({{"id", id},
              {"name", theme["name"]},
              {"kicker", "插件 · " + pluginName},
              {"scheme", theme["scheme"]},
              {"shape", theme["shape"]},
              {"base", theme["base"].str()},
              {"description", theme["description"].str()},
              {"motif", ""},
              {"sources", Json::array()},
              {"plugin", true},
              {"tokens", tokens}});
}

Json scan(const std::wstring& dir) {
    Json list = Json::array();
    std::set<std::string> ids;
    eachFolder(dir, [&](const std::wstring& name) {
        if ((int)list.size() >= kMaxPlugins) return;
        const auto path = util::joinPath(util::joinPath(dir, name), L"plugin.json");
        Json errors = Json::array();
        std::string raw;
        WIN32_FILE_ATTRIBUTE_DATA attr{};
        const bool readable = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr) &&
                              !(attr.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
                              !attr.nFileSizeHigh && attr.nFileSizeLow <= kMaxManifest && util::readFile(path, raw);
        if (!readable) return;  // a folder without plugin.json is not a plugin
        std::string parseError;
        Json m = Json::parse(raw, &parseError);
        Json record = J({{"folder", util::narrow(name)},
                         {"hash", updater::sha256(raw)},
                         {"id", m["id"].str()},
                         {"name", m["name"].str(util::narrow(name))},
                         {"version", m["version"].str()},
                         {"author", m["author"].str()},
                         {"description", m["description"].str()},
                         {"themes", Json::array()},
                         {"modes", Json::array()},
                         {"commands", Json::array()}});
        auto fail = [&](const std::string& why) { errors.push(why); };
        if (!m.isObject()) fail("plugin.json 不是有效的 JSON：" + parseError);
        else {
            if (m["schema"].i64() != 1) fail("schema 必須是 1");
            if (!slug(m["id"].str(), 40)) fail("id 只能用小寫英數字與連字號，2 到 40 字");
            else if (!ids.insert(m["id"].str()).second) fail("id 與其他插件重複");
            if (!text(m["name"], 60) || m["name"].str().empty()) fail("name 必填，60 字以內");
            if (!text(m["version"], 20) || !text(m["author"], 60) || !text(m["description"], 400, true))
                fail("version、author 或 description 過長或含控制字元");
            if (m["themes"].size() > 8 || m["modes"].size() > 8 || m["commands"].size() > 16)
                fail("最多 8 個主題、8 個模式、16 個指令");
            for (const auto& t : m["themes"].items()) {
                const auto label = "主題 " + t["id"].str();
                if (!slug(t["id"].str(), 30) || !text(t["name"], 40) || t["name"].str().empty()) {
                    fail(label + "：id 或 name 不符");
                    continue;
                }
                if (t["scheme"].str() != "light" && t["scheme"].str() != "dark")
                    fail(label + "：scheme 只能是 light 或 dark");
                bool shape = false;
                for (const char* s : kShapes) shape |= t["shape"].str() == s;
                if (!shape) fail(label + "：shape 必須是 CCA 既有的形狀");
                // Borrowing devices from a built-in theme is how a plugin changes more than colors, without CSS.
                const Json base = t["base"].str().empty() ? Json() : themeById(t["base"].str());
                if (!t["base"].str().empty() &&
                    (base.isNull() || !base["edition"].str().empty() || t["base"].str() == "cca"))
                    fail(label + "：base 必須是內建選用主題的 id");
                bool colors = true;
                for (const char* key : kPalette) colors &= hexColor(t["palette"][key]);
                for (const auto& a : t["palette"]["accents"].items()) colors &= hexColor(a);
                if (t["palette"].has("ink")) colors &= hexColor(t["palette"]["ink"]);
                if (!colors || t["palette"]["accents"].size() > 6) {
                    fail(label + "：palette 的顏色必須都是 #rrggbb，accents 最多 6 個");
                    continue;
                }
                for (const char* key : {"display", "reading"})
                    if (t["palette"].has(key) && !fontStack(t["palette"][key]))
                        fail(label + "：" + key + " 只能列出已安裝的字型名稱");
                const auto chat = t["palette"]["chat"].str();
                for (const char* key : {"text", "muted"})
                    if (contrast(t["palette"][key].str(), chat) < 4.5)
                        fail(label + "：" + key + " 與 chat 的對比低於 4.5:1");
                if (!text(t["description"], 200)) fail(label + "：description 過長");
                const auto id = "p." + m["id"].str() + "." + t["id"].str();
                record["themes"].push(themeFromPalette(id, t, m["name"].str()));
            }
            for (const auto& mode : m["modes"].items()) {
                const auto label = "模式 " + mode["id"].str();
                if (!slug(mode["id"].str(), 30) || !text(mode["name"], 40) || mode["name"].str().empty()) {
                    fail(label + "：id 或 name 不符");
                    continue;
                }
                const auto scope = mode["scope"].str("any");
                if (scope != "group" && scope != "dm" && scope != "any")
                    fail(label + "：scope 只能是 group、dm 或 any");
                if (!text(mode["description"], 200) || !text(mode["instructions"], 2000, true))
                    fail(label + "：description 或 instructions 過長");
                const Json& s = mode["settings"];
                if (!s.isNull() && !s.isObject()) fail(label + "：settings 必須是物件");
                for (const auto& [key, value] : s.fields()) {
                    bool ok = false;
                    if (key == "permission")
                        ok = value.str() == "plan" || value.str() == "ask" || value.str() == "edit";  // never bypass
                    else if (key == "maxRounds") ok = value.isNumber() && value.i64() >= 1 && value.i64() <= 20;
                    else if (key == "lead") ok = value.str() == "claude" || value.str() == "codex";
                    else if (key == "autoFallback" || key == "geminiMember" || key == "regressionGuard" ||
                             key == "daily")
                        ok = value.isBool();
                    else if (key == "localMembers") {
                        ok = value.isArray();
                        for (const auto& l : value.items()) ok &= l.str() == "local1" || l.str() == "local2";
                    }
                    if (!ok) fail(label + "：不支援的設定 " + key + "（插件不能設定 bypass 權限或測試指令）");
                }
                record["modes"].push(J({{"id", m["id"].str() + "/" + mode["id"].str()},
                                        {"name", mode["name"]},
                                        {"description", mode["description"].str()},
                                        {"scope", scope},
                                        {"settings", s.isObject() ? s : Json::object()},
                                        {"instructions", mode["instructions"].str()}}));
            }
            for (const auto& command : m["commands"].items()) {
                const auto name = command["name"].str();
                const auto label = "指令 /" + name;
                if (!slug(name, 30) || !text(command["description"], 120) || !text(command["template"], 4000, true) ||
                    command["template"].str().empty()) {
                    fail(label + "：name、description 或 template 不符");
                    continue;
                }
                const auto scope = command["scope"].str("any");
                if (scope != "group" && scope != "dm" && scope != "any")
                    fail(label + "：scope 只能是 group、dm 或 any");
                record["commands"].push(J({{"name", name},
                                           {"plugin", m["id"].str()},
                                           {"description", command["description"].str()},
                                           {"template", command["template"].str()},
                                           {"scope", scope}}));
            }
        }
        record["errors"] = errors;
        list.push(record);
    });
    return list;
}
}  // namespace extensions

namespace {
bool inScope(const std::string& scope, const Conversation& c) {
    return scope == "any" || (scope == "group") == (c.kind == "group");
}
}  // namespace

void App::loadExtensions() {
    Json plugins = extensions::scan(extensions::folder());
    Json themes = Json::array();
    // Read through a const reference: looking up approvals must not add an empty "extensions" key to settings.
    const Json& enabled = static_cast<const Json&>(settings_)["extensions"]["enabled"];
    // Built-in CLI commands keep their meaning; a plugin command may not shadow them.
    static const std::set<std::string> reserved = {"help",    "exit",  "quit",   "status",     "theme",
                                                   "update",  "git",   "plan",   "mode",       "plugin-mode",
                                                   "plugins", "clear", "rounds", "test-guard", "about"};
    std::set<std::string> commands;
    for (auto& p : plugins.items()) {
        const bool pinned = enabled[p["id"].str()].str() == p["hash"].str();
        p["changed"] = enabled.has(p["id"].str()) && !pinned;
        p["enabled"] = pinned && p["errors"].empty();
        if (!p["enabled"].boolean()) continue;
        for (const auto& t : p["themes"].items()) themes.push(t);
        for (auto& cmd : p["commands"].items()) {
            cmd["active"] = !reserved.count(cmd["name"].str()) && commands.insert(cmd["name"].str()).second;
        }
    }
    setPluginThemes(themes);
    extensions_ = plugins;
}

Json App::extensionState() const {
    Json modes = Json::array(), commands = Json::array(), themes = Json::array();
    for (const auto& p : extensions_.items()) {
        if (!p["enabled"].boolean()) continue;
        for (const auto& m : p["modes"].items()) modes.push(m);
        for (const auto& c : p["commands"].items())
            if (c["active"].boolean()) commands.push(c);
        for (const auto& t : p["themes"].items()) themes.push(t);
    }
    Json state = J({{"type", "extensions"},
                    {"folder", util::narrow(extensions::folder())},
                    {"available", true},
                    {"plugins", extensions_},
                    {"themes", themes},
                    {"modes", modes},
                    {"commands", commands}});
    return state;
}

const Json* App::extensionMode(const std::string& id) const {
    for (const auto& p : extensions_.items())
        if (p["enabled"].boolean())
            for (const auto& m : p["modes"].items())
                if (m["id"].str() == id) return &m;
    return nullptr;
}

std::string App::extensionModeInstructions(const Conversation& c) const {
    const Json* mode = c.mode.empty() ? nullptr : extensionMode(c.mode);
    if (!mode || util::trim((*mode)["instructions"].str()).empty()) return "";
    return "【使用者啟用的 CCA 插件模式：" + (*mode)["name"].str() + "】\n" + (*mode)["instructions"].str() +
           "\n（插件模式不能改變工具權限、測試要求或 VERDICT 規則。）";
}

void App::expandExtensionCommand(const Conversation& c, std::string& message) {
    if (message.size() < 2 || message[0] != '/') return;
    const auto space = message.find_first_of(" \n");
    const auto name = message.substr(1, space == std::string::npos ? std::string::npos : space - 1);
    const auto rest = space == std::string::npos ? std::string() : util::trim(message.substr(space + 1));
    for (const auto& p : extensions_.items()) {
        if (!p["enabled"].boolean()) continue;
        for (const auto& cmd : p["commands"].items()) {
            if (cmd["name"].str() != name || !cmd["active"].boolean() || !inScope(cmd["scope"].str(), c)) continue;
            auto expanded = cmd["template"].str();
            if (expanded.find("{{input}}") == std::string::npos) expanded += rest.empty() ? "" : "\n\n" + rest;
            else expanded = util::replaceAll(expanded, "{{input}}", rest);
            message = util::truncateUtf8(expanded, 60000);
            return;
        }
    }
}

bool App::handleExtensions(const Json& m) {
    const auto type = m["type"].str();
    if (!util::startsWith(type, "extensions.")) return false;
    if (type == "extensions.open") {
        if (openExternal) openExternal(extensions::folder());
        return true;
    }
    if (type == "extensions.enable" || type == "extensions.disable") {
        loadExtensions();
        const auto id = m["id"].str();
        const Json* found = nullptr;
        for (const auto& p : extensions_.items())
            if (p["id"].str() == id) found = &p;
        if (!found) {
            toast("error", "找不到這個插件，請重新掃描");
            sendExtensions();
            return true;
        }
        Json previous = settings_;
        if (type == "extensions.enable") {
            // The user approves the exact file they reviewed; a later edit turns the plugin off again.
            if (!(*found)["errors"].empty() || (*found)["hash"].str() != m["hash"].str()) {
                toast("error", "插件有錯誤，或在檢視後被修改；請重新檢視再啟用");
                sendExtensions();
                return true;
            }
            settings_["extensions"]["enabled"][id] = (*found)["hash"];
        } else {
            settings_["extensions"]["enabled"].erase(id);
        }
        if (!util::writeFileAtomic(util::joinPath(util::appDataDir(), L"settings.json"), settings_.dump())) {
            settings_ = previous;
            toast("error", "插件設定未能保存，原設定已保留");
        } else {
            toast("ok", type == "extensions.enable" ? "已啟用插件 " + (*found)["name"].str() : "已停用插件");
        }
        loadExtensions();
        sendSettings();
        sendExtensions();
        return true;
    }
    if (type == "extensions.mode") {
        Conversation* c = find(m["id"].str());
        if (!c) return true;
        const auto id = m["mode"].str();
        if (id.empty()) {
            c->mode.clear();
        } else {
            const Json* mode = extensionMode(id);
            if (!mode || !inScope((*mode)["scope"].str(), *c)) {
                toast("error", "這個模式不存在、未啟用，或不適用於這個對話");
                return true;
            }
            if (anyBusy(*c) || c->group.active) {
                toast("error", "請等目前的工作結束，再切換模式");
                return true;
            }
            Json s = (*mode)["settings"];
            if (c->kind == "group") s.erase("daily");
            else
                for (const char* key : {"maxRounds", "lead", "geminiMember", "localMembers", "regressionGuard"})
                    s.erase(key);
            applySettings(*c, s);
            c->mode = id;
        }
        metaDirty_ = true;
        saveMeta();
        sendConvs();
        return true;
    }
    loadExtensions();
    sendExtensions();
    return true;
}

void App::sendExtensions() {
    send(extensionState());
}
