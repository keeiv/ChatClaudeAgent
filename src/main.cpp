// Win32 host: a frameless window with a WebView2 that renders the UI from
// resources compiled into the exe, plus the bridge between the page and App.
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <windows.h>
#include <windowsx.h>
#include <wrl.h>

#include <WebView2.h>

#include <deque>
#include <functional>
#include <memory>
#include <mutex>

#include "app.hpp"
#include "haiku_window.hpp"
#include "updater.hpp"
#include "util.hpp"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

const wchar_t* kWindowClass = L"CCA.Main";
const wchar_t* kOrigin = L"https://cca.app/";
const UINT WM_APP_RUN = WM_APP + 1;
const UINT_PTR kTickTimer = 1;
const COLORREF kTitleColor = RGB(0x1E, 0x1F, 0x22);

HWND g_hwnd = nullptr;
ComPtr<ICoreWebView2Environment> g_env;
ComPtr<ICoreWebView2Controller> g_controller;
ComPtr<ICoreWebView2> g_webview;
std::unique_ptr<App> g_app;
std::mutex g_queueMu;
std::deque<std::function<void()>> g_queue;

int dpiScale(int px) {
    return MulDiv(px, (int)GetDpiForWindow(g_hwnd), 96);
}

// Thin strip at the top that belongs to the window itself, so it can be grabbed to resize.
int topResizeStrip() {
    return IsZoomed(g_hwnd) ? 0 : dpiScale(4);
}

void layoutWebView() {
    if (!g_controller) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    rc.top += topResizeStrip();
    g_controller->put_Bounds(rc);
}

void drainQueue() {
    std::deque<std::function<void()>> q;
    {
        std::lock_guard<std::mutex> lock(g_queueMu);
        q.swap(g_queue);
    }
    for (auto& fn : q) fn();
}

void sendToPage(const std::string& json) {
    if (g_webview) g_webview->PostWebMessageAsJson(util::widen(json).c_str());
}

void notifyWindowState() {
    if (!g_webview) return;
    sendToPage(J({{"type", "win.state"}, {"maximized", IsZoomed(g_hwnd) != 0}}).dump());
}

std::wstring pickFolder(const std::wstring& initial) {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"選擇專案資料夾");
    if (!initial.empty()) {
        ComPtr<IShellItem> start;
        if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&start))))
            dlg->SetFolder(start.Get());
    }
    if (dlg->Show(g_hwnd) != S_OK) return {};
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return {};
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

std::wstring pickImage() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    COMDLG_FILTERSPEC types[] = {{L"圖片（PNG、JPG、GIF、WebP）", L"*.png;*.jpg;*.jpeg;*.gif;*.webp"}};
    dlg->SetFileTypes(1, types);
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    dlg->SetTitle(L"選擇圖片");
    if (dlg->Show(g_hwnd) != S_OK) return {};
    ComPtr<IShellItem> item;
    if (FAILED(dlg->GetResult(&item))) return {};
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

void windowAction(const std::string& action) {
    if (action == "min") {
        ShowWindow(g_hwnd, SW_MINIMIZE);
    } else if (action == "max") {
        ShowWindow(g_hwnd, IsZoomed(g_hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    } else if (action == "close") {
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    } else if (action == "drag") {
        ReleaseCapture();
        SendMessageW(g_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    } else if (action == "flash") {
        if (GetForegroundWindow() != g_hwnd) {
            FLASHWINFO fi{sizeof fi, g_hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
            FlashWindowEx(&fi);
        }
    }
}

// Resource names are the UI path upper-cased with non-alphanumerics replaced by '_',
// matching tools/gen_resources.js.
std::wstring resourceName(const std::wstring& path) {
    std::wstring n = L"UI_";
    for (wchar_t c : path) n += iswalnum(c) ? (wchar_t)towupper(c) : L'_';
    return n;
}

const wchar_t* mimeType(const std::wstring& path) {
    const wchar_t* ext = PathFindExtensionW(path.c_str());
    if (!_wcsicmp(ext, L".html")) return L"text/html; charset=utf-8";
    if (!_wcsicmp(ext, L".js")) return L"text/javascript; charset=utf-8";
    if (!_wcsicmp(ext, L".css")) return L"text/css; charset=utf-8";
    if (!_wcsicmp(ext, L".svg")) return L"image/svg+xml";
    if (!_wcsicmp(ext, L".png")) return L"image/png";
    if (!_wcsicmp(ext, L".jpg") || !_wcsicmp(ext, L".jpeg")) return L"image/jpeg";
    if (!_wcsicmp(ext, L".gif")) return L"image/gif";
    if (!_wcsicmp(ext, L".webp")) return L"image/webp";
    if (!_wcsicmp(ext, L".woff2")) return L"font/woff2";
    return L"application/octet-stream";
}

void serveResource(ICoreWebView2WebResourceRequestedEventArgs* args) {
    ComPtr<ICoreWebView2WebResourceRequest> req;
    args->get_Request(&req);
    LPWSTR uriRaw = nullptr;
    req->get_Uri(&uriRaw);
    std::wstring uri = uriRaw ? uriRaw : L"";
    CoTaskMemFree(uriRaw);
    std::wstring path = uri.substr(wcslen(kOrigin));
    size_t q = path.find_first_of(L"?#");
    if (q != std::wstring::npos) path = path.substr(0, q);
    if (path.empty()) path = L"index.html";

    HRSRC res = FindResourceW(nullptr, resourceName(path).c_str(), RT_RCDATA);
    ComPtr<ICoreWebView2WebResourceResponse> resp;
    if (!res) {
        g_env->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", &resp);
    } else {
        HGLOBAL data = LoadResource(nullptr, res);
        const BYTE* bytes = (const BYTE*)LockResource(data);
        DWORD size = SizeofResource(nullptr, res);
        ComPtr<IStream> stream;
        stream.Attach(SHCreateMemStream(bytes, size));
        std::wstring headers = std::wstring(L"Content-Type: ") + mimeType(path) + L"\r\nCache-Control: no-store";
        g_env->CreateWebResourceResponse(stream.Get(), 200, L"OK", headers.c_str(), &resp);
    }
    args->put_Response(resp.Get());
}

const wchar_t* kMediaOrigin = L"https://media.cca.app/";

// Avatars and banners the user picked. CCA reads them itself: WebView2's folder mapping
// answers ERR_ACCESS_DENIED for %APPDATA% on some machines.
void serveMedia(ICoreWebView2WebResourceRequestedEventArgs* args) {
    ComPtr<ICoreWebView2WebResourceRequest> req;
    args->get_Request(&req);
    LPWSTR uriRaw = nullptr;
    req->get_Uri(&uriRaw);
    std::wstring uri = uriRaw ? uriRaw : L"";
    CoTaskMemFree(uriRaw);
    std::wstring name = uri.substr(wcslen(kMediaOrigin));
    size_t q = name.find_first_of(L"?#");
    if (q != std::wstring::npos) name = name.substr(0, q);
    DWORD len = (DWORD)name.size() + 1;
    std::wstring decoded(len, L'\0');
    if (SUCCEEDED(UrlUnescapeW(name.data(), decoded.data(), &len, 0))) name = decoded.substr(0, len);

    ComPtr<ICoreWebView2WebResourceResponse> resp;
    ComPtr<IStream> stream;
    // Only plain file names inside the media folder.
    bool ok = !name.empty() && name.find_first_of(L"/\\:") == std::wstring::npos &&
              name.find(L"..") == std::wstring::npos &&
              SUCCEEDED(SHCreateStreamOnFileEx(util::joinPath(App::mediaDir(), name).c_str(),
                                               STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr,
                                               &stream));
    if (ok) {
        std::wstring headers =
            std::wstring(L"Content-Type: ") + mimeType(name) +
            L"\r\nCache-Control: max-age=31536000, immutable\r\nAccess-Control-Allow-Origin: https://cca.app";
        g_env->CreateWebResourceResponse(stream.Get(), 200, L"OK", headers.c_str(), &resp);
    } else {
        g_env->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", &resp);
    }
    args->put_Response(resp.Get());
}

void initWebView() {
    std::wstring dataDir = util::joinPath(util::localAppDataDir(), L"WebView2");
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, dataDir.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([](HRESULT hr,
                                                                                ICoreWebView2Environment* env)
                                                                                 -> HRESULT {
            if (FAILED(hr) || !env) {
                MessageBoxW(
                    g_hwnd,
                    L"無法啟動 Microsoft Edge WebView2。\n請從微軟網站安裝 WebView2 Runtime 後再開啟 ChatClaudeAgent。",
                    L"ChatClaudeAgent", MB_ICONERROR);
                PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
                return S_OK;
            }
            g_env = env;
            return env->CreateCoreWebView2Controller(
                g_hwnd,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [](HRESULT hr, ICoreWebView2Controller* controller) -> HRESULT {
                        if (FAILED(hr) || !controller) return S_OK;
                        g_controller = controller;
                        g_controller->get_CoreWebView2(&g_webview);

                        ComPtr<ICoreWebView2Controller2> c2;
                        if (SUCCEEDED(g_controller.As(&c2))) c2->put_DefaultBackgroundColor({255, 0x31, 0x33, 0x38});

                        ComPtr<ICoreWebView2Settings> s;
                        g_webview->get_Settings(&s);
                        bool dev = !util::getEnv(L"CCA_DEV").empty();
                        s->put_AreDevToolsEnabled(dev);
                        s->put_AreDefaultContextMenusEnabled(dev);
                        s->put_IsStatusBarEnabled(FALSE);
                        s->put_IsZoomControlEnabled(TRUE);
                        ComPtr<ICoreWebView2Settings3> s3;
                        if (SUCCEEDED(s.As(&s3))) s3->put_AreBrowserAcceleratorKeysEnabled(dev);
                        ComPtr<ICoreWebView2Settings9> s9;
                        if (SUCCEEDED(s.As(&s9))) s9->put_IsNonClientRegionSupportEnabled(TRUE);

                        // The UI comes from the exe's resources, or from a folder while developing.
                        std::wstring uiDir = util::getEnv(L"CCA_UI_DIR");
                        ComPtr<ICoreWebView2_3> w3;
                        bool uiFromFolder = !uiDir.empty() && SUCCEEDED(g_webview.As(&w3));
                        if (uiFromFolder) {
                            w3->SetVirtualHostNameToFolderMapping(L"cca.app", uiDir.c_str(),
                                                                  COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
                        } else {
                            g_webview->AddWebResourceRequestedFilter(L"https://cca.app/*",
                                                                     COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                        }
                        g_webview->AddWebResourceRequestedFilter(L"https://media.cca.app/*",
                                                                 COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                        EventRegistrationToken t;
                        g_webview->add_WebResourceRequested(
                            Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                                [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* a) -> HRESULT {
                                    ComPtr<ICoreWebView2WebResourceRequest> req;
                                    a->get_Request(&req);
                                    LPWSTR u = nullptr;
                                    req->get_Uri(&u);
                                    bool media = u && wcsncmp(u, kMediaOrigin, wcslen(kMediaOrigin)) == 0;
                                    CoTaskMemFree(u);
                                    if (media) serveMedia(a);
                                    else serveResource(a);
                                    return S_OK;
                                })
                                .Get(),
                            &t);

                        EventRegistrationToken tok;
                        // Only the app's own pages may load; everything else opens in the browser.
                        g_webview->add_NavigationStarting(
                            Callback<ICoreWebView2NavigationStartingEventHandler>(
                                [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) -> HRESULT {
                                    LPWSTR uri = nullptr;
                                    a->get_Uri(&uri);
                                    std::wstring u = uri ? uri : L"";
                                    CoTaskMemFree(uri);
                                    if (u.rfind(kOrigin, 0) != 0) {
                                        a->put_Cancel(TRUE);
                                        if (u.rfind(L"https://", 0) == 0 || u.rfind(L"http://", 0) == 0) {
                                            ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                                        }
                                    }
                                    return S_OK;
                                })
                                .Get(),
                            &tok);
                        g_webview->add_NewWindowRequested(
                            Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
                                    a->put_Handled(TRUE);
                                    LPWSTR uri = nullptr;
                                    a->get_Uri(&uri);
                                    std::wstring u = uri ? uri : L"";
                                    CoTaskMemFree(uri);
                                    if (u.rfind(L"https://", 0) == 0 || u.rfind(L"http://", 0) == 0) {
                                        ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                                    }
                                    return S_OK;
                                })
                                .Get(),
                            &tok);
                        g_webview->add_WebMessageReceived(
                            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                                    LPWSTR src = nullptr;
                                    a->get_Source(&src);
                                    bool trusted = src && std::wstring(src).rfind(kOrigin, 0) == 0;
                                    CoTaskMemFree(src);
                                    LPWSTR json = nullptr;
                                    if (trusted && SUCCEEDED(a->get_WebMessageAsJson(&json))) {
                                        std::string text = util::narrow(json);
                                        // Handle outside the WebView2 callback so dialogs can run safely.
                                        postToUi([text] {
                                            if (g_app) g_app->onUiMessage(text);
                                        });
                                    }
                                    CoTaskMemFree(json);
                                    return S_OK;
                                })
                                .Get(),
                            &tok);

                        layoutWebView();
                        // CCA_MOCK=<flags> shows scripted demo data (needs CCA_UI_DIR, since mock.js is not embedded).
                        std::wstring mock = util::getEnv(L"CCA_MOCK");
                        g_webview->Navigate(mock.empty() ? L"https://cca.app/index.html"
                                                         : (L"https://cca.app/index.html#mock," + mock).c_str());
                        return S_OK;
                    })
                    .Get());
        }).Get());
    if (FAILED(hr)) {
        MessageBoxW(g_hwnd, L"找不到 Microsoft Edge WebView2 Runtime。\n請安裝後再開啟 ChatClaudeAgent。",
                    L"ChatClaudeAgent", MB_ICONERROR);
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    }
}

std::wstring placementFile() {
    return util::joinPath(util::appDataDir(), L"window.json");
}

void savePlacement() {
    WINDOWPLACEMENT wp{sizeof wp};
    if (!GetWindowPlacement(g_hwnd, &wp)) return;
    RECT r = wp.rcNormalPosition;
    Json j = J({{"x", (long long)r.left},
                {"y", (long long)r.top},
                {"w", (long long)(r.right - r.left)},
                {"h", (long long)(r.bottom - r.top)},
                {"max", wp.showCmd == SW_SHOWMAXIMIZED}});
    util::writeFileAtomic(placementFile(), j.dump());
}

bool restorePlacement(int& showCmd) {
    std::string text;
    if (!util::readFile(placementFile(), text)) return false;
    Json j = Json::parse(text);
    if (!j.isObject() || j["w"].i64() < 400 || j["h"].i64() < 300) return false;
    RECT r{(LONG)j["x"].i64(), (LONG)j["y"].i64(), (LONG)(j["x"].i64() + j["w"].i64()),
           (LONG)(j["y"].i64() + j["h"].i64())};
    // Ignore positions on a monitor that is no longer attached.
    if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) return false;
    WINDOWPLACEMENT wp{sizeof wp};
    wp.rcNormalPosition = r;
    wp.showCmd = j["max"].boolean() ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    showCmd = wp.showCmd;
    SetWindowPlacement(g_hwnd, &wp);
    return true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) {
            auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
            if (IsZoomed(hwnd)) {
                // A maximized window hangs over the monitor edge by its frame; trim that off.
                UINT dpi = GetDpiForWindow(hwnd);
                int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                p->rgrc[0].left += fx;
                p->rgrc[0].right -= fx;
                p->rgrc[0].top += fy;
                p->rgrc[0].bottom -= fy;
                return 0;
            }
            // Drop the caption but keep the side and bottom frame, which stays invisible
            // and still works for resizing.
            LONG top = p->rgrc[0].top;
            LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
            p->rgrc[0].top = top;
            return r;
        }
        break;
    case WM_NCHITTEST: {
        LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
        if (hit != HTCLIENT || IsZoomed(hwnd)) return hit;
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        if (pt.y < topResizeStrip()) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int corner = dpiScale(12);
            if (pt.x < corner) return HTTOPLEFT;
            if (pt.x > rc.right - corner) return HTTOPRIGHT;
            return HTTOP;
        }
        return hit;
    }
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH b = CreateSolidBrush(kTitleColor);
        FillRect((HDC)wp, &rc, b);
        DeleteObject(b);
        return 1;
    }
    case WM_SIZE:
        layoutWebView();
        notifyWindowState();
        return 0;
    case WM_MOVE:
    case WM_MOVING:
        if (g_controller) g_controller->NotifyParentWindowPositionChanged();
        break;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {dpiScale(940), dpiScale(560)};
        return 0;
    }
    case WM_DPICHANGED: {
        auto* r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_APP_RUN:
        drainQueue();
        return 0;
    case WM_TIMER:
        if (wp == kTickTimer && g_app) g_app->tick();
        return 0;
    case WM_CLOSE:
        savePlacement();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kTickTimer);
        if (g_app) g_app->shutdown();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void postToUi(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lock(g_queueMu);
        g_queue.push_back(std::move(fn));
    }
    if (g_hwnd) PostMessageW(g_hwnd, WM_APP_RUN, 0, 0);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    int updateResult = updater::helperMain();
    if (updateResult >= 0) return updateResult;
    // One CCA at a time: a second launch just brings the first window forward.
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\CCA.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(kWindowClass, nullptr)) {
            if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }
    // The CLI uses the same conversations; only one of them may write them at a time.
    void* dataLock = util::lockDataDir();
    if (!dataLock) {
        MessageBoxW(nullptr,
                    L"ChatClaudeAgent CLI 正在使用同一份對話資料。\n請先在 CLI 輸入 /exit 離開，再開啟桌面版。",
                    L"ChatClaudeAgent", MB_ICONINFORMATION | MB_OK);
        if (single) CloseHandle(single);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(101));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(101), IMAGE_ICON, 16, 16, 0);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(0, kWindowClass, L"ChatClaudeAgent", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                             CW_USEDEFAULT, 1280, 820, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return 1;

    BOOL dark = TRUE;
    DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    COLORREF border = kTitleColor;
    DwmSetWindowAttribute(g_hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof border);

    int showCmd = show;
    if (!restorePlacement(showCmd)) {
        int w = MulDiv(1280, (int)GetDpiForWindow(g_hwnd), 96);
        int h = MulDiv(820, (int)GetDpiForWindow(g_hwnd), 96);
        RECT work;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        SetWindowPos(g_hwnd, nullptr, work.left + (work.right - work.left - w) / 2,
                     work.top + (work.bottom - work.top - h) / 2, w, h, SWP_NOZORDER);
    }
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    ShowWindow(g_hwnd, showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);

    g_app = std::make_unique<App>(sendToPage);
    g_app->windowAction = windowAction;
    g_app->pickFolder = pickFolder;
    g_app->pickImage = pickImage;
    g_app->openExternal = [](const std::wstring& target) {
        ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    g_app->openHaiku = [](const std::wstring& apk, std::string& error) { return haikuwin::open(g_hwnd, apk, error); };

    initWebView();
    SetTimer(g_hwnd, kTickTimer, 50, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    g_app.reset();
    if (g_controller) g_controller->Close();
    g_webview.Reset();
    g_controller.Reset();
    g_env.Reset();
    CoUninitialize();
    CloseHandle((HANDLE)dataLock);
    if (single) CloseHandle(single);
    return 0;
}
