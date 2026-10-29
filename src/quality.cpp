// Only host-executed results become verification evidence; model text cannot report a passing exit code.
#include "app.hpp"

#include "util.hpp"

void App::invalidateEvidence(Conversation& c, const std::string& reason) {
    if (!c.collaboration["evidence"].size()) return;
    Json next = c.collaboration;
    bool changed = false;
    for (auto& e : next["evidence"].items()) {
        if (!e["stale"].boolean()) {
            e["stale"] = true;
            e["staleReason"] = reason;
            changed = true;
        }
    }
    if (!changed) return;
    for (auto& step : next["plan"]["steps"].items())
        if (!step["evidenceId"].str().empty()) step["accepted"] = false;
    if (next["plan"]["status"].str() == "accepted") next["plan"]["status"] = "awaiting_acceptance";
    persistCollaboration(c, next);
}

bool App::handleQuality(const Json& message) {
    const auto type = message["type"].str();
    if (type != "quality.get" && type != "quality.run" && type != "browser.action") return false;
    auto c = find(message["id"].str());
    const auto fail = [&](const std::string& error) {
        send(J({{"type", "quality.state"}, {"id", message["id"]}, {"error", error}}));
        toast("error", error);
        return true;
    };
    if (!c) { return fail("找不到對話"); }
    if (qualityBusy_.count(c->id)) { return fail("上一個驗證操作尚未結束"); }
    const auto action = type == "quality.get"      ? "fingerprint"
                        : type == "browser.action" ? "browser"
                                                   : message["kind"].str();
    if (action != "fingerprint" && action != "browser" && action != "test" && action != "repro") return true;
    const bool record =
        action == "test" || action == "repro" || (action == "browser" && message["operation"].str() == "verify");
    if (record && (anyBusy(*c) || (c->group.active && !c->group.paused))) { return fail("請先暫停目前工作再執行驗收"); }
    if (record &&
        (message["revision"].i64(-1) != c->collaboration["plan"]["revision"].i64() || !message["confirm"].boolean())) {
        return fail("請核對命令、專案及目前計畫版本後確認執行");
    }
    if (c->cwd.empty() || !util::dirExists(c->cwd) || !util::projectFolderProblem(c->cwd).empty()) {
        return fail("需要有效的專案資料夾");
    }
    std::string python = util::narrow(runtimePython());
    auto script = runtimeScript(L"QUALITY_WORKER_PY", L"quality_worker.py");
    if (python.empty() || !util::fileExists(script)) { return fail("驗證工具需要 Python 3"); }
    if (!qualityWorker_ || !qualityWorker_->running()) {
        for (const auto& pending : qualityPending_)
            send(J({{"type", "quality.state"}, {"id", pending.second["convId"]}, {"error", "上次驗證工作程序已中斷"}}));
        qualityPending_.clear();
        qualityBusy_.clear();
        for (auto& current : convs_) current->stateDirty = true;
        const auto generation = ++qualityGeneration_;
        ProcessOptions opt;
        opt.exe = util::widen(python);
        opt.args = {L"-I", L"-u", script};
        opt.cwd = util::appDataDir();
        opt.dropEnv = {L"OPENAI_API_KEY", L"ANTHROPIC_API_KEY", L"CLAUDE_CODE_OAUTH_TOKEN"};
        auto life = std::weak_ptr<int>(lifetime_);
        qualityWorker_ = std::make_shared<ChildProcess>();
        std::string error;
        if (!qualityWorker_->start(
                opt,
                [this, life](const std::string& line) {
                    if (line.size() > 4 * 1024 * 1024) return;
                    const Json result = Json::parse(line);
                    postToUi([this, life, result] {
                        if (!life.expired()) qualityResult(result);
                    });
                },
                {},
                [this, life, generation](unsigned long) {
                    postToUi([this, life, generation] {
                        if (life.expired()) return;
                        if (generation != qualityGeneration_) return;
                        for (const auto& pending : qualityPending_)
                            send(J({{"type", "quality.state"},
                                    {"id", pending.second["convId"]},
                                    {"error", "驗證工作程序已結束"}}));
                        qualityBusy_.clear();
                        qualityPending_.clear();
                        for (auto& c : convs_) c->stateDirty = true;
                        toast("error", "驗證工作程序已結束，未完成結果沒有標為通過");
                    });
                },
                &error)) {
            return fail(error);
        }
    }
    Json request = message;
    const auto token = util::newId();
    request["action"] = action;
    request["cwd"] = util::narrow(c->cwd);
    request["convId"] = c->id;
    request["requestToken"] = token;
    request["browserScript"] = util::narrow(runtimeScript(L"BROWSER_WORKER_JS", L"browser_worker.cjs"));
    qualityPending_[token] = J({{"convId", c->id},
                                {"record", record},
                                {"kind", action},
                                {"revision", c->collaboration["plan"]["revision"]},
                                {"step", message["step"]},
                                {"label", util::truncateUtf8(message["label"].str(), 300)},
                                {"command", message["command"]},
                                {"operation", message["operation"]},
                                {"cwd", util::narrow(c->cwd)}});
    qualityBusy_.insert(c->id);
    c->stateDirty = true;
    if (!qualityWorker_->writeLine(request.dump())) {
        qualityBusy_.erase(c->id);
        qualityPending_.erase(token);
        return fail("無法送出驗證要求");
    }
    return true;
}

void App::qualityResult(const Json& message) {
    const auto token = message["requestToken"].str();
    auto it = qualityPending_.find(token);
    if (it == qualityPending_.end()) return;
    const Json request = it->second;
    qualityPending_.erase(it);
    auto c = find(request["convId"].str());
    if (!c) return;
    qualityBusy_.erase(c->id);
    c->stateDirty = true;
    if (!message["ok"].boolean()) {
        send(J({{"type", "quality.state"}, {"id", c->id}, {"error", message["error"]}}));
        toast("error", message["error"].str("驗證未完成"));
        return;
    }
    Json result = message["result"];
    const auto binding = result["currentBinding"].str(result["binding"].str());
    Json next = c->collaboration;
    next["currentBinding"] = binding;
    bool outdated = false;
    for (auto& evidence : next["evidence"].items()) {
        if (evidence["binding"].str() != binding || evidence["revision"] != next["plan"]["revision"]) {
            evidence["stale"] = true;
            evidence["staleReason"] = "來源或計畫版本已改變";
            outdated = true;
        }
    }
    if (outdated) {
        bool acceptanceChanged = false;
        for (auto& step : next["plan"]["steps"].items())
            for (const auto& e : next["evidence"].items())
                if (step["evidenceId"] == e["id"] && e["stale"].boolean()) {
                    acceptanceChanged = acceptanceChanged || step["accepted"].boolean();
                    step["accepted"] = false;
                }
        if (acceptanceChanged && next["plan"]["status"].str() == "accepted")
            next["plan"]["status"] = "awaiting_acceptance";
    }
    std::string image;
    if (request["record"].boolean() && !result["screenshot"].str().empty()) {
        auto png = decodePngDataUrl(result["screenshot"].str(), true);
        image = "quality-" + util::newId() + ".png";
        if (png.empty() || !util::writeFileAtomic(util::joinPath(mediaDir(), util::widen(image)), png)) {
            send(J({{"type", "quality.state"}, {"id", c->id}, {"error", "無法保存瀏覽器證據，沒有標為通過"}}));
            toast("error", "無法保存瀏覽器證據，沒有標為通過");
            return;
        }
    }
    if (request["record"].boolean()) {
        const auto id = util::newId();
        Json evidence =
            J({{"id", id},
               {"kind", request["kind"]},
               {"label", request["label"]},
               {"revision", request["revision"]},
               {"step", request["step"]},
               {"command", request["command"]},
               {"binding", result["binding"]},
               {"ts", util::nowMs()},
               {"passed", result["passed"].boolean()},
               {"stale", result["sourceChanged"].boolean() || request["revision"] != next["plan"]["revision"]},
               {"exitCode", result["exitCode"]},
               {"output", util::truncateUtf8(result["output"].str(), 30000)},
               {"before", result["before"]},
               {"after", result["after"]},
               {"baseCommit", result["baseCommit"]},
               {"url", result["url"]},
               {"steps", result["steps"]},
               {"image", image},
               {"environment", result["environment"]},
               {"paths", result["paths"]},
               {"note", result["note"]},
               {"source", "host"}});
        auto& list = next["evidence"];
        if (!list.isArray()) list = Json::array();
        if (list.size() >= 80) {
            send(
                J({{"type", "quality.state"}, {"id", c->id}, {"error", "每份對話最多保存 80 筆證據，原有資料已保留"}}));
            toast("error", "每份對話最多保存 80 筆證據，原有資料已保留");
            return;
        }
        list.push(evidence);
        for (auto& step : next["plan"]["steps"].items())
            if (step["id"] == request["step"]) {
                step["evidenceId"] = id;
                step["accepted"] = false;
            }
    }
    if (!persistCollaboration(*c, next)) {
        send(J({{"type", "quality.state"}, {"id", c->id}, {"error", "驗證結果未能保存，沒有新增證據"}}));
        return;
    }
    send(J({{"type", "quality.state"}, {"id", c->id}, {"result", result}}));
}
