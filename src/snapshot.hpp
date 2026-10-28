// Work checkpoints. Each conversation gets a private "shadow" git repository under
// %LOCALAPPDATA%\CCA\snapshots whose work tree is the project folder, so CCA can
// snapshot and restore files without touching the project's own .git.
#pragma once

#include <string>
#include <vector>

#include "json.hpp"

struct SnapshotResult {
    bool ok = false;
    std::string commit;  // snapshot id
    std::string error;
};

struct RestoreResult {
    bool ok = false;
    std::string error;
    std::string backup;                 // snapshot of the state just before restoring (undo)
    std::vector<std::string> restored;  // files changed back
    std::vector<std::string> removed;   // files deleted because they did not exist yet
};

class Snapshots {
public:
    // gitExe empty: look up git on PATH.
    explicit Snapshots(std::wstring gitExe = L"");
    bool available() const { return !git_.empty(); }

    SnapshotResult take(const std::string& convId, const std::wstring& folder, const std::string& label);
    RestoreResult restore(const std::string& convId, const std::wstring& folder, const std::string& commit);

    static std::wstring repoFor(const std::string& convId);
    // Folders that never go into snapshots (dependencies and build output).
    static const char* defaultExcludes();
    Json compare(const std::string& convId, const std::wstring& folder, const std::string& base,
                 const std::string& head);

    // The review pane (review.cpp): a unified diff between two snapshots, optionally of one path.
    bool diff(const std::string& convId, const std::wstring& folder, const std::string& base, const std::string& head,
              const std::string& path, std::string* text, std::string* error);
    // Puts one file back the way it was in `commit`, deleting it when it did not exist there yet.
    bool restorePath(const std::string& convId, const std::wstring& folder, const std::string& commit,
                     const std::string& path, std::string* error);
    // Reverses a patch in the work tree: one hunk the user rejected.
    bool reverseApply(const std::string& convId, const std::wstring& folder, const std::string& patch,
                      std::string* error);
    // A project-relative path safe to hand to git and to join onto the project folder.
    static bool safeRelativePath(const std::string& path);

private:
    int git(const std::wstring& repo, const std::wstring& folder, const std::vector<std::wstring>& args,
            std::string* out, std::string* err, unsigned long timeoutMs = 60000);
    bool ensureRepo(const std::wstring& repo, const std::wstring& folder, std::string* error);

    std::wstring git_;
};
