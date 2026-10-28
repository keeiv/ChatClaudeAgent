// The Haiku phone simulator window (desktop only): a phone-sized window that runs the unmodified Haiku APK's editor
// page in WebView2 and answers its host bridge natively, so Haiku can be tried on a PC without being changed for it.
#pragma once

#include <windows.h>

#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl.h>

#include <WebView2.h>

#include <functional>
#include <string>

namespace haikuwin {

// Opens the simulator for a verified APK, or brings an open one forward. False with a reason when it cannot start.
bool open(HWND owner, const std::wstring& apk, std::string& error);

// Reads the editor page's web assets (assets/www) out of the APK; the same check that gates opening the window.
bool loadAssets(const std::wstring& apk, std::string& error);

// The simulator's own data (projects, checkpoints, keys): %APPDATA%\CCA\haiku-sim.
std::wstring dataDir();

// Tests only: called on the UI thread each time the editor page finishes loading.
void setLoadedHook(std::function<void(ICoreWebView2* editor, HWND window)> hook);
// Tests only: the window that is open, or null; and the preview window and its page while one is open.
HWND window();
HWND previewWindow();
ICoreWebView2* previewPage();

}  // namespace haikuwin
