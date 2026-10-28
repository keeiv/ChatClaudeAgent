// UI-thread orchestration of release checks and the handoff to the independent update broker.
#include "app.hpp"

#include <thread>

#include "updater.hpp"
#include "util.hpp"
#include "version.hpp"

namespace {
// The folder is rescanned this often; only a changed fingerprint triggers a full inspection.
constexpr long long kInboxScanMs = 5000;
// Time between announcing an automatic install and closing CCA, so a draft or a new task can intervene.
constexpr long long kAutoInstallDelayMs = 10000;
}  // namespace

bool App::updateIdle() const {
    if (!qualityBusy_.empty()) return false;
    for (const auto& c : convs_)
        if (anyBusy(*c) || c->group.active || !c->preparing.empty() || c->queueArmed) return false;
    for (const auto& worker : controls_)
        if (worker->busy()) return false;
    return true;
}

void App::tickUpdate(long long now) {
    if (!uiReady_ || updateBusy_ || updateInstalling_) return;
    if (now - updateScanAt_ >= kInboxScanMs) {
        updateScanAt_ = now;
        const auto stamp = updater::inboxStamp(updater::inboxDir());
        if (stamp != updateStamp_) {
            // A new or replaced package may fix what an earlier attempt rejected.
            updateStamp_ = stamp;
            updateAutoAt_ = 0;
            updateSkip_.clear();
            handleUpdate(J({{"type", "update.check"}}));
            return;
        }
    }
    const auto version = update_["version"].str();
    const bool wanted =
        settings_["updateAuto"].boolean(true) && update_["available"].boolean() && version != updateSkip_;
    if (!wanted) {
        if (updateAutoAt_) {
            updateAutoAt_ = 0;
            update_["status"] = update_["available"].boolean() ? "available" : update_["status"];
            handleUpdate(J({{"type", "update.get"}}));
        }
        return;
    }
    if (!updateIdle()) {
        if (update_["status"].str() != "waiting") {
            updateAutoAt_ = 0;
            update_["status"] = "waiting";
            handleUpdate(J({{"type", "update.get"}}));
        }
        return;
    }
    if (!updateAutoAt_) {
        updateAutoAt_ = now + kAutoInstallDelayMs;
        update_["status"] = "scheduled";
        update_["scheduledAt"] = updateAutoAt_;
        toast("info", "本機更新資料夾有新安裝包 " + version + "，10 秒後自動更新並重新開啟；可在設定取消");
        handleUpdate(J({{"type", "update.get"}}));
        return;
    }
    if (now >= updateAutoAt_) {
        updateAutoAt_ = 0;
        handleUpdate(J({{"type", "update.install"}}));
    }
}

bool App::handleUpdate(const Json& m) {
    const auto type = m["type"].str();
    if (!util::startsWith(type, "update.")) return false;
    auto report = [&] {
        Json state = update_;
        state["type"] = "update.state";
        state["current"] = CCA_VERSION;
        state["edition"] = CCA_EDITION;
        state["editionName"] = updater::editionName(CCA_EDITION);
        state["busy"] = updateBusy_;
        state["installing"] = updateInstalling_;
        state["folder"] = util::narrow(updater::inboxDir());
        state["auto"] = settings_["updateAuto"].boolean(true);
        state["skipped"] = !updateSkip_.empty() && updateSkip_ == update_["version"].str();
        // The remote channel stays visible but cannot be enabled until a release site exists.
        state["remote"] = J({{"enabled", false}, {"source", settings_["updateSource"]}});
        if (state["status"].str().empty()) state["status"] = "unchecked";
        send(state);
    };
    if (type == "update.get") {
        report();
        return true;
    }
    if (type == "update.open") {
        if (openExternal) openExternal(updater::inboxDir());
        return true;
    }
    if (type == "update.haiku") {
        // Only a package the folder scan listed, and only once it matches its manifest.
        std::wstring apk;
        for (const auto& item : update_["android"].items())
            if (item["path"].str() == m["path"].str()) apk = util::widen(item["path"].str());
        std::string problem;
        if (apk.empty()) problem = "資料夾裡沒有這個 Android 安裝包；請重新掃描";
        else if (!openHaiku) problem = "手機模擬只在桌面版提供";
        else if (updater::verifyAndroidPackage(apk, problem)) {
            problem.clear();
            openHaiku(apk, problem);
        }
        if (!problem.empty()) toast("error", "無法開啟 Haiku 手機模擬：" + problem);
        return true;
    }
    if (type == "update.source") {
        toast("info", "遠端更新尚未啟用；請把安裝包放進本機更新資料夾");
        report();
        return true;
    }
    if (type == "update.auto") {
        Json previous = settings_;
        settings_["updateAuto"] = m["auto"].boolean();
        if (!util::writeFileAtomic(util::joinPath(util::appDataDir(), L"settings.json"), settings_.dump())) {
            settings_ = previous;
            toast("error", "自動更新設定未能保存，原設定已保留");
        }
        updateAutoAt_ = 0;
        if (update_["status"].str() == "scheduled") update_["status"] = "available";
        report();
        return true;
    }
    if (type == "update.cancel") {
        if (update_["available"].boolean()) {
            updateSkip_ = update_["version"].str();
            updateAutoAt_ = 0;
            update_["status"] = "available";
            toast("info", "已取消這次自動更新；可隨時手動更新，或放入新的安裝包");
        }
        report();
        return true;
    }
    if (updateBusy_) {
        toast("info", "更新檢查或下載進行中");
        return true;
    }
    if (type != "update.check" && type != "update.install") return true;
    bool install = type == "update.install";
    // Switching to a special edition, or back to the ordinary one, is always the user's explicit choice.
    Json chosen;
    if (install && m.has("choice")) {
        const auto wanted = m["choice"].str() == "normal" ? std::string() : m["choice"].str();
        for (const auto& option : update_["choices"].items())
            if (option["edition"].str() == wanted) chosen = option;
        if (chosen.isNull()) {
            toast("info", "資料夾裡沒有這個版本的安裝包；請重新掃描");
            return true;
        }
    }
    if (install && ((chosen.isNull() && !update_["available"].boolean()) || !updateIdle())) {
        toast("info", "先檢查較新版本，並等候所有任務、驗證與待送佇列停止後再更新");
        return true;
    }
    updateBusy_ = true;
    updateInstalling_ = install;
    update_["status"] = install ? "downloading" : "checking";
    report();
    auto life = std::weak_ptr<int>(lifetime_);
    auto checked = update_;
    if (!chosen.isNull()) {
        for (const char* key : {"version", "notes", "file", "source", "package", "edition"}) checked[key] = chosen[key];
        checked["available"] = true;
    }
    checked["edition"] = checked["edition"].str();
    checked["theme"] = settings_["theme"];
    checked["motion"] = settings_["visual"]["motion"].boolean(true);
    unsigned generation = ++updateGeneration_;
    const auto exe = updater::executable();
    const auto folder = updater::inboxDir();
    const auto flavor = hostFlavor;
    const bool switching = !chosen.isNull();
    std::thread([this, life, folder, checked, exe, flavor, generation, install, switching] {
        Json result = install ? updater::prepare(checked, exe)
                              : updater::inspectFolder(folder, CCA_VERSION, exe, flavor, CCA_EDITION);
        postToUi([this, life, result, generation, install, switching] {
            if (life.expired() || generation != updateGeneration_) return;
            updateBusy_ = false;
            updateInstalling_ = false;
            if (!install) update_ = result;
            else if (result["ok"].boolean() && updateIdle()) {
                // Flush explicitly and check every write before authorizing the broker to replace this EXE.
                bool saved =
                    saveConversations() &&
                    util::writeFileAtomic(util::joinPath(util::appDataDir(), L"settings.json"), settings_.dump());
                for (auto& c : convs_) {
                    Json messages = Json::array();
                    for (const auto& message : c->messages) messages.push(message);
                    const auto path = util::joinPath(util::appDataDir(), L"messages\\" + util::widen(c->id + ".json"));
                    saved = util::writeFileAtomic(path, messages.dump()) && saved;
                    saved = saveDesk(*c, c->desk["queue"].isArray()
                                             ? c->desk
                                             : J({{"queue", Json::array()}, {"contexts", Json::array()}})) &&
                            saved;
                }
                saved =
                    util::writeFileAtomic(util::joinPath(util::appDataDir(), L"usage.json"), usageHistory_.dump()) &&
                    saved;
                std::string error;
                if (saved && updater::launch(result, error)) {
                    updateInstalling_ = true;
                    if (windowAction) windowAction("close");
                    return;
                }
                update_["error"] = saved ? error : "資料尚未成功保存，已取消更新；CCA 保持開啟";
                update_["status"] = "error";
            } else {
                update_["status"] = "error";
                update_["error"] = result["error"].str("目前已有工作執行，已取消更新");
            }
            if (!update_["ok"].boolean()) update_["status"] = "error";
            // A failed install is not retried automatically until the folder changes.
            if (install && update_["status"].str() == "error" && !switching) updateSkip_ = update_["version"].str();
            handleUpdate(J({{"type", "update.get"}}));
        });
    }).detach();
    return true;
}
