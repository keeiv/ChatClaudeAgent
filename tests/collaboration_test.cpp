// Real persistence and Git worktrees, with model events supplied locally and no paid calls.
#include <windows.h>

#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>

#include "app.hpp"
#include "collaboration.hpp"
#include "process.hpp"
#include "util.hpp"
#include "workspaces.hpp"

namespace {
int checks = 0, failures = 0;
std::mutex queueMutex;
std::deque<std::function<void()>> queue;
void check(bool ok, const char* label) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", label);
    }
}
bool git(const std::wstring& cwd, std::vector<std::wstring> args) {
    ProcessOptions opt;
    opt.exe = util::findOnPath(L"git");
    opt.cwd = cwd;
    opt.args = std::move(args);
    std::string out, error;
    int code = runWait(opt, &out, &error, 30000);
    if (code) std::printf("Git fixture error: %s\n", error.c_str());
    return code == 0;
}
void commit(const std::wstring& dir, const wchar_t* file, const std::string& content) {
    check(util::writeFileAtomic(util::joinPath(dir, file), content), "fixture file written");
    check(git(dir, {L"add", L"--", file}), "fixture file staged");
    check(git(dir, {L"commit", L"-m", L"Test change"}), "fixture commit created");
}
}  // namespace

void postToUi(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(queueMutex);
    queue.push_back(std::move(fn));
}

struct CollaborationTestAccess {
    static void run() {
        std::vector<Json> events;
        App app([&](const std::string& text) { events.push_back(Json::parse(text)); });
        app.uiReady_ = true;
        auto c = app.find("dm-claude");
        check(c != nullptr, "default conversation exists");
        if (!c) return;
        auto send = [&](Json message) {
            message["id"] = c->id;
            app.onUiMessage(message.dump());
        };
        Json draft = J({{"title", "修正輸入驗證"},
                        {"steps", Json(Json::Array{J({{"text", "重現空輸入"}, {"acceptance", "空輸入回傳錯誤"}})})}});
        send(J({{"type", "plan.save"}, {"revision", 0}, {"plan", draft}}));
        check(c->collaboration["plan"]["revision"].i64() == 1 && c->collaboration["plan"]["status"].str() == "draft",
              "plan starts as a saved draft");
        send(J({{"type", "plan.execute"}, {"revision", 1}}));
        check(c->messages.empty(), "an unapproved plan cannot start any model");
        send(J({{"type", "plan.approve"}, {"revision", 0}}));
        check(c->collaboration["plan"]["status"].str() == "draft", "stale approval is refused");
        send(J({{"type", "plan.approve"}, {"revision", 1}}));
        check(c->collaboration["plan"]["status"].str() == "approved", "matching revision may be approved");
        send(J({{"type", "plan.comment"}, {"revision", 1}, {"text", "補上多位元組輸入"}, {"step", "step-1"}}));
        check(c->collaboration["plan"]["status"].str() == "draft" && c->collaboration["plan"]["revision"].i64() == 2,
              "a comment invalidates approval and advances the revision");
        send(J({{"type", "plan.execute"}, {"revision", 1}}));
        check(c->messages.empty(), "an earlier approved revision never executes after a comment");
        check(planRequest(c->collaboration["plan"]).find("多位元組") != std::string::npos,
              "execution brief includes commander comments");
        Json reloaded = loadCollaboration(c->id);
        check(reloaded["plan"]["revision"].i64() == 2 && reloaded["plan"]["comments"].size() == 1,
              "plan and comments survive reload");
        std::string error;
        check(!makePlan(Json(), J({{"title", "空計畫"}, {"steps", Json::array()}}), 1, &error).isObject(),
              "empty plans are rejected");
        check(!saveCollaboration("../outside", Json::object()), "collaboration files reject traversal IDs");
        Json saved = c->collaboration;
        Json oversized = saved;
        oversized["oversized"] = std::string(2 * 1024 * 1024, 'x');
        check(!app.persistCollaboration(*c, oversized) && c->collaboration.dump() == saved.dump() &&
                  loadCollaboration(c->id)["plan"].dump() == saved["plan"].dump(),
              "failed persistence preserves both in-memory and saved decisions");
        c->collaboration["plan"]["status"] = "running";
        app.finishPlan(*c, "completed");
        check(c->collaboration["plan"]["status"].str() == "awaiting_acceptance" &&
                  !c->collaboration["plan"]["steps"].at(0)["accepted"].boolean(),
              "model completion never substitutes for commander acceptance");
        c->collaboration["plan"]["status"] = "running";
        check(saveCollaboration(c->id, c->collaboration) &&
                  loadCollaboration(c->id)["plan"]["status"].str() == "interrupted",
              "a plan interrupted by restart never silently resumes");
        c->collaboration = saved;
        Json profiles = defaultReviewProfiles();
        check(validateReviewProfiles(profiles, &error) && profiles.size() == 4,
              "four distinct default review profiles validate");
        profiles.push(profiles.at(0));
        check(!validateReviewProfiles(profiles, &error), "duplicate profile IDs are rejected");
        send(J({{"type", "focus.assign"}, {"agent", "codex"}, {"focus", "security"}}));
        check(app.reviewFocus(*c, AgentKind::Codex).find("越權") != std::string::npos,
              "assigned focus reaches the actual review prompt");
        send(J({{"type", "focus.assign"}, {"agent", "codex"}, {"focus", "missing"}}));
        check(c->collaboration["focus"]["codex"].str() == "security",
              "unknown focus does not discard the previous assignment");

        app.pushMessage(*c, J({{"author", "system"}, {"kind", "summary"}, {"text", "已完成"}}));
        Json blocks = Json(Json::Array{
            J({{"type", "approval"}, {"id", "pending-test"}, {"state", "pending"}, {"title", "需要核准"}})});
        app.pushMessage(*c, J({{"author", "claude"}, {"blocks", blocks}}));
        app.sendInbox();
        Json inbox = events.back();
        check(inbox["type"].str() == "inbox" && inbox["total"].i64() >= 3,
              "inbox combines approvals, plans and completed work");
        send(J({{"type", "inbox.read"}}));
        bool approval = false, done = false;
        for (const auto& item : events.back()["items"].items()) {
            approval |= item["kind"].str() == "approval";
            done |= item["kind"].str() == "done";
        }
        check(approval && !done, "mark read hides notifications but never approvals");

        auto& op = c->collaboration["opinion"];
        op = J({{"id", "run-test"},
                {"status", "running"},
                {"agents", Json(Json::Array{"claude", "codex"})},
                {"results", J({{"claude", J({{"status", "done"}, {"text", "已有一份有效意見"}})},
                               {"codex", J({{"status", "running"}, {"messageId", "op-failed"}})}})}});
        app.pushMessage(*c,
                        J({{"id", "op-failed"}, {"author", "codex"}, {"opinion", "run-test"}, {"streaming", true}}));
        app.opinionEvent(c->id, "codex", "old-run", {AgentEvent::Kind::TurnDone, "", J({{"status", "failed"}})});
        check(op["status"].str() == "running", "old opinion events cannot change a newer run");
        app.opinionEvent(c->id, "codex", "run-test",
                         {AgentEvent::Kind::TurnDone, "", J({{"status", "failed"}, {"error", "fixture failure"}})});
        check(op["status"].str() == "failed" && op["results"]["claude"]["text"].str() == "已有一份有效意見",
              "one failed model preserves the successful opinion without inventing consensus");
        Json next = c->collaboration;
        next["opinion"]["status"] = "running";
        check(saveCollaboration(c->id, next) && loadCollaboration(c->id)["opinion"]["status"].str() == "interrupted",
              "interrupted opinion runs do not resume or spend quota silently");
        app.stopOpinion(*c);
        const auto messageCount = c->messages.size();
        app.opinionLaunch(*c, "claude", "must not launch after stop or save failure");
        check(c->messages.size() == messageCount && !c->opinionClaude,
              "stopped or unsaved opinion runs cannot launch a later member");
        check(app.convState(*c)["typing"].size() == 0, "stopped opinion results do not leave a false typing indicator");
        app.shutdown();
    }
};

int wmain() {
    auto root = util::joinPath(util::exeDir(), L"collaboration-qa-" + util::widen(util::newId()));
    util::ensureDir(root);
    auto previous = util::getEnv(L"CCA_DATA_DIR");
    SetEnvironmentVariableW(L"CCA_DATA_DIR", util::joinPath(root, L"data").c_str());
    CollaborationTestAccess::run();
    auto origin = util::joinPath(root, L"project");
    util::ensureDir(origin);
    if (git(origin, {L"init", L"-b", L"main"}) && git(origin, {L"config", L"core.autocrlf", L"false"}) &&
        git(origin, {L"config", L"user.name", L"CCA Test"}) &&
        git(origin, {L"config", L"user.email", L"cca-test@example.invalid"})) {
        commit(origin, L"shared.txt", "base\n");
        Json one = createTaskWorktree(origin, "first"), two = createTaskWorktree(origin, "second");
        check(one["ok"].boolean() && two["ok"].boolean() && one["path"].str() != two["path"].str(),
              "parallel tasks get independent worktrees and branches");
        if (one["ok"].boolean() && two["ok"].boolean()) {
            auto a = util::widen(one["path"].str()), b = util::widen(two["path"].str());
            commit(a, L"one.txt", "first task\n");
            commit(b, L"two.txt", "second task\n");
            check(!util::fileExists(util::joinPath(origin, L"one.txt")),
                  "task edits do not reach the original checkout before merging");
            Json preview = checkTaskMerge(one, false, "", "");
            check(preview["ok"].boolean() && preview["clean"].boolean(),
                  "Git confirms a clean merge without touching the checkout");
            check(!checkTaskMerge(one, true, "stale", preview["source"].str())["ok"].boolean(),
                  "a stale merge preview cannot be applied");
            check(checkTaskMerge(one, true, preview["target"].str(), preview["source"].str())["merged"].boolean(),
                  "an explicitly previewed task merges successfully");
            preview = checkTaskMerge(two, false, "", "");
            check(preview["clean"].boolean() &&
                      checkTaskMerge(two, true, preview["target"].str(), preview["source"].str())["merged"].boolean(),
                  "a second parallel branch merges after the first");
            check(util::fileExists(util::joinPath(origin, L"one.txt")) &&
                      util::fileExists(util::joinPath(origin, L"two.txt")),
                  "both tasks' files are retained");
            Json three = createTaskWorktree(origin, "third"), four = createTaskWorktree(origin, "fourth");
            if (three["ok"].boolean() && four["ok"].boolean()) {
                commit(util::widen(three["path"].str()), L"shared.txt", "third task\n");
                commit(util::widen(four["path"].str()), L"shared.txt", "fourth task\n");
                preview = checkTaskMerge(three, false, "", "");
                check(checkTaskMerge(three, true, preview["target"].str(), preview["source"].str())["merged"].boolean(),
                      "first change to the shared file merges");
                preview = checkTaskMerge(four, false, "", "");
                check(preview["ok"].boolean() && !preview["clean"].boolean(),
                      "conflicts are reported by the real Git merge algorithm");
                check(!checkTaskMerge(four, true, preview["target"].str(), preview["source"].str())["ok"].boolean(),
                      "a conflict is never merged automatically");
                std::string text;
                util::readFile(util::joinPath(origin, L"shared.txt"), text);
                check(text == "third task\n", "conflict preview and refusal preserve the target file");
            } else check(false, "conflict fixtures created");
            util::writeFileAtomic(util::joinPath(a, L"uncommitted.txt"), "keep me\n");
            check(!checkTaskMerge(one, false, "", "")["ok"].boolean(),
                  "uncommitted changes block merging without automatic staging");
        }
    } else check(false, "isolated Git fixture initialized");
    SetEnvironmentVariableW(L"CCA_DATA_DIR", previous.empty() ? nullptr : previous.c_str());
    std::printf("%d collaboration checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
