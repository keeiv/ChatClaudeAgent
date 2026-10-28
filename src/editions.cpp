// Edition switches: a special edition first copies what it does not support into a vault, and the ordinary
// edition restores that vault when the user comes back. Nothing here deletes a conversation or a message.
#include "editions.hpp"

#include <windows.h>

#include "app.hpp"
#include "updater.hpp"
#include "util.hpp"
#include "version.hpp"

namespace {
// The settings and conversation fields a special edition may set aside; the ordinary edition puts them back.
const char* const kSettings[] = {"theme", "persona", "extensions"};
// Only the fields a vault holds are put back.
void restoreFields(Conversation& c, const Json& saved) {
    if (saved.has("daily")) c.daily = c.kind != "group" && saved["daily"].boolean();
    if (saved.has("localMembers")) {
        c.localMembers.clear();
        for (const auto& l : saved["localMembers"].items())
            if (l.str() == "local1" || l.str() == "local2") c.localMembers.push_back(l.str());
    }
    if (saved["adaptive"].isObject()) c.adaptive = saved["adaptive"];
    if (saved.has("claudeModel")) c.claudeModel = saved["claudeModel"].str();
    if (saved.has("claudeEffort")) c.claudeEffort = saved["claudeEffort"].str();
    if (saved.has("codexModel")) c.codexModel = saved["codexModel"].str();
    if (saved.has("codexEffort")) c.codexEffort = saved["codexEffort"].str();
    if (saved.has("regressionGuard")) c.regressionGuard = saved["regressionGuard"].boolean() && !c.testCommand.empty();
    if (saved.has("mode")) c.mode = saved["mode"].str();
}
std::wstring vaultPath() {
    const auto dir = util::joinPath(util::appDataDir(), L"editions");
    util::ensureDir(dir);
    return util::joinPath(dir, L"ordinary-vault.json");
}
}  // namespace

bool specialEdition() {
    return CCA_EDITION[0] != 0;
}
const char* editionId() {
    return CCA_EDITION;
}
const char* editionName() {
    return CCA_EDITION_NAME;
}

void App::editionTransition(const std::string& now) {
    const auto markerPath = util::joinPath(util::appDataDir(), L"edition.json");
    std::string text;
    Json marker;
    if (util::readFile(markerPath, text)) marker = Json::parse(text);
    // Data written by builds before 1.0.1-astra has no marker and belongs to the ordinary edition.
    const std::string previous = marker["edition"].str();
    const auto vault = vaultPath();
    bool markerOk = true;
    if (now.empty() && !previous.empty()) {
        std::string raw;
        Json saved = util::readFile(vault, raw) ? Json::parse(raw) : Json();
        if (saved.isObject()) {
            for (const char* key : kSettings) {
                if (saved["settings"].has(key)) settings_[key] = saved["settings"][key];
                else settings_.erase(key);
            }
            int restored = 0;
            for (auto& c : convs_) {
                if (!saved["conversations"].has(c->id)) continue;
                restoreFields(*c, saved["conversations"][c->id]);
                ++restored;
            }
            const bool ok =
                saveConversations() &&
                util::writeFileAtomic(util::joinPath(util::appDataDir(), L"settings.json"), settings_.dump());
            const auto archived = util::joinPath(util::joinPath(util::appDataDir(), L"editions"),
                                                 util::widen("ordinary-vault-restored-" + util::newId() + ".json"));
            if (ok && MoveFileExW(vault.c_str(), archived.c_str(), MOVEFILE_WRITE_THROUGH)) {
                editionNotice_ = "已從 " + saved["to"]["name"].str(previous) +
                                 " 回到普通版本：還原了主題、性格、插件與 " + std::to_string(restored) +
                                 " 個對話的模式設定。";
            } else {
                // Keep the vault and the old marker so the next start tries again; nothing was lost.
                markerOk = false;
                editionNotice_ =
                    "普通版本的設定還沒能完整還原；保存的資料仍在 editions\\ordinary-vault.json，下次啟動會再試一次。";
            }
        }
    }
    if (markerOk && (previous != now || marker["version"].str() != CCA_VERSION))
        util::writeFileAtomic(markerPath,
                              J({{"edition", now}, {"version", CCA_VERSION}, {"at", util::nowMs()}}).dump());
}

