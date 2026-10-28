// Real-window test of the Haiku phone simulator: opens the window on the built, unmodified APK, lets WebView2 load the
// editor page, drives the host bridge through the page, opens a preview, checks what the preview can and cannot do,
// and saves screenshots. Needs the WebView2 runtime and build\android\CCA-Haiku-0.0.2.apk; uses an isolated data
// folder.
#include <windows.h>

#include <cstdio>
#include <string>

#include "haiku_window.hpp"
#include "json.hpp"
#include "util.hpp"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {
int checks = 0, failures = 0;
void check(bool ok, const std::string& name) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", name.c_str());
    }
}

void pump(int ms) {
    const ULONGLONG until = GetTickCount64() + ms;
    MSG m;
    while (GetTickCount64() < until) {
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(10);
    }
}
template <class Cond>
bool until(Cond cond, int ms = 15000) {
    const ULONGLONG end = GetTickCount64() + ms;
    MSG m;
    while (GetTickCount64() < end) {
        if (cond()) return true;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(10);
    }
    return cond();
}

// Runs a script in a page and returns the JSON text of its result ("" on timeout).
std::string runScript(ICoreWebView2* view, const std::string& script) {
    bool done = false;
    std::string out;
    view->ExecuteScript(util::widen(script).c_str(),
                        Callback<ICoreWebView2ExecuteScriptCompletedHandler>([&](HRESULT hr, LPCWSTR json) -> HRESULT {
                            if (SUCCEEDED(hr) && json) out = util::narrow(json);
                            done = true;
                            return S_OK;
                        }).Get());
    until([&] { return done; }, 8000);
    return out;
}

void capturePng(ICoreWebView2* view, const std::wstring& path) {
    ComPtr<IStream> stream;
    CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    bool done = false;
    view->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.Get(),
                         Callback<ICoreWebView2CapturePreviewCompletedHandler>([&](HRESULT) -> HRESULT {
                             done = true;
                             return S_OK;
                         }).Get());
    until([&] { return done; }, 8000);
    HGLOBAL mem = nullptr;
    GetHGlobalFromStream(stream.Get(), &mem);
    const SIZE_T size = GlobalSize(mem);
    const char* data = (const char*)GlobalLock(mem);
    if (data && size) util::writeFileAtomic(path, std::string(data, size));
    GlobalUnlock(mem);
}

// The whole window, banner included, as a 24-bit BMP.
void captureWindow(HWND hwnd, const std::wstring& path) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right, h = rc.bottom;
    HDC dc = GetDC(hwnd), mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    SelectObject(mem, bmp);
    PrintWindow(hwnd, mem, PW_CLIENTONLY | 2);
    BITMAPINFOHEADER bi{sizeof bi, w, -h, 1, 24, BI_RGB};
    const int stride = (w * 3 + 3) & ~3;
    std::string pixels((size_t)stride * h, '\0');
    GetDIBits(mem, bmp, 0, h, pixels.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    BITMAPFILEHEADER fh{0x4D42, (DWORD)(14 + sizeof bi + pixels.size()), 0, 0, 14 + sizeof bi};
    bi.biHeight = -h;
    std::string file((const char*)&fh, 14);
    file.append((const char*)&bi, sizeof bi);
    file += pixels;
    util::writeFileAtomic(path, file);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, dc);
}

std::string unquote(const std::string& json) {
    return Json::parse("[" + json + "]").at(0).str();
}
}  // namespace

int wmain() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const auto data = util::joinPath(tmp, util::widen("cca-haiku-window-" + util::newId()));
    util::ensureDir(data);
    SetEnvironmentVariableW(L"CCA_DATA_DIR", data.c_str());

    std::wstring apk = L"build\\android\\CCA-Haiku-0.0.2.apk";
    if (!util::fileExists(apk)) {
        std::printf("SKIP: %ls is not built\n", apk.c_str());
        return 0;
    }
    ICoreWebView2* editor = nullptr;
    int loads = 0;
    haikuwin::setLoadedHook([&](ICoreWebView2* view, HWND) {
        editor = view;
        ++loads;
    });
    std::string error;
    check(haikuwin::open(nullptr, apk, error), "the window opens: " + error);
    check(until([&] { return loads > 0; }), "the editor page loads from the APK");
    HWND window = haikuwin::window();
    check(window != nullptr, "the simulator window exists");
    if (!window || !editor) {
        std::printf("%d window checks, %d failures\n", checks, failures);
        return 1;
    }
    pump(800);

    // the window is phone-sized
    RECT client;
    GetClientRect(window, &client);
    const double dip = 96.0 / GetDpiForWindow(window);
    check(
        client.right * dip > 400 && client.right * dip < 425 && client.bottom * dip > 600 && client.bottom * dip < 900,
        "the window is phone-sized (about 412 wide)");
    wchar_t title[200];
    GetWindowTextW(window, title, 200);
    check(std::wstring(title).find(L"手機模擬") != std::wstring::npos, "the title says it is a phone simulation");

    // the unmodified page is running and has its bridge
    check(unquote(runScript(editor, "document.title")) == "CCA Haiku", "the page is Haiku's own");
    check(runScript(editor, "typeof window.HaikuHost") == "\"object\"" &&
              runScript(editor, "typeof window.Haiku.receive") == "\"function\"",
          "the host bridge is present and the page's receiver is registered");
    check(runScript(editor, "String(!!document.querySelector('script[src=\"mock-host.js\"]'))") == "\"false\"",
          "the page did not fall back to the development stand-in host");
    check(unquote(runScript(editor, "document.body.innerText")).find("這不是遠端操作電腦") != std::string::npos,
          "the first screen says what Haiku is and is not");
    check(unquote(runScript(editor, "document.querySelector('.brand small').textContent")) == "0.0.2",
          "the page shows the APK's version");

    // bridge: call it as the page does and record what comes back
    runScript(editor,
              "window.__r = []; window.__orig = window.Haiku.receive; window.Haiku.receive = function (m) { "
              "window.__r.push(JSON.parse(m)); }; 1");
    auto call = [&](const std::string& id, const std::string& method, const std::string& params) {
        runScript(editor, "window.HaikuHost.call(JSON.stringify({ id: '" + id + "', method: '" + method +
                              "', params: " + params + " })); 1");
        until([&] { return runScript(editor, "String(window.__r.some(r => r.id === '" + id + "'))") == "\"true\""; },
              8000);
        return Json::parse(unquote(runScript(editor, "JSON.stringify(window.__r.find(r => r.id === '" + id + "'))")));
    };
    check(call("a", "keys.status", "{}")["result"]["anthropic"].boolean() == false, "bridge: keys.status answers");
    check(call("b", "projects.create", "{ name: 'sim test' }")["ok"].boolean(), "bridge: a project is created");
    Json listed = call("c", "projects.list", "{}");
    check(listed["ok"].boolean() && listed["result"].size() == 1 && listed["result"].at(0)["name"].str() == "sim test",
          "bridge: the project is listed");
    check(util::fileExists(util::joinPath(data, L"haiku-sim\\projects\\sim test\\index.html")),
          "the project lives in the simulator's own data folder");
    Json noKey = call("d", "model.send", "{ provider: 'anthropic', body: '{}' }");
    check(!noKey["ok"].boolean() && noKey["error"].str().find("API 金鑰") != std::string::npos,
          "bridge: a model call without a key says so");
    check(!call("e", "files.write", "{ project: 'sim test', path: '../escape.txt', text: 'x' }")["ok"].boolean(),
          "bridge: a path outside the project is refused");
    check(call("f", "files.write",
               "{ project: 'sim test', path: 'index.html', text: '<!doctype html><title>t</title><h1 "
               "id=\"hello\">hello from the simulator</h1>' }")["ok"]
              .boolean(),
          "bridge: a file is written");

    // navigation out of the editor is cancelled
    runScript(editor, "location.href = 'https://example.com/'; 1");
    pump(1200);
    check(unquote(runScript(editor, "location.hostname")) == "haiku.app",
          "the editor cannot navigate away from its own pages");

    // preview: opens in a window with no bridge
    runScript(editor, "window.__closed = 0; window.Haiku.previewClosed = function () { window.__closed++; }; 1");
    check(call("g", "preview.open", "{ project: 'sim test' }")["ok"].boolean(), "bridge: preview.open succeeds");
    check(until([&] { return haikuwin::previewWindow() != nullptr && haikuwin::previewPage() != nullptr; }),
          "a preview window opens");
    ICoreWebView2* preview = haikuwin::previewPage();
    if (preview) {
        until(
            [&] {
                return runScript(preview, "document.readyState") == "\"complete\"" &&
                       unquote(runScript(preview, "document.title")) == "t";
            },
            10000);
        check(unquote(runScript(preview,
                                "document.getElementById('hello') && document.getElementById('hello').textContent")) ==
                  "hello from the simulator",
              "the preview shows the project's file");
        check(runScript(preview, "typeof window.HaikuHost") == "\"undefined\"", "the preview has no HaikuHost");
        // Even if the preview page posts a bridge call by hand, nothing on the host side listens to it.
        runScript(preview,
                  "try { window.chrome.webview.postMessage(JSON.stringify({ id: 'x', method: 'projects.create', "
                  "params: { name: 'from preview' } })); } catch (e) {} 1");
        pump(800);
        check(!util::dirExists(util::joinPath(data, L"haiku-sim\\projects\\from preview")),
              "a bridge call posted by the preview page does nothing");
        capturePng(preview, L"build\\haiku-sim-preview.png");
    }
    capturePng(editor, L"build\\haiku-sim-editor.png");
    captureWindow(window, L"build\\haiku-sim-window.bmp");
    if (haikuwin::previewWindow()) SendMessageW(haikuwin::previewWindow(), WM_COMMAND, 1, 0);  // 返回編輯
    check(until([&] { return haikuwin::previewWindow() == nullptr; }), "going back closes the preview");
    check(until([&] { return runScript(editor, "String(window.__closed)") == "\"1\""; }, 5000),
          "the editor is told the preview closed");

    DestroyWindow(window);
    pump(500);
    check(haikuwin::window() == nullptr, "closing the window releases it");
    std::printf("%d window checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
