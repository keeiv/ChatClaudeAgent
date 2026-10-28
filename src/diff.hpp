#pragma once

#include <string>

#include "json.hpp"

struct DiffStat {
    int added = 0;
    int removed = 0;
};

// Counts +/- lines of a unified diff, skipping the ---/+++ file headers.
DiffStat countUnifiedDiff(const std::string& diff);

// Converts Claude Code's structuredPatch (jsdiff hunks) to unified diff text.
std::string hunksToDiff(const Json& hunks, DiffStat* stat);

// A brand new file: every line is an addition.
std::string newFileDiff(const std::string& content, DiffStat* stat);

// Line diff between two snippets (used for Edit previews before the edit happens).
std::string lineDiff(const std::string& before, const std::string& after, DiffStat* stat);

// Codex sends whole file content for added files and unified diffs for updates.
std::string normalizeCodexDiff(const std::string& kind, const std::string& diff, DiffStat* stat);

// Reviewer verdicts from the last "VERDICT: X" line of a reply.
enum class Verdict { None, Approve, Changed, Issues };
Verdict parseVerdict(const std::string& text, std::string* textWithoutVerdict);
const char* verdictName(Verdict v);
