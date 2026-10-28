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
#include <cstdio>
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
// ---- Diagnosis. The broker is a separate process that outlives CCA, so what it finds is written down: a trace
// (update.log) for the curious, and a short list of findings (result.json) a person can act on.
std::string describeError(DWORD code) {
    switch (code) {
    case ERROR_ACCESS_DENIED:
        return "權限不足，或防毒軟體正在掃描這個檔案";
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return "檔案正被其他程式使用";
    case ERROR_USER_MAPPED_FILE:
        return "檔案正被其他程式載入";
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return "磁碟空間不足";
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return "找不到檔案或資料夾";
    case ERROR_WRITE_PROTECT:
        return "磁碟或資料夾是唯讀的";
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        return "已有同名的檔案";
    default:
        return "系統錯誤碼 " + std::to_string(code);
    }
}
// Only these are worth waiting for: a scanner or the shell lets go of a file within a few seconds.
bool transient(DWORD code) {
    return code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION ||
           code == ERROR_USER_MAPPED_FILE;
}
std::string describeExit(DWORD code) {
    char hex[16];
    std::snprintf(hex, sizeof hex, "0x%08lX", (unsigned long)code);
    if (code == 0xC0000135 || code == 0xC0000139 || code == 0xC0000142)
        return std::string("新版無法載入必要的系統元件（") + hex +
               "）；請確認已安裝 Microsoft Edge WebView2 與 Visual C++ 執行階段";
    if (code == 0xC0000005) return std::string("新版啟動時當機（") + hex + "）";
    return std::string("新版啟動後立即結束（") + hex + "）";
}
struct Trace {
    std::string text;
    ULONGLONG start = GetTickCount64();
    void add(const std::string& line) {
        text += "[+" + std::to_string(GetTickCount64() - start) + " ms] " + line + "\n";
    }
};
Trace trace;
Json findings = Json::array();
void diagnose(const std::string& finding) {
    findings.push(finding);
    trace.add("診斷：" + finding);
}

// ---- Broker window. The two C strokes of the CCA mark close up as the update advances; a stepper below names where it
// is.
enum class Stage { Wait, Verify, Replace, Start, Done, Failed };
Stage stage = Stage::Wait;
Stage failedAt = Stage::Wait;
ULONGLONG stageSince = 0;
double shownProgress = 0.0;
std::wstring statusText = L"等待 CCA 儲存並關閉", detailText;
unsigned frame = 0;
bool complete = false;
bool animate = true;
COLORREF windowBackground = RGB(19, 22, 26), windowText = RGB(233, 237, 241), windowSignal = RGB(124, 196, 255);
constexpr COLORREF kFailure = RGB(224, 96, 96);
COLORREF asColor(uint32_t rgb) {
    return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}
COLORREF blend(COLORREF a, COLORREF b, double t) {
    auto mix = [&](int x, int y) { return (int)(x + (y - x) * t); };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}
double ease(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return 1 - std::pow(1 - t, 3);
}
// Progress of the current stage, 0..1; without animation every transition is instant.
double stageTime(double milliseconds) {
    return animate ? ease((double)(GetTickCount64() - stageSince) / milliseconds) : 1.0;
}
int stageIndex(Stage s) {
    return s == Stage::Failed ? (int)failedAt : (int)s;
}
double stageTarget(Stage s) {
    switch (s) {
    case Stage::Wait:
        return 0.08;
    case Stage::Verify:
        return 0.36;
    case Stage::Replace:
        return 0.62;
    case Stage::Start:
        return 0.86;
    case Stage::Done:
        return 1.0;
    default:
        return stageTarget(failedAt);  // a failed update stops the bar where it failed
    }
}
void setStage(Stage next) {
    if (stage == next) return;
    if (next == Stage::Failed) failedAt = stage;
    stage = next;
    stageSince = GetTickCount64();
}
void setDetail(const std::wstring& text) {
    detailText = text;
}
void fillPolygon(HDC dc, const POINT* points, int count, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    auto old = SelectObject(dc, brush);
    Polygon(dc, points, count);
    SelectObject(dc, old);
    DeleteObject(brush);
}
void outlinePolygon(HDC dc, const POINT* points, int count, COLORREF color, int width) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    auto oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Polygon(dc, points, count);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}
void paintBroker(HDC dc, const RECT& area) {
    HBRUSH bg = CreateSolidBrush(windowBackground);
    FillRect(dc, &area, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    const int x = area.right / 2 - 64, y = 20;
    // How far the strokes are apart, in pixels: they breathe while waiting and lock together as the new version goes
    // in.
    double gap = 4;
    COLORREF markColor = windowText;
    switch (stage) {
    case Stage::Wait:
        gap = 5 + (animate ? std::sin(frame * .065) * 4 : 0);
        break;
    case Stage::Verify:
        gap = 5 - 3 * stageTime(500);
        break;
    case Stage::Replace:
        gap = 2 - 2 * stageTime(380);
        break;
    case Stage::Start:
        gap = 0;
        break;
    case Stage::Done:
        gap = 0;
        markColor = windowSignal;
        break;
    case Stage::Failed:
        gap = 16 * stageTime(320) + (animate && GetTickCount64() - stageSince < 450 ? std::sin(frame * 1.3) * 3 : 0);
        markColor = kFailure;
        break;
    }
    static const POINT top[] = {{20, 3},  {8, 3},   {3, 8},  {3, 16}, {8, 21}, {20, 21},
                                {20, 16}, {10, 16}, {8, 14}, {8, 10}, {10, 8}, {20, 8}};
    POINT upper[12], lower[12];
    for (int i = 0; i < 12; ++i) {
        upper[i] = {x + top[i].x * 4 - (int)gap, y + top[i].y * 4};
        lower[i] = {x + (32 - top[i].x) * 4 + (int)gap, y + (32 - top[i].y) * 4};
    }
    HPEN none = (HPEN)GetStockObject(NULL_PEN);
    auto oldPen = SelectObject(dc, none);
    fillPolygon(dc, upper, 12, markColor);
    fillPolygon(dc, lower, 12, markColor);
    SelectObject(dc, oldPen);
    // A signal outline: one flash as the files are swapped, then a slow pulse while the new version starts.
    if (stage == Stage::Replace && animate) {
        const double flash = 1 - stageTime(520);
        if (flash > 0.02) {
            outlinePolygon(dc, upper, 12, blend(windowBackground, windowSignal, flash), 3);
            outlinePolygon(dc, lower, 12, blend(windowBackground, windowSignal, flash), 3);
        }
    } else if (stage == Stage::Start) {
        const double pulse = animate ? 0.5 + 0.5 * std::sin(frame * 0.18) : 1.0;
        outlinePolygon(dc, upper, 12, blend(windowBackground, windowSignal, 0.35 + 0.65 * pulse), 3);
        outlinePolygon(dc, lower, 12, blend(windowBackground, windowSignal, 0.35 + 0.65 * pulse), 3);
    }
    // The check mark is drawn stroke by stroke over the signal-coloured mark.
    if (stage == Stage::Done) {
        const double t = stageTime(480);
        const POINT a{x + 36, y + 66}, b{x + 56, y + 86}, c{x + 94, y + 44};
        const double first = std::min(1.0, t * 2.0), second = std::max(0.0, t * 2.0 - 1.0);
        // A dark outline first, so the light stroke reads against the signal-coloured mark and its seam.
        for (const auto& [width, color] : {std::pair{19, windowBackground}, std::pair{9, windowText}}) {
            HPEN pen = CreatePen(PS_SOLID, width, color);
            auto previous = SelectObject(dc, pen);
            MoveToEx(dc, a.x, a.y, nullptr);
            LineTo(dc, a.x + (int)((b.x - a.x) * first), a.y + (int)((b.y - a.y) * first));
            if (second > 0) LineTo(dc, b.x + (int)((c.x - b.x) * second), b.y + (int)((c.y - b.y) * second));
            SelectObject(dc, previous);
            DeleteObject(pen);
        }
    }
    // Progress bar
    const int left = 40, right = area.right - 40, barTop = 172;
    RECT track{left, barTop, right, barTop + 4};
    HBRUSH trackBrush = CreateSolidBrush(blend(windowBackground, windowText, 0.16));
    FillRect(dc, &track, trackBrush);
    DeleteObject(trackBrush);
    RECT fill{left, barTop, left + (int)((right - left) * shownProgress), barTop + 4};
    HBRUSH fillBrush = CreateSolidBrush(stage == Stage::Failed ? kFailure : windowSignal);
    FillRect(dc, &fill, fillBrush);
    DeleteObject(fillBrush);
    // Status and detail lines
    SetTextColor(dc, stage == Stage::Failed ? kFailure : windowText);
    HFONT big = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                            L"Segoe UI");
    auto oldFont = SelectObject(dc, big);
    RECT caption{20, 186, area.right - 20, 212};
    DrawTextW(dc, statusText.c_str(), -1, &caption, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
    DeleteObject(big);
    if (!detailText.empty()) {
        HFONT small = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                                  CLEARTYPE_QUALITY, 0, L"Segoe UI");
        oldFont = SelectObject(dc, small);
        SetTextColor(dc, blend(windowText, windowBackground, 0.38));
        RECT detail{28, 214, area.right - 28, 250};
        DrawTextW(dc, detailText.c_str(), -1, &detail, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
        SelectObject(dc, oldFont);
        DeleteObject(small);
    }
    // Stepper: five squares joined by a line that fills as the update advances.
    static const wchar_t* labels[] = {L"等待關閉", L"核對", L"替換", L"啟動", L"完成"};
    const int current = stageIndex(stage), nodeY = 262, firstX = 60, step = (area.right - 120) / 4;
    HFONT tiny = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                             L"Segoe UI");
    oldFont = SelectObject(dc, tiny);
    for (int i = 0; i < 5; ++i) {
        const int nx = firstX + i * step;
        if (i > 0) {
            RECT line{nx - step + 8, nodeY + 4, nx - 8, nodeY + 6};
            HBRUSH b = CreateSolidBrush(i <= current ? windowSignal : blend(windowBackground, windowText, 0.16));
            FillRect(dc, &line, b);
            DeleteObject(b);
        }
        const bool reached = i < current || (i == current && stage != Stage::Failed) || stage == Stage::Done;
        RECT node{nx - 5, nodeY, nx + 5, nodeY + 10};
        COLORREF color = (i == current && stage == Stage::Failed) ? kFailure
                         : reached                                ? windowSignal
                                                                  : blend(windowBackground, windowText, 0.3);
        if (reached || (i == current && stage == Stage::Failed)) {
            HBRUSH b = CreateSolidBrush(color);
            FillRect(dc, &node, b);
            DeleteObject(b);
        } else {
            HBRUSH b = CreateSolidBrush(color);
            FrameRect(dc, &node, b);
            DeleteObject(b);
        }
        // The active node sends out a square ring that fades as it grows.
        if (i == current && !complete && stage != Stage::Failed && stage != Stage::Done && animate) {
            const double t = (frame % 40) / 40.0;
            const int r = 5 + (int)(t * 9);
            RECT ring{nx - r, nodeY + 5 - r, nx + r, nodeY + 5 + r};
            HBRUSH b = CreateSolidBrush(blend(windowBackground, windowSignal, 1.0 - t));
            FrameRect(dc, &ring, b);
            DeleteObject(b);
        }
        SetTextColor(dc, reached || i == current ? windowText : blend(windowText, windowBackground, 0.45));
        RECT label{nx - step / 2, nodeY + 16, nx + step / 2, nodeY + 34};
        DrawTextW(dc, labels[i], -1, &label, DT_CENTER | DT_SINGLELINE);
    }
    SelectObject(dc, oldFont);
    DeleteObject(tiny);
}
LRESULT CALLBACK brokerWindow(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_TIMER) {
        if (animate) ++frame;
        const double target = stageTarget(stage);
        shownProgress = animate ? shownProgress + (target - shownProgress) * 0.10 : target;
        if (std::abs(target - shownProgress) < 0.002) shownProgress = target;
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
        paintBroker(dc, area);
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
// Runs `operation` until it succeeds, waiting a little longer each time while the failure is one a scanner or the
// shell will let go of; any other failure stops at once. The last error is left in `lastError`.
template <class Operation>
bool retrying(HWND window, const std::string& what, Operation operation, DWORD& lastError, int attempts = 6) {
    for (int attempt = 1; attempt <= attempts; ++attempt) {
        SetLastError(0);
        if (operation()) {
            if (attempt > 1) trace.add(what + "：第 " + std::to_string(attempt) + " 次嘗試成功");
            setDetail(L"");
            return true;
        }
        lastError = GetLastError();
        trace.add(what + "失敗（第 " + std::to_string(attempt) + "/" + std::to_string(attempts) + " 次）：" +
                  describeError(lastError));
        if (!transient(lastError) || attempt == attempts) return false;
        setDetail(util::widen("檔案暫時被占用（" + describeError(lastError) + "），第 " + std::to_string(attempt) +
                              " 次重試"));
        const auto until = GetTickCount64() + 300ull * attempt;
        while (GetTickCount64() < until) {
            pump(window, statusText);
            Sleep(25);
        }
    }
    return false;
}
// Checked while CCA is still open, so a problem that would only appear after it has closed is reported with CCA
// running.
std::string preflight(const std::wstring& exe, unsigned long long size) {
    const auto probe = exe + L".cca-write-test";
    HANDLE handle = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return "CCA 所在的資料夾無法寫入（" + describeError(GetLastError()) +
               "），更新無法替換執行檔；請把 CCA 放在可寫入的資料夾。";
    CloseHandle(handle);
    const unsigned long long need = size * 3 + (16ull << 20);
    for (const auto& place : {exe.substr(0, exe.find_last_of(L"\\/")), util::localAppDataDir()}) {
        ULARGE_INTEGER freeBytes{};
        if (GetDiskFreeSpaceExW(place.c_str(), &freeBytes, nullptr, nullptr) && freeBytes.QuadPart < need)
            return "磁碟空間不足：更新需要約 " + std::to_string(need >> 20) + " MB 的可用空間（" + util::narrow(place) +
                   "）。";
    }
    return "";
}
// Staged updates of earlier runs pile up under the local updates folder; only the files the updater itself writes
// are removed, and only from folders that have not been touched for two weeks.
void pruneOldUpdates() {
    const auto root = util::joinPath(util::localAppDataDir(), L"updates");
    WIN32_FIND_DATAW entry{};
    HANDLE find = FindFirstFileW((root + L"\\*").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) return;
    FILETIME nowFile{};
    GetSystemTimeAsFileTime(&nowFile);
    ULARGE_INTEGER now{};
    now.LowPart = nowFile.dwLowDateTime;
    now.HighPart = nowFile.dwHighDateTime;
    const unsigned long long fortnight = 14ull * 24 * 3600 * 10000000ull;
    int seen = 0;
    do {
        const std::wstring name = entry.cFileName;
        if (name == L"." || name == L".." || !(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            continue;
        ULARGE_INTEGER written{};
        written.LowPart = entry.ftLastWriteTime.dwLowDateTime;
        written.HighPart = entry.ftLastWriteTime.dwHighDateTime;
        if (now.QuadPart < written.QuadPart || now.QuadPart - written.QuadPart < fortnight) continue;
        const auto folder = util::joinPath(root, name);
        for (const wchar_t* file : {L"payload.exe", L"broker.exe", L"plan.json", L"result.json", L"update.log"})
            DeleteFileW(util::joinPath(folder, file).c_str());
        RemoveDirectoryW(folder.c_str());
    } while (++seen < 512 && FindNextFileW(find, &entry));
    FindClose(find);
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
bool verifyAndroidPackage(const std::wstring& apk, std::string& error) {
    const auto slash = apk.find_last_of(L"\\/");
    std::string raw, data;
    if (slash == std::wstring::npos || !boundedRead(apk.substr(0, slash + 1) + L"manifest.json", raw, 65536)) {
        error = "找不到這個安裝包的發行清單";
        return false;
    }
    Json manifest = Json::parse(raw);
    const Json& file = manifest["files"]["android"];
    if (manifest["schema"].i64() != 1 || manifest["edition"].str() != "haiku" ||
        file["name"].str() != util::narrow(apk.substr(slash + 1)) || !hexHash(file["sha256"].str())) {
        error = "發行清單與安裝包不一致";
        return false;
    }
    if (!boundedRead(apk, data, kMaxBinary) || (long long)data.size() != file["size"].i64() ||
        sha256(data) != file["sha256"].str()) {
        error = "安裝包的大小或 SHA-256 與發行清單不符";
        return false;
    }
    return true;
}
Json prepare(const Json& checked, const std::wstring& exe) {
    if (!checked["ok"].boolean() || !checked["available"].boolean()) return failure("沒有可安裝的較新版本");
    const auto file = checked["file"];
    std::string data;
    if (!fetch(checked["source"].str(), data, (size_t)file["size"].i64()) || data.size() != file["size"].i64() ||
        sha256(data) != file["sha256"].str() || !pe64(data))
        return failure("更新檔下載或核對失敗，本機版本未變更");
    if (!hashFile(exe, checked["localHash"].str())) return failure("本機執行檔在檢查後改變，請重新檢查更新");
    if (const auto why = preflight(exe, (unsigned long long)file["size"].i64()); !why.empty()) return failure(why);
    pruneOldUpdates();
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
    RECT frameRect{0, 0, 420, 312};
    AdjustWindowRectEx(&frameRect, WS_OVERLAPPED | WS_CAPTION, FALSE, WS_EX_TOPMOST);
    const int width = frameRect.right - frameRect.left, height = frameRect.bottom - frameRect.top;
    HWND window =
        CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"CCA · 更新", WS_OVERLAPPED | WS_CAPTION,
                        (GetSystemMetrics(SM_CXSCREEN) - width) / 2, (GetSystemMetrics(SM_CYSCREEN) - height) / 2,
                        width, height, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) {
        CloseHandle(parent);
        return 5;
    }
    stageSince = GetTickCount64();
    trace.add("更新程式啟動：" + plan["flavor"].str() + " → " + plan["version"].str());
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
    trace.add(exited ? "CCA 已關閉" : "CCA 沒有在 60 秒內關閉");
    const auto replacement = exe + L".cca-next",
               backup = exe + L".cca-previous-" + util::widen(plan["oldHash"].str().substr(0, 12));
    const std::string oldHash = plan["oldHash"].str(), newHash = plan["newHash"].str();
    bool replaced = false;
    std::string error;
    DWORD lastError = 0;
    if (!exited) {
        diagnose("CCA 沒有在 60 秒內關閉，更新已取消，舊版沒有被更動。");
    } else {
        setStage(Stage::Verify);
        pump(window, L"核對舊版與新版");
        bool ready = hashFile(exe, oldHash);
        if (!ready) diagnose("執行檔在更新開始後被改動，更新已取消。");
        // What an interrupted earlier update left behind: clear it instead of refusing to start.
        bool reuse = false;
        if (ready && util::fileExists(replacement)) {
            if (hashFile(replacement, newHash)) {
                reuse = true;
                trace.add("沿用上次留下、已核對的暫存檔");
            } else if (retrying(
                           window, "刪除上次留下的暫存檔", [&] { return DeleteFileW(replacement.c_str()) != FALSE; },
                           lastError)) {
                trace.add("已清除上次留下的暫存檔");
            } else {
                diagnose("上次更新留下的暫存檔無法刪除（" + describeError(lastError) + "）：" +
                         util::narrow(replacement));
                ready = false;
            }
        }
        if (ready && util::fileExists(backup)) {
            // A backup identical to the installed file is redundant; anything else is only moved aside, never deleted.
            const bool same = hashFile(backup, oldHash);
            const bool cleared = retrying(
                window, same ? "清除重複的舊版備份" : "移開未知的備份檔",
                [&] {
                    return same ? DeleteFileW(backup.c_str()) != FALSE
                                : MoveFileExW(backup.c_str(), (backup + L".stale").c_str(),
                                              MOVEFILE_REPLACE_EXISTING) != FALSE;
                },
                lastError);
            if (!cleared) {
                diagnose("上次留下的備份檔無法處理（" + describeError(lastError) + "）：" + util::narrow(backup));
                ready = false;
            }
        }
        if (ready) {
            setStage(Stage::Replace);
            pump(window, L"替換已核對的版本");
            const bool copied =
                reuse || retrying(
                             window, "複製新版",
                             [&] { return CopyFileW(payload.c_str(), replacement.c_str(), TRUE) != FALSE; }, lastError);
            if (!copied) {
                diagnose("無法複製新版（" + describeError(lastError) + "）；舊版沒有被更動。");
            } else {
                replaced = retrying(
                    window, "替換執行檔",
                    [&] {
                        return ReplaceFileW(exe.c_str(), replacement.c_str(), backup.c_str(), 0, nullptr, nullptr) !=
                               FALSE;
                    },
                    lastError);
                if (!replaced && hashFile(backup, oldHash)) {
                    if (hashFile(exe, newHash)) replaced = true;
                    else if (!hashFile(exe, oldHash))
                        MoveFileExW(backup.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
                }
                if (replaced && !hashFile(exe, newHash)) {
                    replaced = false;
                    MoveFileExW(backup.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
                    diagnose("替換後的檔案核對不符，已改回舊版。");
                }
                if (!replaced) diagnose("替換執行檔失敗（" + describeError(lastError) + "），舊版已保留。");
            }
        }
    }
    const auto logPath = util::joinPath(dir, L"update.log");
    DWORD holdExtra = 0;
    {
        wchar_t hold[16];
        if (GetEnvironmentVariableW(L"CCA_TEST_UPDATE_HOLD_MS", hold, 16)) holdExtra = (DWORD)_wtoi(hold);
    }
    if (replaced) {
        setStage(Stage::Start);
        pump(window, L"正在重新開啟 CCA");
        auto eventName = util::widen("Local\\CCA.Update.Ready." + util::randomHex(16));
        HANDLE startup = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
        HANDLE child =
            startup ? launchUpdatedApplication(exe, eventName, plan["flavor"].str() == "cli", &error) : nullptr;
        bool launched = false;
        DWORD childExit = STILL_ACTIVE;
        const auto startupDeadline = GetTickCount64() + 30000;
        while (child && GetTickCount64() < startupDeadline) {
            if (WaitForSingleObject(startup, 0) == WAIT_OBJECT_0) {
                launched = true;
                break;
            }
            if (WaitForSingleObject(child, 0) == WAIT_OBJECT_0) {
                GetExitCodeProcess(child, &childExit);
                break;
            }
            pump(window, L"正在確認 CCA 已啟動");
            Sleep(25);
        }
        if (child) {
            if (!launched) {
                if (childExit == STILL_ACTIVE) diagnose("新版在 30 秒內沒有回報啟動完成，已結束它並復原舊版。");
                else diagnose(describeExit(childExit) + "，已復原舊版。");
                TerminateProcess(child, 9);
                WaitForSingleObject(child, 5000);
            }
            CloseHandle(child);
        } else if (!launched) {
            diagnose("無法啟動新版（" + (error.empty() ? std::string("沒有取得程序") : error) + "），已復原舊版。");
        }
        if (startup) CloseHandle(startup);
        if (!launched) {
            // Preserve both executables if rollback itself fails; never delete the last good copy.
            setStage(Stage::Failed);
            setDetail(util::widen(findings.size() ? findings.items().back().str() : std::string()));
            MoveFileExW(backup.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            HANDLE restored = launchUpdatedApplication(exe, L"", plan["flavor"].str() == "cli", &error);
            if (restored) CloseHandle(restored);
            pump(window, L"新版啟動失敗，已復原舊版");
        } else {
            // The retained .cca-previous file is the explicit recovery copy for this release.
            setStage(Stage::Done);
            pump(window, L"更新完成");
        }
        trace.add(launched ? "完成：新版已啟動" : "失敗：已復原舊版");
        util::writeFileAtomic(logPath, trace.text);
        util::writeFileAtomic(util::joinPath(dir, L"result.json"), J({{"ok", launched},
                                                                      {"version", plan["version"]},
                                                                      {"error", error},
                                                                      {"diagnosis", findings},
                                                                      {"log", util::narrow(logPath)}})
                                                                       .dump());
        complete = true;
        const auto hold = GetTickCount64() + (launched ? 900 : 5000) + holdExtra;
        while (GetTickCount64() < hold) {
            pump(window, statusText);
            Sleep(25);
        }
    } else {
        if (exited) {
            HANDLE restored = launchUpdatedApplication(exe, L"", plan["flavor"].str() == "cli", &error);
            if (restored) CloseHandle(restored);
        }
        setStage(Stage::Failed);
        const std::string first = findings.size() ? findings.items().front().str() : "替換失敗，舊版已保留";
        statusText = L"更新未完成，舊版已保留";
        setDetail(util::widen(first));
        trace.add("失敗：" + first);
        util::writeFileAtomic(logPath, trace.text);
        Json result = failure(first);
        result["diagnosis"] = findings;
        result["log"] = util::narrow(logPath);
        util::writeFileAtomic(util::joinPath(dir, L"result.json"), result.dump());
        complete = true;
        pump(window, statusText);
        if (!GetEnvironmentVariableW(L"CCA_TEST_UPDATE_SILENT", nullptr, 0)) {
            std::wstring text = L"更新未完成。舊版執行檔仍保留。\n\n";
            for (const auto& item : findings.items()) text += L"．" + util::widen(item.str()) + L"\n";
            text += L"\n詳細記錄：" + logPath;
            MessageBoxW(window, text.c_str(), L"CCA 更新", MB_OK | MB_ICONINFORMATION);
        } else {
            const auto hold = GetTickCount64() + 400 + holdExtra;
            while (GetTickCount64() < hold) {
                pump(window, statusText);
                Sleep(25);
            }
        }
    }
    KillTimer(window, 1);
    DestroyWindow(window);
    return replaced ? 0 : 7;
}
}  // namespace updater
