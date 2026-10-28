#include "claude_usage.hpp"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <ctime>

#include "util.hpp"

#pragma comment(lib, "winhttp.lib")

namespace {

const wchar_t* kDefaultUrl = L"https://api.anthropic.com/api/oauth/usage";

Json window(const Json& w, const std::string& source) {
    // The endpoint reports percent (23.0); CCA keeps 0..1 like rate_limit_event does.
    double pct = w["utilization"].num(-1);
    if (pct < 0) return Json();
    long long reset = w["resets_at"].isNumber() ? w["resets_at"].i64() : parseIsoTime(w["resets_at"].str());
    std::string status = pct >= 100 ? "rejected" : pct >= 80 ? "allowed_warning" : "allowed";
    return J(
        {{"utilization", std::min(pct, 100.0) / 100.0}, {"resetsAt", reset}, {"status", status}, {"source", source}});
}

std::wstring credentialsPath() {
    std::wstring over = util::getEnv(L"CCA_CLAUDE_CREDENTIALS");
    if (!over.empty()) return over;
    return util::joinPath(util::joinPath(util::homeDir(), L".claude"), L".credentials.json");
}

}  // namespace

long long parseIsoTime(const std::string& iso) {
    if (iso.size() < 20 || iso[4] != '-' || iso[7] != '-' || iso[10] != 'T' || iso[13] != ':' || iso[16] != ':')
        return 0;
    for (int i : {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18})
        if (!isdigit((unsigned char)iso[i])) return 0;
    auto number = [&](size_t from, size_t count) {
        int n = 0;
        while (count--) n = n * 10 + iso[from++] - '0';
        return n;
    };
    int y = number(0, 4), mo = number(5, 2), d = number(8, 2), h = number(11, 2), mi = number(14, 2), s = number(17, 2);
    const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || h > 23 || mi > 59 || s > 59 ||
        d > days[mo - 1] + (mo == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)))
        return 0;
    size_t off = 19;
    if (iso[off] == '.') {
        size_t start = ++off;
        while (off < iso.size() && isdigit((unsigned char)iso[off])) ++off;
        if (off == start) return 0;
    }
    long long offset = 0;
    if (off == iso.size() - 1 && iso[off] == 'Z') {
    } else if (off + 6 == iso.size() && (iso[off] == '+' || iso[off] == '-') && iso[off + 3] == ':' &&
               isdigit((unsigned char)iso[off + 1]) && isdigit((unsigned char)iso[off + 2]) &&
               isdigit((unsigned char)iso[off + 4]) && isdigit((unsigned char)iso[off + 5])) {
        int oh = number(off + 1, 2), om = number(off + 4, 2);
        if (oh > 23 || om > 59) return 0;
        offset = (iso[off] == '+' ? 1 : -1) * (oh * 3600LL + om * 60LL);
    } else return 0;
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    long long t = (long long)_mkgmtime(&tm);
    if (t < 0) return 0;
    return t - offset;
}

Json parseClaudeUsage(const Json& body) {
    Json out = Json::object();
    for (const auto& f : body.fields()) {
        if (!f.second.isObject()) continue;
        Json w = window(f.second, "usage");
        if (!w.isNull()) out[f.first] = w;
    }
    // A newer shape lists windows in an array; take whatever names them.
    for (const auto& item : body["limits"].items()) {
        std::string name = item["type"].str(item["name"].str(item["window"].str()));
        Json w = window(item, "usage");
        if (!name.empty() && !w.isNull()) out[name] = w;
    }
    return out;
}

ClaudeUsageResult fetchClaudeUsage() {
    ClaudeUsageResult r;
    std::string cred;
    if (!util::readFile(credentialsPath(), cred)) {
        r.error = "找不到 Claude Code 的登入資料，請先在 Claude Code 登入";
        return r;
    }
    std::string token;
    long long expires = 0;
    {
        Json parsed = Json::parse(cred);
        token = parsed["claudeAiOauth"]["accessToken"].str();
        expires = parsed["claudeAiOauth"]["expiresAt"].i64();
    }
    cred.assign(cred.size(), '\0');
    if (token.empty()) {
        r.error = "Claude Code 的登入資料裡沒有訂閱權杖";
        return r;
    }
    // Refreshing would rotate Claude Code's own tokens, so an expired one is left alone.
    if (expires > 0 && expires < util::nowMs()) {
        r.error = "Claude Code 的登入權杖已過期，下次使用 Claude 後會自動更新";
        return r;
    }

    std::wstring url = util::getEnv(L"CCA_CLAUDE_USAGE_URL");
    if (url.empty()) url = kDefaultUrl;
    URL_COMPONENTS parts{sizeof parts};
    wchar_t host[256] = L"", path[1024] = L"";
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        r.error = "額度網址無效";
        return r;
    }
    bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    bool test = std::wstring(host) == L"127.0.0.1" && !util::getEnv(L"CCA_CLAUDE_CREDENTIALS").empty();
    if ((!secure || _wcsicmp(host, L"api.anthropic.com") != 0 || parts.nPort != 443) && !test) {
        r.error = "額度查詢只允許 Anthropic 官方 HTTPS 來源；測試必須使用本機模擬登入檔";
        return r;
    }
    HINTERNET session = WinHttpOpen(L"ChatClaudeAgent", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET conn = session ? WinHttpConnect(session, host, parts.nPort, 0) : nullptr;
    HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0)
                         : nullptr;
    std::string body;
    if (req) {
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect);
        WinHttpSetTimeouts(req, 5000, 5000, 10000, 10000);
        std::wstring headers = L"Authorization: Bearer " + util::widen(token) +
                               L"\r\nanthropic-beta: oauth-2025-04-20\r\nAccept: application/json\r\n";
        BOOL sent = WinHttpSendRequest(req, headers.c_str(), (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        headers.assign(headers.size(), L'\0');
        if (sent && WinHttpReceiveResponse(req, nullptr)) {
            DWORD code = 0, size = sizeof code;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
            r.http = (int)code;
            char buf[4096];
            DWORD n = 0;
            while (WinHttpReadData(req, buf, sizeof buf, &n) && n > 0 && body.size() < 256 * 1024) body.append(buf, n);
        }
    }
    token.assign(token.size(), '\0');
    if (req) WinHttpCloseHandle(req);
    if (conn) WinHttpCloseHandle(conn);
    if (session) WinHttpCloseHandle(session);
    if (r.http == 0) {
        r.error = "連不上 Claude 的額度服務";
        return r;
    }
    if (r.http == 401 || r.http == 403) {
        r.error = "Claude 拒絕了額度查詢（登入可能已失效）";
        return r;
    }
    if (r.http != 200) {
        r.error = "Claude 的額度服務回應 " + std::to_string(r.http);
        return r;
    }
    r.limits = parseClaudeUsage(Json::parse(body));
    r.ok = r.limits.fields().size() > 0;
    if (!r.ok) r.error = "額度服務的回應格式看不懂，已改回只用對話中的回報";
    return r;
}
