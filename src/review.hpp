// Parsing for the review pane (review.cpp): a unified diff from the shadow repository split into
// files and hunks, and a one-hunk patch to reverse. Pure functions, covered by the unit tests.
#pragma once

#include <string>
#include <vector>

#include "json.hpp"

struct ReviewFile {
    std::string path;               // with forward slashes, as git prints it
    std::string change = "update";  // add | delete | update
    bool binary = false;
    long long added = 0, removed = 0;
    std::vector<std::string> header;              // "diff --git", "index", "---", "+++" ...
    std::vector<std::vector<std::string>> hunks;  // [0] is the "@@" line, then the hunk's lines
};

std::vector<ReviewFile> parseReviewDiff(const std::string& text);
// What the UI gets: paths with backslashes, hunks with their lines; capped in files and bytes.
Json reviewFilesJson(const std::vector<ReviewFile>& files, bool* truncated);
// The file header plus one hunk, ready for `git apply -R`. Empty when the hunk does not exist.
std::string reviewHunkPatch(const ReviewFile& file, size_t hunk);
