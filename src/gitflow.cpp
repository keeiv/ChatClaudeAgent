// Version-control pane rules: what a file change is, whether it should be committed, and what a commit holds.
#include "gitflow.hpp"

#include <regex>

#include "util.hpp"

namespace gitflow {
namespace {

// '*' and '?' stay inside one segment; "**" crosses segments.
bool wild(const char* p, const char* s) {
    while (*p) {
        if (p[0] == '*' && p[1] == '*') {
            p += 2;
            if (*p == '/') ++p;
            for (const char* t = s;; ++t) {
                if (wild(p, t)) return true;
                if (!*t) return false;
            }
        }
        if (*p == '*') {
            ++p;
            for (const char* t = s;; ++t) {
                if (wild(p, t)) return true;
                if (!*t || *t == '/') return false;
            }
        }
        if (!*s) return false;
        if (*p == '?' ? *s == '/' : *p != *s) return false;
        ++p;
        ++s;
    }
    return !*s;
}

std::vector<std::string> segments(const std::string& path) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        if (end > start) out.push_back(path.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

std::string baseName(const std::string& path) {
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string extension(const std::string& path) {
    auto name = baseName(path);
    auto dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? "" : util::toLower(name.substr(dot + 1));
}

// Likely credentials, dependency or build output, binaries and scratch files: shown red, never preselected.
std::string riskReason(const std::string& path, long long size) {
    const auto lower = util::toLower(path), name = baseName(lower), ext = extension(lower);
    if ((name == ".env" || util::startsWith(name, ".env.")) && name != ".env.example" && name != ".env.sample" &&
        name != ".env.template")
        return "可能含金鑰或環境設定";
    for (const char* pattern : {"*.pem", "*.key", "*.p12", "*.pfx", "*.keystore", "*.jks", "*.kdbx", "id_rsa*",
                                "id_ed25519*", "id_ecdsa*", "credentials*", "secrets*", "*.secret"})
        if (wild(pattern, name.c_str())) return "可能含金鑰或憑證";
    for (const auto& part : segments(lower))
        for (const char* dir : {"node_modules", "__pycache__", ".vs", ".idea", "dist", "build", "out", "obj", "bin",
                                "target", ".gradle", ".next", ".venv", "venv"})
            if (part == dir && part != name) return "建置產物或相依套件";
    for (const char* binary : {"exe", "dll", "obj", "pdb", "lib", "so", "dylib", "o", "class", "jar", "zip", "7z",
                               "rar", "msi", "iso", "bin"})
        if (ext == binary) return "二進位檔";
    for (const char* scratch : {"log", "tmp", "bak", "swp"})
        if (ext == scratch) return "暫存或記錄檔";
    if (name == ".ds_store" || name == "thumbs.db" || name == "desktop.ini") return "系統產生的檔案";
    if (size > 5 * 1024 * 1024) return "檔案大於 5 MB";
    return "";
}

}  // namespace

bool globMatch(const std::string& pattern, const std::string& path) {
    std::string p = util::toLower(util::replaceAll(util::trim(pattern), "\\", "/"));
    const std::string s = util::toLower(util::replaceAll(path, "\\", "/"));
    if (p.empty()) return false;
    const bool directory = p.back() == '/';
    if (directory) p.pop_back();
    const bool anchored = p.find('/') != std::string::npos;
    if (!p.empty() && p.front() == '/') p.erase(0, 1);
    const auto parts = segments(s);
    if (!anchored) {
        // A bare name matches a file name anywhere, or any directory on the way to the file.
        for (size_t i = 0; i < parts.size(); ++i) {
            const bool last = i + 1 == parts.size();
            if ((!directory || !last) && wild(p.c_str(), parts[i].c_str())) return true;
        }
        return false;
    }
    if (directory) {
        std::string prefix;
        for (size_t i = 0; i + 1 < parts.size(); ++i) {
            prefix += (i ? "/" : "") + parts[i];
            if (wild(p.c_str(), prefix.c_str())) return true;
        }
        return false;
    }
    if (wild(p.c_str(), s.c_str())) return true;
    // An anchored directory such as "docs/private" also covers everything under it.
    return wild((p + "/**").c_str(), s.c_str());
}

Json parseStatus(const std::string& raw) {
    Json files = Json::array();
    size_t at = 0;
    while (at + 3 < raw.size()) {
        const size_t end = raw.find('\0', at);
        if (end == std::string::npos) break;
        const std::string entry = raw.substr(at, end - at);
        at = end + 1;
        if (entry.size() < 4) continue;
        const char x = entry[0], y = entry[1];
        Json file = J({{"path", entry.substr(3)}, {"x", std::string(1, x)}, {"y", std::string(1, y)}});
        if (x == 'R' || x == 'C') {
            // Renames carry the original path as the following NUL-separated field.
            const size_t next = raw.find('\0', at);
            if (next == std::string::npos) break;
            file["from"] = raw.substr(at, next - at);
            at = next + 1;
        }
        const std::string xy{x, y};
        std::string kind = "modified";
        if (xy == "??") kind = "untracked";
        else if (x == 'U' || y == 'U' || xy == "AA" || xy == "DD") kind = "conflict";
        else if (x == 'A') kind = "added";
        else if (x == 'D' || y == 'D') kind = "deleted";
        else if (x == 'R' || x == 'C') kind = "renamed";
        file["kind"] = kind;
        files.push(file);
    }
    return files;
}

Json classify(const Json& files, const Json& rules, const Json& sizes) {
    Json out = Json::array();
    for (const auto& item : files.items()) {
        Json file = item;
        const auto path = file["path"].str(), kind = file["kind"].str();
        std::string rule;
        for (const auto& r : rules.items())
            if (globMatch(r.str(), path)) {
                rule = r.str();
                break;
            }
        std::string reason = kind == "conflict" ? "有未解決的衝突" : "";
        if (reason.empty() && kind != "deleted") reason = riskReason(path, sizes[path].i64());
        Json reasons = Json::array();
        if (!rule.empty()) reasons.push("永遠不提交規則：" + rule);
        if (!reason.empty()) reasons.push(reason);
        file["locked"] = !rule.empty();
        file["rule"] = rule;
        file["reasons"] = reasons;
        file["tone"] = !reasons.empty() ? "risk" : kind == "untracked" || kind == "added" ? "new" : "changed";
        file["size"] = sizes[path].i64();
        // Only ordinary changes are preselected; a red tile has to be chosen on purpose.
        file["selected"] = reasons.empty();
        out.push(file);
    }
    return out;
}

Json secretHits(const std::string& diff) {
    static const std::vector<std::pair<std::string, std::regex>> patterns = {
        {"私鑰", std::regex("-----BEGIN [A-Z ]*PRIVATE KEY-----")},
        {"AWS 存取金鑰", std::regex("AKIA[0-9A-Z]{16}")},
        {"API 金鑰（sk-）", std::regex("sk-[A-Za-z0-9_-]{20,}")},
        {"GitHub 權杖", std::regex("(ghp_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{20,})")},
        {"Slack 權杖", std::regex("xox[baprs]-[A-Za-z0-9-]{10,}")},
        {"Google API 金鑰", std::regex("AIza[0-9A-Za-z_-]{35}")},
        {"寫死的密碼或權杖",
         std::regex("(api[_-]?key|secret|password|passwd|token)[\"']?\\s*[:=]\\s*[\"'][^\"'\\s]{12,}[\"']",
                    std::regex::icase)},
    };
    Json hits = Json::array();
    std::string path;
    long long line = 0;
    for (const auto& text : util::splitLines(diff)) {
        if (util::startsWith(text, "+++ ")) {
            path = text.substr(4);
            if (util::startsWith(path, "b/")) path.erase(0, 2);
            continue;
        }
        if (util::startsWith(text, "@@")) {
            static const std::regex hunk("\\+(\\d+)");
            std::smatch m;
            if (std::regex_search(text, m, hunk)) line = std::stoll(m[1].str()) - 1;
            continue;
        }
        if (text.empty() || text[0] == '-' || text[0] == '\\') continue;
        ++line;
        if (text[0] != '+' || hits.size() >= 50) continue;
        for (const auto& p : patterns)
            if (std::regex_search(text, p.second)) {
                hits.push(J({{"path", path}, {"line", line}, {"kind", p.first}}));
                break;
            }
    }
    return hits;
}

Json parseLog(const std::string& raw) {
    Json commits = Json::array();
    size_t at = 0;
    while (at < raw.size()) {
        size_t end = raw.find('\x1e', at);
        if (end == std::string::npos) end = raw.size();
        std::string record = util::trim(raw.substr(at, end - at));
        at = end + 1;
        if (record.empty()) continue;
        std::vector<std::string> fields;
        size_t start = 0;
        while (true) {
            size_t sep = record.find('\x1f', start);
            fields.push_back(record.substr(start, sep == std::string::npos ? std::string::npos : sep - start));
            if (sep == std::string::npos) break;
            start = sep + 1;
        }
        if (fields.size() < 6 || fields[0].size() < 7) continue;
        Json parents = Json::array();
        for (const auto& p : segments(util::replaceAll(fields[1], " ", "/"))) parents.push(p);
        Json refs = Json::array();
        for (auto ref : util::splitLines(util::replaceAll(fields[4], ", ", "\n")))
            if (!util::trim(ref).empty()) refs.push(util::trim(ref));
        commits.push(J({{"hash", fields[0]},
                        {"parents", parents},
                        {"author", util::truncateUtf8(fields[2], 120)},
                        {"ts", fields[3].empty() ? 0LL : std::stoll(fields[3]) * 1000},
                        {"refs", refs},
                        {"subject", util::truncateUtf8(fields[5], 300)},
                        {"task", fields.size() > 6 ? util::trim(fields[6]) : ""}}));
    }
    return commits;
}

Json defaultRules() {
    return Json(Json::Array{".env", ".env.local", "*.pem", "*.key", "*.pfx", "id_rsa*", ".cca-artifacts/"});
}

bool validRule(const std::string& pattern) {
    const auto p = util::trim(pattern);
    if (p.empty() || p.size() > 200 || p.find("..") != std::string::npos || p.find(':') != std::string::npos)
        return false;
    for (unsigned char c : p)
        if (c < 32) return false;
    return true;
}

std::string authProblem(const std::string& gitError) {
    const auto e = util::toLower(gitError);
    // GitHub answers "not found" for a private repository when no account is signed in.
    for (const char* text :
         {"authentication failed", "could not read username", "could not read password", "terminal prompts disabled",
          "permission denied (publickey", "access denied", "returned error: 401", "returned error: 403",
          "repository not found", "invalid username or password", "password authentication was removed",
          "user cancelled", "interactivity has been disabled", "host key verification failed"})
        if (e.find(text) != std::string::npos) return "auth";
    for (const char* text :
         {"could not resolve host", "failed to connect", "connection timed out", "connection refused",
          "network is unreachable", "operation timed out", "connection was reset", "could not resolve hostname"})
        if (e.find(text) != std::string::npos) return "network";
    return "";
}

Json remoteHost(const std::string& url) {
    std::string rest = util::trim(url);
    bool ssh = false;
    const auto scheme = rest.find("://");
    if (scheme != std::string::npos) {
        ssh = util::toLower(rest.substr(0, scheme)) == "ssh";
        rest = rest.substr(scheme + 3);
    } else if (rest.find(':') != std::string::npos && rest.find('@') != std::string::npos) {
        ssh = true;  // scp-like "git@host:owner/repo.git"
    } else {
        return J({{"host", ""}, {"ssh", false}});  // a local path or bundle
    }
    const auto end = rest.find_first_of(ssh && scheme == std::string::npos ? ":/" : "/");
    std::string authority = rest.substr(0, end);
    if (const auto at = authority.rfind('@'); at != std::string::npos) authority = authority.substr(at + 1);
    if (!ssh || scheme != std::string::npos)
        if (const auto colon = authority.find(':'); colon != std::string::npos) authority.resize(colon);
    return J({{"host", util::toLower(authority)}, {"ssh", ssh}});
}
}  // namespace gitflow
