// Pure Git helpers for the version-control pane: status parsing, never-commit rules, risk tones,
// staged-diff secret scanning and history parsing. No process is started here.
#pragma once

#include <string>
#include <vector>

#include "json.hpp"

namespace gitflow {
// gitignore-style match on a '/'-separated project path, case-insensitive (Windows file system).
// "name" matches any segment, "dir/" any directory, "a/b*" is anchored, "**" crosses directories.
bool globMatch(const std::string& pattern, const std::string& path);
// `git status --porcelain=v1 -z --untracked-files=all` → [{path, from, x, y, kind}].
Json parseStatus(const std::string& porcelainZ);
// Adds tone (new | changed | risk), reasons, locked and selected to every file.
// sizes: path → bytes of the working copy (missing for deleted files).
Json classify(const Json& files, const Json& rules, const Json& sizes);
// Lines added in a unified diff that look like credentials: [{path, line, kind}], never the secret itself.
Json secretHits(const std::string& unifiedDiff);
// `git log --format=%H%x1f%P%x1f%an%x1f%at%x1f%D%x1f%s%x1f%(trailers:key=CCA-Task,valueonly)%x1e`.
Json parseLog(const std::string& raw);
// Default never-commit patterns for a new installation.
Json defaultRules();
// Validates one user pattern: no control characters, no drive or "..", at most 200 bytes.
bool validRule(const std::string& pattern);
// Why a remote command failed, from Git's error text: "auth" (sign-in needed), "network" or "" (anything else).
std::string authProblem(const std::string& gitError);
// Host and transport of a remote URL: {host, ssh}. Credentials in the URL are dropped.
Json remoteHost(const std::string& url);
}  // namespace gitflow
