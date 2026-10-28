// Display profiles (name, avatar, banner) for the user and native agents,
// persona assignment, and the model choice for daily (casual chat) mode.
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <vector>

#include "app.hpp"
#include "badges.hpp"
#include "collaboration.hpp"
#include "gitflow.hpp"
#include "humanize.hpp"
#include "personas.hpp"
#include "themes.hpp"
#include "util.hpp"

namespace {

const size_t kMaxImageBytes = 10 * 1024 * 1024;

bool validWho(const std::string& w) {
    return w == "user" || w == "claude" || w == "codex" || w == "gemini" || w == "local1" || w == "local2";
}
bool validAgent(const std::string& a) {
    return a == "claude" || a == "codex" || a == "gemini" || a == "local1" || a == "local2";
}

bool validColor(const std::string& c) {
    if (c.size() != 7 || c[0] != '#') return false;
    for (size_t i = 1; i < 7; ++i) {
        if (!isxdigit((unsigned char)c[i])) return false;
    }
    return true;
}

std::string extOf(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return {};
    return util::toLower(util::narrow(path.substr(dot)));
}

bool isTempMedia(const std::string& file) {
    return util::startsWith(file, "tmp-") && isMediaName(file);
}

bool validDate(const std::string& d) {
    if (d.size() != 10 || d[4] != '-' || d[7] != '-') return false;
    for (int i : {0, 1, 2, 3, 5, 6, 8, 9}) {
        if (!isdigit((unsigned char)d[i])) return false;
    }
    int year = 0;
    for (int i = 0; i < 4; ++i) year = year * 10 + d[i] - '0';
    int month = (d[5] - '0') * 10 + d[6] - '0', day = (d[8] - '0') * 10 + d[9] - '0';
    if (year < 1 || month < 1 || month > 12) return false;
    const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int maxDay = days[month - 1] + (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    return day >= 1 && day <= maxDay;
}

bool readablePng(const std::string& png, bool screenshot) {
    using Microsoft::WRL::ComPtr;
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        UINT width = 0, height = 0;
        GUID format{};
        if (SUCCEEDED(
                CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromMemory((BYTE*)png.data(), (DWORD)png.size())) &&
            SUCCEEDED(
                factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) &&
            SUCCEEDED(decoder->GetContainerFormat(&format)) && IsEqualGUID(format, GUID_ContainerFormatPng) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&width, &height)) && width > 0 &&
            height > 0 && (screenshot ? (width <= 1600 && height <= 1000) : (width == height && width <= 1024)) &&
            SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr,
                                            0, WICBitmapPaletteTypeCustom))) {
            // Decode the bounded image as well as its header; a PNG signature alone is not a valid picture.
            std::vector<BYTE> pixels(width * height * 4);
            ok = SUCCEEDED(converter->CopyPixels(nullptr, width * 4, (UINT)pixels.size(), pixels.data()));
        }
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}

int effortRank(const std::string& e) {
    static const char* order[] = {"minimal", "low", "medium", "high", "xhigh", "max", "ultra", "ultracode"};
    for (int i = 0; i < 8; ++i) {
        if (e == order[i]) return i;
    }
    return 100;
}

}  // namespace

bool isMediaName(const std::string& name) {
    if (name.empty() || name.size() >= 200 || name.front() == '.' || name.back() == '.' ||
        name.find("..") != std::string::npos)
        return false;
    for (unsigned char ch : name) {
        if (!(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z') && !(ch >= '0' && ch <= '9') && ch != '-' &&
            ch != '_' && ch != '.')
            return false;
    }
    return true;
}

void removeMediaFile(const std::string& file) {
    // Only files CCA copied into its media folder are ever deleted.
    if (!isMediaName(file)) return;
    DeleteFileW(util::joinPath(App::mediaDir(), util::widen(file)).c_str());
}

std::string decodePngDataUrl(const std::string& data, bool screenshot) {
    const std::string prefix = "data:image/png;base64,";
    if (!util::startsWith(data, prefix) || data.size() > 3 * 1024 * 1024) return "";
    std::string b64 = data.substr(prefix.size());
    if (b64.empty() || b64.size() % 4) return "";
    size_t padding = 0;
    for (unsigned char ch : b64) {
        if (ch == '=') {
            if (++padding > 2) return "";
        } else if (padding || (!(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z') && !(ch >= '0' && ch <= '9') &&
                               ch != '+' && ch != '/'))
            return "";
    }
    std::string png = util::base64UrlDecode(b64);
    static const char kMagic[] = "\x89PNG\r\n\x1a\n";
    if (png.size() < 8 || png.size() > 2 * 1024 * 1024 || png.compare(0, 8, kMagic, 8) != 0 ||
        !readablePng(png, screenshot))
        return "";
    return png;
}

void cleanTempMedia() {
    // Pictures picked for cropping and never saved (CCA closed with the crop dialog open).
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(util::joinPath(App::mediaDir(), L"tmp-*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::string file = util::narrow(fd.cFileName);
        if (isTempMedia(file)) removeMediaFile(file);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::wstring App::mediaDir() {
    std::wstring d = util::joinPath(util::appDataDir(), L"media");
    util::ensureDir(d);
    return d;
}

std::string App::nameOf(AgentKind k) const {
    std::string n = util::trim(settings_["profiles"][agentName(k)]["name"].str());
    if (!n.empty()) return n;
    if (isLocal(k)) {
        // Local reviewers go by their persona's name until the user names them.
        Json p = findPersona(settings_["personas"], settings_["persona"][agentName(k)].str(), personaPresets_);
        if (p.isObject()) return p["name"].str();
        return k == AgentKind::Local1 ? "本地模型 1" : "本地模型 2";
    }
    return k == AgentKind::Claude ? "Claude" : k == AgentKind::Gemini ? "Gemini" : "GPT";
}

std::string App::userName() const {
    std::string n = util::trim(settings_["profiles"]["user"]["name"].str());
    return n.empty() ? util::windowsUserName() : n;
}

Json App::allPersonas() const {
    Json list = Json::array();
    for (const auto& p : personaPresets_.items()) {
        if (findPersona(settings_["personas"], p["id"].str()).isNull()) list.push(p);
    }
    for (const auto& p : settings_["personas"].items()) {
        Json copy = p;
        copy["builtin"] = false;
        list.push(copy);
    }
    return list;
}

std::string App::personaPrompt(AgentKind k) const {
    return personaInstructions(
        findPersona(settings_["personas"], settings_["persona"][agentName(k)].str(), personaPresets_));
}

std::string App::dailyModel(AgentKind k) const {
    std::string pick = settings_["daily"][k == AgentKind::Claude ? "claudeModel" : "codexModel"].str();
    if (!pick.empty()) return pick;
    if (k == AgentKind::Claude) {
        for (const auto& m : claudeModels_.items()) {
            std::string v = util::toLower(m["value"].str() + " " + m["displayName"].str());
            if (v.find("haiku") != std::string::npos) return m["value"].str();
        }
        return "haiku";
    }
    // Codex has no price list in its protocol; its small models are named luna / mini / nano.
    for (const char* kw : {"luna", "nano", "mini"}) {
        for (const auto& m : codexModels_.items()) {
            if (util::toLower(m["value"].str()).find(kw) != std::string::npos) return m["value"].str();
        }
    }
    return "";
}

std::string App::dailyEffort(AgentKind k) const {
    std::string pick = settings_["daily"][k == AgentKind::Claude ? "claudeEffort" : "codexEffort"].str();
    if (!pick.empty()) return pick;
    std::string model = dailyModel(k);
    const Json& list = k == AgentKind::Claude ? claudeModels_ : codexModels_;
    for (const auto& m : list.items()) {
        if (m["value"].str() != model) continue;
        std::string lowest;
        for (const auto& e : m["supportedEffortLevels"].items()) {
            if (lowest.empty() || effortRank(e.str()) < effortRank(lowest)) lowest = e.str();
        }
        return lowest;  // empty when the model has no effort setting
    }
    return k == AgentKind::Codex ? "low" : "";
}

void App::ensureProfileDefaults() {
    // Plugin themes load later; their IDs stay saved even while the plugin is off, so the choice comes back.
    if (themeById(settings_["theme"].str()).isNull() && !util::startsWith(settings_["theme"].str(), "p."))
        settings_["theme"] = "cca";
    if (!settings_["layout"].isObject())
        settings_["layout"] =
            J({{"preset", "collaboration"}, {"side", 240}, {"panel", 340}, {"font", 15}, {"density", "comfortable"}});
    if (!settings_["updateSource"].isString()) settings_["updateSource"] = "";
    if (!settings_["updateAuto"].isBool()) settings_["updateAuto"] = true;
    if (!settings_["git"]["neverCommit"].isArray()) settings_["git"]["neverCommit"] = gitflow::defaultRules();
    if (!settings_["git"]["auto"].isObject()) settings_["git"]["auto"] = Json::object();
    // Automatic resume after a quota stop is opt-in; the fallback waits apply when a provider reports no reset time.
    Json& resume = settings_["autoResume"];
    if (!resume.isObject()) resume = Json::object();
    if (!resume["enabled"].isBool()) resume["enabled"] = false;
    if (!resume["delayMinutes"].isNumber()) resume["delayMinutes"] = 3;
    if (!resume["maxAttempts"].isNumber()) resume["maxAttempts"] = 3;
    if (!resume["fallback"].isObject()) resume["fallback"] = J({{"claude", 300}, {"codex", 300}, {"gemini", 60}});
    if (!resume["window"].isObject()) resume["window"] = J({{"enabled", false}, {"from", "00:00"}, {"to", "08:00"}});
    if (!settings_["reviewProfiles"].isArray()) settings_["reviewProfiles"] = defaultReviewProfiles();
    if (!settings_["profiles"].isObject()) settings_["profiles"] = Json::object();
    for (const char* who : {"user", "claude", "codex", "gemini", "local1", "local2"}) {
        if (!settings_["profiles"][who].isObject()) settings_["profiles"][who] = Json::object();
    }
    if (!settings_["personas"].isArray()) settings_["personas"] = Json::array();
    if (!settings_["persona"].isObject()) settings_["persona"] = Json::object();
    for (const char* agent : {"claude", "codex", "gemini", "local1", "local2"})
        if (!settings_["persona"].has(agent)) {
            settings_["persona"][agent] = "none";
            for (const auto& preset : personaPresets_.items()) {
                const auto target = preset["agent"].str();
                if (!preset["assignOnFirstUse"].boolean() ||
                    (target != agent && !(target == "local" && util::startsWith(agent, "local"))))
                    continue;
                settings_["persona"][agent] = preset["id"];
                if (settings_["profiles"][agent]["name"].str().empty())
                    settings_["profiles"][agent]["name"] = preset["name"];
                metaDirty_ = true;
                break;
            }
        }
    if (!settings_["daily"].isObject()) settings_["daily"] = Json::object();
    if (!settings_["visual"].isObject()) settings_["visual"] = J({{"mode", "auto"}, {"motion", true}});
    // Reading Claude Code's sign-in for the live usage query needs the user's consent.
    if (!settings_["claudeUsageLive"].isBool()) settings_["claudeUsageLive"] = false;
    // 去 AI 化 is on unless the user turns it off.
    if (!settings_["humanize"].isObject()) settings_["humanize"] = J({{"enabled", true}, {"extra", ""}});
    // Compliance checks on every reply (guard.hpp): on by default, two send-backs before pausing.
    if (!settings_["guard"].isObject()) settings_["guard"] = J({{"enabled", true}, {"retries", 2}});
    // Offer the Docker sandbox to Claude and GPT as a shared tool (sandbox_mcp.hpp); false hides it.
    if (!settings_["sandboxShare"].isBool()) settings_["sandboxShare"] = true;
    if (!settings_["locals"].isObject()) settings_["locals"] = Json::object();
    for (const char* slot : {"local1", "local2"}) {
        if (!settings_["locals"][slot].isObject()) settings_["locals"][slot] = J({{"model", ""}, {"enabled", true}});
    }
}

void App::sendSettings() {
    if (!settings_["reviewProfiles"].isArray()) settings_["reviewProfiles"] = defaultReviewProfiles();
    send(J({{"type", "settings"},
            {"settings", settings_},
            {"personas", allPersonas()},
            {"badges", badgeState()},
            {"groupOrder", groupOrder()},
            {"humanizeRules", humanizeInstructions("")}}));
}

Json App::badgeState() const {
    return profileBadges(badgeGrants_, settings_["badgeDisplay"]);
}

void App::handleBadgeDisplay(const Json& m) {
    const std::string token = m["requestToken"].str();
    auto result = [&](bool ok, const std::string& error) {
        send(J({{"type", "badges.result"}, {"requestToken", token}, {"ok", ok}, {"error", error}}));
    };
    if (!validBadgeDisplay(m, badgeGrants_)) {
        result(false, "徽章清單無效，無法儲存。");
        return;
    }
    const Json old = settings_;
    settings_["badgeDisplay"] = J({{"order", m["order"]}, {"hidden", m["hidden"]}});
    if (!util::writeFileAtomic(util::joinPath(util::appDataDir(), L"settings.json"), settings_.dump())) {
        settings_ = old;
        result(false, "徽章顯示設定未能儲存，原設定已保留。");
        return;
    }
    sendSettings();
    result(true, "");
}

void App::reconfigureAll() {
    for (auto& c : convs_) {
        if (c->claude) c->claude->configure(configFor(*c, AgentKind::Claude));
        if (c->codex) c->codex->configure(configFor(*c, AgentKind::Codex));
    }
}

bool App::persistProfile(const std::string& who, const Json& profile) {
    Json old = settings_["profiles"][who];
    settings_["profiles"][who] = profile;
    if (!util::writeFileAtomic(util::joinPath(util::appDataDir(), L"settings.json"), settings_.dump())) {
        settings_["profiles"][who] = old;
        return false;
    }
    // A saved reference must never point to a picture that was already deleted.
    for (const char* kind : {"avatar", "banner"}) {
        if (old[kind].str() != profile[kind].str()) removeMediaFile(old[kind].str());
    }
    return true;
}

bool App::persistGroupAvatar(Conversation& c, const std::string& file) {
    const std::string old = c.avatar;
    c.avatar = file;
    if (!saveConversations()) {
        c.avatar = old;
        return false;
    }
    if (old != file) removeMediaFile(old);
    return true;
}

void App::handleProfileSet(const Json& m) {
    std::string who = m["who"].str();
    if (!validWho(who)) return;
    if (who != "user" && m.has("joinedAt")) {
        const Json& date = m["joinedAt"];
        if (!date.isNull() && (!date.isString() || (!date.str().empty() && !validDate(date.str())))) {
            toast("error", "加入日期不是有效的日期");
            return;
        }
    }
    Json p = settings_["profiles"][who];
    if (m.has("name")) {
        std::string n = util::trim(m["name"].str());
        p["name"] = util::truncateUtf8(n, 96);  // about 32 CJK characters, like Discord
    }
    if (m.has("bannerColor")) {
        std::string c = m["bannerColor"].str();
        if (c.empty() || validColor(c)) p["bannerColor"] = c;
    }
    if (who != "user" && m.has("joinedAt")) {
        const Json& date = m["joinedAt"];
        if (date.isNull() || (date.isString() && date.str().empty())) p.erase("joinedAt");
        else p["joinedAt"] = date;
    }
    for (const char* kind : {"avatar", "banner"}) {
        if (m.has(kind) && m[kind].isNull()) {
            p.erase(kind);
        }
    }
    if (!persistProfile(who, p)) {
        toast("error", "無法儲存個人資料，已保留原設定");
        sendSettings();
        return;
    }
    if (who != "user") reconfigureAll();  // names appear in the group instructions
    sendSettings();
}

void App::handleProfileImage(const Json& m) {
    beginImage(m["who"].str(), m["kind"].str(), "");
}

void App::beginImage(const std::string& who, const std::string& kind, const std::string& convId) {
    Conversation* group = convId.empty() ? nullptr : find(convId);
    if ((!convId.empty() && (!group || group->kind != "group" || kind != "avatar")) ||
        (convId.empty() && !validWho(who)) || (kind != "avatar" && kind != "banner") || !pickImage)
        return;
    if (!pendingImage_["token"].str().empty()) {
        toast("info", "請先完成或取消目前的裁切");
        return;
    }
    std::wstring src = pickImage();
    if (src.empty()) return;
    std::string ext = extOf(src);
    if (ext != ".png" && ext != ".jpg" && ext != ".jpeg" && ext != ".gif" && ext != ".webp") {
        toast("error", "只支援 PNG、JPG、GIF、WebP 圖片");
        return;
    }
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &info)) {
        toast("error", "讀不到這個檔案");
        return;
    }
    unsigned long long size = ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    if (size > kMaxImageBytes) {
        toast("error", "圖片太大了，請選 10 MB 以內的檔案");
        return;
    }
    std::string token = util::newId();
    std::string file = (kind == "avatar" ? "tmp-" : who + "-banner-") + token + ext;
    if (!CopyFileW(src.c_str(), util::joinPath(mediaDir(), util::widen(file)).c_str(), FALSE)) {
        toast("error", "無法複製圖片");
        return;
    }
    if (kind == "avatar") {
        pendingImage_ = J({{"token", token}, {"file", file}, {"who", who}, {"convId", convId}});
        send(J({{"type", "image.crop"}, {"token", token}, {"file", file}, {"who", who}, {"convId", convId}}));
    } else {
        Json p = settings_["profiles"][who];
        p[kind] = file;
        if (!persistProfile(who, p)) {
            removeMediaFile(file);
            toast("error", "無法儲存橫幅設定，已保留原圖");
            return;
        }
        sendSettings();
    }
}

void App::handleImageAction(const Json& m) {
    std::string token = m["token"].str();
    if (token.empty() || token != pendingImage_["token"].str()) return;
    if (m["type"].str() == "image.cancel") {
        removeMediaFile(pendingImage_["file"].str());
        pendingImage_ = Json::object();
        return;
    }
    auto fail = [&](const std::string& error) {
        send(J({{"type", "image.result"}, {"token", token}, {"ok", false}, {"error", error}}));
    };
    std::string png = decodePngDataUrl(m["data"].str());
    if (png.empty()) {
        fail("裁切圖片無法讀取，請重新選擇圖片");
        return;
    }
    std::string who = pendingImage_["who"].str(), id = pendingImage_["convId"].str();
    Conversation* group = id.empty() ? nullptr : find(id);
    if (!id.empty() && (!group || group->kind != "group")) {
        fail("這個群組已不存在");
        return;
    }
    std::string file = (id.empty() ? who : "group") + "-avatar-" + util::newId() + ".png";
    if (!util::writeFileAtomic(util::joinPath(mediaDir(), util::widen(file)), png)) {
        fail("無法儲存頭像，請確認資料目錄可以寫入");
        return;
    }
    bool saved;
    if (group) saved = persistGroupAvatar(*group, file);
    else {
        Json p = settings_["profiles"][who];
        p["avatar"] = file;
        saved = persistProfile(who, p);
    }
    if (!saved) {
        removeMediaFile(file);
        fail("無法儲存頭像設定，已保留原圖；請確認資料目錄可以寫入後重試");
        return;
    }
    if (group) sendConvs();
    else sendSettings();
    removeMediaFile(pendingImage_["file"].str());
    pendingImage_ = Json::object();
    send(J({{"type", "image.result"}, {"token", token}, {"ok", true}}));
}

bool App::persistPersonaSettings(const Json& previous) {
    if (!util::writeFileAtomic(util::joinPath(util::appDataDir(), L"settings.json"), settings_.dump())) {
        settings_ = previous;
        toast("error", "無法儲存性格設定，已保留原設定");
        sendSettings();
        return false;
    }
    reconfigureAll();
    sendSettings();
    return true;
}

void App::handlePersonaSave(const Json& m) {
    const Json& in = m["persona"];
    std::string name = util::trim(in["name"].str());
    std::string prompt = util::trim(in["prompt"].str());
    if (name.empty() || prompt.empty()) {
        toast("error", "名稱和性格描述都要填");
        return;
    }
    std::string id = in["id"].str();
    bool isBuiltin = false;
    for (const auto& b : personaPresets_.items()) isBuiltin |= b["id"].str() == id;
    if (id.empty() || (isBuiltin && findPersona(settings_["personas"], id).isNull())) id = "p-" + util::newId();
    std::string agent = in["agent"].str();
    if (agent != "claude" && agent != "codex" && agent != "gemini" && agent != "local") agent = "any";
    Json p = J({{"id", id},
                {"name", util::truncateUtf8(name, 96)},
                {"agent", agent},
                {"summary", util::truncateUtf8(util::trim(in["summary"].str()), 400)},
                {"address", util::truncateUtf8(util::trim(in["address"].str()), 60)},
                {"prompt", util::truncateUtf8(prompt, 12000)}});
    const Json previous = settings_;
    Json& list = settings_["personas"];
    bool replaced = false;
    for (auto& x : list.items()) {
        if (x["id"].str() == id) { x = p; replaced = true; }
    }
    if (!replaced) list.push(p);
    std::string assign = m["assignTo"].str();
    if (validAgent(assign)) settings_["persona"][assign] = id;
    if (persistPersonaSettings(previous)) toast("ok", "已儲存性格「" + name + "」");
}

void App::handlePersonaDelete(const Json& m) {
    std::string id = m["id"].str();
    if (findPersona(settings_["personas"], id).isNull()) return;
    const Json previous = settings_;
    Json& list = settings_["personas"];
    auto& items = list.items();
    items.erase(std::remove_if(items.begin(), items.end(), [&](const Json& p) { return p["id"].str() == id; }), items.end());
    for (const char* a : {"claude", "codex", "gemini", "local1", "local2"}) {
        if (settings_["persona"][a].str() == id) settings_["persona"][a] = "none";
    }
    if (persistPersonaSettings(previous)) toast("ok", "已刪除自訂性格");
}

void App::handlePersonaAssign(const Json& m) {
    std::string agent = m["agent"].str();
    std::string id = m["id"].str("none");
    if (id.empty()) id = "none";
    if (!validAgent(agent)) return;
    Json persona = findPersona(settings_["personas"], id, personaPresets_);
    if (id != "none" && persona.isNull()) return;
    const Json previous = settings_;
    settings_["persona"][agent] = id.empty() ? "none" : id;
    if (m["rename"].boolean() && persona.isObject()) settings_["profiles"][agent]["name"] = persona["name"].str();
    if (persistPersonaSettings(previous)) toast("ok", "已更新性格指派");
}

void App::handleDailySet(const Json& m) {
    for (const char* key : {"claudeModel", "claudeEffort", "codexModel", "codexEffort"}) {
        if (m.has(key)) settings_["daily"][key] = util::truncateUtf8(m[key].str(), 80);
    }
    metaDirty_ = true;
    reconfigureAll();
    sendSettings();
}

void App::handleHumanizeSet(const Json& m) {
    Json& h = settings_["humanize"];
    if (m.has("enabled")) h["enabled"] = m["enabled"].boolean();
    if (m.has("extra")) h["extra"] = util::truncateUtf8(m["extra"].str(), 8000);
    metaDirty_ = true;
    reconfigureAll();
    sendSettings();
}
