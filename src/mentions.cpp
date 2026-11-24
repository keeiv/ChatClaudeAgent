// "@path" mentions in the composer: a project file search for the picker, and the note agents receive listing the
// mentioned files. Agents read the files with their own tools; CCA only confirms the paths exist inside the project.
#include "app.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <thread>

#include "snapshot.hpp"
#include "util.hpp"

namespace {

// Dependency and build folders never help a mention and can hold hundreds of thousands of files.
bool skippedFolder(const std::wstring& name) {
    static const wchar_t* skip[] = {
        L".git", L"node_modules", L".venv", L"venv",  L"__pycache__", L".mypy_cache", L"dist", L"build",
        L"out",  L"target",       L".next", L".nuxt", L".gradle",     L".idea",       L".vs",  L".cca-artifacts"};
    for (const auto* s : skip)
        if (_wcsicmp(name.c_str(), s) == 0) return true;
    return false;
}

void walk(const std::wstring& root, const std::wstring& rel, int depth, std::vector<std::string>& out) {
    if (depth > 12 || out.size() >= 20000) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(util::joinPath(util::joinPath(root, rel), L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        // Links could lead outside the project; they are not offered.
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        const std::wstring child = rel.empty() ? name : rel + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!skippedFolder(name)) walk(root, child, depth + 1, out);
        } else {
            out.push_back(util::replaceAll(util::narrow(child), "\\", "/"));
        }
    } while (FindNextFileW(h, &fd) && out.size() < 20000);
    FindClose(h);
}

std::string lower(std::string s) {
    for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
    return s;
}

long long lineCount(const std::wstring& file) {
    std::string text;
    if (!util::readFile(file, text) || text.size() > 8 * 1024 * 1024) return -1;
    if (text.find('\0') != std::string::npos) return -1;  // binary
    long long n = std::count(text.begin(), text.end(), '\n');
    return text.empty() || text.back() == '\n' ? n : n + 1;
}

}  // namespace

Json searchProjectFiles(const std::wstring& cwd, const std::string& query, size_t limit) {
    std::vector<std::string> files;
    walk(cwd, L"", 0, files);
    const std::string q = lower(util::replaceAll(util::trim(query), "\\", "/"));
    std::vector<std::pair<int, std::string>> hits;
    for (const auto& f : files) {
        const std::string l = lower(f);
        const size_t slash = l.rfind('/');
        const std::string base = slash == std::string::npos ? l : l.substr(slash + 1);
        int score = q.empty()                           ? 3
                    : util::startsWith(base, q)         ? 0
                    : base.find(q) != std::string::npos ? 1
                    : l.find(q) != std::string::npos    ? 2
                                                        : -1;
        if (score >= 0) hits.push_back({score, f});
    }
    std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) {
        return a.first != b.first                   ? a.first < b.first
               : a.second.size() != b.second.size() ? a.second.size() < b.second.size()
                                                    : a.second < b.second;
    });
    Json items = Json::array();
    for (const auto& h : hits) {
        if (items.size() >= limit) break;
        items.push(h.second);
    }
    return J({{"items", items}, {"scanned", (long long)files.size()}, {"truncated", files.size() >= 20000}});
}

std::string mentionNote(const std::wstring& cwd, const std::string& text) {
    if (cwd.empty()) return "";
    std::vector<std::string> lines, seen;
    for (size_t at = text.find('@'); at != std::string::npos && lines.size() < 20; at = text.find('@', at + 1)) {
        if (at > 0 && !isspace((unsigned char)text[at - 1])) continue;  // e-mail addresses and the like
        // The path ends at a space or at sentence punctuation; Chinese text often follows without a space.
        size_t end = at + 1;
        auto stops = [&](size_t i) {
            if (isspace((unsigned char)text[i]) || strchr(",;)]\"'", text[i])) return true;
            for (const char* mark : {"，", "。", "；", "：", "）", "」", "、", "！", "？"})
                if (text.compare(i, strlen(mark), mark) == 0) return true;
            return false;
        };
        while (end < text.size() && !stops(end)) ++end;
        std::string token = text.substr(at + 1, end - at - 1);
        while (!token.empty() && (token.back() == '.' || token.back() == ':')) token.pop_back();
        std::string range;
        if (size_t hash = token.find("#L"); hash != std::string::npos) {
            range = token.substr(hash + 2);
            token = token.substr(0, hash);
        }
        token = util::replaceAll(token, "\\", "/");
        if (token.empty() || !Snapshots::safeRelativePath(token)) continue;
        if (std::find(seen.begin(), seen.end(), token + "#" + range) != seen.end()) continue;
        const std::wstring full = util::joinPath(cwd, util::widen(util::replaceAll(token, "/", "\\")));
        std::string what;
        if (util::dirExists(full)) what = "資料夾";
        else if (util::fileExists(full)) {
            const long long n = lineCount(full);
            what = n < 0 ? "檔案" : std::to_string(n) + " 行";
        } else continue;
        seen.push_back(token + "#" + range);
        std::string range_text;
        if (!range.empty()) {
            const size_t dash = range.find('-');
            const std::string a = range.substr(0, dash), b = dash == std::string::npos ? "" : range.substr(dash + 1);
            const bool digits =
                !a.empty() && std::all_of(a.begin(), a.end(), ::isdigit) && std::all_of(b.begin(), b.end(), ::isdigit);
            if (digits) range_text = b.empty() ? "，第 " + a + " 行" : "，第 " + a + "–" + b + " 行";
        }
        lines.push_back("- " + token + "（" + what + range_text + "）");
    }
    if (lines.empty()) return "";
    std::string note = "\n\n使用者用 @ 指定了下列專案內的路徑，回答前先讀取它們：";
    for (const auto& l : lines) note += "\n" + l;
    return note;
}

void App::searchFiles(const Json& m) {
    Conversation* c = find(m["id"].str());
    const std::string reqId = m["reqId"].str();
    if (!c || c->cwd.empty() || !util::dirExists(c->cwd)) {
        send(J({{"type", "files.found"}, {"reqId", reqId}, {"items", Json::array()}}));
        return;
    }
    std::weak_ptr<int> life = lifetime_;
    std::thread([this, life, cwd = c->cwd, query = util::truncateUtf8(m["query"].str(), 200), reqId] {
        Json found = searchProjectFiles(cwd, query, 30);
        postToUi([this, life, reqId, found] {
            if (life.expired()) return;
            Json reply = found;
            reply["type"] = "files.found";
            reply["reqId"] = reqId;
            send(reply);
        });
    }).detach();
}
