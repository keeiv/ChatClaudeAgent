// Read-only tools for the local reviewers (Ollama tool calls), confined to the project
// folder, and the check that every finding a local reviewer reports quotes the file and
// line it points at. Pure functions: safe to call from the Ollama worker thread.
#pragma once

#include <set>
#include <string>
#include <vector>

#include "guard.hpp"
#include "json.hpp"

// The tool list sent with /api/chat.
Json localToolSpecs();

struct LocalToolResult {
    bool ok = false;
    std::string kind;    // read | search (for the chat's tool rows)
    std::string title;   // 讀取檔案、列出資料夾、搜尋內容
    std::string detail;  // what was asked, for the tool row
    std::string path;    // normalized relative path that was read (read_file only)
    std::string output;  // what goes back to the model
};

LocalToolResult runLocalTool(const std::wstring& root, const std::string& name, const Json& args);

// Some models (Llama 3.1 among them) write a tool call into the answer as JSON instead of
// making one. Finds such calls; *before gets the text ahead of the first one, since anything
// after it was written without the result. Returns false when there is none.
bool extractTextToolCalls(const std::string& content, Json* calls, std::string* before);

// Resolves a project-relative path and refuses anything outside root (.., drive paths, links).
bool resolveInside(const std::wstring& root, const std::string& rel, std::wstring* full, std::string* error);
// Lower-case, backslash-separated, no leading ".\" — how read paths and changed paths are compared.
std::string normalizeRelPath(const std::string& path);

struct LocalFinding {
    int index = 0;
    std::string path;
    int line = 0, lineEnd = 0;
    std::string quote;
    std::string text;  // the whole finding as the reviewer wrote it
    bool verified = false;
    std::string why;  // why it could not be verified
};

struct LocalReviewCheck {
    std::vector<LocalFinding> findings;
    bool saysNoIssues = false;
    std::vector<GuardIssue> issues;  // what to send back (empty: accept)
    std::string forwarded;           // the notes passed on to Claude/GPT: verified findings only
    int removed = 0;                 // findings dropped because their quote did not match
};

// readPaths: normalized paths the reviewer read with read_file this turn.
// changedPaths: normalized paths the turn under review changed (deleted files excluded).
LocalReviewCheck checkLocalReview(const std::wstring& root, const std::string& reply,
                                  const std::set<std::string>& readPaths, const std::vector<std::string>& changedPaths,
                                  const GuardRules& rules);
