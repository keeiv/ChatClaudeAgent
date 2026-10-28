// The review pane: what changed in a conversation's project since a checkpoint, file by file and
// hunk by hunk, read from the shadow repository; and putting back a whole file or one hunk the
// user rejects. Every revert first takes a checkpoint, so it can be undone like a rollback.
#include "review.hpp"

#include <windows.h>

#include <thread>

#include "app.hpp"
#include "util.hpp"

namespace {

const size_t kMaxDiffBytes = 1536 * 1024;
const size_t kMaxFiles = 300;
const size_t kMaxFileLines = 4000;

bool startsWith(const std::string& s, const char* prefix) {
    return util::startsWith(s, prefix);
}

// "--- a/src/x.cpp" or "+++ b/src/x.cpp"; empty for /dev/null.
std::string headerPath(const std::string& line) {
    std::string p = line.substr(4);
    if (!p.empty() && p.back() == '\r') p.pop_back();
    if (p == "/dev/null") return "";
    if (p.size() >= 2 && p[0] == '"' && p.back() == '"') p = p.substr(1, p.size() - 2);
    if (startsWith(p, "a/") || startsWith(p, "b/")) p = p.substr(2);
    return p;
}

}  // namespace

std::vector<ReviewFile> parseReviewDiff(const std::string& text) {
    std::vector<ReviewFile> files;
    ReviewFile* cur = nullptr;
    bool inHunk = false;
    for (std::string line : util::splitLines(text)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (startsWith(line, "diff --git ")) {
            files.push_back(ReviewFile());
            cur = &files.back();
            cur->header.push_back(line);
            // Fallback path from "diff --git a/x b/x" until the ---/+++ lines name it.
            size_t b = line.rfind(" b/");
            if (b != std::string::npos) cur->path = line.substr(b + 3);
            inHunk = false;
            continue;
        }
        if (!cur) continue;
        if (startsWith(line, "@@ ")) {
            cur->hunks.push_back({line});
            inHunk = true;
            continue;
        }
        if (!inHunk) {
            cur->header.push_back(line);
            if (startsWith(line, "new file mode")) cur->change = "add";
            else if (startsWith(line, "deleted file mode")) cur->change = "delete";
            else if (startsWith(line, "Binary files")) cur->binary = true;
            else if (startsWith(line, "--- ")) {
                std::string p = headerPath(line);
                if (!p.empty()) cur->path = p;
            } else if (startsWith(line, "+++ ")) {
                std::string p = headerPath(line);
                if (!p.empty()) cur->path = p;
            }
            continue;
        }
        if (line.empty()) line = " ";
        char t = line[0];
        if (t != ' ' && t != '+' && t != '-' && t != '\\') continue;
        cur->hunks.back().push_back(line);
        if (t == '+') ++cur->added;
        if (t == '-') ++cur->removed;
    }
    return files;
}

Json reviewFilesJson(const std::vector<ReviewFile>& files, bool* truncated) {
    Json out = Json::array();
    size_t bytes = 0;
    for (const auto& f : files) {
        if (out.size() >= kMaxFiles || bytes > kMaxDiffBytes) {
            if (truncated) *truncated = true;
            break;
        }
        Json hunks = Json::array();
        size_t lines = 0;
        bool cut = false;
        for (const auto& h : f.hunks) {
            if (lines >= kMaxFileLines || bytes > kMaxDiffBytes) {
                cut = true;
                break;
            }
            Json body = Json::array();
            for (size_t i = 1; i < h.size(); ++i) {
                body.push(h[i]);
                bytes += h[i].size();
                ++lines;
            }
            hunks.push(J({{"header", h.empty() ? "" : h[0]}, {"lines", body}}));
        }
        if (cut && truncated) *truncated = true;
        out.push(J({{"path", util::replaceAll(f.path, "/", "\\")},
                    {"change", f.binary ? "binary" : f.change},
                    {"added", f.added},
                    {"removed", f.removed},
                    {"hunks", hunks},
                    {"cut", cut}}));
    }
    return out;
}

std::string reviewHunkPatch(const ReviewFile& f, size_t hunk) {
    if (hunk >= f.hunks.size()) return "";
    std::string patch;
    for (const auto& line : f.header) patch += line + "\n";
    for (const auto& line : f.hunks[hunk]) patch += line + "\n";
    return patch;
}

std::string App::reviewBase(const Conversation& c, const std::string& scope) const {
    const auto& msgs = c.messages;
    if (scope == "all") {
        for (const auto& m : msgs) {
            if (!m["snapshot"].str().empty()) return m["snapshot"].str();
        }
        return "";
    }
    if (scope == "turn") {
        for (auto it = msgs.rbegin(); it != msgs.rend(); ++it) {
            if (!(*it)["snapshot"].str().empty()) return (*it)["snapshot"].str();
        }
        return "";
    }
    // This task: the first checkpoint after the commander's latest instruction.
    size_t from = 0;
    for (size_t i = 0; i < msgs.size(); ++i) {
        if (msgs[i]["author"].str() == "user") from = i;
    }
    for (size_t i = from; i < msgs.size(); ++i) {
        if (!msgs[i]["snapshot"].str().empty()) return msgs[i]["snapshot"].str();
    }
    return "";
}

void App::handleReviewOpen(const Json& m) {
    Conversation* c = find(m["id"].str());
    if (!c) return;
    std::string scope = m["scope"].str("task");
    if (scope != "turn" && scope != "all") scope = "task";
    auto fail = [&](const std::string& why) {
        send(J({{"type", "review"},
                {"id", c->id},
                {"scope", scope},
                {"requestToken", m["requestToken"]},
                {"error", why}}));
    };
    if (c->cwd.empty() || !util::dirExists(c->cwd)) return fail("這個對話還沒有專案資料夾。");
    if (!snapshots_.available()) return fail("找不到 git，無法比較檢查點。");
    if (anyBusy(*c)) return fail("AI 還在工作，這一輪結束後再打開審查窗格。");
    std::string base = reviewBase(*c, scope);
    if (base.empty()) return fail("還沒有檢查點。AI 在這個資料夾開始工作後，才有可以比較的版本。");
    std::string convId = c->id;
    std::wstring folder = c->cwd;
    Snapshots snaps = snapshots_;
    Json requestToken = m["requestToken"];
    std::thread([this, convId, folder, base, scope, snaps, requestToken]() mutable {
        Json reply =
            J({{"type", "review"}, {"id", convId}, {"scope", scope}, {"base", base}, {"requestToken", requestToken}});
        // Compare against the files as they are now, new and untracked ones included.
        SnapshotResult head = snaps.take(convId, folder, "審查窗格");
        std::string text, err;
        if (!head.ok) {
            reply["error"] = head.error;
        } else if (!snaps.diff(convId, folder, base, head.commit, "", &text, &err)) {
            reply["error"] = err;
        } else {
            bool truncated = false;
            reply["files"] = reviewFilesJson(parseReviewDiff(text), &truncated);
            reply["truncated"] = truncated;
            reply["head"] = head.commit;
        }
        postToUi([this, reply] { send(reply); });
    }).detach();
}

void App::handleReviewRevert(const Json& m) {
    Conversation* c = find(m["id"].str());
    if (!c) return;
    std::string base = m["base"].str(), path = util::replaceAll(m["path"].str(), "\\", "/");
    long long hunk = m.has("hunk") ? m["hunk"].i64(-1) : -1;
    std::string expect = m["header"].str();
    std::string head = m["head"].str();
    auto fail = [&](const std::string& why) {
        send(J({{"type", "review.reverted"},
                {"id", c->id},
                {"requestToken", m["requestToken"]},
                {"ok", false},
                {"error", why}}));
    };
    if (c->cwd.empty() || !snapshots_.available()) return fail("沒有可以還原的檢查點。");
    if (anyBusy(*c)) return fail("AI 還在工作，結束後再還原。");
    if (!Snapshots::safeRelativePath(path)) return fail("路徑不正確。");
    if (head.empty()) return fail("請先重新整理審查窗格，再還原檔案。");
    std::string convId = c->id;
    std::wstring folder = c->cwd;
    Snapshots snaps = snapshots_;
    Json requestToken = m["requestToken"];
    std::thread([this, convId, folder, base, path, hunk, expect, head, snaps, requestToken]() mutable {
        std::string err;
        bool ok = false;
        // Checkpoint first: the revert itself can be undone from the system message.
        SnapshotResult backup = snaps.take(convId, folder, "審查窗格還原前");
        std::string sinceReview;
        if (!backup.ok) {
            err = backup.error;
        } else if (!snaps.diff(convId, folder, head, backup.commit, path, &sinceReview, &err)) {
            // An invalid checkpoint must never become an unconditional restore.
        } else if (!sinceReview.empty()) {
            err = "這個檔案在開啟審查後已經變更，請重新整理再還原；目前內容已保留。";
        } else if (hunk < 0) {
            ok = snaps.restorePath(convId, folder, base, path, &err);
        } else {
            std::string text;
            if (snaps.diff(convId, folder, base, backup.commit, path, &text, &err)) {
                std::vector<ReviewFile> files = parseReviewDiff(text);
                if (files.empty() || (size_t)hunk >= files[0].hunks.size()) {
                    err = "這段修改已經不存在，請重新整理。";
                } else if (!expect.empty() && files[0].hunks[(size_t)hunk][0] != expect) {
                    err = "這個檔案在你打開審查窗格後又變了，請重新整理再還原。";
                } else {
                    ok = snaps.reverseApply(convId, folder, reviewHunkPatch(files[0], (size_t)hunk), &err);
                }
            }
        }
        std::string backupId = backup.ok ? backup.commit : "";
        postToUi([this, convId, path, hunk, ok, err, backupId, requestToken] {
            Conversation* cp = find(convId);
            if (!cp) return;
            std::string shown = util::replaceAll(path, "/", "\\");
            if (ok) {
                Json restored = Json::array();
                restored.push(shown);
                pushMessage(*cp, J({{"author", "system"},
                                    {"kind", "rollback"},
                                    {"text", hunk < 0 ? "你在審查窗格把 " + shown + " 還原到檢查點的版本。"
                                                      : "你在審查窗格還原了 " + shown + " 的一段修改。"},
                                    {"restored", restored},
                                    {"backup", backupId}}));
                // Files changed under the task: earlier reviews and test results no longer describe them.
                invalidatePipelineTask(*cp, backupId,
                                       "使用者在審查窗格還原了 " + shown + "；以目前 Git 版本為準，必要測試須重跑");
            }
            send(J({{"type", "review.reverted"},
                    {"id", convId},
                    {"requestToken", requestToken},
                    {"ok", ok},
                    {"error", err},
                    {"path", shown},
                    {"backup", ok ? backupId : ""}}));
        });
    }).detach();
}
