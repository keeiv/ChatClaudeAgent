#include "diff.hpp"

#include <algorithm>
#include <vector>

#include "util.hpp"

DiffStat countUnifiedDiff(const std::string& diff) {
    DiffStat s;
    for (const auto& line : util::splitLines(diff)) {
        if (line.empty()) continue;
        if (util::startsWith(line, "+++") || util::startsWith(line, "---")) continue;
        if (line[0] == '+') ++s.added;
        else if (line[0] == '-') ++s.removed;
    }
    return s;
}

std::string hunksToDiff(const Json& hunks, DiffStat* stat) {
    std::string out;
    DiffStat s;
    for (const auto& h : hunks.items()) {
        out += "@@ -" + std::to_string(h["oldStart"].i64()) + "," + std::to_string(h["oldLines"].i64()) + " +" +
               std::to_string(h["newStart"].i64()) + "," + std::to_string(h["newLines"].i64()) + " @@\n";
        for (const auto& l : h["lines"].items()) {
            const std::string& line = l.strRef();
            if (!line.empty() && line[0] == '+') ++s.added;
            else if (!line.empty() && line[0] == '-') ++s.removed;
            out += line;
            out += '\n';
        }
    }
    if (stat) *stat = s;
    return out;
}

static std::string prefixed(const std::string& content, char mark, int* count) {
    std::string out;
    int n = 0;
    for (const auto& line : util::splitLines(content)) {
        out += mark;
        out += line;
        out += '\n';
        ++n;
    }
    if (count) *count = n;
    return out;
}

std::string newFileDiff(const std::string& content, DiffStat* stat) {
    int n = 0;
    std::string out = prefixed(content, '+', &n);
    if (stat) { stat->added = n; stat->removed = 0; }
    return out;
}

std::string lineDiff(const std::string& before, const std::string& after, DiffStat* stat) {
    std::vector<std::string> a = util::splitLines(before), b = util::splitLines(after);
    DiffStat s;
    std::string out;
    // Plain LCS table. Edit snippets are small; very large inputs fall back to remove-all/add-all.
    if ((double)a.size() * (double)b.size() > 4e6) {
        for (const auto& l : a) { out += "-" + l + "\n"; ++s.removed; }
        for (const auto& l : b) { out += "+" + l + "\n"; ++s.added; }
        if (stat) *stat = s;
        return out;
    }
    size_t n = a.size(), m = b.size();
    std::vector<std::vector<int>> lcs(n + 1, std::vector<int>(m + 1, 0));
    for (size_t i = n; i-- > 0;) {
        for (size_t j = m; j-- > 0;) {
            lcs[i][j] = a[i] == b[j] ? lcs[i + 1][j + 1] + 1 : std::max(lcs[i + 1][j], lcs[i][j + 1]);
        }
    }
    size_t i = 0, j = 0;
    while (i < n || j < m) {
        if (i < n && j < m && a[i] == b[j]) {
            out += " " + a[i] + "\n";
            ++i; ++j;
        } else if (j < m && (i == n || lcs[i][j + 1] >= lcs[i + 1][j])) {
            out += "+" + b[j] + "\n";
            ++s.added; ++j;
        } else {
            out += "-" + a[i] + "\n";
            ++s.removed; ++i;
        }
    }
    if (stat) *stat = s;
    return out;
}

std::string normalizeCodexDiff(const std::string& kind, const std::string& diff, DiffStat* stat) {
    bool unified = diff.find("\n@@") != std::string::npos || util::startsWith(diff, "@@") ||
                   util::startsWith(diff, "--- ") || diff.find("\n+++ ") != std::string::npos;
    if (!unified && kind == "add") return newFileDiff(diff, stat);
    if (!unified && kind == "delete") {
        int n = 0;
        std::string out = prefixed(diff, '-', &n);
        if (stat) { stat->added = 0; stat->removed = n; }
        return out;
    }
    if (stat) *stat = countUnifiedDiff(diff);
    return diff;
}

Verdict parseVerdict(const std::string& text, std::string* rest) {
    std::vector<std::string> lines = util::splitLines(text);
    Verdict v = Verdict::None;
    int at = -1;
    for (int k = (int)lines.size() - 1; k >= 0; --k) {
        std::string t = util::trim(lines[k]);
        // Tolerate markdown decoration such as **VERDICT: APPROVE** or `VERDICT: APPROVE`.
        std::string bare;
        for (char c : t) {
            if (c != '*' && c != '`' && c != '_') bare += c;
        }
        bare = util::trim(bare);
        std::string up = bare;
        for (auto& c : up) {
            if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        }
        if (!util::startsWith(up, "VERDICT")) {
            if (!t.empty()) break;  // only trailing lines count
            continue;
        }
        size_t colon = up.find(':');
        size_t wide = up.find("\xEF\xBC\x9A");  // full-width colon
        std::string val;
        if (colon != std::string::npos && (wide == std::string::npos || colon < wide)) val = up.substr(colon + 1);
        else if (wide != std::string::npos) val = up.substr(wide + 3);
        else val = up.substr(7);
        val = util::trim(val);
        if (util::startsWith(val, "APPROVE")) v = Verdict::Approve;
        else if (util::startsWith(val, "CHANGED")) v = Verdict::Changed;
        else if (util::startsWith(val, "ISSUES")) v = Verdict::Issues;
        at = k;
        break;
    }
    if (rest) {
        if (at < 0) {
            *rest = text;
        } else {
            std::string out;
            for (int k = 0; k < at; ++k) {
                out += lines[k];
                out += '\n';
            }
            *rest = util::trim(out);
        }
    }
    return v;
}

const char* verdictName(Verdict v) {
    switch (v) {
    case Verdict::Approve: return "approve";
    case Verdict::Changed: return "changed";
    case Verdict::Issues: return "issues";
    default: return "";
    }
}
