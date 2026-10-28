// Native evidence provenance, version invalidation and real worker IPC, isolated from user data and models.
#include <windows.h>

#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>

#include "app.hpp"
#include "util.hpp"

namespace {
std::mutex mutex;
std::deque<std::function<void()>> queue;
int checks = 0, failures = 0;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}
void pump() {
    std::deque<std::function<void()>> jobs;
    {
        std::lock_guard<std::mutex> lock(mutex);
        jobs.swap(queue);
    }
    for (auto& job : jobs) job();
}
}  // namespace
void postToUi(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(mutex);
    queue.push_back(std::move(fn));
}

struct QualityTestAccess {
    static void run(const std::wstring& root) {
        const std::string rectangle =
            "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAYAAAD0In+KAAAADklEQVR4nGP4z8DwH4QBEfcD/"
            "ePF9e8AAAAASUVORK5CYII=";
        check(!decodePngDataUrl(rectangle).size(), "avatar decoding still rejects rectangular pictures");
        check(!decodePngDataUrl(rectangle, true).empty(), "browser evidence decodes bounded rectangular PNGs");
        std::vector<Json> events;
        App app([&](const std::string& text) { events.push_back(Json::parse(text)); });
        app.uiReady_ = true;
        auto c = app.find("dm-claude");
        c->cwd = root;
        app.onUiMessage(
            J({{"type", "plan.save"},
               {"id", c->id},
               {"revision", 0},
               {"plan",
                J({{"title", "驗證資料保存"},
                   {"steps", Json(Json::Array{J({{"text", "執行測試"}, {"acceptance", "主機取得成功結果"}})})}})}})
                .dump());
        util::writeFileAtomic(util::joinPath(root, L"test.py"), "print('HOST_VERIFIED')\n");
        auto request = J({{"type", "quality.run"},
                          {"id", c->id},
                          {"kind", "test"},
                          {"revision", 1},
                          {"step", "step-1"},
                          {"label", "實際測試"},
                          {"command", Json(Json::Array{util::narrow(runtimePython()), "test.py"})},
                          {"paths", Json(Json::Array{"test.py"})}});
        app.onUiMessage(request.dump());
        check(app.qualityPending_.empty(), "unconfirmed tests never start a worker");
        request["confirm"] = true;
        request["revision"] = 0;
        app.onUiMessage(request.dump());
        check(app.qualityPending_.empty(), "stale plan revision cannot execute a verification");
        request["revision"] = 1;
        app.onUiMessage(request.dump());
        check(app.anyBusy(*c), "verification is registered as running");
        auto wait = [&] {
            for (int i = 0; i < 1500; ++i) {
                pump();
                app.tick();
                if (app.qualityPending_.empty()) break;
                Sleep(10);
            }
        };
        wait();
        if (!c->collaboration["evidence"].size()) {
            for (const auto& event : events)
                if (event["type"].str() == "toast" || event["type"].str() == "quality.state")
                    std::printf("Worker diagnostic: %s\n", event.dump().c_str());
            check(false, "host worker produces an evidence record");
            app.shutdown();
            pump();
            return;
        }
        const auto& evidence = c->collaboration["evidence"].at(0);
        check(evidence["passed"].boolean() && evidence["exitCode"].i64(-1) == 0 && evidence["source"].str() == "host",
              "only real host exit codes create passing evidence");
        check(evidence["output"].str().find("HOST_VERIFIED") != std::string::npos && evidence["paths"].size() == 1,
              "actual output and mapped source paths are retained");
        const auto count = c->collaboration["evidence"].size();
        app.qualityResult(J({{"requestToken", "forged"}, {"ok", true}, {"result", J({{"passed", true}})}}));
        check(c->collaboration["evidence"].size() == count, "unsolicited model or client results are ignored");
        app.onUiMessage(J({{"type", "plan.approve"}, {"id", c->id}, {"revision", 1}}).dump());
        app.onUiMessage(
            J({{"type", "plan.accept"}, {"id", c->id}, {"revision", 1}, {"step", "step-1"}, {"accepted", true}})
                .dump());
        check(c->collaboration["plan"]["status"].str() == "accepted",
              "fresh passing evidence can be accepted by the owner");
        util::writeFileAtomic(util::joinPath(root, L"test.py"), "print('CHANGED')\n");
        app.onUiMessage(J({{"type", "quality.get"}, {"id", c->id}}).dump());
        wait();
        check(c->collaboration["evidence"].at(0)["stale"].boolean() &&
                  !c->collaboration["plan"]["steps"].at(0)["accepted"].boolean(),
              "external source changes invalidate evidence and its acceptance");
        app.onUiMessage(
            J({{"type", "plan.accept"}, {"id", c->id}, {"revision", 1}, {"step", "step-1"}, {"accepted", true}})
                .dump());
        check(c->collaboration["plan"]["status"].str() != "accepted", "stale evidence cannot be accepted as verified");
        app.onUiMessage(request.dump());
        wait();
        app.onUiMessage(
            J({{"type", "plan.accept"}, {"id", c->id}, {"revision", 1}, {"step", "step-1"}, {"accepted", true}})
                .dump());
        app.onUiMessage(J({{"type", "quality.get"}, {"id", c->id}}).dump());
        wait();
        check(c->collaboration["plan"]["status"].str() == "accepted",
              "historical stale evidence does not undo acceptance backed by fresh evidence");
        const auto eventCount = events.size();
        app.onUiMessage(
            J({{"type", "browser.action"}, {"id", c->id}, {"operation", "verify"}, {"revision", 0}}).dump());
        bool rejected = false;
        for (auto i = eventCount; i < events.size(); ++i)
            if (events[i]["type"].str() == "quality.state" && !events[i]["error"].str().empty()) rejected = true;
        check(rejected, "immediate verification rejection releases the browser UI wait state");
        app.shutdown();
        pump();
    }
};

int wmain() {
    const auto original = util::getEnv(L"CCA_DATA_DIR");
    auto root = util::joinPath(util::exeDir(), L"quality-test-" + util::widen(util::newId()));
    util::ensureDir(root);
    SetEnvironmentVariableW(L"CCA_DATA_DIR", util::joinPath(root, L"data").c_str());
    auto project = util::joinPath(root, L"project");
    util::ensureDir(project);
    QualityTestAccess::run(project);
    SetEnvironmentVariableW(L"CCA_DATA_DIR", original.empty() ? nullptr : original.c_str());
    std::printf("%d native quality checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
