# 貢獻指南

謝謝你願意改進 CCA。這份文件說明怎麼取得原始碼、怎麼在自己的電腦上跑出和 CI 相同的結果、提交時要遵守什麼，以及哪些東西不該進這個倉庫。程式風格與架構規則寫在 [AGENTS.md](AGENTS.md)，人和 AI 工具共用同一份。

## 開始之前

- 小修正（錯字、明確的錯誤、補測試）可以直接開拉取請求。
- 新功能、改動訊息格式、改動權限或保存方式的變更，請先開一個 issue 說明想解決的問題，確認方向後再動手。CCA 的權限、驗收與保存規則很嚴，做到一半才被退回會浪費你的時間。
- 發現安全問題請**不要**公開回報，見 [SECURITY.md](SECURITY.md)。
- 在這裡的交流只談技術與事實。對事不對人，批評要附上可以檢驗的理由（檔案、行號、重現步驟）。

## 環境

只支援 Windows 10／11（x64）。需要：

| 工具 | 用途 |
| --- | --- |
| Visual Studio 2022 Build Tools，含「使用 C++ 的桌面開發」與 C++ Clang 工具 | 編譯、`clang-format` |
| Python 3.12 | 建置腳本、風格檢查、部分測試 |
| Node.js 22 | 資源產生、介面測試 |
| Git for Windows | 部分測試會建立真實的暫存倉庫 |
| Microsoft Edge | 介面測試（Playwright 使用已安裝的 Edge，不另外下載瀏覽器） |

WebView2 SDK 不放在倉庫裡。第一次建置時 `build.bat` 會執行 `tools/fetch_webview2.py`，從 NuGet 下載固定版本，核對 SHA-256 後才解開；離線時把 `.nupkg` 路徑放進 `CCA_WEBVIEW2_NUPKG`。

## 建置與測試

```powershell
git clone https://github.com/keeiv/ChatClaudeAgent.git
cd ChatClaudeAgent
cmd /c ".\build.bat"        # 桌面版 → build\ChatClaudeAgent.exe
cmd /c ".\build.bat cli"    # 終端機版 → build\ChatClaudeAgent-CLI.exe
```

`build.bat` 要從 PowerShell 用 `cmd /c` 執行。如果你的 CCA 正在執行，它會鎖住 exe，改用 `CCA_DESKTOP_OUT` 或 `CCA_CLI_OUT` 指定另一個輸出檔名，**不要結束使用中的 CCA**。

改了什麼，就跑對應的測試。所有測試都用假的後端，不會用到任何模型額度：

| 你改的地方 | 先跑 |
| --- | --- |
| `src/` 的核心邏輯 | `python tools/run_native_tests.py`（全部原生測試）或單一目標，例如 `python tools/run_native_tests.py test takeover-test` |
| 解析、ZIP、JSON、diff 等吃外部輸入的程式 | `cmd /c ".\build.bat memory-test"`，再用 `$env:CCA_SANITIZE="1"` 跑一次 AddressSanitizer 版 |
| `ui/` | `python tests/run_browser_tests.py`（見下方安裝說明） |
| 終端機版 | `cmd /c ".\build.bat cli"`、`cmd /c ".\build.bat pty"`、`cmd /c ".\build.bat takeover-test"` 之後 `python tests/cli_smoke.py` |
| 錯誤處理、保存、路徑、環境相依的程式 | `python tests/extreme_env.py`（需先建置 `test`、`cli`、`pty`、`takeover-test`） |
| 任何檔案 | `python tools/check_style.py --format --js` |

介面測試需要 Playwright 與 LinkeDOM，版本固定在 [ci.yml](.github/workflows/ci.yml) 的 `PLAYWRIGHT_VERSION`、`LINKEDOM_VERSION`：

```powershell
npm install --prefix $env:TEMP\pw --no-audit --no-fund --ignore-scripts playwright@1.64.1
npm install --prefix build/workflow-qa --no-audit --no-fund --ignore-scripts linkedom@0.18.12
$env:NODE_PATH = "$env:TEMP\pw\node_modules"
python tests/run_browser_tests.py
```

測試一律用 `CCA_DATA_DIR` 指到暫存資料夾，不要碰真正的 `%APPDATA%\CCA`。會花訂閱額度的測試（`build.bat live`、`sandbox_live --run`）CI 不跑，貢獻者也不需要跑。沒辦法跑某項驗證時，在拉取請求裡直接寫「沒有驗證」和原因，不要寫成通過。

## CI 會檢查什麼

每個拉取請求都要通過 [CI](.github/workflows/ci.yml)。最後一項 `CI 全部通過` 彙整所有工作，建議把它設為必要的狀態檢查。

| 工作 | 內容 | 在本機怎麼重現 |
| --- | --- | --- |
| 倉庫衛生與靜態檢查 | 沒有金鑰、個人路徑、二進位檔與內部紀錄；工作流程規則；Markdown 連結；風格；主題目錄 | `python tools/check_public.py`、`check_workflows.py`、`check_links.py`、`check_style.py --js`、`make_themes.py --check` |
| 建置與風格 | 兩個 Windows 映像檔上建置桌面版與終端機版，改動的 C++ 行符合 `.clang-format`，產出執行檔雜湊 | `cmd /c ".\build.bat"`、`python tools/check_style.py --format --js` |
| 原生測試 | 單元、協定、保存、Git、品質、Gemini、管線、協作與 Haiku 模擬器的原生部分 | `python tools/run_native_tests.py` |
| 模擬測試 | 假的 Claude／Codex 子程序、真實 ConPTY 裡的終端機版、工作程序、沙箱 MCP、更新程式替換 | 見上表「終端機版」 |
| 介面測試 | 真實 Edge 與 DOM：主題、版面、訊息、設定、Markdown 連結 | `python tests/run_browser_tests.py` |
| 記憶體測試 | 吃外部輸入的程式在數萬次迭代後私有記憶體不增長；AddressSanitizer 下沒有越界與釋放後使用 | `cmd /c ".\build.bat memory-test"` |
| 極端環境 | 單一行程記憶體上限、單核心、剝離環境變數、中文／emoji／超長路徑、損毀資料、唯讀資料夾、第二個實例、極小與極大終端機 | `python tests/extreme_env.py` |
| CodeQL | C++、JavaScript、Python 的靜態安全掃描 | 在 GitHub 上看結果 |

這些檢查是**門檻**，不是補考。看到紅燈時請先找出原因，不要刪掉測試或放寬檢查讓它過；如果檢查本身有誤，請在拉取請求裡說明並一併修正檢查。

## 程式碼

- C++20（MSVC），4 格縮排，新寫或改到的行不超過 120 欄，**只格式化你改到的行**，不要為了風格重排整個檔案。
- JavaScript 在 `ui/`，2 格縮排、單引號、有分號，沒有框架也沒有打包步驟。
- 介面與核心之間只走 JSON 訊息；新增一種訊息要同時改核心分派、`ui/app.js`、`ui/mock.js`，需要時改 CLI（細節見 AGENTS.md「架構規則」）。
- 不用 C++ 例外；錯誤用回傳值。模型輸出一律當成不可信的文字，桌面版經 `MD.render` 跳脫後才放進 `innerHTML`，CLI 輸出工具文字前要濾掉控制碼。
- 介面、說明、文件用台灣繁體中文，寫具體的檔名、數字與步驟。
- 新功能附測試；修錯誤先寫一個會失敗的測試，再修到通過。

## 提交與拉取請求

- 一個提交做一件事。提交訊息第一行用 `類型: 說明`，類型是 `feat`、`fix`、`docs`、`test`、`ci`、`build`、`refactor`、`perf`、`chore` 之一，說明用英文祈使句，例如 `fix: reject junctions in project paths`。需要的話空一行，補上為什麼這樣改。
- 不要改寫已經推送的歷史。拉取請求通常以 squash 合併，標題請依同樣格式寫。
- 拉取請求描述說明：問題是什麼、怎麼解、跑了哪些測試、哪些**沒有**跑。介面改動附截圖。
- 保持拉取請求小而聚焦。順手修的無關問題請另開一個。

### 使用 AI 工具

可以用，但你要對送出的每一行負責：逐行讀過、自己跑過測試、確認沒有帶進不該公開的內容。CCA 本身的審查流程（撰寫者、審查者、雙方都沒有意見才收）也適用於這裡：審查意見要附上可以核對的依據，而不是「看起來正確」。

## 不要提交的東西

`python tools/check_public.py` 會擋下這些，CI 也會跑：

- 金鑰、權杖、憑證、`.env` 檔、簽署用的 keystore。
- 你電腦上的路徑、使用者名稱、電子郵件、雲端同步資料夾名稱。測試資料請用 `C:\Users\dev\...` 這類通用寫法。
- 建置輸出、記錄檔、執行檔、安裝包（`build/` 不進版本控制；執行檔放在發行版）。
- 個別 AI 工具的本機設定（例如 `.claude/`）和內部作業紀錄。通用的開發規則放在 AGENTS.md。
- 超過 512 KiB 的圖片或 1 MiB 的文字檔。

## 授權

CCA 以 [GNU GPL v3](LICENSE) 授權。送出貢獻即表示你有權提供這些內容，並同意它們以同一個授權發行。`third_party/` 內的第三方元件依各自的授權條款。
