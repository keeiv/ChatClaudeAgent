// Live Claude subscription usage. Claude Code only puts a utilization figure in its
// rate_limit_event once a window is close to full, so CCA can (opt-in) ask the same usage
// endpoint Claude Code's /status uses, with the sign-in Claude Code already stored.
// The token is read from disk for one request and never logged, sent to the UI or saved.
#pragma once

#include <string>

#include "json.hpp"

struct ClaudeUsageResult {
    bool ok = false;
    int http = 0;
    std::string error;             // shown to the user; never contains the token
    Json limits = Json::object();  // CCA's format: {five_hour: {utilization 0..1, resetsAt, status, source}, ...}
};

// The usage response turned into CCA's limits format. Accepts the flat window keys
// (five_hour, seven_day, seven_day_opus, ...) and a `limits` array.
Json parseClaudeUsage(const Json& body);

// "2026-10-24T05:00:00Z" (or with an offset) as Unix seconds; 0 when it cannot be read.
long long parseIsoTime(const std::string& iso);

// Blocking network call; run it on a background thread. CCA_CLAUDE_USAGE_URL overrides the
// endpoint for tests (http is only allowed to 127.0.0.1), CCA_CLAUDE_CREDENTIALS the file.
ClaudeUsageResult fetchClaudeUsage();
