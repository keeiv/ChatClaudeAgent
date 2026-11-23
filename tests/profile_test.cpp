// Settings and navigation persistence without live model backends: picture validation, data written by
// older versions, group order transactions, usage history and restart.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <vector>

#include "app.hpp"
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

std::string today() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", t.wYear, t.wMonth, t.wDay);
    return buf;
}
}  // namespace

struct ProfileTestAccess {
    static void run(const std::wstring& root) {
        (void)root;
        const std::string png = decodePngDataUrl(kPng);
        check(!png.empty(), "valid PNG is fully decoded");
        check(decodePngDataUrl("data:image/png;base64,iVBORw0KGgo=").empty(), "signature alone is not a valid PNG");
        check(decodePngDataUrl("data:image/jpeg;base64,aGVsbG8=").empty(), "wrong MIME rejected");
        check(decodePngDataUrl(std::string(kPng).substr(0, 40) + "!===").empty(), "invalid base64 rejected");
        check(decodePngDataUrl(std::string(3 * 1024 * 1024 + 1, 'A')).empty(), "oversized payload rejected");
        check(!isMediaName("../settings.json") && !isMediaName("photo.png:stream") &&
                  !isMediaName(std::string("x\0.png", 6)),
              "media names cannot escape their folder");
        check(isMediaName("screenshot-example.png"), "managed media name accepted");

        // Data written by 1.0.2-oss: themes, personas, profile pictures and badges that this version no longer has.
        const std::wstring data = util::appDataDir();
        const std::wstring stale = util::joinPath(App::mediaDir(), L"tmp-stale.png");
        check(util::writeFileAtomic(stale, png), "fixture abandoned crop saved");
        const Json legacy =
            J({{"theme", "halloween-dark"},
               {"persona", J({{"claude", "teacher"}, {"local1", "teacher"}})},
               {"personas", Json::Array{J({{"id", "teacher"}, {"name", "老師"}, {"prompt", "x"}})}},
               {"profiles", J({{"claude", J({{"name", "撰寫者"}, {"avatar", "claude-avatar-1.png"}})}})},
               {"badgeDisplay", J({{"order", Json::Array{"member"}}, {"hidden", Json::array()}})},
               {"humanize", J({{"enabled", false}, {"extra", ""}})}});
        check(util::writeFileAtomic(util::joinPath(data, L"settings.json"), legacy.dump()), "legacy settings saved");
        std::vector<Json> events;
        App app([&](const std::string& text) { events.push_back(Json::parse(text)); });
        check(!util::fileExists(stale), "startup cleans a crop abandoned by an older version");
        check(app.settings_["theme"].str() == "cca", "a theme that no longer exists falls back to the default look");
        check(app.nameOf(AgentKind::Claude) == "Claude" && app.nameOf(AgentKind::Local1) == "本地模型 1",
              "saved display names and personas no longer rename the members");
        check(!app.settings_["humanize"]["enabled"].boolean(true), "settings that still exist keep their values");
        check(app.settings_["personas"].size() == 1, "removed settings are left in the file, not deleted");
        AgentConfig cfg = app.configFor(*app.find("dm-claude"), AgentKind::Claude);
        check(cfg.instructions.find("老師") == std::string::npos, "an old persona never reaches the instructions");
        app.uiReady_ = true;
        auto send = [&](Json message) { app.onUiMessage(message.dump()); };

        const Json initialGroups = app.groupOrder();
        check(initialGroups.size() == 1, "fresh installations keep one starter group");
        send(J({{"type", "conv.create"}, {"name", "Navigation B"}}));
        send(J({{"type", "conv.create"}, {"name", "Navigation C"}}));
        const Json createdGroups = app.groupOrder();
        check(createdGroups.size() == 3 && createdGroups.at(0) == initialGroups.at(0),
              "new groups append without moving existing groups");
        Json reorderedGroups = createdGroups;
        std::reverse(reorderedGroups.items().begin(), reorderedGroups.items().end());
        send(J({{"type", "groups.reorder"}, {"order", reorderedGroups}, {"requestToken", "nav-saved"}}));
        check(app.groupOrder() == reorderedGroups && events.back()["ok"].boolean() &&
                  events.back()["requestToken"].str() == "nav-saved",
              "group order is saved and acknowledged by the host");
        const Json beforeNavigation = app.settings_;
        const std::wstring navigationTemp = util::joinPath(data, L"settings.json.tmp");
        HANDLE navigationLock =
            CreateFileW(navigationTemp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        check(navigationLock != INVALID_HANDLE_VALUE, "navigation save failure fixture holds the settings temp file");
        if (navigationLock != INVALID_HANDLE_VALUE) {
            send(J({{"type", "groups.reorder"}, {"order", createdGroups}, {"requestToken", "nav-failed"}}));
            check(app.settings_ == beforeNavigation && app.groupOrder() == reorderedGroups,
                  "failed navigation save retains the entire settings object and original order");
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
                  "stale, incomplete, duplicate, private or unknown IDs cannot change the order");
        }
        app.settings_["groupOrder"] = Json::Array{"removed", createdGroups.at(1), createdGroups.at(1), "dm-claude"};
        check(app.groupOrder().size() == 3 && app.groupOrder().at(0) == createdGroups.at(1),
              "obsolete saved IDs are filtered and missing existing groups are appended");
        app.settings_["groupOrder"] = reorderedGroups;
        send(J({{"type", "conv.open"}, {"id", "dm-codex"}}));
        send(J({{"type", "conv.open"}, {"id", createdGroups.at(0)}}));
        check(app.settings_["lastDm"].str() == "dm-codex" && app.groupOrder() == reorderedGroups,
              "opening a group preserves the last private conversation and fixed group positions");
        app.sendBoot();
        bool bootClean = false;
        for (const auto& event : events)
            if (event["type"].str() == "boot")
                bootClean = !event.has("badges") && !event.has("personas") && !event.has("modelDates");
        check(bootClean, "boot no longer carries badges, personas or model dates");

        for (int i = 0; i < 180; ++i)
            app.usageHistory_["2025-" + std::to_string(1000 + i)]["claude"] = J({{"input", 1}});
        app.recordUsage("api", J({{"inputTokens", 10}, {"outputTokens", 5}}));
        check(app.usageHistory_.size() == 181, "recording usage preserves older dates");
        const std::wstring usageTemp = util::joinPath(data, L"usage.json.tmp");
        HANDLE blockedUsage = CreateFileW(usageTemp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        check(blockedUsage != INVALID_HANDLE_VALUE, "usage failure fixture holds its temp file");
        if (blockedUsage != INVALID_HANDLE_VALUE) {
            check(!app.saveUsage() && app.usageDirty_, "failed usage save remains dirty for retry");
            CloseHandle(blockedUsage);
            DeleteFileW(usageTemp.c_str());
        }
        check(app.saveUsage() && !app.usageDirty_, "usage persistence succeeds after the failure clears");
        app.recordUsage("api", J({{"inputTokens", 7}}));

        Conversation* group = nullptr;
        for (auto& c : app.convs_)
            if (c->kind == "group") {
                group = c.get();
                break;
            }
        check(group != nullptr, "default group exists");
        if (!group) return;
        const std::string groupId = group->id;
        const std::wstring convTemp = util::joinPath(data, L"conversations.json.tmp");
        HANDLE blockedDelete = CreateFileW(convTemp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        check(blockedDelete != INVALID_HANDLE_VALUE, "group deletion failure fixture acquires its lock");
        if (blockedDelete != INVALID_HANDLE_VALUE) {
            send(J({{"type", "conv.delete"}, {"id", groupId}}));
            check(app.find(groupId) != nullptr, "failed group deletion keeps the conversation");
            CloseHandle(blockedDelete);
            DeleteFileW(convTemp.c_str());
        }
        app.shutdown();
        App reloaded([](const std::string&) {});
        check(reloaded.groupOrder() == reorderedGroups && reloaded.settings_["lastDm"].str() == "dm-codex",
              "group order and the last private conversation survive restart");
        check(reloaded.usageHistory_[today()]["api"]["input"].i64() == 17,
              "shutdown flush preserves usage without waiting for the save timer");
        reloaded.onUiMessage(J({{"type", "conv.delete"}, {"id", groupId}}).dump());
        check(!reloaded.find(groupId), "a group can be deleted");
        reloaded.shutdown();
        App emptyRail([](const std::string&) {});
        emptyRail.uiReady_ = true;
        const Json removeGroups = emptyRail.groupOrder();
        for (const auto& id : removeGroups.items())
            emptyRail.onUiMessage(J({{"type", "conv.delete"}, {"id", id}}).dump());
        check(emptyRail.groupOrder().empty(), "deleting the last group does not recreate a placeholder group");
        emptyRail.shutdown();
        App emptyReload([](const std::string&) {});
        check(emptyReload.groupOrder().empty() && emptyReload.find("dm-claude") && emptyReload.find("dm-codex"),
              "no groups survives restart while private conversations remain available");
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
