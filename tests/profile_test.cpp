// Exercise avatar validation, crop transactions and persistence without live model backends.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <vector>

#include "activity.hpp"
#include "app.hpp"
#include "badges.hpp"
#include "model_dates.hpp"
#include "personas.hpp"
#include "util.hpp"

void postToUi(std::function<void()> fn) {
    fn();
}

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}
const char* kPng =
    "data:image/png;base64,"
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/iZk9HQAAAABJRU5ErkJggg==";
}  // namespace

struct ProfileTestAccess {
    static void run(const std::wstring& root) {
        const std::string png = decodePngDataUrl(kPng);
        check(!png.empty(), "valid PNG is fully decoded");
        check(decodePngDataUrl("data:image/png;base64,iVBORw0KGgo=").empty(), "signature alone is not a valid PNG");
        check(decodePngDataUrl("data:image/jpeg;base64,aGVsbG8=").empty(), "wrong MIME rejected");
        check(decodePngDataUrl(std::string(kPng).substr(0, 40) + "!===").empty(), "invalid base64 rejected");
        check(decodePngDataUrl(std::string(3 * 1024 * 1024 + 1, 'A')).empty(), "oversized payload rejected");
        check(!isMediaName("../settings.json") && !isMediaName("photo.png:stream") &&
                  !isMediaName(std::string("x\0.png", 6)),
              "media names cannot escape their folder");
        check(isMediaName("claude-avatar-example.png"), "managed media name accepted");
        check(modelReleaseDate("fable") == "2026-09-15", "Fable date follows official announcement");
        check(modelReleaseDate("claude-opus-5-5-20260922") == "2026-09-22", "dated model snapshot resolves");
        check(modelReleaseDate("qwen2.5-coder:7b") == "2024-09-19", "initial Coder sizes use initial release");
        check(modelReleaseDate("qwen2.5-coder:14b-instruct-q4_K_M") == "2024-11-12",
              "later Coder sizes keep their release");
        check(modelReleaseDate("qwen3-coder").empty() && modelReleaseDate("gpt-6-sol-custom").empty(),
              "new families and arbitrary suffixes are not guessed");
        check(modelReleaseDate("llama3.1:8b") == "2024-07-23", "known local model tag resolves");
        check(modelReleaseDate("qwen3:4b-thinking-2507").empty() && modelReleaseDate("qwen3:14b-custom").empty() &&
                  modelReleaseDate("llama3.1:99b").empty(),
              "retrained and unknown size variants keep unknown dates");
        check(modelReleaseDate("unlisted-model").empty(), "unknown models have no invented date");
        const Json dates = modelDateTable();
        check(dates.size() >= 16, "release table is populated");
        for (const auto& entry : dates.items()) {
            check(util::startsWith(entry["url"].str(), "https://"), "model date has a primary source URL");
        }
        const std::wstring source = util::joinPath(root, L"source.png");
        check(util::writeFileAtomic(source, png), "fixture source saved");
        std::vector<Json> events;
        const std::wstring presetFile = util::joinPath(util::appDataDir(), L"persona-presets.json");
        std::string presetError;
        check(loadPersonaPresets(presetFile, presetError).size() == 0 && presetError.empty(),
              "no preset file means no bundled characters");
        check(util::writeFileAtomic(presetFile, "invalid"), "invalid preset fixture saved");
        check(loadPersonaPresets(presetFile, presetError).size() == 0 && !presetError.empty(),
              "invalid preset file is reported without creating characters");
        check(util::writeFileAtomic(presetFile,
                                    R"([{"id":"external","name":"本機風格","agent":"any","prompt":"說明保持簡潔。"}])"),
              "external preset fixture saved");
        Json pack;
        check(parsePersonaPack(Json::parse(R"([{"id":"gemini-style","name":"Gemini reviewer","agent":"gemini",
                         "prompt":"GEMINI_PERSONA_MARKER","assignOnFirstUse":true}])"),
                               pack, presetError),
              "Gemini targeted first-use preset is valid");
        App seed([](const std::string&) {});
        seed.settings_ = Json::object();
        seed.personaPresets_ = pack;
        seed.ensureProfileDefaults();
        check(seed.settings_["persona"]["gemini"].str() == "gemini-style" &&
                  seed.nameOf(AgentKind::Gemini) == "Gemini reviewer" && seed.metaDirty_,
              "missing Gemini assignment receives explicit local first-use preset and persists it");
        seed.settings_["persona"]["gemini"] = "none";
        seed.settings_["profiles"]["gemini"]["name"] = "Manual reviewer";
        seed.ensureProfileDefaults();
        check(
            seed.settings_["persona"]["gemini"].str() == "none" && seed.nameOf(AgentKind::Gemini) == "Manual reviewer",
            "an explicit opt-out and manual name are not overwritten");
        seed.settings_["persona"].erase("gemini");
        seed.ensureProfileDefaults();
        check(seed.nameOf(AgentKind::Gemini) == "Manual reviewer", "first-use assignment preserves a custom name");
        check(!parsePersonaPack(Json::parse(R"([{"id":"x","name":"x","agent":"gemini","prompt":"x",
                        "assignOnFirstUse":"true"}])"),
                                pack, presetError),
              "first-use flag must be a boolean");
        check(!parsePersonaPack(Json::parse(R"([{"id":"x","name":"x","agent":"any","prompt":"x",
                        "assignOnFirstUse":true}])"),
                                pack, presetError),
              "unscoped presets cannot auto-assign");
        seed.personaPresets_ = Json::array();
        seed.settings_ = Json::object();
        seed.ensureProfileDefaults();
        check(seed.nameOf(AgentKind::Gemini) == "Gemini" && seed.personaPrompt(AgentKind::Gemini).empty(),
              "an installation with no private pack remains neutral");
        seed.shutdown();
        const std::wstring stale = util::joinPath(App::mediaDir(), L"tmp-stale.png");
        check(util::writeFileAtomic(stale, png), "fixture abandoned crop saved");
        const std::wstring grantFile = util::joinPath(util::appDataDir(), L"badges.json");
        std::string grantError;
        check(loadBadgeGrants(grantFile, grantError).empty() && grantError.empty(), "absent grants are optional");
        const Json freshBadges = profileBadges(Json::object(), Json::object());
        check(freshBadges["owned"] == Json(Json::Array{"member"}) && freshBadges["display"].size() == 1,
              "every new installation owns only the member badge");
        for (const char* invalid :
             {"broken", "[]", R"({"version":2,"profile":"x","grants":[]})",
              R"({"version":1.5,"profile":"x","grants":[]})", R"({"version":1,"profile":"x","grants":["unknown"]})",
              R"({"version":1,"profile":"x","grants":["developer","developer"]})",
              R"({"version":1,"profile":"x","grants":[1]})"}) {
            check(util::writeFileAtomic(grantFile, invalid), "invalid grant fixture saved");
            check(loadBadgeGrants(grantFile, grantError).empty() && !grantError.empty(),
                  "invalid whole grant file rejected");
            std::string unchanged;
            check(util::readFile(grantFile, unchanged) && unchanged == invalid, "rejected grant file is preserved");
        }
        check(util::writeFileAtomic(grantFile, std::string(64 * 1024 + 1, ' ')), "oversized grants fixture saved");
        check(loadBadgeGrants(grantFile, grantError).empty() && !grantError.empty(),
              "grant size is bounded before reading");
        const Json localGrants = J(
            {{"version", 1},
             {"profile", "fixture"},
             {"grants", Json::Array{"developer", "community-developer", "origin", "familiar", "present", "pioneer"}}});
        check(util::writeFileAtomic(grantFile, localGrants.dump()), "local grant fixture saved");
        App app([&](const std::string& text) { events.push_back(Json::parse(text)); });
        check(!util::fileExists(stale), "startup cleans a crop abandoned by a previous process");
        app.uiReady_ = true;
        check(app.allPersonas().size() == 1 && app.allPersonas().at(0)["builtin"].boolean(),
              "local preset loaded from data without compiling it");
        for (const char* agent : {"claude", "codex", "gemini", "local1", "local2"})
            check(app.settings_["persona"][agent].str() == "none", "fresh installations have no persona assignment");
        app.settings_["persona"]["local2"] = "unavailable-preset";
        app.ensureProfileDefaults();
        check(app.settings_["persona"]["local2"].str() == "unavailable-preset",
              "unavailable existing assignment is preserved");
        app.pickImage = [&] { return source; };
        auto send = [&](Json message) { app.onUiMessage(message.dump()); };
        const Json initialGroups = app.groupOrder();
        check(initialGroups.size() == 1, "fresh installations keep one starter group");
        send(J({{"type", "conv.create"}, {"name", "Navigation B"}}));
        send(J({{"type", "conv.create"}, {"name", "Navigation C"}}));
        const Json createdGroups = app.groupOrder();
        check(createdGroups.size() == 3 && createdGroups.at(0) == initialGroups.at(0),
              "new groups append without moving existing navigation icons");
        Json reorderedGroups = createdGroups;
        std::reverse(reorderedGroups.items().begin(), reorderedGroups.items().end());
        send(J({{"type", "groups.reorder"}, {"order", reorderedGroups}, {"requestToken", "nav-saved"}}));
        check(app.groupOrder() == reorderedGroups && events.back()["ok"].boolean() &&
                  events.back()["requestToken"].str() == "nav-saved",
              "group order is saved and acknowledged by the host");
        const Json beforeNavigation = app.settings_;
        const std::wstring navigationTemp = util::joinPath(util::appDataDir(), L"settings.json.tmp");
        HANDLE navigationLock =
            CreateFileW(navigationTemp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        check(navigationLock != INVALID_HANDLE_VALUE, "navigation save failure fixture holds the settings temp file");
        if (navigationLock != INVALID_HANDLE_VALUE) {
            send(J({{"type", "groups.reorder"}, {"order", createdGroups}, {"requestToken", "nav-failed"}}));
            check(app.settings_ == beforeNavigation && app.groupOrder() == reorderedGroups,
                  "failed navigation save retains the entire settings object and original icon order");
            check(!events.back()["ok"].boolean() && events.back()["requestToken"].str() == "nav-failed",
                  "failed navigation transaction acknowledges its matching request");
            CloseHandle(navigationLock);
            DeleteFileW(navigationTemp.c_str());
        }
        for (const Json& invalid :
             std::vector<Json>{Json(), Json::object(), Json::Array{"unknown"},
                               Json::Array{createdGroups.at(0), createdGroups.at(0), createdGroups.at(2)},
                               Json::Array{"dm-claude", createdGroups.at(1), createdGroups.at(2)},
                               Json::Array{createdGroups.at(0), createdGroups.at(1)}}) {
            send(J({{"type", "groups.reorder"}, {"order", invalid}}));
            check(!events.back()["ok"].boolean() && app.groupOrder() == reorderedGroups,
                  "stale, incomplete, duplicate, private or unknown navigation IDs cannot change order");
        }
        app.settings_["groupOrder"] = Json::Array{"removed", createdGroups.at(1), createdGroups.at(1), "dm-claude"};
        check(app.groupOrder().size() == 3 && app.groupOrder().at(0) == createdGroups.at(1),
              "obsolete saved IDs are filtered and missing existing groups are appended");
        app.settings_["groupOrder"] = reorderedGroups;
        send(J({{"type", "conv.open"}, {"id", "dm-codex"}}));
        send(J({{"type", "conv.open"}, {"id", createdGroups.at(0)}}));
        check(app.settings_["lastDm"].str() == "dm-codex" && app.groupOrder() == reorderedGroups,
              "opening a group preserves the last private conversation and fixed group positions");
        check(app.badgeState()["owned"].size() == 7 && app.badgeState()["display"].size() == 6,
              "six local grants plus built-in membership fill six display slots");
        app.sendBoot();
        bool bootBadges = false;
        for (const auto& event : events)
            if (event["type"].str() == "boot") bootBadges = event["badges"]["owned"].size() == 7;
        check(bootBadges, "native boot delivers badge ownership to both shells");
        const Json order =
            Json::Array{"member", "pioneer", "present", "familiar", "origin", "community-developer", "developer"};
        send(J({{"type", "badges.display"},
                {"order", order},
                {"hidden", Json::Array{"origin"}},
                {"requestToken", "saved"}}));
        const Json savedBadgeDisplay = app.settings_["badgeDisplay"];
        check(savedBadgeDisplay["order"] == order && app.badgeState()["display"].at(0).str() == "member" &&
                  app.badgeState()["hidden"].size() == 1,
              "reordering and hiding persist while all badges remain owned");
        send(J({{"type", "badges.display"}, {"order", Json::Array{"member", "member"}}, {"hidden", Json::array()}}));
        check(app.settings_["badgeDisplay"] == savedBadgeDisplay,
              "duplicate or incomplete order cannot change settings");
        const Json savedGrants = app.badgeGrants_;
        app.badgeGrants_ = Json::object();
        send(J({{"type", "badges.display"}, {"order", Json::Array{"developer", "member"}}, {"hidden", Json::array()}}));
        check(app.badgeState()["owned"].size() == 1, "display preferences cannot grant a developer badge");
        app.settings_["profiles"]["user"]["name"] = "developer";
        check(app.badgeState()["owned"].size() == 1, "renaming to developer does not grant anything");
        app.badgeGrants_ = savedGrants;
        const std::wstring badgeTemp = util::joinPath(util::appDataDir(), L"settings.json.tmp");
        HANDLE badgeLock = CreateFileW(badgeTemp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        check(badgeLock != INVALID_HANDLE_VALUE, "badge save failure holds the settings temp file");
        if (badgeLock != INVALID_HANDLE_VALUE) {
            send(J(
                {{"type", "badges.display"}, {"order", order}, {"hidden", Json::array()}, {"requestToken", "failed"}}));
            check(app.settings_["badgeDisplay"] == savedBadgeDisplay,
                  "failed badge save rolls back in-memory preferences");
            check(events.back()["requestToken"].str() == "failed" && !events.back()["ok"].boolean(),
                  "failed save acknowledges the matching request without claiming success");
            CloseHandle(badgeLock);
            DeleteFileW(badgeTemp.c_str());
        }
        const Json noisy =
            J({{"order", Json::Array{"unknown", "member", "member"}}, {"hidden", Json::Array{"member"}}});
        const Json normalized = profileBadges(savedGrants, noisy);
        check(normalized["order"].size() == 7 && normalized["owned"].size() == 7 && normalized["display"].size() == 6,
              "damaged legacy preferences normalize without losing badges");
        const Json allHidden = profileBadges(savedGrants, J({{"order", order}, {"hidden", order}}));
        check(allHidden["display"].empty() && allHidden["owned"].size() == 7,
              "all badges may be hidden without losing membership or grants");
        check(!validBadgeDisplay(J({{"order", order}, {"hidden", Json::Array{"unknown"}}}), savedGrants),
              "unknown hidden ID is rejected");
        check(!validBadgeDisplay(J({{"order", order}, {"hidden", Json::Array{"member", "member"}}}), savedGrants),
              "duplicate hidden ID is rejected");
        send(J({{"type", "persona.assign"}, {"agent", "claude"}, {"id", "external"}}));
        check(app.settings_["persona"]["claude"].str() == "external" && !app.personaPrompt(AgentKind::Claude).empty(),
              "external preset is assignable and reaches the prompt");
        send(J({{"type", "persona.delete"}, {"id", "external"}}));
        check(app.settings_["persona"]["claude"].str() == "external", "deleting a preset cannot clear its assignment");
        app.settings_["personas"].push(J({{"id", "external"}, {"name", "自訂覆寫"}, {"prompt", "原自訂內容。"}}));
        send(J({{"type", "persona.save"},
                {"persona", J({{"id", "external"}, {"name", "自訂覆寫"}, {"prompt", "更新自訂內容。"}})}}));
        check(app.settings_["personas"].size() == 1 &&
                  app.personaPrompt(AgentKind::Claude).find("更新自訂內容") != std::string::npos,
              "editing a saved override keeps its ID and assignment");
        const Json beforePersona = app.settings_;
        const std::wstring settingsTemp = util::joinPath(util::appDataDir(), L"settings.json.tmp");
        HANDLE blockedPersona = CreateFileW(settingsTemp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        check(blockedPersona != INVALID_HANDLE_VALUE, "persona failure fixture holds the settings temp file");
        if (blockedPersona != INVALID_HANDLE_VALUE) {
            send(J({{"type", "persona.save"}, {"persona", J({{"name", "自訂風格"}, {"prompt", "明確說明風險。"}})}}));
            check(app.settings_ == beforePersona, "failed persona save preserves assignment and library");
            send(J({{"type", "persona.assign"}, {"agent", "claude"}, {"id", "none"}}));
            check(app.settings_ == beforePersona, "failed assignment keeps the previous persona");
            CloseHandle(blockedPersona);
            DeleteFileW(settingsTemp.c_str());
        }
        send(J({{"type", "persona.save"},
                {"persona", J({{"id", "own"}, {"name", "自訂風格"}, {"prompt", "明確說明風險。"}})}}));
        check(!findPersona(app.settings_["personas"], "own").isNull(), "custom persona saved");
        blockedPersona = CreateFileW(settingsTemp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (blockedPersona != INVALID_HANDLE_VALUE) {
            send(J({{"type", "persona.delete"}, {"id", "own"}}));
            check(!findPersona(app.settings_["personas"], "own").isNull(),
                  "failed deletion preserves the custom persona");
            CloseHandle(blockedPersona);
            DeleteFileW(settingsTemp.c_str());
        }
        const Json otherProfiles = app.settings_["profiles"]["codex"];
        send(J({{"type", "profile.set"},
                {"who", "gemini"},
                {"name", "Gemini reviewer"},
                {"bannerColor", "#7799aa"},
                {"joinedAt", "2026-10-27"}}));
        check(app.nameOf(AgentKind::Gemini) == "Gemini reviewer" &&
                  app.settings_["profiles"]["gemini"]["bannerColor"].str() == "#7799aa" &&
                  app.settings_["profiles"]["codex"] == otherProfiles,
              "Gemini profile is independent of Codex");
        send(J({{"type", "persona.save"},
                {"assignTo", "gemini"},
                {"persona", J({{"id", "gemini-own"},
                               {"name", "Gemini style"},
                               {"agent", "gemini"},
                               {"address", "Reviewer"},
                               {"prompt", "GEMINI_PERSONA_MARKER"}})}}));
        check(app.personaPrompt(AgentKind::Gemini).find("GEMINI_PERSONA_MARKER") != std::string::npos &&
                  app.guardRulesFor(*app.find("dm-codex"), AgentKind::Gemini).personaName == "Gemini style",
              "Gemini assignment reaches instructions and guard identity");
        send(J({{"type", "profile.image"}, {"who", "gemini"}, {"kind", "avatar"}}));
        const auto geminiToken = app.pendingImage_["token"];
        check(!geminiToken.str().empty(), "Gemini avatar opens a normal crop transaction");
        send(J({{"type", "image.commit"}, {"token", geminiToken}, {"data", kPng}}));
        const auto geminiAvatar = app.settings_["profiles"]["gemini"]["avatar"].str();
        check(!geminiAvatar.empty() && util::fileExists(util::joinPath(App::mediaDir(), util::widen(geminiAvatar))),
              "Gemini cropped avatar is saved in managed media");
        const Json beforeGemini = app.settings_;
        blockedPersona = CreateFileW(settingsTemp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        check(blockedPersona != INVALID_HANDLE_VALUE, "Gemini settings failure fixture can lock atomic destination");
        if (blockedPersona != INVALID_HANDLE_VALUE) {
            send(J({{"type", "profile.set"}, {"who", "gemini"}, {"name", "must not persist"}}));
            send(J({{"type", "persona.assign"}, {"agent", "gemini"}, {"id", "none"}}));
            check(app.settings_ == beforeGemini, "failed Gemini profile and persona saves roll back in memory");
            CloseHandle(blockedPersona);
            DeleteFileW(settingsTemp.c_str());
        }
        send(J({{"type", "persona.delete"}, {"id", "gemini-own"}}));
        check(app.settings_["persona"]["gemini"].str() == "none", "deleting Gemini persona clears its assignment");
        send(J({{"type", "persona.assign"}, {"agent", "gemini"}, {"id", "external"}}));
        const Json activityFixture = tokenActivity(Json::object(), activityToday(), 180);
        for (const auto& day : activityFixture["days"].items())
            app.usageHistory_[day["date"].str()]["claude"] = J({{"input", 1}});
        app.recordUsage("api", J({{"inputTokens", 10}, {"outputTokens", 5}}));
        check(app.usageHistory_.size() == 180, "recording usage preserves more than 120 historical dates");
        const std::wstring usageTemp = util::joinPath(util::appDataDir(), L"usage.json.tmp");
        HANDLE blockedUsage = CreateFileW(usageTemp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        check(blockedUsage != INVALID_HANDLE_VALUE, "usage failure fixture holds its temp file");
        if (blockedUsage != INVALID_HANDLE_VALUE) {
            check(!app.saveUsage() && app.usageDirty_, "failed usage save remains dirty for retry");
            CloseHandle(blockedUsage);
            DeleteFileW(usageTemp.c_str());
        }
        check(app.saveUsage() && !app.usageDirty_, "usage persistence succeeds after the failure clears");
        app.recordUsage("api", J({{"inputTokens", 7}}));
        auto lastCrop = [&]() {
            for (auto it = events.rbegin(); it != events.rend(); ++it)
                if ((*it)["type"].str() == "image.crop") return *it;
            return Json::object();
        };
        auto exists = [&](const std::string& file) {
            return util::fileExists(util::joinPath(App::mediaDir(), util::widen(file)));
        };
        send(J({{"type", "profile.image"}, {"who", "claude"}, {"kind", "avatar"}}));
        Json crop = lastCrop();
        check(!crop["token"].str().empty() && exists(crop["file"].str()), "picker creates pending crop");
        check(app.settings_["profiles"]["claude"]["avatar"].str().empty(), "picking does not replace the avatar");
        send(J({{"type", "image.cancel"}, {"token", crop["token"]}}));
        check(!exists(crop["file"].str()) && app.pendingImage_["token"].str().empty(),
              "cancel removes temporary image");
        send(J({{"type", "profile.image"}, {"who", "claude"}, {"kind", "avatar"}}));
        crop = lastCrop();
        send(J({{"type", "image.commit"}, {"token", "wrong"}, {"data", kPng}}));
        check(app.settings_["profiles"]["claude"]["avatar"].str().empty(), "unissued token cannot save");
        send(J({{"type", "image.commit"}, {"token", crop["token"]}, {"data", "invalid"}}));
        check(exists(crop["file"].str()) && !events.back()["ok"].boolean(), "invalid crop leaves the original intact");
        send(J({{"type", "image.commit"}, {"token", crop["token"]}, {"who", "user"}, {"data", kPng}}));
        std::string avatar = app.settings_["profiles"]["claude"]["avatar"].str();
        check(exists(avatar) && !exists(crop["file"].str()), "save creates permanent PNG and removes temporary file");
        check(app.settings_["profiles"]["user"]["avatar"].str().empty(), "crop target cannot be changed by the page");
        send(J({{"type", "profile.image"}, {"who", "claude"}, {"kind", "avatar"}}));
        crop = lastCrop();
        send(J({{"type", "image.cancel"}, {"token", crop["token"]}}));
        check(exists(avatar) && app.settings_["profiles"]["claude"]["avatar"].str() == avatar,
              "cancel replacement keeps previous avatar");
        send(J({{"type", "profile.image"}, {"who", "claude"}, {"kind", "avatar"}}));
        send(J({{"type", "image.commit"}, {"token", lastCrop()["token"]}, {"data", kPng}}));
        const std::string previousAvatar = avatar;
        avatar = app.settings_["profiles"]["claude"]["avatar"].str();
        check(exists(avatar) && avatar != previousAvatar && !exists(previousAvatar),
              "successful replacement retires only the previous avatar");
        for (const char* who : {"user", "codex", "local1", "local2"}) {
            send(J({{"type", "profile.image"}, {"who", who}, {"kind", "avatar"}}));
            send(J({{"type", "image.commit"}, {"token", lastCrop()["token"]}, {"data", kPng}}));
            check(exists(app.settings_["profiles"][who]["avatar"].str()),
                  "user and every AI can save their own avatar");
        }
        auto failedMetadata = [&](Conversation* target) {
            app.saveMeta();
            const std::wstring store =
                util::joinPath(util::appDataDir(), target ? L"conversations.json" : L"settings.json");
            std::string persisted;
            check(util::readFile(store, persisted), "metadata baseline is readable");
            const std::string old = target ? target->avatar : app.settings_["profiles"]["claude"]["avatar"].str();
            HANDLE blocked =
                CreateFileW((store + L".tmp").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            check(blocked != INVALID_HANDLE_VALUE, "metadata failure fixture acquires its lock");
            if (blocked == INVALID_HANDLE_VALUE) return;
            send(target ? J({{"type", "conv.image"}, {"id", target->id}})
                        : J({{"type", "profile.image"}, {"who", "claude"}, {"kind", "avatar"}}));
            const Json pending = lastCrop();
            send(J({{"type", "image.commit"}, {"token", pending["token"]}, {"data", kPng}}));
            check(events.back()["type"].str() == "image.result" && !events.back()["ok"].boolean(),
                  "metadata failure is reported as a failed save");
            check((target ? target->avatar : app.settings_["profiles"]["claude"]["avatar"].str()) == old && exists(old),
                  "metadata failure preserves old image and in-memory reference");
            check(app.pendingImage_["token"].str() == pending["token"].str() && exists(pending["file"].str()),
                  "metadata failure keeps the crop available for retry");
            std::string disk;
            check(util::readFile(store, disk) && disk == persisted, "metadata failure leaves persisted data unchanged");
            if (target) {
                send(J({{"type", "conv.avatar.remove"}, {"id", target->id}}));
                check(target->avatar == old && exists(old), "failed group avatar removal also keeps the original");
            } else {
                send(J({{"type", "profile.set"}, {"who", "claude"}, {"avatar", nullptr}}));
                check(app.settings_["profiles"]["claude"]["avatar"].str() == old && exists(old),
                      "failed profile avatar removal also keeps the original");
            }
            CloseHandle(blocked);
            DeleteFileW((store + L".tmp").c_str());
            send(J({{"type", "image.commit"}, {"token", pending["token"]}, {"data", kPng}}));
            check(events.back()["type"].str() == "image.result" && events.back()["ok"].boolean() &&
                      app.pendingImage_["token"].str().empty(),
                  "same crop can save once metadata becomes writable");
        };
        failedMetadata(nullptr);
        avatar = app.settings_["profiles"]["claude"]["avatar"].str();
        send(J({{"type", "profile.set"}, {"who", "claude"}, {"joinedAt", "2024-02-29"}}));
        check(app.settings_["profiles"]["claude"]["joinedAt"].str() == "2024-02-29", "valid leap date saves");
        for (const char* date : {"2025-02-29", "1900-02-29", "2026-04-31", "0000-01-01", "bad"}) {
            send(J({{"type", "profile.set"}, {"who", "claude"}, {"joinedAt", date}}));
            check(app.settings_["profiles"]["claude"]["joinedAt"].str() == "2024-02-29",
                  "invalid calendar date rejected");
        }
        const std::string oldName = app.settings_["profiles"]["claude"]["name"].str();
        send(J({{"type", "profile.set"}, {"who", "claude"}, {"name", "must not save"}, {"joinedAt", "bad"}}));
        check(app.settings_["profiles"]["claude"]["name"].str() == oldName, "rejected profile update is atomic");
        send(J({{"type", "profile.set"}, {"who", "claude"}, {"joinedAt", nullptr}}));
        check(!app.settings_["profiles"]["claude"].has("joinedAt"), "clear date restores model default");
        Conversation* group = nullptr;
        for (auto& c : app.convs_)
            if (c->kind == "group") {
                group = c.get();
                break;
            }
        check(group != nullptr, "default group exists");
        if (!group) return;
        const std::string groupId = group->id;
        send(J({{"type", "conv.image"}, {"id", groupId}}));
        send(J({{"type", "image.commit"}, {"token", lastCrop()["token"]}, {"data", kPng}}));
        failedMetadata(group);
        check(!app.settings_["profiles"].has(""), "group avatar never creates an empty profile slot");
        const std::string groupFile = group->avatar;
        check(exists(groupFile) && app.convMeta(*group)["avatar"].str() == groupFile, "group avatar reaches metadata");
        const std::wstring convTemp = util::joinPath(util::appDataDir(), L"conversations.json.tmp");
        HANDLE blockedDelete = CreateFileW(convTemp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        check(blockedDelete != INVALID_HANDLE_VALUE, "group deletion failure fixture acquires its lock");
        if (blockedDelete != INVALID_HANDLE_VALUE) {
            send(J({{"type", "conv.delete"}, {"id", groupId}}));
            const bool kept = app.find(groupId) != nullptr;
            check(kept && exists(groupFile), "failed group deletion keeps the conversation and avatar");
            CloseHandle(blockedDelete);
            DeleteFileW(convTemp.c_str());
            if (!kept) return;
        }
        send(J({{"type", "profile.set"}, {"who", "claude"}, {"joinedAt", "2024-02-29"}}));
        app.shutdown();
        App reloaded([](const std::string&) {});
        check(reloaded.groupOrder() == reorderedGroups && reloaded.settings_["lastDm"].str() == "dm-codex",
              "group navigation order and the last private conversation survive restart");
        check(reloaded.badgeState()["owned"].size() == 7 && reloaded.settings_["badgeDisplay"] == savedBadgeDisplay,
              "ownership and display preferences survive restart");
        check(reloaded.usageHistory_[activityToday()]["api"]["input"].i64() == 17,
              "shutdown flush preserves activity without waiting for the save timer");
        check(reloaded.settings_["persona"]["claude"].str() == "external" &&
                  !findPersona(reloaded.settings_["personas"], "own").isNull(),
              "external assignment and custom persona survive restart");
        check(reloaded.nameOf(AgentKind::Gemini) == "Gemini reviewer" &&
                  reloaded.settings_["persona"]["gemini"].str() == "external" &&
                  reloaded.settings_["profiles"]["gemini"]["joinedAt"].str() == "2026-10-27",
              "Gemini name, date and persona assignment survive restart");
        check(reloaded.settings_["profiles"]["claude"]["avatar"].str() == avatar && exists(avatar),
              "AI avatar survives restart");
        check(reloaded.settings_["profiles"]["claude"]["joinedAt"].str() == "2024-02-29",
              "custom date survives restart");
        check(reloaded.find(groupId)->avatar == groupFile && exists(groupFile), "group avatar survives restart");
        reloaded.onUiMessage(J({{"type", "conv.avatar.remove"}, {"id", groupId}}).dump());
        check(reloaded.find(groupId)->avatar.empty() && !exists(groupFile), "group can restore default avatar");
        reloaded.pickImage = [&] { return source; };
        reloaded.onUiMessage(J({{"type", "conv.image"}, {"id", groupId}}).dump());
        reloaded.onUiMessage(
            J({{"type", "image.commit"}, {"token", reloaded.pendingImage_["token"]}, {"data", kPng}}).dump());
        const std::string removedAvatar = reloaded.find(groupId)->avatar;
        reloaded.onUiMessage(J({{"type", "conv.delete"}, {"id", groupId}}).dump());
        check(!reloaded.find(groupId) && !exists(removedAvatar), "successful group deletion removes its avatar");
        reloaded.onUiMessage(J({{"type", "profile.image"}, {"who", "claude"}, {"kind", "avatar"}}).dump());
        const std::string temporary = reloaded.pendingImage_["file"].str();
        reloaded.shutdown();
        check(!exists(temporary), "closing with an open crop releases its temporary file");
        App emptyRail([](const std::string&) {});
        emptyRail.uiReady_ = true;
        const Json removeGroups = emptyRail.groupOrder();
        for (const auto& id : removeGroups.items())
            emptyRail.onUiMessage(J({{"type", "conv.delete"}, {"id", id}}).dump());
        check(emptyRail.groupOrder().empty(), "deleting the last group does not recreate a placeholder group");
        emptyRail.shutdown();
        App emptyReload([](const std::string&) {});
        check(emptyReload.groupOrder().empty() && emptyReload.find("dm-claude") && emptyReload.find("dm-codex"),
              "empty group navigation survives restart while private conversations remain available");
    }
};

int main() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::wstring root = util::joinPath(temp, util::widen("cca-profiles-" + util::newId()));
    util::ensureDir(root);
    SetEnvironmentVariableW(L"CCA_DATA_DIR", util::joinPath(root, L"data").c_str());
    ProfileTestAccess::run(root);
    std::printf("%d profile checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
