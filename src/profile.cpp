// Display names of the participants, settings defaults, the picture helpers shared by
// screenshots and image artifacts, and the model choice for daily (casual chat) mode.
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <vector>

#include "app.hpp"
#include "collaboration.hpp"
#include "gitflow.hpp"
#include "humanize.hpp"
#include "themes.hpp"
#include "util.hpp"

namespace {

bool isTempMedia(const std::string& file) {
    return util::startsWith(file, "tmp-") && isMediaName(file);
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
    // Versions before 1.0.2-luna copied avatar pictures here before cropping; nothing uses them any more.
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
    switch (k) {
    case AgentKind::Claude:
        return "Claude";
    case AgentKind::Codex:
        return "GPT";
    case AgentKind::Gemini:
        return "Gemini";
    case AgentKind::Cursor:
        return "Cursor";
    case AgentKind::Local1:
        return "本地模型 1";
    default:
        return "本地模型 2";
    }
}

std::string App::userName() const {
    return util::windowsUserName();
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
    // Themes of older versions are gone: their IDs fall back to the default look.
    if (themeById(settings_["theme"].str()).isNull() && !util::startsWith(settings_["theme"].str(), "p."))
        settings_["theme"] = "cca";
    if (!settings_["layout"].isObject())
        settings_["layout"] =
            J({{"preset", "collaboration"}, {"side", 260}, {"panel", 340}, {"font", 15}, {"density", "comfortable"}});
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
    if (!resume["fallback"]["cursor"].isNumber()) resume["fallback"]["cursor"] = 60;
    if (!resume["window"].isObject()) resume["window"] = J({{"enabled", false}, {"from", "00:00"}, {"to", "08:00"}});
    if (!settings_["reviewProfiles"].isArray()) settings_["reviewProfiles"] = defaultReviewProfiles();
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
            {"groupOrder", groupOrder()},
            {"humanizeRules", humanizeInstructions("")}}));
}

void App::reconfigureAll() {
    for (auto& c : convs_) {
        if (c->claude) c->claude->configure(configFor(*c, AgentKind::Claude));
        if (c->codex) c->codex->configure(configFor(*c, AgentKind::Codex));
    }
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
