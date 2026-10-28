#include "util.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <iterator>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

namespace util {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string randomHex(size_t bytes) {
    std::vector<unsigned char> buf(bytes);
    BCryptGenRandom(nullptr, buf.data(), (ULONG)buf.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned char b : buf) {
        out += hex[b >> 4];
        out += hex[b & 15];
    }
    return out;
}

std::string newId() {
    // Time prefix keeps ids sortable; the random suffix keeps them unique.
    char buf[32];
    std::snprintf(buf, sizeof buf, "%011llx", (unsigned long long)nowMs());
    return std::string(buf) + randomHex(4);
}

static std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) out = p;
    CoTaskMemFree(p);
    return out;
}

std::wstring appDataDir() {
    // CCA_DATA_DIR keeps tests and experiments away from the real conversation history.
    std::wstring d = getEnv(L"CCA_DATA_DIR");
    if (d.empty()) d = joinPath(knownFolder(FOLDERID_RoamingAppData), L"CCA");
    ensureDir(d);
    return d;
}

void* lockDataDir() {
    // One lock per data folder (FNV-1a of its lower-cased path), so a test run with
    // CCA_DATA_DIR does not collide with the real one.
    std::wstring dir = appDataDir();
    // Relative paths, trailing slashes and directory aliases must share the lock.
    HANDLE folder = CreateFileW(dir.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (folder != INVALID_HANDLE_VALUE) {
        DWORD count = GetFinalPathNameByHandleW(folder, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (count) {
            std::wstring resolved(count, L'\0');
            DWORD n = GetFinalPathNameByHandleW(folder, resolved.data(), count, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (n && n < count) { resolved.resize(n); dir = std::move(resolved); }
        }
        CloseHandle(folder);
    }
    if (dir.rfind(L"\\\\?\\UNC\\", 0) == 0) dir = L"\\\\" + dir.substr(8);
    else if (dir.rfind(L"\\\\?\\", 0) == 0) dir.erase(0, 4);
    for (auto& c : dir) if (c == L'/') c = L'\\';
    while (dir.size() > 3 && dir.back() == L'\\') dir.pop_back();
    unsigned long long h = 1469598103934665603ULL;
    for (wchar_t c : dir) {
        h ^= (unsigned long long)towlower(c);
        h *= 1099511628211ULL;
    }
    wchar_t name[64];
    swprintf(name, 64, L"Local\\CCA.Data.%016llx", h);
    HANDLE m = CreateMutexW(nullptr, TRUE, name);
    if (!m) return nullptr;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(m);
        return nullptr;
    }
    return m;
}

std::wstring localAppDataDir() {
    std::wstring isolated = getEnv(L"CCA_DATA_DIR");
    std::wstring d =
        isolated.empty() ? joinPath(knownFolder(FOLDERID_LocalAppData), L"CCA") : joinPath(isolated, L"local");
    ensureDir(d);
    return d;
}

std::wstring homeDir() { return knownFolder(FOLDERID_Profile); }

std::wstring exeDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    std::wstring p(buf, n);
    size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? p : p.substr(0, k);
}

std::wstring joinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

bool ensureDir(const std::wstring& path) {
    if (dirExists(path)) return true;
    return SHCreateDirectoryExW(nullptr, path.c_str(), nullptr) == ERROR_SUCCESS || dirExists(path);
}

bool fileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dirExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool readFile(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    out.resize((size_t)size.QuadPart);
    DWORD read = 0;
    bool ok = size.QuadPart == 0 || ReadFile(h, out.data(), (DWORD)out.size(), &read, nullptr);
    CloseHandle(h);
    out.resize(read);
    return ok;
}

bool writeFileAtomic(const std::wstring& path, const std::string& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, data.data(), (DWORD)data.size(), &written, nullptr) && written == data.size();
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::wstring getEnv(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return {};
    std::wstring v(n, L'\0');
    n = GetEnvironmentVariableW(name, v.data(), n);
    v.resize(n);
    return v;
}

std::string windowsUserName() {
    wchar_t buf[256];
    DWORD n = (DWORD)std::size(buf);
    if (GetUserNameW(buf, &n) && n > 1) return narrow(std::wstring(buf, n - 1));
    return "使用者";
}

std::wstring findOnPath(const std::wstring& name) {
    std::wstring path = getEnv(L"PATH");
    std::wstring exts = getEnv(L"PATHEXT");
    if (exts.empty()) exts = L".COM;.EXE;.BAT;.CMD";
    std::vector<std::wstring> extList;
    size_t s = 0;
    while (s <= exts.size()) {
        size_t e = exts.find(L';', s);
        if (e == std::wstring::npos) e = exts.size();
        if (e > s) extList.push_back(exts.substr(s, e - s));
        s = e + 1;
    }
    // Prefer real executables over script shims.
    std::stable_sort(extList.begin(), extList.end(), [](const std::wstring& a, const std::wstring& b) {
        auto rank = [](const std::wstring& x) { return _wcsicmp(x.c_str(), L".EXE") == 0 ? 0 : 1; };
        return rank(a) < rank(b);
    });
    s = 0;
    while (s <= path.size()) {
        size_t e = path.find(L';', s);
        if (e == std::wstring::npos) e = path.size();
        std::wstring dir = path.substr(s, e - s);
        s = e + 1;
        if (dir.empty()) continue;
        if (dir.front() == L'"' && dir.back() == L'"' && dir.size() >= 2) dir = dir.substr(1, dir.size() - 2);
        for (const auto& ext : extList) {
            std::wstring cand = joinPath(dir, name + ext);
            if (fileExists(cand)) return cand;
        }
    }
    return {};
}

std::string base64UrlDecode(const std::string& in) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-' || c == '+') return 62;
        if (c == '_' || c == '/') return 63;
        return -1;
    };
    std::string out;
    int buf = 0, bits = 0;
    for (char c : in) {
        int v = val(c);
        if (v < 0) continue;
        // Only the low bits are ever read back; masking keeps long inputs (images) from overflowing.
        buf = ((buf << 6) | v) & 0xFFFFFF;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += char((buf >> bits) & 0xFF);
        }
    }
    return out;
}

std::string jwtPayload(const std::string& token) {
    size_t a = token.find('.');
    if (a == std::string::npos) return {};
    size_t b = token.find('.', a + 1);
    if (b == std::string::npos) return {};
    return base64UrlDecode(token.substr(a + 1, b - a - 1));
}

std::wstring quoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') { ++i; ++backslashes; }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
        } else {
            out.append(backslashes, L'\\');
            out += arg[i];
        }
    }
    out += L'"';
    return out;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

bool endsWith(const std::string& s, const std::string& x) {
    return s.size() >= x.size() && s.compare(s.size() - x.size(), x.size(), x) == 0;
}

std::string toLower(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    }
    return s;
}

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t e = s.find('\n', start);
        if (e == std::string::npos) {
            if (start < s.size()) out.push_back(s.substr(start));
            break;
        }
        std::string line = s.substr(start, e - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
        start = e + 1;
    }
    return out;
}

std::string truncateUtf8(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t n = maxBytes;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}

std::string tailUtf8(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t start = s.size() - maxBytes;
    while (start < s.size() && ((unsigned char)s[start] & 0xC0) == 0x80) ++start;
    return s.substr(start);
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string displayPath(const std::string& path, const std::wstring& rootW) {
    std::string p = replaceAll(path, "/", "\\");
    std::string root = replaceAll(narrow(rootW), "/", "\\");
    if (!root.empty() && root.back() != '\\') root += '\\';
    if (!root.empty() && p.size() > root.size() && _strnicmp(p.c_str(), root.c_str(), root.size()) == 0) {
        return p.substr(root.size());
    }
    return p;
}

static std::wstring normalizeDir(const std::wstring& p) {
    if (p.empty()) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(p.c_str(), (DWORD)std::size(buf), buf, nullptr);
    std::wstring s = n && n < std::size(buf) ? std::wstring(buf, n) : p;
    for (auto& c : s) {
        if (c == L'/') c = L'\\';
    }
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::string projectFolderProblem(const std::wstring& path) {
    std::wstring p = normalizeDir(path);
    if (p.empty()) return "請選擇一個資料夾";
    if (p.size() <= 3) return "不能用整個磁碟當專案資料夾，請選擇裡面的某個專案資料夾";  // "c:\"
    if (p.rfind(L"\\\\", 0) == 0 && std::count(p.begin(), p.end(), L'\\') <= 3) return "不能用整個網路共用資料夾當專案資料夾";
    std::wstring home = normalizeDir(homeDir());
    if (!home.empty() && (p == home || (home.size() > p.size() && home.compare(0, p.size(), p) == 0 && home[p.size()] == L'\\'))) {
        return "不能用使用者資料夾（或它的上層）當專案資料夾，AI 會取得整個資料夾的寫入權限。請選擇裡面的某個專案資料夾";
    }
    const KNOWNFOLDERID* broad[] = {&FOLDERID_Documents, &FOLDERID_Desktop, &FOLDERID_Downloads, &FOLDERID_RoamingAppData,
                                    &FOLDERID_LocalAppData, &FOLDERID_Pictures, &FOLDERID_Music, &FOLDERID_Videos};
    for (const KNOWNFOLDERID* id : broad) {
        if (p == normalizeDir(knownFolder(*id))) return "這個資料夾範圍太大（裡面通常有很多不相關的檔案），請選擇裡面的某個專案資料夾";
    }
    std::wstring onedrive = normalizeDir(getEnv(L"OneDrive"));
    if (!onedrive.empty() && p == onedrive) return "不能用整個 OneDrive 當專案資料夾，請選擇裡面的某個專案資料夾";
    const KNOWNFOLDERID* system[] = {&FOLDERID_Windows, &FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86, &FOLDERID_ProgramData};
    for (const KNOWNFOLDERID* id : system) {
        std::wstring s = normalizeDir(knownFolder(*id));
        if (!s.empty() && (p == s || (p.size() > s.size() && p.compare(0, s.size(), s) == 0 && p[s.size()] == L'\\'))) {
            return "不能用系統資料夾當專案資料夾";
        }
    }
    return {};
}

}  // namespace util
