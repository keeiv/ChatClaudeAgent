// Isolated core tests and an update-broker fixture; never starts a model backend.
#include <windows.h>

#include <cstdio>
#include <functional>
#include <vector>

#include "app.hpp"
#include "updater.hpp"
#include "util.hpp"

void postToUi(std::function<void()> fn) {
    fn();
}
namespace {
int count = 0, failed = 0;
void check(bool ok, const char* name) {
    ++count;
    if (!ok) {
        ++failed;
        std::printf("FAIL: %s\n", name);
    }
}
}  // namespace
struct DeskTestAccess {
    static void run() {
        Json response;
        App app([&](const std::string& raw) { response = Json::parse(raw); });
        app.uiReady_ = true;
        auto c = app.find("dm-claude");
        c->preparing = "fixture";
        auto act = [&](const std::string& type, Json extra = Json::object()) {
            extra["type"] = "queue." + type;
            extra["id"] = c->id;
            extra["requestToken"] = "fixture";
            app.onUiMessage(extra.dump());
            return response["ok"].boolean();
        };
        check(act("add", J({{"text", "first"}})), "add queued instruction");
        check(c->queueArmed && c->desk["queue"].size() == 1, "running session arms queue");
        const auto first = c->desk["queue"].at(0)["id"].str();
        check(act("add", J({{"text", "second"}})), "add second instruction");
        auto second = c->desk["queue"].at(1)["id"].str();
        check(act("edit", J({{"itemId", first}, {"text", "edited"}})), "edit durable queue");
        check(act("move", J({{"itemId", second}, {"to", 0}})) && c->desk["queue"].at(0)["id"].str() == second,
              "reorder durable queue");
        check(!act("move", J({{"itemId", second}, {"to", 99}})), "reject invalid position");
        check(!act("edit", J({{"itemId", first}, {"text", " "}})), "reject blank edits");
        auto path = util::joinPath(util::appDataDir(), L"desk\\dm-claude.json");
        HANDLE lock = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        check(lock != INVALID_HANDLE_VALUE, "lock queue persistence fixture");
        auto before = c->desk;
        check(!act("remove", J({{"itemId", second}})) && before == c->desk, "failed save preserves queue");
        check(act("pause") && !c->queueArmed, "pause remains available when persistence is locked");
        CloseHandle(lock);
        check(act("resume") && c->queueArmed, "resume restored before busy-boundary test");
        app.tickQueue(*c);
        check(c->queueActive.empty(), "busy boundary does not dispatch");
        c->preparing.clear();
        act("pause");
        app.tickQueue(*c);
        check(c->queueActive.empty(), "paused queue does not dispatch");
        act("resume");
        app.tickQueue(*c);
        check(c->desk["queue"].at(0)["status"].str() == "dispatching", "dispatch intent persisted first");
        app.tickQueue(*c);
        check(c->desk["queue"].at(0)["status"].str() == "interrupted" && !c->queueArmed,
              "rejected launch not marked complete and queue stops");
        app.tickQueue(*c);
        check(c->desk["queue"].at(1)["status"].str() == "pending", "failure does not consume next item");
        check(act("retry", J({{"itemId", second}})), "manual retry restores pending item");
        c->desk["queue"].at(0)["status"] = "dispatching";
        app.saveDesk(*c, c->desk);
        Conversation reload;
        reload.id = c->id;
        app.loadDesk(reload);
        check(!reload.queueArmed && reload.desk["queue"].at(0)["status"].str() == "interrupted",
              "restart retains queue without replaying dispatch");
        AgentConfig cfg;
        cfg.model = "fixture";
        cfg.resumeId = "external";
        cfg.permission = "ask";
        c->lastRequest = "original";
        app.captureContext(*c, "claude", cfg, std::string(70000, 'x'));
        const auto receipt = c->desk["contexts"].at(0);
        check(receipt["input"]["truncated"].boolean() && receipt["input"]["bytes"].i64() == 70000 &&
                  receipt["input"]["text"].str().size() == 65536,
              "context truncation explicitly labeled");
        check(receipt["request"]["text"].str() == "original" && receipt["history"].str() == "external-session",
              "source request and unknown external history recorded");
        app.contextEvent(*c, AgentKind::Claude,
                         {AgentEvent::Kind::ToolStart, "", J({{"tool", "read"}, {"path", "main.cpp"}})});
        check(c->desk["contexts"].at(0)["observations"].at(0)["version"].str() == "unknown",
              "backend reported read is not invented file evidence");
        app.contextEvent(*c, AgentKind::Claude, {AgentEvent::Kind::TurnDone, "", J({{"status", "completed"}})});
        auto historical = c->desk["contexts"];
        lock = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        app.captureContext(*c, "claude", cfg, "must not become the previous turn");
        app.contextEvent(*c, AgentKind::Claude, {AgentEvent::Kind::TurnStarted, "", Json::object()});
        app.contextEvent(*c, AgentKind::Claude, {AgentEvent::Kind::ToolStart, "", J({{"path", "unrelated.cpp"}})});
        check(c->desk["contexts"] == historical, "failed receipt save cannot attach events to a previous turn");
        CloseHandle(lock);
        app.captureContext(*c, "claude:opinion", cfg, "independent question");
        check(c->desk["contexts"].items().back()["request"]["text"].str() == "independent question",
              "independent opinion records its current request instead of stale chat requirements");
        for (int i = 0; i < 22; ++i) app.captureContext(*c, "codex", cfg, "short");
        check(c->desk["contexts"].size() == 20, "context retention bounded");
        Conversation replay;
        replay.id = c->id;
        app.loadDesk(replay);
        bool interrupted = true;
        for (const auto& receipt : replay.desk["contexts"].items())
            interrupted = interrupted && receipt["status"].str() == "interrupted";
        check(interrupted, "restart cannot present previous context submissions as still running");
        Json layout = J({{"preset", "review"}, {"side", 200}, {"panel", 460}, {"font", 14}, {"density", "compact"}});
        app.onUiMessage(J({{"type", "layout.set"}, {"layout", layout}}).dump());
        check(response["ok"].boolean() && app.settings_["layout"] == layout, "layout saved");
        layout["side"] = 999;
        app.onUiMessage(J({{"type", "layout.set"}, {"layout", layout}}).dump());
        check(!response["ok"].boolean() && app.settings_["layout"]["side"].i64() == 200, "invalid layout unchanged");
        c->queueArmed = true;
        check(!app.updateIdle(), "armed or busy work prevents install");
        c->preparing.clear();
        c->queueArmed = true;
        c->userStopped = true;
        app.tickQueue(*c);
        check(!c->queueArmed, "manual stop disarms queue");
        c->userStopped = false;
        c->queueArmed = true;
        c->group.active = c->group.paused = true;
        app.tickQueue(*c);
        check(!c->queueArmed && c->queueActive.empty(), "paused group does not drain queue");
        c->group.active = c->group.paused = false;
        c->collaboration["plan"]["status"] = "draft";
        act("resume");
        app.tickQueue(*c);
        check(c->queueActive.empty() && !c->queueArmed, "queue cannot approve draft plans");
        c->collaboration["plan"] = Json();
        app.resetConversation(*c);
        check(c->desk["queue"].empty() && c->desk["contexts"].empty() && !c->queueArmed,
              "clear conversation cancels old queue and source receipts");
        util::writeFileAtomic(path, "{invalid");
        app.loadDesk(*c);
        check(!act("add", J({{"text", "must not overwrite"}})), "damaged queue refuses overwrite");
        std::string damaged;
        util::readFile(path, damaged);
        check(damaged == "{invalid", "damaged queue remains available for recovery");
    }
};

int wmain(int argc, wchar_t** argv) {
    int helper = updater::helperMain();
    if (helper >= 0) return helper;
    const auto probe = util::getEnv(L"CCA_UPDATE_PROBE_FILE");
    if (argc == 3 && (std::wstring(argv[1]) == L"--apply" || std::wstring(argv[1]) == L"--apply-folder")) {
        auto result = std::wstring(argv[1]) == L"--apply"
                          ? updater::inspect(util::narrow(argv[2]), "1.0.0-", updater::executable(), "desktop")
                          : updater::inspectFolder(argv[2], "1.0.0-", updater::executable(), "desktop");
        if (result["available"].boolean()) result = updater::prepare(result, updater::executable());
        util::writeFileAtomic(probe + L".plan", result.dump());
        std::string error;
        if (!result["ok"].boolean() || !updater::launch(result, error)) {
            std::fprintf(stderr, "update not launched: %s\n",
                         (result["ok"].boolean() ? error : result["error"].str()).c_str());
            return 3;
        }
        return 0;
    }
    if (!probe.empty()) {
        updater::confirmStartup();
        util::writeFileAtomic(probe, "restored");
        return 0;
    }
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    auto root = util::joinPath(temp, util::widen("cca-desk-test-" + util::newId()));
    SetEnvironmentVariableW(L"CCA_DATA_DIR", root.c_str());
    util::ensureDir(root);
    const std::vector<std::string> versions{"1.0.0-",      "1.0.0-oss", "1.0.0-luna", "1.0.0-terra", "1.0.0-sol",
                                            "1.0.0-astra", "1.0.1-",    "1.1.0-",     "2.0.0-"};
    int parts[4]{};
    for (size_t i = 0; i < versions.size(); ++i) {
        check(updater::version(versions[i], parts), "valid release version");
        check(updater::compare(versions[i], versions[i]) == 0, "same version never updates");
        if (i)
            check(updater::compare(versions[i], versions[i - 1]) > 0 &&
                      updater::compare(versions[i - 1], versions[i]) < 0,
                  "monotonic suffix and patch ordering");
    }
    for (const auto& invalid : {"", "1.2", "1.2.3.4", "1.2.3-Luna", "1.2.3-beta", "01.0.0", "1.2.-3", "1.2.3--"})
        check(!updater::version(invalid, parts), "reject invalid release");
    check(updater::compare("1.0.0", "1.0.0-") == 0, "legacy version equals initial annotation");
    check(updater::sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA256 known vector");
    check(!updater::validSource("http://example.org/manifest.json"), "reject cleartext source");
    check(!updater::validSource("https://user:pass@example.org/manifest.json"), "reject credentials in source");
    auto unconfigured = updater::inspect("", "1.0.0-", updater::executable(), "desktop");
    check(unconfigured["status"].str() == "unconfigured" && !unconfigured["available"].boolean(),
          "no source is a truthful inactive state");
    {
        const auto inbox = updater::inboxDir();
        check(util::dirExists(inbox) && util::startsWith(util::narrow(inbox), util::narrow(root)),
              "local update folder is created inside the isolated data directory");
        auto empty = updater::inspectFolder(inbox, "1.0.0-", updater::executable(), "desktop");
        check(empty["ok"].boolean() && empty["status"].str() == "empty" && !empty["available"].boolean(),
              "an empty folder offers nothing");
        const auto before = updater::inboxStamp(inbox);
        util::writeFileAtomic(util::joinPath(inbox, L"notes.txt"), "not a release");
        util::writeFileAtomic(util::joinPath(inbox, L"fake.exe"), std::string(4096, 'x'));
        check(updater::inboxStamp(inbox) != before, "a new file changes the folder fingerprint");
        auto junk = updater::inspectFolder(inbox, "1.0.0-", updater::executable(), "desktop");
        check(junk["status"].str() == "empty" && junk["packages"].i64() == 0,
              "files without CCA product metadata are ignored");
        util::ensureDir(util::joinPath(inbox, L"release"));
        util::writeFileAtomic(
            util::joinPath(inbox, L"release\\manifest.json"),
            J({{"schema", 1},
               {"version", "9.0.0-"},
               {"files",
                J({{"desktop", J({{"name", "..\\fake.exe"}, {"size", 4096}, {"sha256", std::string(64, 'a')}})}})}})
                .dump());
        auto escaped = updater::inspectFolder(inbox, "1.0.0-", updater::executable(), "desktop");
        check(!escaped["available"].boolean(), "a manifest cannot point outside its release folder");
    }
    DeskTestAccess::run();
    std::printf("%d desk/update checks, %d failures\n", count, failed);
    return failed ? 1 : 0;
}
