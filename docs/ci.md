# CI 與公開倉庫的檢查

公開倉庫（GPL-3.0）的每個推送與拉取請求都跑 `.github/workflows/ci.yml`；另有 `codeql.yml`（靜態安全掃描）與 `dependency-review.yml`（相依套件弱點）。本文說明每一組檢查證明了什麼、沒有證明什麼，以及怎麼在本機重現。貢獻者的操作流程見公開倉庫的 `CONTRIBUTING.md`。

## 七組檢查

| 工作 | 內容 | 本機指令 |
| --- | --- | --- |
| 倉庫衛生與靜態檢查（Ubuntu） | 沒有私鑰、權杖、個人路徑、真實電子郵件、二進位檔與內部紀錄；必備檔案齊全；工作流程規則；Markdown 相對連結；機械式風格與 JS 語法；主題目錄與產生檔一致 | `tools/check_public.py`、`check_workflows.py`、`check_links.py`、`check_style.py --js`、`make_themes.py --check` |
| 建置與風格（`windows-2022`、`windows-latest`） | 改動的 C++ 行符合 `.clang-format`（以拉取請求的基底提交比較）；桌面版與 CLI 版能建置；上傳執行檔與 SHA-256 | `build.bat`、`build.bat cli`、`check_style.py --format --js` |
| 原生測試（兩個映像檔） | `tools/run_native_tests.py` 依序建置並執行 13 個目標，失敗也繼續，最後逐項列出 | `python tools\run_native_tests.py` |
| 模擬測試 | 假的 Claude／Codex 子程序、真實 ConPTY 裡的 CLI、工作程序、沙箱 MCP、品質工具、風格與發行工具的回歸測試、更新程式替換暫存 EXE | `tests\cli_smoke.py` 等，見 `CONTRIBUTING.md` |
| 介面測試 | 純 DOM 的 LinkeDOM 測試，與用真實 Edge（Playwright）驅動 `ui/` 的測試 | `python tests\run_browser_tests.py` |
| 記憶體測試 | 見下 | `build.bat memory-test` |
| 極端環境 | 見下 | `python tests\extreme_env.py` |

`CI 全部通過` 在其他工作之後執行，任何一項不是 success（含被略過）就失敗。把它設為分支保護的必要狀態檢查，就只需要維護這一個名稱。

GitHub 的執行環境有兩個限制，測試對它們的處理方式要看得見：

- **更新程式替換測試**（`tests/test_update_broker.py`）：CCA 刻意拒絕在不允許脫離的宿主 Job 物件裡更新（更新程式必須比 CCA 活得久），而 GitHub 的每個步驟都在這樣的 Job 裡。測試用 `host_job_allows_breakaway()` 偵測到這種環境就**跳過**並說明原因，所以真實的替換與復原只在本機（`python tests\test_update_broker.py`，要從 PowerShell 或 cmd 跑，不是 Git Bash）驗證，CI 沒有涵蓋。
- **瀏覽器冷啟動逾時**：`tests/run_browser_tests.py` 對「頁面載入或等待逾時」且沒有任何斷言失敗的測試重試一次，並印出 `RETRY`。斷言失敗不重試。

這些檢查**沒有**涵蓋：真實模型與訂閱額度（`build.bat live`）、真的 Docker 容器隔離（需要映像檔與 Docker，由 `tests\test_sandbox_mcp.py` 在有 Docker 時多跑一次）、Android 實機（Haiku 的 APK 與手機版編輯頁在私有的 Haiku 流程測）、使用者目前正在使用的 CCA 的實際更新。

## 記憶體測試

`tests/memory_test.cpp` 用固定種子的產生器對吃外部輸入的程式施壓：JSON（隨機結構、隨機變異、20 萬層巢狀、10 萬層物件、20 萬元素陣列）、行 diff 與 VERDICT 解析、deflate 垃圾資料、被變異與截斷的 ZIP、專案檔案寫入與檢查點（含路徑跳脫與裝置名稱）。

- 一般建置：每組迴圈先暖機，再量私有位元組（`PrivateUsage`）在數千到數萬次迭代後的成長，超過 16 MiB 就失敗。這抓得到洩漏。
- AddressSanitizer 建置：`CCA_SANITIZE=1` 讓 `build.bat` 把 `/MT` 換成 `/MD` 並加 `/fsanitize=address /Zi`（ASan 需要 DLL 執行階段；`vcvars` 已把 ASan 的 DLL 目錄放進 PATH）。這抓得到越界與釋放後使用，同樣的迴圈與變異輸入會被執行。MSVC 沒有 LeakSanitizer，而且 ASan 的隔離區會讓私有位元組暫時膨脹，所以這個模式不檢查成長量，只印出數字。
- CI 另外用 `python tools/run_native_tests.py --asan` 把全部原生測試目標（單元、接手、管線、品質…）都用 ASan 建置後執行。
  `gemini-test` 與 `cursor-test` 除外：它們把受測程式複製到另一個資料夾當成 `node.exe` 來跑，ASan 版在那裡找不到執行階段 DLL，探測程序啟動不了。
  這一項在 CI 上抓到過真實缺陷：非 const 的 `Json::operator[]` 讀到缺鍵時會插入並重新配置，運算式裡先取得的參照因此懸空（`claude_agent.cpp`、`codex_agent.cpp`、`gemini_agent.cpp`、`app.cpp` 的 `TurnDone`），一般建置只會偶發當機。

## 極端環境

`tests/extreme_env.py` 用 Windows Job 物件把受測程式以暫停狀態啟動、套上限制後才放行，限制會被子程序繼承：

| 情境 | 檢查 |
| --- | --- |
| 每個行程 512 MiB 上限、單核心加閒置優先權、剝離環境變數（只留 `SystemRoot`、`TEMP`、極簡 `PATH`）、`TEMP` 在中文／emoji／空白的資料夾、C 語系 | 620 項單元測試全部通過 |
| 48 MiB 上限 | CLI 能啟動並印出版本 |
| 256 MiB 上限 | CLI 在 ConPTY 裡完整跑完一個工作階段 |
| 資料夾與專案夾名稱含中文、雙空白、emoji、重音字元、結尾句點 | 能啟動、能保存、能結束 |
| 170 字元與 300 字元以上的路徑 | 不當機、不卡住（超過 `MAX_PATH` 時可以拒絕，但要結束） |
| 設定與已保存檔案被換成 12 種壞內容（空檔、全零、無效 UTF-8、30 萬層括號、型別錯亂、超大字串…） | 不當機、不卡住 |
| 資料夾用 `icacls` 拒絕寫入 | 不當機（先驗證拒絕規則真的擋得住寫入，否則情境本身不成立） |
| 第二個實例使用同一份資料 | 被拒絕並顯示說明，第一個實例不受影響 |
| 20x5 到 500x200 的終端機 | 不當機 |
| 8000 字元的輸入列、數百個 emoji、很長的未知指令 | 不當機 |

判斷「當機」看結束碼是否為 `0xC0000005`、`0xC00000FD`、`0xC0000409`、`0xC0000374` 等；「卡住」看逾時。負向對照：把單元測試的上限降到 6 MiB 就會失敗，表示上限確實有套用。

## 固定的外部版本

| 項目 | 固定在 | 位置 |
| --- | --- | --- |
| WebView2 SDK | NuGet `Microsoft.Web.WebView2` 1.0.4191.47，套件 SHA-256 `f492bbf5…5f23d0` | `tools/fetch_webview2.py`。只解開需要的 5 個檔案；離線時用 `CCA_WEBVIEW2_NUPKG` |
| Playwright | 1.64.1（`--ignore-scripts`，使用已安裝的 Edge，不下載瀏覽器） | `ci.yml` 的 `PLAYWRIGHT_VERSION` |
| LinkeDOM | 0.18.12 | `ci.yml` 的 `LINKEDOM_VERSION` |
| 每個 Action | 完整 40 字元提交 SHA，旁邊註解版本 | `tools/check_workflows.py` 強制；Dependabot 每週更新 |

## 維護

- 新增測試：原生目標加進 `tools/run_native_tests.py` 的 `TARGETS`，介面測試加進 `tests/run_browser_tests.py` 的 `TESTS`，極端情境加進 `tests/extreme_env.py` 的 `SCENARIOS`。
- 新增工作流程：頂層 `permissions` 預設 `contents: read`，需要寫入權限的工作自己宣告；每個工作設 `timeout-minutes`；外部 Action 釘 SHA；不用 `pull_request_target`；不把 `github.event.*` 的標題、內文、分支名稱直接放進 `run:`（改用 `env:`）。`tools/check_workflows.py` 會擋。
- 調整記憶體預算或逾時要在拉取請求裡寫出實測數字，不能只是為了讓紅燈變綠。
- main 的分支保護（倉庫設定，不在工作流程裡）：只能經拉取請求合併、必要檢查 `CI 全部通過` 且分支須是最新、要求對話解決、線性歷史、不允許強制推送與刪除。倉庫只允許 squash 合併，合併後自動刪除來源分支。

## 自動合併與發行

- `auto-merge.yml`：擁有者從本倉庫分支開的拉取請求，一開啟就排定自動合併；分支保護會等到 `CI 全部通過` 綠燈才真的合併。fork 與機器人的拉取請求不會自動合併。
- `cd.yml`：`src/version.hpp` 的版本號沒有對應的 `v<版本>` 標籤時，在 `windows-latest` 建置桌面版與 CLI、跑 `build.bat test`，用 `tools/make_release.py` 核對版本資源並產生發行資料夾，附上 SHA-256 與建置來源證明（`gh attestation verify <檔案> -R developer/ChatClaudeAgent` 可核對）後建立 GitHub Release。觸發來源是版本檔變更的 push、每天 04:23 UTC 的排程與手動執行；排程是因為 `GITHUB_TOKEN` 完成的自動合併不會再觸發 push 事件。已存在的標籤不重發。
- `branch-cleanup.yml`：倉庫設定只在人手合併時刪除來源分支；`GITHUB_TOKEN` 完成的自動合併不會刪，所以這個工作流程每小時（以及手動、人手合併時）刪除已合併拉取請求留下的本倉庫分支；只刪最新提交仍是合併當時那一個的分支，不碰 main，也保留仍有開啟中拉取請求的分支。
- `dependency-review.yml` 需要倉庫開啟相依圖（Settings → Code security 的 Dependabot alerts 會一併開啟），否則 API 回 403。
