@echo off
rem Builds build\ChatClaudeAgent.exe (desktop). "build.bat cli" builds build\ChatClaudeAgent-CLI.exe,
rem "build.bat test" builds and runs build\tests.exe.
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSPATH="
if not exist "%VSWHERE%" goto :novs
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
:novs
if not defined VSPATH goto :nomsvc
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

if not exist build mkdir build
if not exist build\obj mkdir build\obj
rem Alternate outputs let a running desktop/CLI session stay open during verification.
if not defined CCA_DESKTOP_OUT set "CCA_DESKTOP_OUT=build\ChatClaudeAgent.exe"
if not defined CCA_CLI_OUT set "CCA_CLI_OUT=build\ChatClaudeAgent-CLI.exe"
rem The WebView2 SDK is not stored in the repository; this downloads it (hash-pinned) when it is missing.
if not exist third_party\webview2\build\native\include\WebView2.h (
    python tools\fetch_webview2.py
    if errorlevel 1 exit /b 1
)

set CFLAGS=/nologo /std:c++20 /EHsc /O2 /utf-8 /W3 /MT /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /Ithird_party\webview2\build\native\include /Isrc
rem CCA_SANITIZE=1 builds with MSVC AddressSanitizer. It needs the DLL runtime (/MD), so /MT is swapped out.
if defined CCA_SANITIZE set "CFLAGS=%CFLAGS:/MT=/MD% /fsanitize=address /Zi"
set CORE=src\json.cpp src\util.cpp src\process.cpp src\diff.cpp src\claude_agent.cpp src\codex_agent.cpp src\app.cpp src\auth.cpp src\group.cpp src\plugins.cpp src\personas.cpp src\profile.cpp src\snapshot.cpp src\hardware.cpp src\ollama.cpp src\local.cpp src\rollback.cpp src\review.cpp src\humanize.cpp src\guard.cpp src\local_tools.cpp src\takeover.cpp src\workflow.cpp src\model_dates.cpp src\claude_usage.cpp src\task_ledger.cpp src\runtime.cpp src\gemini_agent.cpp src\gemini_auth.cpp src\sandbox_mcp.cpp src\pipeline.cpp src\platform.cpp
set CORE=%CORE% src\themes.cpp src\routing.cpp src\task_analysis.cpp src\adaptive.cpp src\desk.cpp src\app_update.cpp src\updater.cpp src\navigation.cpp src\badges.cpp src\activity.cpp src\collaboration.cpp src\opinion.cpp src\workspaces.cpp src\quality.cpp src\gitflow.cpp src\gitpanel.cpp src\regression_guard.cpp src\editions.cpp src\extensions.cpp src\haiku_sim.cpp
set "RCFLAGS="
set LIBS=version.lib winhttp.lib bcrypt.lib crypt32.lib user32.lib gdi32.lib ole32.lib oleaut32.lib shell32.lib dwmapi.lib advapi32.lib shlwapi.lib uuid.lib windowscodecs.lib

python tools\make_themes.py --check
if errorlevel 1 exit /b 1

if /i "%~1"=="appearance-test" goto :appearance-test
if /i "%~1"=="gemini-test" goto :gemini-test
if /i "%~1"=="git-test" goto :git-test
if /i "%~1"=="desk-test" goto :desk-test
if /i "%~1"=="edition-test" goto :edition-test
if /i "%~1"=="test" goto :test
if /i "%~1"=="collaboration-test" goto :collaboration-test
if /i "%~1"=="quality-test" goto :quality-test
if /i "%~1"=="pipeline-test" goto :pipeline-test
if /i "%~1"=="profile-test" goto :profile-test
if /i "%~1"=="takeover-test" goto :takeover-test
if /i "%~1"=="haiku-sim-test" goto :haiku-sim-test
if /i "%~1"=="haiku-window-test" goto :haiku-window-test
if /i "%~1"=="memory-test" goto :memory-test
if /i "%~1"=="cli" goto :cli
if /i "%~1"=="pty" goto :pty
if /i "%~1"=="live" goto :live
if /i "%~1"=="sandbox-live" goto :sandbox-live

rem The UI files are compiled into the exe as resources.
node tools\gen_resources.js
if errorlevel 1 exit /b 1
rc /nologo %RCFLAGS% /fo build\obj\resources.res build\resources.rc
if errorlevel 1 exit /b 1
cl %CFLAGS% /Fobuild\obj\ src\main.cpp src\haiku_window.cpp %CORE% build\obj\resources.res /Fe:"%CCA_DESKTOP_OUT%" /link /SUBSYSTEM:WINDOWS /MANIFEST:NO third_party\webview2\build\native\x64\WebView2LoaderStatic.lib %LIBS%
if errorlevel 1 exit /b 1
echo.
echo Built %CCA_DESKTOP_OUT%
exit /b 0

:gemini-test
cl %CFLAGS% /Fobuild\obj\ tests\gemini_test.cpp %CORE% /Fe:build\gemini_test.exe /link %LIBS%
if errorlevel 1 exit /b 1
build\gemini_test.exe
exit /b %errorlevel%

:git-test
cl %CFLAGS% /Fobuild\obj\ tests\git_test.cpp %CORE% /Fe:build\git_test.exe /link %LIBS%
if errorlevel 1 exit /b 1
build\git_test.exe
exit /b %errorlevel%

:appearance-test
cl %CFLAGS% /Fobuild\obj\ tests\appearance_test.cpp %CORE% src\cli_render.cpp src\cli_flow.cpp /Fe:build\appearance_test.exe /link %LIBS%
if errorlevel 1 exit /b 1
build\appearance_test.exe
exit /b %errorlevel%

:desk-test
rc /nologo /fo build\obj\update_probe.res tests\update_probe.rc
if errorlevel 1 exit /b 1
cl %CFLAGS% /Fobuild\obj\ tests\update_probe.cpp build\obj\update_probe.res /Fe:build\update_probe.exe /link %LIBS%
if errorlevel 1 exit /b 1
cl %CFLAGS% /Fobuild\obj\ tests\desk_test.cpp %CORE% /Fe:build\desk_test.exe /link %LIBS%
if errorlevel 1 exit /b 1
build\desk_test.exe
exit /b %errorlevel%

:edition-test
rem CCA plugins and the edition marker, built against the same fakes as the other native tests.
rc /nologo /fo build\obj\edition_probe_astra.res tests\edition_probe_astra.rc
if errorlevel 1 exit /b 1
cl %CFLAGS% /Fobuild\obj\ tests\update_probe.cpp build\obj\edition_probe_astra.res /Fe:build\edition_probe_astra.exe /link %LIBS%
if errorlevel 1 exit /b 1
cl %CFLAGS% /Fobuild\obj\ tests\edition_test.cpp %CORE% /Fe:build\edition_test.exe /link %LIBS%
if errorlevel 1 exit /b 1
build\edition_test.exe
if errorlevel 1 exit /b 1
exit /b %errorlevel%

:test
cl %CFLAGS% /Fobuild\obj\ tests\test_main.cpp %CORE% src\cli_render.cpp src\cli_flow.cpp /Fe:build\tests.exe /link %LIBS%
if errorlevel 1 exit /b 1
build\tests.exe
exit /b %errorlevel%

:haiku-sim-test
rem The phone simulator's native side: inflate against zlib, the real APK when built, projects, keys and the bridge.
cl %CFLAGS% /Fobuild\obj\ tests\haiku_sim_test.cpp src\haiku_sim.cpp src\json.cpp src\util.cpp /Fe:build\haiku_sim_test.exe /link %LIBS% ws2_32.lib
if errorlevel 1 exit /b 1
build\haiku_sim_test.exe
exit /b %errorlevel%

:haiku-window-test
rem Opens the simulator window on the built APK, drives its bridge through the page and saves screenshots.
cl %CFLAGS% /Fobuild\obj\ tests\haiku_window_test.cpp src\haiku_window.cpp src\haiku_sim.cpp src\json.cpp src\util.cpp /Fe:build\haiku_window_test.exe /link /SUBSYSTEM:CONSOLE third_party\webview2\build\native\x64\WebView2LoaderStatic.lib %LIBS%
if errorlevel 1 exit /b 1
build\haiku_window_test.exe
exit /b %errorlevel%

:memory-test
rem Parsers and stores on hostile input; set CCA_SANITIZE=1 first for the AddressSanitizer run.
cl %CFLAGS% /Fobuild\obj\ tests\memory_test.cpp src\haiku_sim.cpp src\json.cpp src\util.cpp src\diff.cpp src\humanize.cpp /Fe:build\memory_test.exe /link %LIBS%
if errorlevel 1 exit /b 1
build\memory_test.exe
exit /b %errorlevel%

:takeover-test
cl %CFLAGS% /Fobuild\obj\ tests\takeover_test.cpp %CORE% /Fe:build\takeover_test.exe /link %LIBS%
if errorlevel 1 exit /b 1
build\takeover_test.exe
exit /b %errorlevel%

:pipeline-test
cl %CFLAGS% /Fobuild\obj\ tests\pipeline_test.cpp %CORE% /Fe:build\pipeline_test.exe /link %LIBS%
if errorlevel 1 exit /b 1
build\pipeline_test.exe
exit /b %errorlevel%

:collaboration-test
cl %CFLAGS% /Fobuild\obj\ tests\collaboration_test.cpp %CORE% /Fe:build\collaboration_test.exe /link %LIBS%
if errorlevel 1 exit /b 1
build\collaboration_test.exe
exit /b %errorlevel%

:quality-test
cl %CFLAGS% /Fobuild\obj\ tests\quality_test.cpp %CORE% /Fe:build\quality_test.exe /link %LIBS%
if errorlevel 1 exit /b 1
build\quality_test.exe
exit /b %errorlevel%

:profile-test
cl %CFLAGS% /Fobuild\obj\ tests\profile_test.cpp %CORE% /Fe:build\profile_test.exe /link %LIBS%
if errorlevel 1 exit /b 1
build\profile_test.exe
exit /b %errorlevel%

:cli
rem The terminal version: the same core, without WebView2.
node tools\gen_resources.js cli
if errorlevel 1 exit /b 1
rc /nologo %RCFLAGS% /fo build\obj\cli.res build\cli.rc
if errorlevel 1 exit /b 1
cl %CFLAGS% /Fobuild\obj\ src\cli_main.cpp src\term.cpp src\cli_render.cpp src\cli_flow.cpp %CORE% build\obj\cli.res /Fe:"%CCA_CLI_OUT%" /link /SUBSYSTEM:CONSOLE /MANIFEST:NO %LIBS%
if errorlevel 1 exit /b 1
echo.
echo Built %CCA_CLI_OUT%
exit /b 0


:pty
rem A pseudoconsole driver for checking the CLI from scripts (tests\pty_run.cpp).
cl %CFLAGS% /Fobuild\obj\ tests\pty_run.cpp /Fe:build\pty_run.exe /link /SUBSYSTEM:CONSOLE
exit /b %errorlevel%

:live
cl %CFLAGS% /Fobuild\obj\ tests\live_main.cpp %CORE% /Fe:build\live.exe /link %LIBS%
exit /b %errorlevel%

:sandbox-live
rem Build only. Executing with --run needs authorization to use Claude subscription quota.
cl %CFLAGS% /Fobuild\obj\ tests\sandbox_live.cpp %CORE% /Fe:build\sandbox_live.exe /link %LIBS%
exit /b %errorlevel%

:nomsvc
echo MSVC not found. Install Visual Studio Build Tools with "Desktop development with C++".
exit /b 1
