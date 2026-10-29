// Haiku phone simulator window; see haiku_window.hpp. The editor page is served from the APK's assets at
// https://haiku.app/ (the address the phone shell uses) and gets a HaikuHost object whose call() reaches
// haikusim::Host. Previews run in a second window with no bridge at all, like the phone's second WebView.
#include "haiku_window.hpp"

#include <shellapi.h>
#include <shlwapi.h>
#include <wrl.h>

#include <algorithm>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include "haiku_sim.hpp"
#include "util.hpp"

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace haikuwin {
namespace {

const wchar_t* kSimClass = L"CCA.HaikuSim";
const wchar_t* kPreviewClass = L"CCA.HaikuPreview";
const wchar_t* kAppOrigin = L"https://haiku.app/";
const wchar_t* kPreviewOrigin = L"https://haiku.preview/";
const UINT WM_SIM_WORK = WM_APP + 7;
// Phone-sized, in device-independent pixels. The banner above the page belongs to the simulator, not to Haiku.
const int kPhoneWidth = 412, kBannerHeight = 58, kToolbarHeight = 40;
const COLORREF kBack = RGB(19, 22, 26), kText = RGB(233, 237, 241), kMuted = RGB(139, 148, 159),
               kSignal = RGB(124, 196, 255);

// State shared with worker threads, so a late reply after the window closed is dropped instead of touching freed
// objects.
struct Shared {
    std::mutex mu;
    std::deque<std::function<void()>> queue;
    bool alive = true;
};

struct Sim {
    HWND hwnd = nullptr, preview = nullptr;
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();
    std::map<std::string, std::string> assets;
    std::unique_ptr<haikusim::Host> host;
    ComPtr<ICoreWebView2Environment> env;
    ComPtr<ICoreWebView2Controller> controller, previewController;
    ComPtr<ICoreWebView2> view, previewView;
    int contentHeight = 800;
    std::string previewProject;
};
Sim* g_sim = nullptr;
std::function<void(ICoreWebView2*, HWND)> g_loadedHook;

int scaled(HWND hwnd, int px) {
    return MulDiv(px, (int)GetDpiForWindow(hwnd), 96);
}

std::wstring decodeUrl(const std::wstring& in) {
    DWORD len = (DWORD)in.size() + 1;
    std::wstring out(len, L'\0');
    if (SUCCEEDED(UrlUnescapeW(const_cast<wchar_t*>(in.c_str()), out.data(), &len, 0)))
        return out.substr(0, wcslen(out.c_str()));
    return in;
}

// "https://host/a/b?x#y" -> "a/b" (percent-decoded), for a known origin prefix.
std::string pathOf(const std::wstring& uri, const wchar_t* origin) {
    std::wstring path = uri.substr(wcslen(origin));
    const auto q = path.find_first_of(L"?#");
    if (q != std::wstring::npos) path.resize(q);
    return util::narrow(decodeUrl(path));
}

void respond(ICoreWebView2Environment* env, ICoreWebView2WebResourceRequestedEventArgs* args, int status,
             const std::string& type, const std::string& body) {
    ComPtr<ICoreWebView2WebResourceResponse> resp;
    ComPtr<IStream> stream;
    if (!body.empty()) stream.Attach(SHCreateMemStream((const BYTE*)body.data(), (UINT)body.size()));
    // Nothing is cached: a project file edited by Haiku must show up on the next load.
    const std::wstring headers = L"Content-Type: " + util::widen(type) + L"; charset=utf-8\r\nCache-Control: no-store";
    env->CreateWebResourceResponse(stream.Get(), status, status == 200 ? L"OK" : L"Not Found", headers.c_str(), &resp);
    args->put_Response(resp.Get());
}

void serveApp(Sim& sim, ICoreWebView2WebResourceRequestedEventArgs* args, const std::wstring& uri) {
    std::string path = pathOf(uri, kAppOrigin);
    if (path.empty()) path = "index.html";
    const bool plain =
        path.find("..") == std::string::npos && std::all_of(path.begin(), path.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
                   c == '/' || c == '-';
        });
    const auto found = plain ? sim.assets.find(path) : sim.assets.end();
    if (found == sim.assets.end()) respond(sim.env.Get(), args, 404, "text/plain", "");
    else respond(sim.env.Get(), args, 200, haikusim::contentType(path), found->second);
}

void servePreview(Sim& sim, ICoreWebView2WebResourceRequestedEventArgs* args, const std::wstring& uri) {
    const std::string path = pathOf(uri, kPreviewOrigin);
    const auto slash = path.find('/');
    const std::string project = path.substr(0, slash);
    std::string file = slash == std::string::npos ? "" : path.substr(slash + 1);
    if (file.empty()) file = "index.html";
    std::string data, error;
    if (project.empty() || !sim.host->projects().readBytes(project, file, data, error))
        respond(sim.env.Get(), args, 404, "text/plain", "");
    else respond(sim.env.Get(), args, 200, haikusim::contentType(file), data);
}

void runOnEditor(const std::wstring& script) {
    if (g_sim && g_sim->view) g_sim->view->ExecuteScript(script.c_str(), nullptr);
}

void closePreview(bool tellEditor) {
    if (!g_sim || !g_sim->preview) return;
    HWND p = g_sim->preview;
    g_sim->preview = nullptr;
    if (g_sim->previewController) g_sim->previewController->Close();
    g_sim->previewController.Reset();
    g_sim->previewView.Reset();
    DestroyWindow(p);
    if (tellEditor) runOnEditor(L"window.Haiku && window.Haiku.previewClosed && window.Haiku.previewClosed()");
}

void layoutPreview(HWND hwnd) {
    if (!g_sim || !g_sim->previewController) return;
    RECT rc;
    GetClientRect(hwnd, &rc);
    rc.top = scaled(hwnd, kToolbarHeight);
    g_sim->previewController->put_Bounds(rc);
}

LRESULT CALLBACK previewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == 1) closePreview(true);
        else if (LOWORD(wp) == 2 && g_sim && g_sim->previewView) g_sim->previewView->Reload();
        return 0;
    case WM_SIZE:
        layoutPreview(hwnd);
        return 0;
    case WM_CLOSE:
        closePreview(true);
        return 0;
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wp, kText);
        SetBkColor((HDC)wp, kBack);
        return (LRESULT)CreateSolidBrush(kBack);
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH b = CreateSolidBrush(kBack);
        FillRect((HDC)wp, &rc, b);
        DeleteObject(b);
        return 1;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void openPreview(const std::string& project) {
    if (!g_sim) return;
    Sim& sim = *g_sim;
    const std::wstring url = std::wstring(kPreviewOrigin) + util::widen(project) + L"/index.html";
    const std::wstring title = L"預覽 · " + util::widen(project);
    if (sim.preview) {
        SetWindowTextW(sim.preview, title.c_str());
        if (sim.previewView) sim.previewView->Navigate(url.c_str());
        SetForegroundWindow(sim.preview);
        return;
    }
    RECT outer;
    GetWindowRect(sim.hwnd, &outer);
    sim.preview = CreateWindowExW(0, kPreviewClass, title.c_str(), WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                  outer.left + 28, outer.top + 28, outer.right - outer.left, outer.bottom - outer.top,
                                  sim.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!sim.preview) return;
    const int h = scaled(sim.preview, kToolbarHeight), pad = scaled(sim.preview, 6);
    HFONT font = CreateFontW(-scaled(sim.preview, 14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                             CLEARTYPE_QUALITY, 0, L"Segoe UI");
    auto button = [&](const wchar_t* text, int id, int x, int w) {
        HWND b = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, x, pad, w, h - 2 * pad,
                                 sim.preview, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(b, WM_SETFONT, (WPARAM)font, TRUE);
    };
    button(L"返回編輯", 1, pad, scaled(sim.preview, 88));
    button(L"重新整理", 2, pad * 2 + scaled(sim.preview, 88), scaled(sim.preview, 88));
    ShowWindow(sim.preview, SW_SHOWNORMAL);
    auto shared = sim.shared;
    sim.env->CreateCoreWebView2Controller(
        sim.preview,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>([url](
                                                                                HRESULT hr,
                                                                                ICoreWebView2Controller* c) -> HRESULT {
            if (FAILED(hr) || !c || !g_sim || !g_sim->preview) return S_OK;
            g_sim->previewController = c;
            c->get_CoreWebView2(&g_sim->previewView);
            ComPtr<ICoreWebView2Settings> s;
            g_sim->previewView->get_Settings(&s);
            // The preview has no bridge: pages the model wrote cannot reach the host, the keys or the other projects.
            s->put_IsWebMessageEnabled(FALSE);
            s->put_AreDevToolsEnabled(FALSE);
            s->put_AreHostObjectsAllowed(FALSE);
            s->put_IsStatusBarEnabled(FALSE);
            g_sim->previewView->AddWebResourceRequestedFilter(L"https://haiku.preview/*",
                                                              COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
            EventRegistrationToken t;
            g_sim->previewView->add_WebResourceRequested(
                Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                    [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* a) -> HRESULT {
                        ComPtr<ICoreWebView2WebResourceRequest> req;
                        a->get_Request(&req);
                        LPWSTR u = nullptr;
                        req->get_Uri(&u);
                        const std::wstring uri = u ? u : L"";
                        CoTaskMemFree(u);
                        if (g_sim && uri.rfind(kPreviewOrigin, 0) == 0) servePreview(*g_sim, a, uri);
                        return S_OK;
                    })
                    .Get(),
                &t);
            g_sim->previewView->add_NewWindowRequested(
                Callback<ICoreWebView2NewWindowRequestedEventHandler>([](ICoreWebView2*,
                                                                         ICoreWebView2NewWindowRequestedEventArgs* a)
                                                                          -> HRESULT {
                    a->put_Handled(TRUE);
                    return S_OK;
                }).Get(),
                &t);
            layoutPreview(g_sim->preview);
            g_sim->previewView->Navigate(url.c_str());
            return S_OK;
        }).Get());
}

// ------------------------------------------------------------------ the editor page
void drainWork() {
    if (!g_sim) return;
    std::deque<std::function<void()>> q;
    {
        std::lock_guard<std::mutex> lock(g_sim->shared->mu);
        q.swap(g_sim->shared->queue);
    }
    for (auto& fn : q) fn();
}

void onBridgeCall(const std::string& text) {
    if (!g_sim) return;
    const Json m = Json::parse(text);
    const std::string id = m["id"].str(), method = m["method"].str();
    Json params = m["params"].isObject() ? m["params"] : Json::object();
    auto finish = [](const std::string& id, const std::string& method, const std::string& project,
                     const haikusim::Reply& reply) {
        if (!g_sim) return;
        runOnEditor(util::widen(haikusim::Host::replyScript(id, reply)));
        if (reply.ok && method == "preview.open") openPreview(project);
    };
    const std::string project = params["project"].str();
    if (haikusim::Host::slow(method)) {
        // Network calls wait on the provider for up to four minutes: off the UI thread, back on it for the reply.
        auto shared = g_sim->shared;
        haikusim::Host* host = g_sim->host.get();
        HWND hwnd = g_sim->hwnd;
        std::thread([shared, host, hwnd, id, method, params, project, finish] {
            haikusim::Reply reply = host->handle(method, params);
            std::lock_guard<std::mutex> lock(shared->mu);
            if (!shared->alive) return;
            shared->queue.push_back([=] { finish(id, method, project, reply); });
            PostMessageW(hwnd, WM_SIM_WORK, 0, 0);
        }).detach();
        return;
    }
    finish(id, method, project, g_sim->host->handle(method, params));
}

void setupEditor(HWND hwnd) {
    Sim& sim = *g_sim;
    sim.view->AddScriptToExecuteOnDocumentCreated(
        L"(function(){ if (window.chrome && window.chrome.webview) window.HaikuHost = "
        L"{ call: function (m) { window.chrome.webview.postMessage(String(m)); } }; })();",
        nullptr);
    ComPtr<ICoreWebView2Settings> s;
    sim.view->get_Settings(&s);
    s->put_AreDevToolsEnabled(!util::getEnv(L"CCA_DEV").empty());
    s->put_AreDefaultContextMenusEnabled(FALSE);
    s->put_IsStatusBarEnabled(FALSE);
    s->put_IsZoomControlEnabled(FALSE);
    sim.view->AddWebResourceRequestedFilter(L"https://haiku.app/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    EventRegistrationToken t;
    sim.view->add_WebResourceRequested(
        Callback<ICoreWebView2WebResourceRequestedEventHandler>([](ICoreWebView2*,
                                                                   ICoreWebView2WebResourceRequestedEventArgs* a)
                                                                    -> HRESULT {
            ComPtr<ICoreWebView2WebResourceRequest> req;
            a->get_Request(&req);
            LPWSTR u = nullptr;
            req->get_Uri(&u);
            const std::wstring uri = u ? u : L"";
            CoTaskMemFree(u);
            if (g_sim && uri.rfind(kAppOrigin, 0) == 0) serveApp(*g_sim, a, uri);
            return S_OK;
        }).Get(),
        &t);
    // The editor never leaves its own pages, and opens nothing else.
    sim.view->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                                         [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) -> HRESULT {
                                             LPWSTR u = nullptr;
                                             a->get_Uri(&u);
                                             const std::wstring uri = u ? u : L"";
                                             CoTaskMemFree(u);
                                             if (uri.rfind(kAppOrigin, 0) != 0) a->put_Cancel(TRUE);
                                             return S_OK;
                                         })
                                         .Get(),
                                     &t);
    sim.view->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                                         [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
                                             a->put_Handled(TRUE);
                                             return S_OK;
                                         })
                                         .Get(),
                                     &t);
    sim.view->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                         [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                                             LPWSTR src = nullptr;
                                             a->get_Source(&src);
                                             const bool trusted = src && std::wstring(src).rfind(kAppOrigin, 0) == 0;
                                             CoTaskMemFree(src);
                                             LPWSTR text = nullptr;
                                             if (trusted && SUCCEEDED(a->TryGetWebMessageAsString(&text)) && text)
                                                 onBridgeCall(util::narrow(text));
                                             CoTaskMemFree(text);
                                             return S_OK;
                                         })
                                         .Get(),
                                     &t);
    sim.view->add_NavigationCompleted(
        Callback<ICoreWebView2NavigationCompletedEventHandler>([hwnd](ICoreWebView2* v,
                                                                      ICoreWebView2NavigationCompletedEventArgs*)
                                                                   -> HRESULT {
            if (g_loadedHook) g_loadedHook(v, hwnd);
            return S_OK;
        }).Get(),
        &t);
    RECT rc;
    GetClientRect(hwnd, &rc);
    rc.top = scaled(hwnd, kBannerHeight);
    sim.controller->put_Bounds(rc);
    ComPtr<ICoreWebView2Controller2> c2;
    if (SUCCEEDED(sim.controller.As(&c2))) c2->put_DefaultBackgroundColor({255, 247, 245, 240});
    sim.view->Navigate((std::wstring(kAppOrigin) + L"index.html").c_str());
}

void paintBanner(HWND hwnd, HDC dc) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    RECT band{0, 0, rc.right, scaled(hwnd, kBannerHeight)};
    HBRUSH bg = CreateSolidBrush(kBack);
    FillRect(dc, &band, bg);
    DeleteObject(bg);
    RECT mark{scaled(hwnd, 16), scaled(hwnd, 15), scaled(hwnd, 24), scaled(hwnd, 23)};
    HBRUSH signal = CreateSolidBrush(kSignal);
    FillRect(dc, &mark, signal);
    DeleteObject(signal);
    SetBkMode(dc, TRANSPARENT);
    HFONT bold = CreateFontW(-scaled(hwnd, 15), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                             CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT fineFont = CreateFontW(-scaled(hwnd, 12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                                 CLEARTYPE_QUALITY, 0, L"Segoe UI");
    auto old = SelectObject(dc, bold);
    SetTextColor(dc, kText);
    RECT line1{scaled(hwnd, 32), scaled(hwnd, 8), rc.right - scaled(hwnd, 12), scaled(hwnd, 28)};
    DrawTextW(dc, L"你已經有桌面版了，建議使用桌面版", -1, &line1, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    SelectObject(dc, fineFont);
    SetTextColor(dc, kMuted);
    RECT line2{scaled(hwnd, 16), scaled(hwnd, 30), rc.right - scaled(hwnd, 12), scaled(hwnd, kBannerHeight - 4)};
    DrawTextW(dc, L"這是 Haiku 手機版在電腦上的模擬：專案存在這台電腦，不會同步到手機。", -1, &line2,
              DT_WORDBREAK | DT_END_ELLIPSIS);
    SelectObject(dc, old);
    DeleteObject(bold);
    DeleteObject(fineFont);
}

LRESULT CALLBACK simProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIM_WORK:
        drainWork();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paintBanner(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (g_sim && g_sim->controller) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            rc.top = scaled(hwnd, kBannerHeight);
            g_sim->controller->put_Bounds(rc);
        }
        return 0;
    case WM_DPICHANGED: {
        const RECT* r = (const RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_sim && g_sim->hwnd == hwnd) {
            closePreview(false);
            {
                std::lock_guard<std::mutex> lock(g_sim->shared->mu);
                g_sim->shared->alive = false;
            }
            if (g_sim->controller) g_sim->controller->Close();
            delete g_sim;
            g_sim = nullptr;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void registerClasses() {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSW sim{};
    sim.lpfnWndProc = simProc;
    sim.hInstance = GetModuleHandleW(nullptr);
    sim.lpszClassName = kSimClass;
    sim.hIcon = LoadIconW(sim.hInstance, MAKEINTRESOURCEW(101));
    sim.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&sim);
    WNDCLASSW preview = sim;
    preview.lpfnWndProc = previewProc;
    preview.lpszClassName = kPreviewClass;
    RegisterClassW(&preview);
}

bool readAssets(const std::wstring& apk, std::map<std::string, std::string>& assets, std::string& error) {
    std::string zip;
    if (!util::readFile(apk, zip)) {
        error = "無法讀取安裝包";
        return false;
    }
    if (!haikusim::readZipFolder(zip, "assets/www/", assets, error)) return false;
    for (const char* needed : {"index.html", "haiku.js", "haiku.css"})
        if (!assets.count(needed)) {
            error = std::string("這個 APK 不是 Haiku：缺少 ") + needed;
            return false;
        }
    return true;
}

}  // namespace

std::wstring dataDir() {
    return util::joinPath(util::appDataDir(), L"haiku-sim");
}

bool loadAssets(const std::wstring& apk, std::string& error) {
    std::map<std::string, std::string> assets;
    return readAssets(apk, assets, error);
}

void setLoadedHook(std::function<void(ICoreWebView2*, HWND)> hook) {
    g_loadedHook = std::move(hook);
}
HWND window() {
    return g_sim ? g_sim->hwnd : nullptr;
}
HWND previewWindow() {
    return g_sim ? g_sim->preview : nullptr;
}
ICoreWebView2* previewPage() {
    return g_sim ? g_sim->previewView.Get() : nullptr;
}

bool open(HWND owner, const std::wstring& apk, std::string& error) {
    if (g_sim) {
        ShowWindow(g_sim->hwnd, SW_RESTORE);
        SetForegroundWindow(g_sim->hwnd);
        return true;
    }
    auto sim = std::make_unique<Sim>();
    if (!readAssets(apk, sim->assets, error)) return false;
    util::ensureDir(dataDir());
    sim->host = std::make_unique<haikusim::Host>(dataDir(), haikusim::defaultExportDir());
    registerClasses();
    // A phone-sized window: as tall as the screen allows, up to a tall phone.
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY), &mi);
    const UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
    const int workHeight = MulDiv(mi.rcWork.bottom - mi.rcWork.top, 96, (int)dpi);
    sim->contentHeight = std::max(560, std::min(800, workHeight - kBannerHeight - 48));
    RECT frame{0, 0, MulDiv(kPhoneWidth, (int)dpi, 96), MulDiv(kBannerHeight + sim->contentHeight, (int)dpi, 96)};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectExForDpi(&frame, style, FALSE, 0, dpi);
    const int w = frame.right - frame.left, h = frame.bottom - frame.top;
    const int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2,
              y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
    Sim* raw = sim.release();
    g_sim = raw;
    raw->hwnd = CreateWindowExW(0, kSimClass, L"CCA Haiku · 手機模擬", style, x, y, w, h, nullptr, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
    if (!raw->hwnd) {
        delete raw;
        g_sim = nullptr;
        error = "無法建立視窗";
        return false;
    }
    ShowWindow(raw->hwnd, SW_SHOWNORMAL);
    const std::wstring userData = util::joinPath(util::localAppDataDir(), L"WebView2-haiku");
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userData.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([](HRESULT hr,
                                                                                ICoreWebView2Environment* env)
                                                                                 -> HRESULT {
            if (!g_sim) return S_OK;
            if (FAILED(hr) || !env) {
                MessageBoxW(g_sim->hwnd, L"無法啟動 Microsoft Edge WebView2；請先安裝 WebView2 Runtime。", L"CCA Haiku",
                            MB_ICONERROR);
                DestroyWindow(g_sim->hwnd);
                return S_OK;
            }
            g_sim->env = env;
            return env->CreateCoreWebView2Controller(
                g_sim->hwnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                                 [](HRESULT hr, ICoreWebView2Controller* c) -> HRESULT {
                                     if (!g_sim) return S_OK;
                                     if (FAILED(hr) || !c) {
                                         DestroyWindow(g_sim->hwnd);
                                         return S_OK;
                                     }
                                     g_sim->controller = c;
                                     c->get_CoreWebView2(&g_sim->view);
                                     setupEditor(g_sim->hwnd);
                                     return S_OK;
                                 })
                                 .Get());
        }).Get());
    if (FAILED(hr)) {
        DestroyWindow(raw->hwnd);
        error = "無法啟動 Microsoft Edge WebView2；請先安裝 WebView2 Runtime";
        return false;
    }
    return true;
}

}  // namespace haikuwin
