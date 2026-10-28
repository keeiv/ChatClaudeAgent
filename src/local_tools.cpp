// Read-only file tools for the local reviewers and the citation check on their findings
// (see local_tools.hpp). Nothing here writes to disk or runs a program.
#include "local_tools.hpp"

#include <windows.h>

#include <algorithm>
#include <regex>

#include "util.hpp"

namespace {

const size_t kMaxFileBytes = 2 * 1024 * 1024;
const size_t kMaxToolOutput = 16 * 1024;
const int kMaxLinesPerRead = 300;
const int kMaxSearchHits = 60;
const int kMaxListEntries = 200;
const int kMaxScannedFiles = 3000;
const DWORD kRecallOnDataAccess = 0x00400000;  // FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS, missing from older SDKs

bool skipDir(const std::wstring& name) {
    static const wchar_t* skip[] = {L".git",        L"node_modules", L"build", L"dist", L".vs",
                                    L"__pycache__", L"out",          L"bin",   L"obj"};
    for (const wchar_t* s : skip) {
        if (_wcsicmp(name.c_str(), s) == 0) return true;
    }
    return false;
}

std::wstring fullPath(const std::wstring& p) {
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (n == 0) return L"";
    std::wstring out(n, L'\0');
    n = GetFullPathNameW(p.c_str(), n, out.data(), nullptr);
    out.resize(n);
    while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return out;
}

bool startsWithFold(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() &&
           CompareStringOrdinal(s.c_str(), (int)prefix.size(), prefix.c_str(), (int)prefix.size(), TRUE) == CSTR_EQUAL;
}

// The path with every junction and symbolic link resolved; empty when it cannot be opened.
// Opening with no access rights does not download OneDrive online-only files.
std::wstring finalPath(const std::wstring& p) {
    HANDLE h = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring out(MAX_PATH, L'\0');
    DWORD n = GetFinalPathNameByHandleW(h, out.data(), (DWORD)out.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n >= out.size()) {
        out.resize(n + 1);
        n = GetFinalPathNameByHandleW(h, out.data(), (DWORD)out.size(), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    CloseHandle(h);
    if (n == 0 || n >= out.size()) return L"";
    out.resize(n);
    if (out.rfind(L"\\\\?\\UNC\\", 0) == 0) out = L"\\\\" + out.substr(8);
    else if (out.rfind(L"\\\\?\\", 0) == 0) out = out.substr(4);
    while (out.size() > 3 && out.back() == L'\\') out.pop_back();
    return out;
}

// True when `path` exists and, with every link resolved, is the project root or inside it.
bool insideFinal(const std::wstring& root, const std::wstring& path, std::wstring* tail = nullptr) {
    std::wstring r = finalPath(root), p = finalPath(path);
    if (r.empty() || p.empty()) return false;
    bool same = CompareStringOrdinal(p.c_str(), -1, r.c_str(), -1, TRUE) == CSTR_EQUAL;
    if (!same && !startsWithFold(p, r + L"\\")) return false;
    if (tail) *tail = same ? L"" : p.substr(r.size() + 1);
    return true;
}

// The names tools/agent_runtime.py keeps away from agent tools too: secrets, credentials and tool state.
bool deniedName(std::wstring name) {
    for (auto& ch : name) ch = (wchar_t)towlower(ch);
    if (name.rfind(L".env", 0) == 0) return true;
    for (const wchar_t* d :
         {L".git", L".claude", L".codex", L".npmrc", L".netrc", L"credentials", L"provider-credentials"}) {
        if (name == d) return true;
    }
    return false;
}

bool deniedPath(const std::wstring& relative) {
    size_t start = 0;
    while (start <= relative.size()) {
        size_t end = relative.find_first_of(L"\\/", start);
        if (end == std::wstring::npos) end = relative.size();
        if (end > start && deniedName(relative.substr(start, end - start))) return true;
        start = end + 1;
    }
    return false;
}

std::string relOf(const std::wstring& root, const std::wstring& full) {
    std::wstring r = fullPath(root);
    if (full.size() <= r.size()) return ".";
    return util::narrow(full.substr(r.size() + 1));
}

bool readText(const std::wstring& path, std::string& out, std::string* error) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) ||
        (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        if (error) *error = "找不到這個檔案";
        return false;
    }
    if (fa.nFileSizeHigh || fa.nFileSizeLow > kMaxFileBytes) {
        if (error) *error = "檔案太大（超過 2 MB），請用 search 找需要的段落";
        return false;
    }
    if (!util::readFile(path, out)) {
        if (error) *error = "無法讀取這個檔案";
        return false;
    }
    if (out.substr(0, 8192).find('\0') != std::string::npos) {
        if (error) *error = "這是二進位檔，不能閱讀";
        return false;
    }
    if (util::startsWith(out, "\xEF\xBB\xBF")) out.erase(0, 3);
    return true;
}

std::vector<std::string> fileLines(const std::string& text) {
    std::vector<std::string> lines = util::splitLines(text);
    for (auto& l : lines) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
    }
    return lines;
}

int intArg(const Json& args, const char* key, int def) {
    const Json& v = args[key];
    if (v.isNumber()) return (int)v.i64();
    std::string s = util::trim(v.str());
    if (s.empty()) return def;
    int n = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return def;
        n = n * 10 + (c - '0');
        if (n > 1000000) return def;
    }
    return n;
}

std::string collapse(const std::string& s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        bool ws = c == ' ' || c == '\t' || c == '\r' || c == '\n';
        if (ws) {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

std::string stripQuote(std::string q) {
    q = util::trim(q);
    for (const char* pair : {"`", "\"", "'"}) {
        while (q.size() >= 2 && util::startsWith(q, pair) && util::endsWith(q, pair))
            q = util::trim(q.substr(1, q.size() - 2));
    }
    if (util::startsWith(q, "「") && util::endsWith(q, "」") && q.size() >= 6)
        q = util::trim(q.substr(3, q.size() - 6));
    for (const char* tail : {"…", "..."}) {
        while (util::endsWith(q, tail)) q = util::trim(q.substr(0, q.size() - std::string(tail).size()));
    }
    return q;
}

// Models often copy read_file's "12| " line prefixes into their quote; the code after them is what counts.
std::string withoutLineNumber(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && line[i] == ' ') ++i;
    size_t digits = i;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
    if (i == digits || i >= line.size() || line[i] != '|') return line;
    ++i;
    if (i < line.size() && line[i] == ' ') ++i;
    return line.substr(i);
}

// "位置：src\a.cpp:12-14" / "位置：`src/a.cpp` 第 12 行".
bool parseLocation(const std::string& line, std::string* path, int* from, int* to) {
    static const std::regex re(
        R"(([A-Za-z0-9_.\-\\/]+)`?\s*(?::|：|(?:，\s*)?第)\s*(\d+)(?:\s*(?:-|–|~|到|至)\s*(\d+))?)");
    std::string clean;
    for (char c : line) {
        if (c != '`' && c != '*') clean += c;
    }
    std::smatch m;
    if (!std::regex_search(clean, m, re)) return false;
    *path = m[1].str();
    *from = std::atoi(m[2].str().c_str());
    *to = m[3].matched ? std::atoi(m[3].str().c_str()) : *from;
    if (*to < *from) std::swap(*from, *to);
    return *from > 0;
}

// The text after "原文：" or "位置：" style labels (full- or half-width colon).
bool labelValue(const std::string& line, const char* label, std::string* value) {
    // The label may start the line ("- **位置：**") or follow the finding title on the same
    // line ("發現 1：位置：…"); it has to be followed by a colon either way.
    std::string lab = label;
    size_t at = 0;
    std::string rest;
    while ((at = line.find(lab, at)) != std::string::npos) {
        std::string after = line.substr(at + lab.size());
        while (util::startsWith(after, "*")) after.erase(0, 1);
        after = util::trim(after);
        if (util::startsWith(after, "：") || util::startsWith(after, ":")) {
            rest = after;
            break;
        }
        at += lab.size();
    }
    if (at == std::string::npos) return false;
    if (util::startsWith(rest, "：")) rest = rest.substr(3);
    else if (util::startsWith(rest, ":")) rest = rest.substr(1);
    else return false;
    // "**原文：**" leaves the closing bold marker behind.
    rest = util::trim(rest);
    while (util::startsWith(rest, "*")) rest.erase(0, 1);
    *value = util::trim(rest);
    return true;
}

// "發現 2：" starts a finding wherever it sits on the line ("指揮官，發現 2：…" included), so a
// second finding can never ride along inside a first one that happens to check out.
bool isFindingStart(const std::string& line) {
    const std::string word = "發現";
    for (size_t at = line.find(word); at != std::string::npos; at = line.find(word, at + word.size())) {
        std::string t = line.substr(at + word.size());
        while (!t.empty() && (t[0] == ' ' || t[0] == '*')) t.erase(0, 1);
        size_t digits = 0;
        while (digits < t.size() && t[digits] >= '0' && t[digits] <= '9') ++digits;
        if (digits == 0) continue;
        std::string rest = t.substr(digits);
        while (!rest.empty() && (rest[0] == ' ' || rest[0] == '*')) rest.erase(0, 1);
        if (util::startsWith(rest, "：") || util::startsWith(rest, ":") || util::startsWith(rest, "、") || rest.empty())
            return true;
    }
    return false;
}

}  // namespace

std::string normalizeRelPath(const std::string& path) {
    std::string p = util::trim(path);
    for (auto& c : p) {
        if (c == '/') c = '\\';
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    while (util::startsWith(p, ".\\")) p.erase(0, 2);
    return p;
}

bool resolveInside(const std::wstring& root, const std::string& rel, std::wstring* full, std::string* error) {
    std::string r = stripQuote(rel);
    for (auto& c : r) {
        if (c == '/') c = '\\';
    }
    std::wstring base = fullPath(root);
    if (base.empty()) {
        if (error) *error = "沒有專案資料夾";
        return false;
    }
    // A colon past the drive letter would be an alternate data stream.
    size_t colon = r.find(':');
    if (colon != std::string::npos && colon != 1) {
        if (error) *error = "路徑格式不正確";
        return false;
    }
    std::wstring candidate = (r.empty() || r == ".")                     ? base
                             : (colon == 1 || util::startsWith(r, "\\")) ? util::widen(r)
                                                                         : util::joinPath(base, util::widen(r));
    std::wstring f = fullPath(candidate);
    bool inside = !f.empty() && (CompareStringOrdinal(f.c_str(), -1, base.c_str(), -1, TRUE) == CSTR_EQUAL ||
                                 startsWithFold(f, base + L"\\"));
    if (!inside) {
        if (error) *error = "只能讀取專案資料夾裡面的檔案";
        return false;
    }
    const char* deniedText = "這個位置不開放閱讀（.git、.env、憑證與工具設定）";
    if (f.size() > base.size() && deniedPath(f.substr(base.size()))) {
        if (error) *error = deniedText;
        return false;
    }
    // A junction or symbolic link anywhere along the path could lead out of the project, so the
    // path the system actually opens has to be inside the project too. (OneDrive placeholders are
    // reparse points as well; they resolve to themselves and stay readable.)
    std::wstring finalTail;
    if (!insideFinal(base, f, &finalTail)) {
        if (error)
            *error =
                util::fileExists(f) || util::dirExists(f) ? "只能讀取專案資料夾裡面的檔案" : "找不到這個檔案或資料夾";
        return false;
    }
    // Short names such as GIT~1 only show their real name after resolution.
    if (deniedPath(finalTail)) {
        if (error) *error = deniedText;
        return false;
    }
    if (full) *full = f;
    return true;
}

Json localToolSpecs() {
    auto required = [](const char* key) {
        Json a = Json::array();
        a.push(key);
        return a;
    };
    auto fn = [](const char* name, const char* desc, Json props, Json req) {
        return J(
            {{"type", "function"},
             {"function", J({{"name", name},
                             {"description", desc},
                             {"parameters", J({{"type", "object"}, {"properties", props}, {"required", req}})}})}});
    };
    Json tools = Json::array();
    tools.push(
        fn("read_file",
           "讀取專案資料夾內的文字檔，回傳帶行號的內容。一次最多 300 行，可以用 start_line 和 end_line 指定範圍。",
           J({{"path", J({{"type", "string"}, {"description", "相對於專案資料夾的路徑，例如 src\\app.cpp"}})},
              {"start_line", J({{"type", "integer"}, {"description", "從第幾行開始（預設 1）"}})},
              {"end_line", J({{"type", "integer"}, {"description", "讀到第幾行（預設 start_line + 299）"}})}}),
           required("path")));
    tools.push(fn("list_dir", "列出專案資料夾內某個資料夾的檔案與子資料夾。",
                  J({{"path", J({{"type", "string"}, {"description", "相對路徑，專案根目錄用 ."}})}}), Json::array()));
    tools.push(fn("search", "在專案資料夾內搜尋文字（不分大小寫），回傳檔案、行號和該行內容，最多 60 筆。",
                  J({{"text", J({{"type", "string"}, {"description", "要找的文字"}})},
                     {"path", J({{"type", "string"}, {"description", "只搜尋這個資料夾或檔案（預設整個專案）"}})}}),
                  required("text")));
    return tools;
}

LocalToolResult runLocalTool(const std::wstring& root, const std::string& name, const Json& args) {
    LocalToolResult r;
    std::string err;
    std::wstring full;
    if (name == "read_file") {
        r.kind = "read";
        r.title = "讀取檔案";
        std::string path = args["path"].str();
        r.detail = path;
        std::string text;
        if (!resolveInside(root, path, &full, &err) || !readText(full, text, &err)) {
            r.output = "錯誤：" + err;
            return r;
        }
        std::vector<std::string> lines = fileLines(text);
        int total = (int)lines.size();
        int from = std::max(1, intArg(args, "start_line", 1));
        int to = intArg(args, "end_line", from + kMaxLinesPerRead - 1);
        to = std::min({to, total, from + kMaxLinesPerRead - 1});
        std::string rel = relOf(root, full);
        r.path = normalizeRelPath(rel);
        r.detail =
            rel + (total ? "（第 " + std::to_string(from) + "–" + std::to_string(std::max(from, to)) + " 行）" : "");
        if (total == 0) {
            r.ok = true;
            r.output = "檔案：" + rel + "（空檔案）";
            return r;
        }
        if (from > total) {
            r.output = "錯誤：這個檔案只有 " + std::to_string(total) + " 行";
            return r;
        }
        std::string out = "檔案：" + rel + "（共 " + std::to_string(total) + " 行，以下是第 " + std::to_string(from) +
                          "–" + std::to_string(to) + " 行）\n";
        int last = from - 1;
        for (int i = from; i <= to; ++i) {
            std::string row = std::to_string(i) + "| " + lines[i - 1] + "\n";
            if (out.size() + row.size() > kMaxToolOutput) break;
            out += row;
            last = i;
        }
        if (last < total)
            out += "（還有第 " + std::to_string(last + 1) + " 行之後的內容，需要時用 start_line 繼續讀）\n";
        r.ok = true;
        r.output = out;
        return r;
    }
    if (name == "list_dir") {
        r.kind = "search";
        r.title = "列出資料夾";
        std::string path = args["path"].str(".");
        r.detail = path;
        if (!resolveInside(root, path, &full, &err)) {
            r.output = "錯誤：" + err;
            return r;
        }
        std::vector<std::string> dirs, files;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(util::joinPath(full, L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            r.output = "錯誤：找不到這個資料夾";
            return r;
        }
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L".." || deniedName(n)) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!skipDir(n)) dirs.push_back(util::narrow(n) + "\\");
            } else {
                files.push_back(util::narrow(n));
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(dirs.begin(), dirs.end());
        std::sort(files.begin(), files.end());
        std::string rel = relOf(root, full);
        std::string out = "資料夾：" + rel + "\n";
        int n = 0;
        for (const auto* list : {&dirs, &files}) {
            for (const auto& e : *list) {
                if (++n > kMaxListEntries) break;
                out += e + "\n";
            }
        }
        if (n > kMaxListEntries) out += "（項目太多，只列出前 " + std::to_string(kMaxListEntries) + " 個）\n";
        r.detail = rel;
        r.ok = true;
        r.output = out;
        return r;
    }
    if (name == "search") {
        r.kind = "search";
        r.title = "搜尋內容";
        std::string needle = args["text"].str(args["pattern"].str());
        std::string path = args["path"].str(".");
        r.detail = needle + (path == "." || path.empty() ? "" : "（" + path + "）");
        if (util::trim(needle).empty()) {
            r.output = "錯誤：請提供要搜尋的文字";
            return r;
        }
        if (!resolveInside(root, path, &full, &err)) {
            r.output = "錯誤：" + err;
            return r;
        }
        std::string lowNeedle = util::toLower(needle);
        std::string out;
        int hits = 0, scanned = 0;
        std::vector<std::wstring> stack{full};
        while (!stack.empty() && hits < kMaxSearchHits && scanned < kMaxScannedFiles) {
            std::wstring cur = stack.back();
            stack.pop_back();
            DWORD attr = GetFileAttributesW(cur.c_str());
            std::vector<std::wstring> targets;
            if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
                targets.push_back(cur);
            } else {
                WIN32_FIND_DATAW fd;
                HANDLE h = FindFirstFileW(util::joinPath(cur, L"*").c_str(), &fd);
                if (h == INVALID_HANDLE_VALUE) continue;
                do {
                    std::wstring n = fd.cFileName;
                    if (n == L"." || n == L".." || deniedName(n)) continue;
                    // Online-only OneDrive files would be downloaded just to be searched; read_file can still open
                    // them.
                    if (fd.dwFileAttributes & (kRecallOnDataAccess | FILE_ATTRIBUTE_OFFLINE)) continue;
                    std::wstring p = util::joinPath(cur, n);
                    // A link or junction is followed only when it leads somewhere inside the project.
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && !insideFinal(root, p)) continue;
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        if (!skipDir(n)) stack.push_back(p);
                    } else if (fd.nFileSizeHigh == 0 && fd.nFileSizeLow <= kMaxFileBytes / 2) {
                        targets.push_back(p);
                    }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
            for (const auto& file : targets) {
                if (++scanned > kMaxScannedFiles || hits >= kMaxSearchHits) break;
                std::string text;
                if (!readText(file, text, nullptr)) continue;
                std::vector<std::string> lines = fileLines(text);
                for (size_t i = 0; i < lines.size() && hits < kMaxSearchHits; ++i) {
                    if (util::toLower(lines[i]).find(lowNeedle) == std::string::npos) continue;
                    out += relOf(root, file) + ":" + std::to_string(i + 1) + ": " +
                           util::truncateUtf8(util::trim(lines[i]), 200) + "\n";
                    ++hits;
                }
            }
        }
        if (hits == 0) out = "沒有找到「" + needle + "」。\n";
        else if (hits >= kMaxSearchHits) out += "（結果太多，只列出前 " + std::to_string(kMaxSearchHits) + " 筆）\n";
        r.ok = true;
        r.output = util::truncateUtf8(out, kMaxToolOutput);
        return r;
    }
    r.kind = "search";
    r.title = "未知工具";
    r.detail = name;
    r.output = "錯誤：沒有這個工具。可以用的工具是 read_file、list_dir、search。";
    return r;
}

bool extractTextToolCalls(const std::string& content, Json* calls, std::string* before) {
    size_t first = std::string::npos;
    for (size_t start = content.find('{'); start != std::string::npos; start = content.find('{', start + 1)) {
        // The matching close brace, skipping braces inside strings.
        int depth = 0;
        bool str = false, esc = false;
        size_t end = std::string::npos;
        for (size_t i = start; i < content.size(); ++i) {
            char c = content[i];
            if (str) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') str = false;
                continue;
            }
            if (c == '"') str = true;
            else if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) {
                end = i;
                break;
            }
        }
        if (end == std::string::npos) break;
        Json j = Json::parse(content.substr(start, end - start + 1));
        std::string name = j["name"].str();
        Json args = j.has("parameters") ? j["parameters"] : j["arguments"];
        if (args.isString()) args = Json::parse(args.str());
        if ((name == "read_file" || name == "list_dir" || name == "search") && args.isObject()) {
            if (first == std::string::npos) first = start;
            calls->push(J({{"function", J({{"name", name}, {"arguments", args}})}}));
            start = end;
        }
    }
    if (first == std::string::npos) return false;
    if (before) *before = util::trim(content.substr(0, first));
    return true;
}

LocalReviewCheck checkLocalReview(const std::wstring& root, const std::string& reply,
                                  const std::set<std::string>& readPaths, const std::vector<std::string>& changedPaths,
                                  const GuardRules& rules) {
    LocalReviewCheck out;
    std::vector<std::string> lines = util::splitLines(reply);
    for (auto& l : lines) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
    }

    // Split the reply into findings.
    int current = -1;
    bool fence = false, quoting = false;
    std::vector<std::string> fenceLines;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        std::string t = util::trim(line);
        if (util::startsWith(t, "```")) {
            fence = !fence;
            if (current >= 0) out.findings[current].text += line + "\n";
            // A fenced block right after an empty "原文：" is the quote.
            if (!fence && current >= 0 && out.findings[current].quote == "\x01") {
                std::string q;
                for (const auto& f : fenceLines) {
                    if (!util::trim(f).empty()) q += (q.empty() ? "" : "\n") + f;
                }
                out.findings[current].quote = q;
                quoting = false;
            }
            fenceLines.clear();
            continue;
        }
        if (fence) {
            fenceLines.push_back(line);
            if (current >= 0) out.findings[current].text += line + "\n";
            continue;
        }
        if (isFindingStart(line)) {
            LocalFinding f;
            f.index = (int)out.findings.size() + 1;
            out.findings.push_back(f);
            current = (int)out.findings.size() - 1;
            quoting = false;
        } else if (current >= 0 &&
                   (util::startsWith(t, "總結") || util::startsWith(t, "結論") || util::startsWith(t, "## "))) {
            current = -1;
        }
        if (current < 0) continue;
        LocalFinding& f = out.findings[current];
        f.text += line + "\n";
        std::string value;
        bool label = false;
        for (const char* l : {"問題", "建議", "信心", "位置", "原文"}) {
            if (labelValue(line, l, &value)) label = true;
        }
        if (f.path.empty() && labelValue(line, "位置", &value)) parseLocation(value, &f.path, &f.line, &f.lineEnd);
        if (f.quote.empty() && labelValue(line, "原文", &value)) {
            f.quote = value.empty() ? "\x01" : value;
            quoting = value.empty();
        } else if (quoting && !label && !t.empty()) {
            // An empty "原文：" followed by plain lines, without a code fence.
            f.quote = f.quote == "\x01" ? line : f.quote + "\n" + line;
        } else if (quoting && (label || (t.empty() && f.quote != "\x01"))) {
            quoting = false;
        }
    }

    // Check every finding against the file it cites.
    for (auto& f : out.findings) {
        if (f.quote == "\x01") f.quote.clear();
        std::string norm = normalizeRelPath(f.path);
        std::wstring full;
        std::string text, err;
        if (f.path.empty() || f.line <= 0) {
            f.why = "沒有寫「位置：檔案:行號」";
        } else if (!resolveInside(root, f.path, &full, &err) || !readText(full, text, &err)) {
            f.why = f.path + " " + err;
        } else if (!readPaths.count(norm)) {
            f.why = "沒有用 read_file 讀過 " + f.path;
        } else {
            std::vector<std::string> fl = fileLines(text);
            std::vector<std::string> quoteLines;
            std::string joined;
            for (const auto& part : util::splitLines(stripQuote(f.quote))) {
                std::string p = collapse(stripQuote(withoutLineNumber(part)));
                if (p.empty()) continue;
                quoteLines.push_back(p);
                joined += (joined.empty() ? "" : " ") + p;
            }
            std::string q = joined;
            if ((int)fl.size() < f.line) {
                f.why =
                    f.path + " 只有 " + std::to_string(fl.size()) + " 行，第 " + std::to_string(f.line) + " 行不存在";
            } else if (q.empty()) {
                f.why = "沒有附「原文：」";
            } else {
                int a = std::max(1, f.line - 3), b = std::min((int)fl.size(), std::max(f.line, f.lineEnd) + 3);
                std::string window;
                for (int i = a; i <= b; ++i) window += fl[i - 1] + "\n";
                window = collapse(window);
                std::string probe = q.size() > 80 ? util::truncateUtf8(q, 60) : q;
                // Multi-line quotes: every line of the quote has to be there.
                bool all = !quoteLines.empty();
                for (const auto& p : quoteLines) {
                    if (window.find(p.size() > 80 ? util::truncateUtf8(p, 60) : p) == std::string::npos) all = false;
                }
                f.verified = window.find(probe) != std::string::npos || all;
                if (!f.verified) f.why = f.path + " 第 " + std::to_string(f.line) + " 行附近找不到這段原文";
            }
        }
    }

    for (const char* phrase :
         {"沒有發現問題", "未發現問題", "沒有發現明顯問題", "沒有發現任何問題", "沒有發現明顯的問題"}) {
        if (reply.find(phrase) != std::string::npos) out.saysNoIssues = true;
    }

    // What must be sent back.
    bool readChanged = changedPaths.empty();
    for (const auto& p : changedPaths) {
        if (readPaths.count(p)) readChanged = true;
    }
    if (!readChanged) {
        std::string list;
        for (size_t i = 0; i < changedPaths.size() && i < 6; ++i) list += (list.empty() ? "" : "、") + changedPaths[i];
        out.issues.push_back({"local-unread", "還沒有用 read_file 讀過任何一個變更的檔案（" + list +
                                                  "）。先讀檔，再根據檔案內容給建議。"});
    }
    if (out.findings.empty() && !out.saysNoIssues) {
        out.issues.push_back({"local-format",
                              "回覆沒有照格式。每條建議都要以「發現 1：」開頭，並寫「位置：檔案:行號」和「原文：」"
                              "（從檔案逐字複製）；看不出問題就寫「沒有發現問題」，並列出讀過的檔案。"});
    }
    std::string bad;
    for (const auto& f : out.findings) {
        if (!f.verified) bad += "\n- 發現 " + std::to_string(f.index) + "：" + f.why;
    }
    if (!bad.empty()) {
        out.issues.push_back({"local-cite", "以下建議和檔案對不上，CCA 無法採用：" + bad +
                                                "\n請用 read_file 重新讀那一段，改正行號並逐字複製原文，或刪掉這條。"});
    }
    GuardRules lang = rules;
    lang.requireVerdict = lang.requireEvidence = false;
    lang.localNames.clear();
    for (auto& i : guardCheck(lang, reply, Json::array())) out.issues.push_back(i);

    // What Claude/GPT get: only findings that match the files.
    for (const auto& f : out.findings) {
        if (f.verified) out.forwarded += util::trim(f.text) + "\n\n";
        else ++out.removed;
    }
    if (out.findings.empty() && out.saysNoIssues && readChanged) {
        std::string read;
        for (const auto& p : readPaths) read += (read.empty() ? "" : "、") + p;
        out.forwarded = "沒有發現問題（讀過：" + read + "）。\n";
    }
    if (out.removed > 0) {
        out.forwarded += "（另有 " + std::to_string(out.removed) + " 條建議的引用和檔案對不上，CCA 已移除。）\n";
    }
    return out;
}
