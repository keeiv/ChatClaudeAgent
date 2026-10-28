// Reviewer regression guard: when a reviewer edits files, run the project's test command and undo those edits if
// they are what turned the tests red. Only exit codes the host measured count; model text never decides this.
#include "app.hpp"

#include <windows.h>

#include <future>
#include <thread>

#include "diff.hpp"
#include "editions.hpp"
#include "process.hpp"
#include "util.hpp"

namespace {

const DWORD kTestTimeoutMs = 10 * 60 * 1000;
const size_t kTailBytes = 6000;     // what the ledger keeps of a test run
const size_t kPromptReason = 1500;  // the reviewer's own explanation inside the next prompt
const size_t kPromptTail = 2000;    // the failing test output inside the next prompt
const size_t kPromptDiff = 1200;    // what the reviewer tried, inside the next prompt
const size_t kLedgerEntries = 30;

struct TestRun {
    bool started = false;  // false: the command could not be launched
    bool timedOut = false;
    int exitCode = -1;
    std::string tail;
};

std::string cleanText(const std::string& raw, size_t tailBytes) {
    // Test output may be in any code page; a round trip through UTF-16 leaves valid UTF-8 for the JSON layer.
    return util::narrow(util::widen(util::tailUtf8(raw, tailBytes)));
}

// The command goes into a throwaway .cmd file so cmd.exe reads it verbatim instead of an argument that was re-quoted.
TestRun runTests(const std::wstring& folder, const std::string& command) {
    TestRun run;
    const std::wstring script =
        util::joinPath(util::appDataDir(), L"guard-test-" + util::widen(util::newId()) + L".cmd");
    const std::string body = "@echo off\r\nchcp 65001 >nul\r\n" + command + "\r\nexit /b %errorlevel%\r\n";
    if (!util::writeFileAtomic(script, body)) return run;
    ProcessOptions options;
    options.exe = script;
    options.cwd = folder;
    options.dropEnv = {L"OPENAI_API_KEY", L"ANTHROPIC_API_KEY", L"CLAUDE_CODE_OAUTH_TOKEN"};
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    runCapture(
        options,
        [&run, promise](int code, std::string out, std::string err) {
            run.started = code != -1 || !out.empty();
            run.timedOut = code == -2;
            run.exitCode = code;
            run.tail = cleanText(out + (err.empty() ? "" : "\n" + err), kTailBytes);
            promise->set_value();
        },
        kTestTimeoutMs);
    future.wait();
    DeleteFileW(script.c_str());
    return run;
}

// Runs on a worker thread. Tests the reviewed state; if it is red, tests the pre-turn checkpoint to see whose fault
// that is, and puts the reviewer's work back unless the checkpoint is green.
Json guardSequence(Snapshots snaps, const std::string& convId, const std::wstring& folder, const std::string& command,
                   const std::string& preTurn) {
    Json out = J({{"command", command}, {"status", "unknown"}, {"baselineExit", Json()}});
    TestRun after = runTests(folder, command);
    out["afterExit"] = after.exitCode;
    out["afterTail"] = after.tail;
    if (!after.started || after.timedOut) {
        out["error"] =
            after.timedOut ? "測試指令超過 10 分鐘，已終止；沒有還原任何檔案" : "測試指令無法啟動；沒有還原任何檔案";
        return out;
    }
    if (after.exitCode == 0) {
        out["status"] = "passed";
        return out;
    }
    // Red. Keep a copy of the reviewer's result first, so the test of the old state can never lose it.
    SnapshotResult keep = snaps.take(convId, folder, "regression guard: reviewer result");
    if (!keep.ok) {
        out["error"] = "無法保存審查者的修改，沒有還原任何檔案：" + keep.error;
        return out;
    }
    out["keep"] = keep.commit;
    RestoreResult restored = snaps.restore(convId, folder, preTurn);
    if (!restored.ok) {
        out["error"] = "無法還原到審查前的檢查點：" + restored.error;
        out["status"] = "restore-failed";
        return out;
    }
    TestRun before = runTests(folder, command);
    out["baselineExit"] = before.exitCode;
    out["baselineTail"] = before.tail;
    if (before.started && !before.timedOut && before.exitCode == 0) {
        Json restoredFiles = Json::array();
        for (const auto& f : restored.restored) restoredFiles.push(f);
        for (const auto& f : restored.removed) restoredFiles.push(f);
        out["status"] = "reverted";
        out["restored"] = restoredFiles;
        return out;
    }
    // The old state is red too (or could not be measured), so the reviewer is not to blame: keep its work.
    RestoreResult back = snaps.restore(convId, folder, keep.commit);
    out["status"] = back.ok ? "baseline-red" : "restore-failed";
    if (!back.ok) out["error"] = "測試轉紅不是審查者造成，但還原審查者的修改失敗：" + back.error;
    return out;
}

std::string pathList(const Json& files) {
    std::string text;
    for (const auto& f : files.items()) text += (text.empty() ? "" : "、") + f["path"].str();
    return text;
}

}  // namespace

bool App::groupGuardApplies(const Conversation& c, AgentKind k) const {
    const GroupRun& g = c.group;
    (void)k;
    bool guarded = c.regressionGuard;
    return c.kind == "group" && guarded && !c.testCommand.empty() && g.active && !g.pipeline &&
           (g.phase == "review" || g.phase == "recheck") && g.turnFiles.size() > 0 && !g.turnSnapshot.empty() &&
           !g.stopRequested && !c.userStopped && snapshots_.available() && !c.cwd.empty() &&
           util::projectFolderProblem(c.cwd).empty();
}

void App::groupGuardStart(Conversation& c, AgentKind k, int verdict) {
    pushMessage(c, J({{"author", "system"},
                      {"kind", "info"},
                      {"text", nameOf(k) + " 修改了 " + std::to_string(c.group.turnFiles.size()) +
                                   " 個檔案，先執行測試指令確認沒有讓測試變紅。"}}));
    c.preparing = "guard";
    c.stateDirty = true;
    const std::string convId = c.id, command = c.testCommand, preTurn = c.group.turnSnapshot;
    const std::wstring folder = c.cwd;
    Snapshots snaps = snapshots_;
    std::weak_ptr<int> life = lifetime_;
    std::thread([this, life, convId, folder, command, preTurn, snaps, k, verdict] {
        Json outcome = guardSequence(snaps, convId, folder, command, preTurn);
        postToUi([this, life, convId, k, verdict, outcome] {
            if (!life.expired()) groupGuardDone(convId, k, verdict, outcome);
        });
    }).detach();
}

void App::groupGuardDone(const std::string& convId, AgentKind k, int verdict, const Json& outcome) {
    Conversation* c = find(convId);
    if (!c) return;
    c->preparing.clear();
    c->stateDirty = true;
    GroupRun& g = c->group;
    const std::string status = outcome["status"].str();
    const Json attempted = g.turnFiles;
    const std::string who = nameOf(k);
    const std::string n = std::to_string(attempted.size());
    std::string text;
    if (status == "passed") text = who + " 修改了 " + n + " 個檔案，測試通過（結束碼 0）。";
    else if (status == "reverted")
        text = who + " 的修改讓測試轉紅（結束碼 " + std::to_string(outcome["afterExit"].i64()) +
               "），審查前的狀態是綠的，已還原這 " + n + " 個檔案。審查者改完的版本另存為檢查點 " +
               outcome["keep"].str().substr(0, 8) + "，沒有自動套用。";
    else if (status == "baseline-red")
        text = "測試是紅的（結束碼 " + std::to_string(outcome["afterExit"].i64()) +
               "），但審查前的狀態也是紅的，保留 " + who + " 的修改。";
    else if (status == "restore-failed") text = outcome["error"].str("還原失敗") + "；請到審查窗格檢查工作區。";
    else text = "沒有驗證：" + outcome["error"].str("無法判斷測試結果") + "。保留 " + who + " 的修改。";
    Json files = Json::array();
    for (const auto& f : attempted.items()) files.push(f["path"]);
    pushMessage(*c, J({{"author", "system"},
                       {"kind", "regression"},
                       {"agent", agentName(k)},
                       {"status", status},
                       {"round", g.round},
                       {"command", outcome["command"]},
                       {"afterExit", outcome["afterExit"]},
                       {"baselineExit", outcome["baselineExit"]},
                       {"files", files},
                       {"tail", util::tailUtf8(outcome["afterTail"].str(), 3000)},
                       {"keep", outcome["keep"]},
                       {"text", text}}));
    if (c->task.isObject()) {
        Json& ledger = c->task["regression_guard"];
        if (!ledger.isArray()) ledger = Json::array();
        ledger.push(J({{"at", util::nowMs()},
                       {"round", g.round},
                       {"agent", agentName(k)},
                       {"status", status},
                       {"afterExit", outcome["afterExit"]},
                       {"baselineExit", outcome["baselineExit"]},
                       {"files", files},
                       {"command", outcome["command"]}}));
        while (ledger.size() > kLedgerEntries) ledger.items().erase(ledger.items().begin());
        auto& handoffs = c->task["handoffs"].items();
        if (!handoffs.empty()) handoffs.back()["regression"] = status;
        saveTask(c->task);
    }
    if (!g.active) return;
    if (g.stopRequested || c->userStopped) {
        groupFinish(*c, "stopped", "已停止協作。");
        return;
    }
    if (status == "reverted") {
        groupGuardReverted(*c, k, outcome, attempted);
        return;
    }
    groupTurnJudged(*c, k, verdict);
}

// The reviewer's files are back at the pre-turn state, so this turn changed nothing; what happened travels to the next
// member inside the reviewer's note, and the reviewer's edit counts as an objection that is still open.
void App::groupGuardReverted(Conversation& c, AgentKind k, const Json& outcome, const Json& attempted) {
    GroupRun& g = c.group;
    std::string note = util::truncateUtf8(util::trim(g.turnText), kPromptReason);
    note +=
        std::string(note.empty() ? "" : "\n\n") + "【CCA 測試守門】" + nameOf(k) + " 修改了 " + pathList(attempted) +
        "，但執行「" + outcome["command"].str() + "」後測試轉紅（結束碼 " + std::to_string(outcome["afterExit"].i64()) +
        "），而審查前的狀態是綠的，所以 CCA 已把這些檔案還原。審查者想處理的問題仍然存在；處理時不能讓測試變紅。\n\n" +
        "修改後的測試輸出（結尾）：\n" + util::tailUtf8(outcome["afterTail"].str(), kPromptTail) + "\n";
    std::string diffs;
    for (const auto& f : attempted.items()) {
        std::string one = "--- " + f["path"].str() + "\n" + f["diff"].str();
        if (!one.empty() && one.back() != '\n') one += '\n';
        if (diffs.size() + one.size() > kPromptDiff) break;
        diffs += one;
    }
    if (!diffs.empty()) note += "\n被還原的修改：\n```diff\n" + diffs + "```\n";
    g.turnText = note;
    g.turnFiles = Json::array();
    if (c.task.isObject()) {
        auto& handoffs = c.task["handoffs"].items();
        if (!handoffs.empty())
            handoffs.back()["next_action"] = "address the reverted reviewer concern without breaking tests";
        saveTask(c.task);
    }
    groupTurnJudged(c, k, (int)Verdict::Issues);
}

