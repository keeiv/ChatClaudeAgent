// All Git commands run off the UI thread. No recursive deletion or implicit staging is used.
#include "workspaces.hpp"

#include <windows.h>

#include <algorithm>
#include <thread>

#include "app.hpp"
#include "process.hpp"
#include "util.hpp"

namespace {
std::string normalPath(std::string path) {
    return util::toLower(util::replaceAll(util::trim(path), "\\", "/"));
}
int git(const std::wstring& cwd, const std::vector<std::wstring>& args, std::string* out, std::string* error) {
    ProcessOptions opt;
    opt.exe = util::findOnPath(L"git");
    opt.args = args;
    opt.cwd = cwd;
    opt.setEnv = {{L"GIT_TERMINAL_PROMPT", L"0"}, {L"GIT_MERGE_AUTOEDIT", L"no"}};
    opt.dropEnv = {L"GIT_DIR", L"GIT_WORK_TREE", L"GIT_INDEX_FILE", L"GIT_COMMON_DIR"};
    if (opt.exe.empty()) {
        *error = "找不到 Git";
        return -1;
    }
    return runWait(opt, out, error, 30000);
}
Json failure(const std::string& error) {
    return J({{"ok", false}, {"error", util::truncateUtf8(error, 4000)}});
}
std::shared_ptr<void> workspaceLock(const std::wstring& folder) {
    HANDLE file = CreateFileW(folder.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    wchar_t buffer[32768];
    DWORD count = GetFinalPathNameByHandleW(file, buffer, 32768, FILE_NAME_NORMALIZED);
    CloseHandle(file);
    if (!count || count >= 32768) return {};
    unsigned long long hash = 1469598103934665603ULL;
    for (DWORD i = 0; i < count; ++i) hash = (hash ^ (unsigned long long)towlower(buffer[i])) * 1099511628211ULL;
    HANDLE mutex = CreateMutexW(nullptr, FALSE, (L"Local\\CCA.Workspace." + std::to_wstring(hash)).c_str());
    if (!mutex) return {};
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return {};
    }
    return std::shared_ptr<void>(mutex, [](void* p) { CloseHandle(p); });
}
}  // namespace

Json createTaskWorktree(const std::wstring& folder, const std::string& taskId) {
    auto lease = workspaceLock(folder);
    if (!lease) return failure("來源工作區正在執行其他任務");
    std::string out, error, root, branch, head;
    if (git(folder, {L"rev-parse", L"--show-toplevel"}, &root, &error))
        return failure("需要已有提交的 Git 倉庫：" + error);
    auto origin = util::widen(util::trim(root));
    if (git(origin, {L"status", L"--porcelain", L"--untracked-files=normal"}, &out, &error) || !util::trim(out).empty())
        return failure("來源還有未提交變更；請先提交或自行保存後再建立 worktree，避免遺漏目前工作。");
    if (git(origin, {L"symbolic-ref", L"--short", L"HEAD"}, &branch, &error))
        return failure("來源必須位於分支，不能是 detached HEAD");
    if (git(origin, {L"rev-parse", L"HEAD"}, &head, &error)) return failure(error);
    // Names are host generated, not paths or branch names received from the page.
    std::string name = "cca/" + util::newId();
    std::wstring parent = util::joinPath(util::localAppDataDir(), L"worktrees");
    if (!util::ensureDir(parent)) return failure("無法建立 worktree 目錄");
    std::wstring path = util::joinPath(parent, util::widen(util::newId()));
    if (git(origin, {L"worktree", L"add", L"-b", util::widen(name), path, util::widen(util::trim(head))}, &out, &error))
        return failure(error.empty() ? out : error);
    return J({{"ok", true},
              {"origin", util::narrow(origin)},
              {"path", util::narrow(path)},
              {"branch", name},
              {"targetBranch", util::trim(branch)},
              {"base", util::trim(head)},
              {"task", taskId},
              {"created", util::nowMs()}});
}

Json checkTaskMerge(const Json& workspace, bool apply, const std::string& expectedTarget,
                    const std::string& expectedSource) {
    auto origin = util::widen(workspace["origin"].str()), path = util::widen(workspace["path"].str());
    auto targetLease = workspaceLock(origin), sourceLease = workspaceLock(path);
    if (!targetLease || !sourceLease) return failure("來源或目標有另一個任務正在寫入，請稍後重試");
    std::string targetCommon, sourceCommon, targetBranch, sourceBranch, out, error, target, source;
    if (git(origin, {L"rev-parse", L"--path-format=absolute", L"--git-common-dir"}, &targetCommon, &error) ||
        git(path, {L"rev-parse", L"--path-format=absolute", L"--git-common-dir"}, &sourceCommon, &error) ||
        normalPath(sourceCommon) != normalPath(targetCommon))
        return failure("worktree 和目標不屬於同一個 Git 倉庫");
    if (git(origin, {L"symbolic-ref", L"--short", L"HEAD"}, &targetBranch, &error) ||
        git(path, {L"symbolic-ref", L"--short", L"HEAD"}, &sourceBranch, &error) ||
        util::trim(targetBranch) != workspace["targetBranch"].str() ||
        util::trim(sourceBranch) != workspace["branch"].str())
        return failure("來源或目標分支已切換，請先回到建立 worktree 時的分支");
    for (const auto& dir : {origin, path}) {
        if (git(dir, {L"status", L"--porcelain", L"--untracked-files=normal"}, &out, &error) ||
            !util::trim(out).empty())
            return failure("worktree 與目標都須先提交變更；不會自動暫存檔案。請先檢查並提交要合併的內容。");
        if (!git(dir, {L"rev-parse", L"--verify", L"MERGE_HEAD"}, &out, &error))
            return failure("已有尚未完成的合併，請先處理");
    }
    if (git(origin, {L"rev-parse", L"HEAD"}, &target, &error) || git(path, {L"rev-parse", L"HEAD"}, &source, &error))
        return failure(error);
    target = util::trim(target);
    source = util::trim(source);
    std::string stat;
    git(origin, {L"diff", L"--stat", util::widen(target), util::widen(source)}, &stat, &error);
    int check = git(origin, {L"merge-tree", L"--write-tree", util::widen(target), util::widen(source)}, &out, &error);
    Json result = J({{"ok", true},
                     {"clean", check == 0},
                     {"source", source},
                     {"target", target},
                     {"summary", util::truncateUtf8(stat, 16000)},
                     {"conflicts", check == 1 ? util::truncateUtf8(out, 12000) : ""}});
    if (check != 0 && check != 1) return failure("Git 無法預演合併（需支援 merge-tree --write-tree）：" + error);
    if (!apply) return result;
    if (expectedSource.empty() || expectedTarget.empty() || target != expectedTarget || source != expectedSource)
        return failure("預覽後版本已改變，請重新檢查合併");
    if (check) return failure("存在合併衝突，兩個工作區都已保留，尚未合併");
    if (git(origin, {L"merge", L"--no-edit", L"--no-ff", util::widen(source)}, &out, &error)) {
        std::string abortOut, abortError;
        // The target was clean and had no merge in progress when this operation began.
        git(origin, {L"merge", L"--abort"}, &abortOut, &abortError);
        return failure("合併未完成，任務分支已保留：" + error + (abortError.empty() ? "" : "\n" + abortError));
    }
    result["merged"] = true;
    return result;
}

void App::handleWorktree(Conversation& c, const Json& m) {
    const auto type = m["type"].str();
    if (type != "worktree.create" && type != "worktree.check" && type != "worktree.merge") return;
    if (anyBusy(c) || c.group.active) {
        toast("error", "對話工作中，不能變更工作區");
        return;
    }
    if (type == "worktree.create" && c.collaboration["workspace"]["ok"].boolean()) {
        toast("error", "此對話已有隔離工作區，請在另一個群組建立另一項平行任務");
        return;
    }
    const auto workspace = c.collaboration["workspace"];
    if (type != "worktree.create" && !workspace["ok"].boolean()) {
        toast("error", "尚未建立 worktree");
        return;
    }
    const auto origin = type == "worktree.create" ? util::narrow(c.cwd) : workspace["origin"].str();
    for (const auto& other : convs_)
        if (other.get() != &c && normalPath(util::narrow(other->cwd)) == normalPath(origin) &&
            (anyBusy(*other) || other->group.active)) {
            toast("error", "目標工作區有另一個工作正在執行");
            return;
        }
    c.preparing = "worktree";
    c.stateDirty = true;
    const auto id = c.id;
    const auto folder = c.cwd;
    const auto target = m["target"].str(), source = m["source"].str();
    auto life = std::weak_ptr<int>(lifetime_);
    std::thread([this, life, id, folder, workspace, type, target, source] {
        Json result = type == "worktree.create" ? createTaskWorktree(folder, id)
                                                : checkTaskMerge(workspace, type == "worktree.merge", target, source);
        postToUi([this, life, id, type, result] {
            if (life.expired()) return;
            auto c = find(id);
            if (!c) return;
            c->preparing.clear();
            c->stateDirty = true;
            if (!result["ok"].boolean()) {
                send(J({{"type", "worktree.result"}, {"id", id}, {"action", type}, {"result", result}}));
                toast("error", result["error"].str());
                return;
            }
            Json next = c->collaboration;
            if (type == "worktree.create") {
                next["workspace"] = result;
                if (!persistCollaboration(*c, next)) return;
                c->cwd = util::widen(result["path"].str());
                clearPipelineSessions(*c);
                if (c->claude) {
                    c->claude->shutdown();
                    c->claude.reset();
                }
                if (c->codex) {
                    c->codex->shutdown();
                    c->codex.reset();
                }
                c->claudeSession.clear();
                c->codexThread.clear();
                c->contextPending = true;
                if (!saveConversations()) toast("error", "worktree 已建立，但對話資料夾保存失敗；已保留路徑供恢復");
                sendConvs();
            } else {
                next["workspace"]["check"] = result;
                persistCollaboration(*c, next);
                if (result["merged"].boolean()) toast("ok", "已合併已提交的任務變更，worktree 保留供查閱");
            }
            send(J({{"type", "worktree.result"}, {"id", id}, {"action", type}, {"result", result}}));
        });
    }).detach();
}
