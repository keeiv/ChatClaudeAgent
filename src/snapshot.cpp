#include "snapshot.hpp"

#include <windows.h>

#include "process.hpp"
#include "util.hpp"

Snapshots::Snapshots(std::wstring gitExe) : git_(gitExe.empty() ? util::findOnPath(L"git") : std::move(gitExe)) {}

std::wstring Snapshots::repoFor(const std::string& convId) {
    std::wstring dir = util::joinPath(util::localAppDataDir(), L"snapshots");
    util::ensureDir(dir);
    return util::joinPath(dir, util::widen(convId + ".git"));
}

const char* Snapshots::defaultExcludes() {
    return "node_modules/\n.venv/\nvenv/\n__pycache__/\n.mypy_cache/\n.pytest_cache/\n.tox/\n"
           "dist/\nbuild/\nout/\ntarget/\n.next/\n.nuxt/\n.gradle/\n.idea/\n.vs/\n.cca-artifacts/\n*.log\n*.tmp\n";
}

int Snapshots::git(const std::wstring& repo, const std::wstring& folder, const std::vector<std::wstring>& args,
                   std::string* out, std::string* err, unsigned long timeoutMs) {
    ProcessOptions opt;
    opt.exe = git_;
    opt.cwd = folder;
    // -c keeps the shadow repo independent of the user's global git settings that could break it.
    opt.args = {L"-c", L"core.autocrlf=false", L"-c", L"core.safecrlf=false", L"-c", L"core.quotepath=false",
                L"-c", L"user.name=CCA", L"-c", L"user.email=cca@localhost", L"-c", L"commit.gpgsign=false",
                L"--git-dir=" + repo, L"--work-tree=" + folder};
    opt.args.insert(opt.args.end(), args.begin(), args.end());
    opt.dropEnv = {L"GIT_DIR", L"GIT_WORK_TREE", L"GIT_INDEX_FILE"};
    return runWait(opt, out, err, timeoutMs);
}

bool Snapshots::ensureRepo(const std::wstring& repo, const std::wstring& folder, std::string* error) {
    if (util::dirExists(util::joinPath(repo, L"objects"))) return true;
    std::string err;
    if (git(repo, folder, {L"init", L"-q"}, nullptr, &err) != 0) {
        if (error) *error = "無法建立快照儲存區：" + util::trim(err);
        return false;
    }
    std::wstring info = util::joinPath(repo, L"info");
    util::ensureDir(info);
    util::writeFileAtomic(util::joinPath(info, L"exclude"), defaultExcludes());
    return true;
}

SnapshotResult Snapshots::take(const std::string& convId, const std::wstring& folder, const std::string& label) {
    SnapshotResult r;
    if (!available()) {
        r.error = "找不到 git，無法建立檢查點";
        return r;
    }
    std::wstring repo = repoFor(convId);
    if (!ensureRepo(repo, folder, &r.error)) return r;
    std::string out, err;
    if (git(repo, folder, {L"add", L"-A", L"--", L"."}, &out, &err, 120000) != 0) {
        r.error = "建立檢查點失敗：" + util::tailUtf8(util::trim(err), 300);
        return r;
    }
    if (git(repo, folder, {L"commit", L"-q", L"--allow-empty", L"--no-verify", L"-m", util::widen(label)}, &out, &err) != 0) {
        r.error = "建立檢查點失敗：" + util::tailUtf8(util::trim(err), 300);
        return r;
    }
    if (git(repo, folder, {L"rev-parse", L"HEAD"}, &out, &err) != 0) {
        r.error = "讀不到檢查點編號";
        return r;
    }
    r.commit = util::trim(out);
    r.ok = r.commit.size() >= 7;
    return r;
}

Json Snapshots::compare(const std::string& convId, const std::wstring& folder, const std::string& base,
                        const std::string& head) {
    for (const auto& revision : {base, head}) {
        if (revision.size() < 7 || revision.size() > 64) return Json();
        for (unsigned char c : revision)
            if (!isxdigit(c)) return Json();
    }
    std::string text, error;
    if (git(repoFor(convId), folder,
            {L"diff", L"--no-ext-diff", L"--no-textconv", L"--no-renames", L"--numstat", L"-z", util::widen(base),
             util::widen(head), L"--"},
            &text, &error) != 0)
        return Json();
    Json files = Json::array();
    size_t at = 0;
    while (at < text.size() && files.size() < 1000) {
        size_t end = text.find('\0', at);
        if (end == std::string::npos) break;
        std::string line = text.substr(at, end - at);
        size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b != std::string::npos)
            files.push(J({{"path", line.substr(b + 1)},
                          {"added", Json::parse(line.substr(0, a))},
                          {"removed", Json::parse(line.substr(a + 1, b - a - 1))},
                          {"change", "update"}}));
        at = end + 1;
    }
    return J({{"files", files}, {"changed", !text.empty()}, {"reference", "snapshot:" + base + ".." + head}});
}

RestoreResult Snapshots::restore(const std::string& convId, const std::wstring& folder, const std::string& commit) {
    RestoreResult r;
    if (!available()) {
        r.error = "找不到 git";
        return r;
    }
    for (char ch : commit) {
        if (!isxdigit((unsigned char)ch)) {
            r.error = "檢查點編號不正確";
            return r;
        }
    }
    std::wstring repo = repoFor(convId);
    // Snapshot the current state first, so the restore itself can be undone.
    SnapshotResult now = take(convId, folder, "回滾前的狀態");
    if (!now.ok) {
        r.error = now.error;
        return r;
    }
    r.backup = now.commit;
    std::string out, err;
    std::wstring target = util::widen(commit), current = util::widen(now.commit);
    if (git(repo, folder, {L"diff", L"--name-status", L"--no-renames", target, current}, &out, &err) != 0) {
        r.error = "找不到這個檢查點：" + util::trim(err);
        return r;
    }
    for (const auto& line : util::splitLines(out)) {
        if (line.size() < 3) continue;
        char status = line[0];
        std::string path = line.substr(line.find('\t') + 1);
        if (status == 'A') r.removed.push_back(path);
        else r.restored.push_back(path);
    }
    // Files that appeared after the checkpoint are deleted; everything else is checked out.
    for (const auto& p : r.removed) {
        std::wstring full = util::joinPath(folder, util::widen(util::replaceAll(p, "/", "\\")));
        DeleteFileW(full.c_str());
    }
    if (!r.restored.empty() && git(repo, folder, {L"checkout", target, L"--", L"."}, &out, &err) != 0) {
        r.error = "還原失敗：" + util::tailUtf8(util::trim(err), 300);
        return r;
    }
    r.ok = true;
    return r;
}

namespace {

bool hexId(const std::string& id) {
    if (id.size() < 7 || id.size() > 64) return false;
    for (unsigned char c : id) {
        if (!isxdigit(c)) return false;
    }
    return true;
}

}  // namespace

bool Snapshots::safeRelativePath(const std::string& path) {
    if (path.empty() || path.size() > 1024 || path[0] == '-' || path[0] == '/' || path[0] == '\\') return false;
    if (path.find(':') != std::string::npos || path.find('\0') != std::string::npos) return false;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find_first_of("/\\", start);
        if (end == std::string::npos) end = path.size();
        std::string part = path.substr(start, end - start);
        if (part == ".." || part == "." || part.empty()) return false;
        start = end + 1;
    }
    return true;
}

bool Snapshots::diff(const std::string& convId, const std::wstring& folder, const std::string& base,
                     const std::string& head, const std::string& path, std::string* text, std::string* error) {
    if (!hexId(base) || !hexId(head) || (!path.empty() && !safeRelativePath(path))) {
        if (error) *error = "檢查點或路徑不正確";
        return false;
    }
    std::vector<std::wstring> args = {L"diff",           L"--no-ext-diff",  L"--no-textconv",
                                      L"--no-renames",   L"--no-color",     L"-U3",
                                      util::widen(base), util::widen(head), L"--"};
    if (!path.empty()) args.push_back(util::widen(util::replaceAll(path, "\\", "/")));
    std::string err;
    if (git(repoFor(convId), folder, args, text, &err) != 0) {
        if (error) *error = "讀不到變更：" + util::tailUtf8(util::trim(err), 300);
        return false;
    }
    return true;
}

bool Snapshots::restorePath(const std::string& convId, const std::wstring& folder, const std::string& commit,
                            const std::string& path, std::string* error) {
    if (!hexId(commit) || !safeRelativePath(path)) {
        if (error) *error = "檢查點或路徑不正確";
        return false;
    }
    std::wstring repo = repoFor(convId);
    std::string gitPath = util::replaceAll(path, "\\", "/"), out, err;
    // A file the checkpoint did not have yet is deleted; any other is checked out as it was.
    if (git(repo, folder, {L"cat-file", L"-e", util::widen(commit + ":" + gitPath)}, &out, &err) != 0) {
        std::wstring full = util::joinPath(folder, util::widen(util::replaceAll(path, "/", "\\")));
        if (util::fileExists(full) && !DeleteFileW(full.c_str())) {
            if (error) *error = "無法刪除 " + path;
            return false;
        }
        return true;
    }
    if (git(repo, folder, {L"checkout", util::widen(commit), L"--", util::widen(gitPath)}, &out, &err) != 0) {
        if (error) *error = "還原失敗：" + util::tailUtf8(util::trim(err), 300);
        return false;
    }
    return true;
}

bool Snapshots::reverseApply(const std::string& convId, const std::wstring& folder, const std::string& patch,
                             std::string* error) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring file = util::joinPath(tmp, util::widen("cca-review-" + util::randomHex(6) + ".patch"));
    if (!util::writeFileAtomic(file, patch)) {
        if (error) *error = "無法寫入暫存的修補檔";
        return false;
    }
    std::string out, err;
    int code = git(repoFor(convId), folder, {L"apply", L"-R", L"--whitespace=nowarn", file}, &out, &err);
    DeleteFileW(file.c_str());
    if (code != 0) {
        if (error) *error = "這段修改和目前的檔案對不上（可能已經被改過）：" + util::tailUtf8(util::trim(err), 200);
        return false;
    }
    return true;
}
