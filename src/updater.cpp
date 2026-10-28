// Release files are staged before shutdown; the broker never writes conversation or project data.
#include "updater.hpp"

#include <windows.h>

#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <vector>

#include "process.hpp"
#include "themes.hpp"
#include "util.hpp"

namespace updater {
namespace {
constexpr size_t kMaxBinary = 256 * 1024 * 1024;
Json failure(const std::string& why) {
    return J({{"ok", false}, {"error", why}});
}
bool plainFile(const std::wstring& path) {
    auto attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}
bool boundedRead(const std::wstring& path, std::string& out, size_t max) {
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!plainFile(path) || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr) || attr.nFileSizeHigh ||
        attr.nFileSizeLow > max)
        return false;
    return util::readFile(path, out) && out.size() <= max;
}
bool hexHash(const std::string& hash) {
    return hash.size() == 64 && std::all_of(hash.begin(), hash.end(),
                                            [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool releaseName(const std::string& name) {
    if (name.size() < 5 || name.size() > 120 || !util::endsWith(name, ".exe")) return false;
    return std::all_of(name.begin(), name.end(),
                       [](unsigned char c) {
                           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                                  c == '-' || c == '_' || c == '.';
                       }) &&
           name.find("..") == std::string::npos;
}
bool fetch(const std::string& source, std::string& out, size_t max) {
    out.clear();
    if (!util::startsWith(source, "https://")) return boundedRead(util::widen(source), out, max);
    auto url = util::widen(source);
    URL_COMPONENTS uc{sizeof uc};
    uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) return false;
    auto session = WinHttpOpen(L"CCA-Updater/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 15000);
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength), path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    auto connection = WinHttpConnect(session, host.c_str(), uc.nPort, 0);
    auto request = connection ? WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, nullptr,
                                                   WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
                              : nullptr;
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    bool ok = request && WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect) &&
              WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0, bytes = sizeof status;
    ok = ok &&
         WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &bytes,
                             nullptr) &&
         status == 200;
    std::array<char, 65536> buffer;
    const auto deadline = GetTickCount64() + 120000;
    while (ok) {
        DWORD count = 0;
        ok = WinHttpReadData(request, buffer.data(), (DWORD)buffer.size(), &count) != FALSE;
        if (!ok || count == 0) break;
        if (out.size() + count > max || GetTickCount64() > deadline) {
            ok = false;
            break;
        }
        out.append(buffer.data(), count);
    }
    if (request) WinHttpCloseHandle(request);
    if (connection) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}
std::string siblingSource(const std::string& source, const std::string& name) {
    const auto slash = source.find_last_of("/\\");
    return slash == std::string::npos ? "" : source.substr(0, slash + 1) + name;
}
bool pe64(const std::string& data) {
    if (data.size() < 256 || data.substr(0, 2) != "MZ") return false;
    DWORD offset = 0;
    memcpy(&offset, data.data() + 60, sizeof offset);
    if (offset > data.size() - 26 || data.substr(offset, 4) != std::string("PE\0\0", 4)) return false;
    WORD machine = 0;
    memcpy(&machine, data.data() + offset + 4, sizeof machine);
    return machine == IMAGE_FILE_MACHINE_AMD64;
}
// The ordinary edition is "", special editions are named after model tiers and keep their own version line.
bool knownEdition(const std::string& edition) {
    return edition.empty() || edition == "haiku" || edition == "sonnet" || edition == "opus" || edition == "fable";
}
// The release version recorded in a CCA executable of this flavor; empty for anything else.
std::string productVersion(const std::wstring& path, const std::string& flavor, std::string* edition = nullptr) {
    DWORD ignored = 0;
    DWORD length = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!length || length > 65536) return "";
    std::vector<unsigned char> data(length);
    if (!GetFileVersionInfoW(path.c_str(), 0, length, data.data())) return "";
    auto field = [&](const wchar_t* key) {
        std::wstring query = L"\\StringFileInfo\\040404b0\\" + std::wstring(key);
        wchar_t* value = nullptr;
        UINT count = 0;
        return VerQueryValueW(data.data(), query.c_str(), (void**)&value, &count) && count > 0
                   ? util::narrow(std::wstring(value, count - 1))
                   : "";
    };
    int parts[4]{};
    const auto release = field(L"ProductVersion");
    // Builds before 1.0.1-astra have no CCAEdition field and are ordinary editions.
    const auto kind = field(L"CCAEdition");
    if (edition) *edition = kind;
    return field(L"ProductName") == "ChatClaudeAgent" && version(release, parts) && knownEdition(kind) &&
                   field(L"OriginalFilename") == (flavor == "cli" ? "ChatClaudeAgent-CLI.exe" : "ChatClaudeAgent.exe")
               ? release
               : "";
}
bool metadata(const std::wstring& path, const std::string& release, const std::string& flavor,
              const std::string& edition) {
    std::string found;
    return !release.empty() && productVersion(path, flavor, &found) == release && found == edition;
}
// Visits the folder's direct entries, never following links or junctions out of it.
template <typename Visit>
void eachEntry(const std::wstring& dir, Visit visit) {
    WIN32_FIND_DATAW entry{};
    HANDLE find =
        FindFirstFileExW(util::joinPath(dir, L"*").c_str(), FindExInfoBasic, &entry, FindExSearchNameMatch, nullptr, 0);
    if (find == INVALID_HANDLE_VALUE) return;
    int seen = 0;
    do {
        const std::wstring name = entry.cFileName;
        if (name == L"." || name == L".." || (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        visit(entry);
    } while (++seen < 256 && FindNextFileW(find, &entry));
    FindClose(find);
}
struct Package {
    std::string release, notes, edition;
    std::wstring path;
    Json file;
    bool manifest = false;
};
bool readManifest(const std::wstring& path, const std::string& flavor, Package& out) {
    std::string raw;
    if (!boundedRead(path, raw, 65536)) return false;
    Json manifest = Json::parse(raw);
    const Json& file = manifest["files"][flavor];
    int parsed[4]{};
    const auto name = file["name"].str();
    if (manifest["schema"].i64() != 1 || !version(manifest["version"].str(), parsed) || !releaseName(name) ||
        !hexHash(file["sha256"].str()) || file["size"].i64() < 256 || file["size"].i64() > kMaxBinary ||
        !knownEdition(manifest["edition"].str()))
        return false;
    out.edition = manifest["edition"].str();
    const auto slash = path.find_last_of(L"\\/");
    out.path = path.substr(0, slash + 1) + util::widen(name);
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!plainFile(out.path) || !GetFileAttributesExW(out.path.c_str(), GetFileExInfoStandard, &attr) ||
        attr.nFileSizeHigh || (long long)attr.nFileSizeLow != file["size"].i64())
        return false;
    out.release = manifest["version"].str();
    out.notes = util::truncateUtf8(manifest["notes"].str(), 8000);
    out.file = file;
    out.manifest = true;
    return true;
}
bool hashFile(const std::wstring& path, const std::string& expected) {
    std::string data;
    return boundedRead(path, data, kMaxBinary) && sha256(data) == expected;
}
std::wstring statusText = L"等待 CCA 儲存並關閉";
unsigned frame = 0;
bool complete = false;
bool animate = true;
COLORREF windowBackground = RGB(19, 22, 26), windowText = RGB(233, 237, 241), windowSignal = RGB(124, 196, 255);
COLORREF asColor(uint32_t rgb) {
    return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}
LRESULT CALLBACK brokerWindow(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_TIMER) {
        if (animate) ++frame;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    if (message == WM_CLOSE) {
        if (complete) DestroyWindow(window);
        return 0;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC target = BeginPaint(window, &ps), dc = CreateCompatibleDC(target);
        RECT area;
        GetClientRect(window, &area);
        HBITMAP bitmap = CreateCompatibleBitmap(target, area.right, area.bottom);
        auto oldBitmap = SelectObject(dc, bitmap);
        HBRUSH bg = CreateSolidBrush(windowBackground);
        FillRect(dc, &area, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        HBRUSH mark = CreateSolidBrush(windowText);
        auto oldBrush = SelectObject(dc, mark), oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        // The two existing C strokes hand a single signal square to each other, without spinning text.
        const int x = area.right / 2 - 64, y = 24;
        const int motion = complete ? 0 : (int)(std::sin(frame * .065) * 5);
        const POINT top[] = {{20, 3},  {8, 3},   {3, 8},  {3, 16}, {8, 21}, {20, 21},
                             {20, 16}, {10, 16}, {8, 14}, {8, 10}, {10, 8}, {20, 8}};
        POINT points[12];
        for (int i = 0; i < 12; ++i) points[i] = {x + top[i].x * 4 - motion, y + top[i].y * 4};
        Polygon(dc, points, 12);
        for (int i = 0; i < 12; ++i) points[i] = {x + (32 - top[i].x) * 4 + motion, y + (32 - top[i].y) * 4};
        Polygon(dc, points, 12);
        SelectObject(dc, oldBrush);
        DeleteObject(mark);
        HBRUSH signal = CreateSolidBrush(windowSignal);
        const int travel = complete ? 0 : (frame % 100);
        RECT dot{x + 40 + travel / 2, y + 59, x + 46 + travel / 2, y + 65};
        FillRect(dc, &dot, signal);
        DeleteObject(signal);
        SelectObject(dc, oldPen);
        SetTextColor(dc, windowText);
        HFONT font =
            CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
        auto oldFont = SelectObject(dc, font);
        RECT caption{20, 178, area.right - 20, 240};
        DrawTextW(dc, statusText.c_str(), -1, &caption, DT_CENTER | DT_WORDBREAK);
        SelectObject(dc, oldFont);
        DeleteObject(font);
        BitBlt(target, 0, 0, area.right, area.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBitmap);
        DeleteObject(bitmap);
        DeleteDC(dc);
        EndPaint(window, &ps);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
void pump(HWND window, const std::wstring& text) {
    statusText = text;
    InvalidateRect(window, nullptr, FALSE);
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}
}  // namespace

bool version(const std::string& text, int parts[4]) {
    const auto dash = text.find('-');
    std::string number = text.substr(0, dash), suffix = dash == std::string::npos ? "" : text.substr(dash + 1);
    const std::array<std::string, 6> suffixes{"", "oss", "luna", "terra", "sol", "astra"};
    auto found = std::find(suffixes.begin(), suffixes.end(), suffix);
    if (found == suffixes.end()) return false;
    parts[3] = (int)std::distance(suffixes.begin(), found);
    size_t start = 0;
    for (int i = 0; i < 3; ++i) {
        auto end = number.find('.', start);
        if ((i < 2 && end == std::string::npos) || (i == 2 && end != std::string::npos)) return false;
        if (end == std::string::npos) end = number.size();
        if (end == start || end - start > 6 || (end - start > 1 && number[start] == '0')) return false;
        const auto result = std::from_chars(number.data() + start, number.data() + end, parts[i]);
        if (result.ec != std::errc() || result.ptr != number.data() + end || parts[i] < 0) return false;
        start = end + 1;
    }
    return true;
}
int compare(const std::string& left, const std::string& right) {
    int a[4]{}, b[4]{};
    if (!version(left, a) || !version(right, b)) return 0;
    for (int i = 0; i < 4; ++i)
        if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
    return 0;
}
std::string sha256(const std::string& data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return "";
    unsigned char bytes[32];
    const auto status = BCryptHash(algorithm, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(), bytes, 32);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) return "";
    std::string out;
    const char* hex = "0123456789abcdef";
    for (auto byte : bytes) {
        out += hex[byte >> 4];
        out += hex[byte & 15];
    }
    return out;
}
std::wstring executable() {
    std::wstring path(32768, L'\0');
    auto length = GetModuleFileNameW(nullptr, path.data(), (DWORD)path.size());
    path.resize(length);
    return path;
}
bool validSource(const std::string& source) {
    if (source.empty()) return true;
    if (source.size() > 2048 || source.find_first_of("?#\r\n@") != std::string::npos) return false;
    if (util::startsWith(source, "https://")) return util::endsWith(source, "/manifest.json");
    return source.size() > 3 && source[1] == ':' && (source[2] == '\\' || source[2] == '/') &&
           util::endsWith(source, "manifest.json") && plainFile(util::widen(source));
}
Json inspect(const std::string& source, const std::string& current, const std::wstring& exe,
             const std::string& flavor) {
    if (source.empty()) return J({{"ok", true}, {"status", "unconfigured"}, {"current", current}});
    if (!validSource(source)) return failure("更新來源須為 HTTPS manifest.json，或明確指定的本機發行清單");
    std::string raw;
    if (!fetch(source, raw, 65536)) return failure("無法讀取更新清單；請確認來源、TLS 憑證與網路");
    Json manifest = Json::parse(raw);
    const auto release = manifest["version"].str();
    int parsed[4]{};
    if (manifest["schema"].i64() != 1 || !version(release, parsed) || !version(current, parsed))
        return failure("更新清單版本格式不符");
    const Json& file = manifest["files"][flavor];
    const std::string name = file["name"].str(), hash = file["sha256"].str();
    if (!releaseName(name) || !hexHash(hash) || file["size"].i64() < 256 || file["size"].i64() > kMaxBinary)
        return failure("更新檔名、大小或 SHA-256 格式不符");
    std::string local;
    if (!boundedRead(exe, local, kMaxBinary)) return failure("無法核對本機執行檔");
    const auto localHash = sha256(local);
    const bool available = compare(release, current) > 0 && localHash != hash;
    return J({{"ok", true},
              {"status", available ? "available" : "current"},
              {"available", available},
              {"current", current},
              {"version", release},
              {"notes", util::truncateUtf8(manifest["notes"].str(), 8000)},
              {"file", file},
              {"flavor", flavor},
              {"localHash", localHash},
              {"source", siblingSource(source, name)}});
}
std::wstring inboxDir() {
    const auto dir = util::joinPath(util::appDataDir(), L"installers");
    util::ensureDir(dir);
    return dir;
}
std::string inboxStamp(const std::wstring& dir) {
    std::string stamp;
    auto add = [&](const std::wstring& name, const WIN32_FIND_DATAW& entry) {
        stamp += util::narrow(name) + "|" + std::to_string(entry.nFileSizeLow) + "|" +
                 std::to_string(entry.ftLastWriteTime.dwLowDateTime) + ";";
    };
    eachEntry(dir, [&](const WIN32_FIND_DATAW& entry) {
        add(entry.cFileName, entry);
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return;
        const auto folder = util::joinPath(dir, entry.cFileName);
        eachEntry(folder, [&](const WIN32_FIND_DATAW& inner) {
            add(entry.cFileName + (L"\\" + std::wstring(inner.cFileName)), inner);
        });
    });
    return stamp;
}
std::string editionName(const std::string& edition) {
    if (edition.empty()) return "普通版本";
    if (edition == "haiku") return "Haiku";
    if (edition == "sonnet") return "Sonnet";
    if (edition == "opus") return "Opus";
    if (edition == "fable") return "Fable";
    return "";
}
Json inspectFolder(const std::wstring& dir, const std::string& current, const std::wstring& exe,
                   const std::string& flavor, const std::string& edition) {
    int parsed[4]{};
    if (!util::dirExists(dir)) return failure("無法建立本機更新資料夾；請確認資料目錄權限");
    if (!version(current, parsed) || !knownEdition(edition)) return failure("本機版本格式不符");
    // A release folder from make_release.py, its manifest dropped directly, or a single CCA executable.
    std::vector<Package> packages;
    eachEntry(dir, [&](const WIN32_FIND_DATAW& entry) {
        const std::wstring name = entry.cFileName, path = util::joinPath(dir, name);
        Package found;
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (readManifest(util::joinPath(path, L"manifest.json"), flavor, found)) packages.push_back(found);
        } else if (_wcsicmp(name.c_str(), L"manifest.json") == 0) {
            if (readManifest(path, flavor, found)) packages.push_back(found);
        } else if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".exe") == 0 && !entry.nFileSizeHigh &&
                   entry.nFileSizeLow >= 256 && entry.nFileSizeLow <= kMaxBinary) {
            found.release = productVersion(path, flavor, &found.edition);
            found.path = path;
            found.file = J({{"name", util::narrow(name)}, {"size", (long long)entry.nFileSizeLow}});
            if (!found.release.empty()) packages.push_back(found);
        }
    });
    // The newest build of each edition; a manifest beats a loose executable of the same version.
    auto newest = [&](const std::string& kind) {
        const Package* best = nullptr;
        for (const auto& p : packages)
            if (p.edition == kind && (!best || compare(p.release, best->release) > 0 ||
                                      (compare(p.release, best->release) == 0 && p.manifest && !best->manifest)))
                best = &p;
        return best;
    };
    std::string local;
    if (!packages.empty() && !boundedRead(exe, local, kMaxBinary)) return failure("無法核對本機執行檔");
    const auto localHash = packages.empty() ? std::string() : sha256(local);
    // A single executable carries no declared hash, so the hash of the inspected bytes becomes the contract.
    auto contract = [&](const Package& p, Json& file) {
        file = p.file;
        if (p.manifest) return true;
        std::string data;
        if (!boundedRead(p.path, data, kMaxBinary) || !pe64(data)) return false;
        file["sha256"] = sha256(data);
        file["size"] = (long long)data.size();
        return true;
    };
    const auto folder = util::narrow(dir);
    // Haiku runs only on Android: its APK is listed so the user knows where it is, never offered for this computer.
    Json android = Json::array();
    eachEntry(dir, [&](const WIN32_FIND_DATAW& entry) {
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return;
        std::string raw;
        if (!boundedRead(util::joinPath(util::joinPath(dir, entry.cFileName), L"manifest.json"), raw, 65536)) return;
        Json manifest = Json::parse(raw);
        const Json& apk = manifest["files"]["android"];
        int parsed[4]{};
        if (manifest["schema"].i64() != 1 || !version(manifest["version"].str(), parsed) ||
            !knownEdition(manifest["edition"].str()) || !apk.isObject() || !util::endsWith(apk["name"].str(), ".apk") ||
            !releaseName(apk["name"].str().substr(0, apk["name"].str().size() - 4) + ".exe") ||
            !hexHash(apk["sha256"].str()))
            return;
        android.push(J({{"edition", manifest["edition"]},
                        {"editionName", editionName(manifest["edition"].str())},
                        {"version", manifest["version"]},
                        {"notes", util::truncateUtf8(manifest["notes"].str(), 2000)},
                        {"path", util::narrow(util::joinPath(util::joinPath(dir, entry.cFileName),
                                                             util::widen(apk["name"].str())))}}));
    });
    Json choices = Json::array();
    for (const char* kind : {"", "haiku", "sonnet", "opus", "fable"}) {
        if (kind == edition) continue;
        const Package* p = newest(kind);
        // Only ordinary releases that know how to restore the preserved settings may receive a special edition's user.
        if (!p || (!edition.empty() && std::string(kind).empty() && compare(p->release, "1.0.1-astra") < 0)) continue;
        Json file;
        if (!contract(*p, file) || file["sha256"].str() == localHash) continue;
        choices.push(J({{"edition", kind},
                        {"editionName", editionName(kind)},
                        {"kind", edition.empty()             ? "switch"
                                 : std::string(kind).empty() ? "return"
                                                             : "switch"},
                        {"version", p->release},
                        {"notes", p->notes},
                        {"file", file},
                        {"package", util::narrow(p->path.substr(dir.size() + 1))},
                        {"source", util::narrow(p->path)}}));
    }
    const Package* best = newest(edition);
    if (best && compare(best->release, current) <= 0) best = nullptr;
    Json result = J({{"ok", true},
                     {"status", packages.empty() ? "empty" : "current"},
                     {"available", false},
                     {"current", current},
                     {"edition", edition},
                     {"editionName", editionName(edition)},
                     {"packages", (long long)packages.size()},
                     {"choices", choices},
                     {"android", android},
                     {"folder", folder},
                     {"flavor", flavor},
                     {"localHash", localHash}});
    if (!best) return result;
    Json file;
    if (!contract(*best, file)) return failure("安裝包不是有效的 64 位元 Windows 執行檔");
    const bool available = localHash != file["sha256"].str();
    result["status"] = available ? "available" : "current";
    result["available"] = available;
    result["version"] = best->release;
    result["notes"] = best->notes;
    result["file"] = file;
    result["package"] = util::narrow(best->path.substr(dir.size() + 1));
    result["source"] = util::narrow(best->path);
    return result;
}
Json prepare(const Json& checked, const std::wstring& exe) {
    if (!checked["ok"].boolean() || !checked["available"].boolean()) return failure("沒有可安裝的較新版本");
    const auto file = checked["file"];
    std::string data;
    if (!fetch(checked["source"].str(), data, (size_t)file["size"].i64()) || data.size() != file["size"].i64() ||
        sha256(data) != file["sha256"].str() || !pe64(data))
        return failure("更新檔下載或核對失敗，本機版本未變更");
    if (!hashFile(exe, checked["localHash"].str())) return failure("本機執行檔在檢查後改變，請重新檢查更新");
    const auto dir = util::joinPath(util::localAppDataDir(), L"updates\\" + util::widen(util::newId()));
    const auto payload = util::joinPath(dir, L"payload.exe"), helper = util::joinPath(dir, L"broker.exe");
    if (!util::ensureDir(dir) || !util::writeFileAtomic(payload, data))
        return failure("無法準備更新檔；請確認儲存空間與目錄權限");
    if (!metadata(payload, checked["version"].str(), checked["flavor"].str(), checked["edition"].str()))
        return failure("發行清單與執行檔的產品、版本或類型不一致");
    if (!CopyFileW(exe.c_str(), helper.c_str(), TRUE)) return failure("無法準備獨立更新程式");
    Json plan = J({{"exe", util::narrow(exe)},
                   {"payload", util::narrow(payload)},
                   {"helper", util::narrow(helper)},
                   {"oldHash", checked["localHash"]},
                   {"newHash", file["sha256"]},
                   {"flavor", checked["flavor"]},
                   {"version", checked["version"]},
                   {"edition", checked["edition"]},
                   {"motion", checked["motion"].boolean(true)},
                   {"theme", checked["theme"]},
                   {"pid", (long long)GetCurrentProcessId()},
                   {"event", "Local\\CCA.Update." + util::randomHex(16)},
                   {"ok", true}});
    plan["plan"] = util::narrow(util::joinPath(dir, L"plan.json"));
    if (!util::writeFileAtomic(util::widen(plan["plan"].str()), plan.dump())) return failure("無法保存更新計畫");
    return plan;
}
bool launch(const Json& plan, std::string& error) {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, util::widen(plan["event"].str()).c_str());
    bool ok = event && launchUpdateBroker(util::widen(plan["helper"].str()), util::widen(plan["plan"].str()), &error);
    if (ok) ok = WaitForSingleObject(event, 10000) == WAIT_OBJECT_0;
    if (event) CloseHandle(event);
    if (!ok && error.empty()) error = "更新程式未準備完成，CCA 保持開啟";
    return ok;
}

void confirmStartup() {
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc == 3 && std::wstring(argv[1]) == L"--cca-update-started" &&
        util::startsWith(util::narrow(argv[2]), "Local\\CCA.Update.Ready.")) {
        HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[2]);
        if (event) {
            SetEvent(event);
            CloseHandle(event);
        }
    }
    if (argv) LocalFree(argv);
}

int helperMain() {
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return -1;
    bool helper = argc == 3 && std::wstring(argv[1]) == L"--cca-update-helper";
    std::wstring planPath = helper ? argv[2] : L"";
    LocalFree(argv);
    if (!helper) return -1;
    std::string raw;
    if (!boundedRead(planPath, raw, 16384)) return 2;
    Json plan = Json::parse(raw);
    const auto exe = util::widen(plan["exe"].str()), payload = util::widen(plan["payload"].str());
    const auto dir = util::exeDir();
    if (plan["helper"].str() != util::narrow(executable()) || payload != util::joinPath(dir, L"payload.exe") ||
        planPath != util::joinPath(dir, L"plan.json") || exe == executable() || !plainFile(exe) ||
        !hexHash(plan["newHash"].str()) || !hashFile(payload, plan["newHash"].str()) ||
        !hashFile(exe, plan["oldHash"].str()))
        return 3;
    BOOL animation = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animation, 0);
    animate = animation && plan["motion"].boolean(true);
    HANDLE parent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)plan["pid"].i64());
    wchar_t parentPath[32768];
    DWORD parentLength = 32768;
    if (!parent || !QueryFullProcessImageNameW(parent, 0, parentPath, &parentLength) ||
        _wcsicmp(exe.c_str(), parentPath) != 0) {
        if (parent) CloseHandle(parent);
        return 4;
    }
    Json theme = themeById(plan["theme"].str());
    if (!theme.isNull()) {
        windowBackground = asColor(themeColor(theme, "bg-side"));
        windowText = asColor(themeColor(theme, "text"));
        windowSignal = asColor(themeColor(theme, "signal"));
    }
    WNDCLASSW wc{};
    wc.lpfnWndProc = brokerWindow;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CCA.Update.Broker";
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(101));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND window = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"CCA · 更新", WS_OVERLAPPED | WS_CAPTION,
                                  (GetSystemMetrics(SM_CXSCREEN) - 420) / 2, (GetSystemMetrics(SM_CYSCREEN) - 285) / 2,
                                  420, 285, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) {
        CloseHandle(parent);
        return 5;
    }
    ShowWindow(window, SW_SHOWNORMAL);
    SetTimer(window, 1, 25, nullptr);
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, util::widen(plan["event"].str()).c_str());
    if (!ready) {
        CloseHandle(parent);
        DestroyWindow(window);
        return 6;
    }
    SetEvent(ready);
    CloseHandle(ready);
    const auto deadline = GetTickCount64() + 60000;
    while (WaitForSingleObject(parent, 0) != WAIT_OBJECT_0 && GetTickCount64() < deadline) {
        pump(window, L"等待 CCA 儲存並關閉");
        Sleep(25);
    }
    bool exited = WaitForSingleObject(parent, 0) == WAIT_OBJECT_0;
    CloseHandle(parent);
    const auto replacement = exe + L".cca-next",
               backup = exe + L".cca-previous-" + util::widen(plan["oldHash"].str().substr(0, 12));
    bool replaced = false;
    std::string error;
    if (exited && hashFile(exe, plan["oldHash"].str()) && !util::fileExists(replacement) && !util::fileExists(backup) &&
        CopyFileW(payload.c_str(), replacement.c_str(), TRUE)) {
        pump(window, L"替換已核對的版本");
        replaced = ReplaceFileW(exe.c_str(), replacement.c_str(), backup.c_str(), 0, nullptr, nullptr) != FALSE;
        if (!replaced && hashFile(backup, plan["oldHash"].str())) {
            if (hashFile(exe, plan["newHash"].str())) replaced = true;
            else if (!hashFile(exe, plan["oldHash"].str()))
                MoveFileExW(backup.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
        if (replaced && !hashFile(exe, plan["newHash"].str())) {
            replaced = false;
            MoveFileExW(backup.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        }
    }
    if (replaced) {
        pump(window, L"正在重新開啟 CCA");
        auto eventName = util::widen("Local\\CCA.Update.Ready." + util::randomHex(16));
        HANDLE startup = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        HANDLE child =
            startup ? launchUpdatedApplication(exe, eventName, plan["flavor"].str() == "cli", &error) : nullptr;
        bool launched = false;
        const auto startupDeadline = GetTickCount64() + 30000;
        while (child && GetTickCount64() < startupDeadline) {
            if (WaitForSingleObject(startup, 0) == WAIT_OBJECT_0) {
                launched = true;
                break;
            }
            if (WaitForSingleObject(child, 0) == WAIT_OBJECT_0) break;
            pump(window, L"正在確認 CCA 已啟動");
            Sleep(25);
        }
        if (child) {
            if (!launched) {
                TerminateProcess(child, 9);
                WaitForSingleObject(child, 5000);
            }
            CloseHandle(child);
        }
        if (startup) CloseHandle(startup);
        if (!launched) {
            // Preserve both executables if rollback itself fails; never delete the last good copy.
            MoveFileExW(backup.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            HANDLE restored = launchUpdatedApplication(exe, L"", plan["flavor"].str() == "cli", &error);
            if (restored) CloseHandle(restored);
            pump(window, L"新版啟動失敗，已嘗試復原舊版");
        } else {
            // The retained .cca-previous file is the explicit recovery copy for this release.
            pump(window, L"更新完成");
        }
        util::writeFileAtomic(util::joinPath(dir, L"result.json"),
                              J({{"ok", launched}, {"version", plan["version"]}, {"error", error}}).dump());
        complete = true;
        const auto hold = GetTickCount64() + (launched ? 800 : 5000);
        while (GetTickCount64() < hold) {
            pump(window, statusText);
            Sleep(25);
        }
    } else {
        if (exited) {
            HANDLE restored = launchUpdatedApplication(exe, L"", plan["flavor"].str() == "cli", &error);
            if (restored) CloseHandle(restored);
        }
        util::writeFileAtomic(util::joinPath(dir, L"result.json"), failure("替換失敗，舊版已保留").dump());
        complete = true;
        MessageBoxW(window, L"更新未完成。舊版執行檔仍保留；請檢查目錄權限或 .cca-previous 復原檔。", L"CCA 更新",
                    MB_OK | MB_ICONINFORMATION);
    }
    KillTimer(window, 1);
    DestroyWindow(window);
    return replaced ? 0 : 7;
}
}  // namespace updater
