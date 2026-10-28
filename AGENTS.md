# ChatClaudeAgent（CCA）開發指南

這份檔案寫給所有替 CCA 寫程式的 AI：Claude Code 等工具直接讀它，Codex 直接讀它，CCA 會把「審查重點」那一段交給本地審查員。人也可以照著做。改了專案的做法，就要回來更新這份檔案。

## 這是什麼

CCA 讓 Claude（Claude Code CLI）和 GPT（Codex app-server）用使用者自己的訂閱一起寫程式。有兩種外殼，共用同一個核心：

- 桌面版 `ChatClaudeAgent.exe`：Win32 視窗加 WebView2，介面在 `ui/`，編進 exe 的資源。
- CLI 版 `ChatClaudeAgent-CLI.exe`：在終端機裡畫同一套對話，不載入 WebView2，主打低記憶體。

群組的流程：Claude 撰寫 → GPT 審查並直接修改 → Claude 複審 → 來回到雙方都沒有意見。本地模型（Ollama）最多兩位，只做唯讀審查。可配置的多代理流程另支援 Gemini CLI、API 與本地節點，依節點 ID 保留交接及執行狀態。Gemini 與其他原生代理共用個人資料及外部性格資料，有獨立私訊，也能在一般群組擔任第三位審查者；沒有模型目錄，一律用 CLI 預設模型。成員額度不足時跳過，全部成員都沒有額度時強制停止並記錄下一步。

## 檔案地圖

| 位置 | 內容 |
| --- | --- |
| `src/app.hpp`、`app.cpp` | `App`：對話、訊息、存檔、和介面之間的 JSON 訊息。只能在 UI 執行緒呼叫 |
| `src/claude_agent.*`、`codex_agent.*` | 兩個 CLI 的協定轉接，轉成 `agent.hpp` 的 `AgentEvent` |
| `src/gemini_agent.*`、`gemini_auth.cpp`、`tools/gemini_account.cjs` | Gemini CLI 的 Google 帳號登入、停用帳號（`retired`）判斷、官方方案／額度、私訊、群組第三位審查者與多代理審查；可修改檔案，無介面權限不足時拒絕工具 |
| `src/group.cpp`、`local.cpp`、`takeover.cpp` | 群組輪替（主刀 → 搭檔 → Gemini）、本地審查員、額度用完時的接手、跳過（`groupSkip`）與全部用完的強制停止（`groupQuotaStop`） |
| `src/guard.*` | 規範審查：每則回覆的語言、角色稱呼、VERDICT、取證檢查，規則卡與退回訊息 |
| `src/local_tools.*` | 本地審查員的唯讀工具（只限專案資料夾）與引用比對 |
| `src/workflow.*` | 協作流程圖的狀態（桌面和 CLI 共用） |
| `src/regression_guard.cpp` | 審查者測試守門：審查回合改了檔案後跑使用者設定的測試指令，轉紅且審查前是綠的就還原，結果寫進聊天室與任務紀錄（設計見 `docs/reviewer-guard.md`） |
| `src/editions.*`、`src/version.hpp` | 普通版本與特殊版本：`CCA_EDITION`、`edition.json` 標記，以及特殊版本不支援設定的保存與還原（`editions\ordinary-vault.json`）。設計見 `docs/editions.md` |
| `src/haiku_sim.*`、`haiku_window.*` | Haiku 手機模擬：桌面版開手機大小視窗，載入未修改的 Haiku APK，並扮演它的 Android 殼層（專案、還原點、金鑰、模型請求、預覽）。見 `docs/phone-simulator.md` |
| `src/extensions.*`、`ui/extensions.js`、`examples/plugins/` | CCA 插件：只含資料的 `plugin.json`（主題、對話模式、指令），雜湊核准與驗證。格式見 `docs/plugins.md` |
| `src/task_ledger.*`、`pipeline.*` | 持久任務、交接索引、順序多代理、實際 Git 差異與審查閘門；沒有 pipeline 的群組沿用既有流程 |
| `src/collaboration.*`、`opinion.cpp`、`workspaces.*` | 版本化計畫、跨對話收件匣、審查關注點、獨立唯讀意見與 Git worktree 合併；介面在 `ui/collaboration.js` |
| `src/quality.cpp`、`ui/quality.js`、`tools/quality_worker.py`、`browser_worker.cjs` | 主機取得的需求驗證、修正前後重現與獨立瀏覽器預覽 |
| `src/runtime.*`、`platform.cpp`、`tools/agent_runtime.py`、`ui/platform.js` | API／Ollama 工作程序、DPAPI 金鑰、MCP stdio、Docker 隔離、圖片產物與管理介面 |
| `src/sandbox_mcp.*`、`tools/sandbox_mcp.py` | Claude 與 GPT 共用的 Docker 沙箱（MCP 伺服器）：啟動參數、Codex／Claude 的設定格式、角色說明 |
| `src/claude_usage.*` | 可選的 Claude 額度輪詢；正式來源限 Anthropic HTTPS，測試使用隔離登入檔與本機 HTTP 替身 |
| `src/profile.cpp`、`personas.cpp`、`humanize.cpp` | 名稱、本機性格預設檔、自訂風格與去 AI 化規則；角色內容不編入程式 |
| `src/activity.*`、`ui/activity.js` | 桌面與 CLI 共用的日曆統計，以及近一年 Token 活動熱力圖 |
| `src/badges.*`、`ui/badges.js`、`ui/badge_icons.js` | 本機個人徽章、六格展示與目錄；授予檔獨立於名稱，徽章不授予權限 |
| `src/gitflow.*`、`gitpanel.cpp`、`ui/git.js` | 版本控制面板：狀態解析、顏色分類、永遠不提交規則、提交前金鑰掃描、選檔提交、推送、快轉拉取、提交紀錄圖與自動同步 |
| `src/desk.cpp`、`ui/desk.js` | 原子保存的待送指令、可觀測的上下文來源與桌面版面設定 |
| `src/app_update.cpp`、`updater.*`、`tools/make_release.py` | 本機 `installers` 資料夾（自動建立）的發行清單或單一 EXE、遠端來源（未啟用）、自動更新排程、版本後綴（重大規範化）與獨立更新程式；不能對使用中的 CCA 測試替換 |
| `src/task_analysis.cpp`、`routing.*`、`adaptive.cpp`、`ui/appearance.js` | 多因素任務輪廓、後端目錄限定的模型／思考程度自動選擇、手動固定與無模型呼叫的預覽 |
| `config/themes.json`、`tools/make_themes.py`、`src/themes.*`、`ui/themes.css` | 桌面、CLI 及更新視窗共用的受信任主題目錄與原子保存；各主題取自來源的裝置集中在 `ui/themes.css` |
| `src/navigation.cpp`、`ui/app.js` 的導覽段落 | 固定的群組圖標列、群組專屬側欄與主機確認的排序；私訊與群組分開 |
| `src/model_dates.*`、`ui/profile_media.js` | 有來源的模型發布日、資料卡、頭像裁切；未知模型不猜日期 |
| `src/rollback.cpp`、`snapshot.*` | 檢查點與回滾（每個對話一個影子 git 倉庫） |
| `src/review.*` | 審查窗格：從檢查點到現在的差異（逐檔、逐段），還原單一檔案或單一段落，還原前先存檢查點 |
| `src/main.cpp` | 桌面版的視窗和 WebView2 |
| `src/cli_main.cpp`、`cli_render.*`、`cli_flow.cpp`、`term.*` | CLI 版：主程式、排版、流程圖、主控台輸入輸出 |
| `src/util.*`、`json.*`、`process.*`、`diff.*` | 共用工具；JSON 用專案自己的 `Json`，不加第三方函式庫 |
| `ui/app.js`、`app.css`、`md.js`、`icons.js`、`workflow.js` | 桌面介面，原生 JS，沒有框架也沒有打包步驟 |
| `ui/mock.js` | 瀏覽器開發用的假主機，不會編進 exe |
| `tests/` | 單元測試、協定測試、ConPTY 與 DOM 測試、假的 Claude CLI |
| `tools/` | 資源產生、圖標產生、風格檢查、介面開發伺服器 |
| `docs/` | 設計紀錄與驗證紀錄 |
| `third_party/webview2/` | WebView2 SDK，不要修改。公開倉庫不含它的二進位檔，`build.bat` 缺檔時經 `tools/fetch_webview2.py` 依 SHA-256 下載固定版本 |
| `.github/`、`tools/check_public.py`、`check_workflows.py`、`check_links.py`、`run_native_tests.py`、`tests/run_browser_tests.py`、`tests/extreme_env.py`、`tests/memory_test.cpp` | 公開倉庫的 CI 與其本機對應版本。貢獻方式見 [CONTRIBUTING.md](CONTRIBUTING.md)。設計見 `docs/ci.md` |

## 建置與測試

需要 Visual Studio Build Tools（C++，含 Clang 格式工具）、Node.js、Python 3。全部在 PowerShell 或 cmd 執行：

```powershell
.\build.bat                 # 桌面版 → build\ChatClaudeAgent.exe
.\build.bat cli             # CLI 版 → build\ChatClaudeAgent-CLI.exe
.\build.bat test            # 單元測試
.\build.bat takeover-test   # 接手與子程序協定測試（模擬後端，不花額度）
.\build.bat desk-test       # 佇列、來源紀錄、版面及更新版本規則（隔離資料）
.\build.bat haiku-sim-test     # Haiku 手機模擬的原生部分：解壓縮、APK、專案、DPAPI、橋接（不連外）
.\build.bat haiku-window-test  # 真的開出模擬視窗並驅動橋接，存截圖（需要 WebView2 與已建置的 APK）
.\build.bat edition-test    # 版本路線選項（真實執行檔）、保存與還原、CCA 插件
.\build.bat appearance-test # 主題、模型路由、保存失敗及真實模擬 CLI 參數
.\build.bat gemini-test     # Gemini 真實子程序模擬協定，不使用模型額度
.\build.bat git-test        # 真實 Git 裸遠端：選檔提交、規則鎖定、金鑰攔截、推送、自動拉取、本機 HTTP 遠端登入與額度自動接續
node tests\git_browser.cjs   # 版本控制面板與自動接續設定；需 Playwright、Edge 與 UI 伺服器
node tests\gemini_account.cjs # 隔離的官方 SDK 替身、方案與額度欄位
node tests\gemini_browser.cjs # Gemini 帳號頁與第二位可修改檔案審查者；需 Playwright、Edge、5220 埠 UI 伺服器
python tools\make_themes.py --check # 目錄變更後先移除 --check 重新產生
node tests\markdown_links.cjs # 本機路徑、行號、Markdown 跳脫與協定白名單
node tests\message_bounds_browser.cjs # 真正 Edge 檢查長指令與本機連結，需 5220 埠 UI 伺服器
node tests\guard_browser.cjs # 測試守門設定、指令對話框與還原訊息；需 Playwright、Edge 及 5220 埠 UI 伺服器
node tests\mcp_pin_browser.cjs # MCP 工具釘住對話框；需 Playwright、Edge 及 5220 埠 UI 伺服器
node tests\editions_browser.cjs # CCA 插件頁、更新頁的版本選項；需 Playwright、Edge 及 5220 埠 UI 伺服器
node tests\appearance_browser.cjs # 十六款主題、各自的裝置、鍵盤、對比與自動選擇；需 5220 埠 UI 伺服器
python tests\test_update_broker.py # 暫存 EXE 真正替換、啟動及失敗復原
node tests\desk_browser.cjs # 工作台與版面；需 5219 埠 UI 伺服器
.\build.bat profile-test    # 頭像交易、圖片驗證、日期與保存測試（自建隔離資料目錄）
.\build.bat pipeline-test   # 本機 HTTP、真實子程序協定、Git 與 DPAPI；不使用真實模型額度
.\build.bat quality-test    # 原生證據來源、來源失效、驗收保護與真實工作程序 IPC
python -m unittest discover -s tests -p test_quality_worker.py
node tests\browser_quality.cjs # 真實 Edge 操作與斷言；無模型用量
python tests\test_agent_runtime.py # 工作程序的路徑、金鑰、MCP、權限、輸出上限與沙箱阻擋
python tests\test_sandbox_mcp.py   # 共用沙箱 MCP 伺服器；有 Docker 與 busybox:1.36 時多跑一次真實容器
.\build.bat sandbox-live    # 只建置 Claude 沙盒驗證程式，不呼叫模型
python tests\test_sandbox_live.py # 模擬 CLI 檢查驗證程式的成功／失敗判定，不呼叫模型或容器
.\build.bat pty             # ConPTY 測試驅動程式，給 tests\cli_smoke.py 用
python tests\cli_smoke.py   # 真實 ConPTY 裡的 CLI 情境
node tests\workflow_ui.cjs  # 流程圖 DOM 測試（需要的開發套件見 docs\workflow.md）
node tests\desktop_smoke.cjs
node tests\activity_browser.cjs # Token 活動與中性性格庫；需 Playwright、Edge 及 5208 埠 UI 伺服器
node tests\badges_browser.cjs # 個人徽章；需 Playwright、Edge 及 5216 埠 UI 伺服器（可用 CCA_UI_URL 指定）
node tests\group_navigation_browser.cjs # 群組導覽、拖曳與排序失敗；需 Playwright、Edge 及 5218 埠 UI 伺服器
node tests\profile_media.cjs # 裁切幾何與模型日期；瀏覽器測試見 docs\profiles.md
node tests\pipeline_models_browser.cjs # 模型下拉選單、API 自動載入與失敗重試；需要 Playwright 及介面開發伺服器
python tests\test_check_style.py             # 風格工具的回歸測試，使用暫存檔與隔離 Git 倉庫
.\build.bat memory-test     # 吃外部輸入的程式（JSON、diff、ZIP、專案檔）的洩漏量；先設 $env:CCA_SANITIZE=1 再跑就是 AddressSanitizer 版
python tools\run_native_tests.py             # 全部原生測試目標，失敗也繼續跑完並逐項回報
python tests\run_browser_tests.py            # 全部 DOM 與 Edge 測試，自己啟動 UI 伺服器；需 Playwright 與 LinkeDOM（版本見 docs\ci.md）
python tests\extreme_env.py                  # 極端環境：記憶體上限、單核心、剝離環境、怪路徑、損毀資料、唯讀資料夾、第二個實例
python tools\check_public.py                 # 倉庫衛生：金鑰、個人路徑、二進位檔、內部紀錄
python tools\check_workflows.py              # 工作流程：SHA 釘版、權限、逾時
python tools\check_links.py                  # Markdown 相對連結
python tools\check_style.py --format --js   # 交出前必跑，見「程式碼風格」
```

注意：

- 你多半就是在 CCA 裡面執行的。執行中的 `ChatClaudeAgent.exe` 會鎖住 exe，**不要關掉或結束 CCA 的程序**。要建置新版就設 `CCA_DESKTOP_OUT` 或 `CCA_CLI_OUT` 換一個輸出檔名，例如 `$env:CCA_CLI_OUT = "build\ChatClaudeAgent-CLI-review.exe"`。
- 測試一律用 `CCA_DATA_DIR` 指到暫存資料夾，不要碰使用者真正的 `%APPDATA%\CCA`。要 Claude 回覆時用 `tests\fake_claude.cmd`（在測試資料夾的 `settings.json` 設 `claudePath`）。
- 會用到使用者訂閱額度的測試（`build.bat live`）要先問過使用者。額度不足、跳過與強制停止的測試用 `tests/takeover_test.cpp` 的替身（`CCA_TEST_CODEX_QUOTA`、`CCA_TEST_GEMINI_QUOTA`），不消耗額度。
- `build\sandbox_live.exe <claude.exe> --connect-only` 只測原生工作階段及 MCP 連線，不發送提示詞；`--run` 會使用 Claude 訂閱額度，必須先取得使用者授權。測試使用獨立的暫存專案與 `CCA_DATA_DIR`。
- 公開倉庫的 CI（[ci.yml](.github/workflows/ci.yml)）把上面的檢查分成衛生、建置與風格、原生測試、模擬測試、介面測試、記憶體測試、極端環境七組，加上 CodeQL；`CI 全部通過` 彙整全部。紅燈要找原因，不能刪測試或放寬檢查讓它過。工作流程的外部 Action 一律釘完整 SHA、頂層權限唯讀、每個工作有逾時。
- 不能用某項驗證時（例如沒有瀏覽器、沒有真實訂閱），回報時直接寫「沒有驗證」，不要寫成通過。
- 明確指定 `--format` 或 `--js` 卻缺少工具時，風格檢查會失敗；`--format` 也需要 Git 與至少一個提交作為基準。先補齊環境再重跑，不能把未執行的檢查當成通過。

## 架構規則

- `App` 只在 UI 執行緒使用。背景執行緒（子程序讀取、Ollama、下載）要用 `postToUi(...)` 把工作丟回 UI 執行緒。
- 介面和核心之間只走 JSON 訊息：介面送 `{"type": ...}` 給 `App::onUiMessage`，核心用 `App::send` 回傳。新增一種訊息時要一起改：
  1. `src/app.cpp` 的分派（或對應的 `*.cpp`）
  2. `ui/app.js` 的處理
  3. `ui/mock.js`，讓瀏覽器開發時也能用
  4. CLI 需要的話改 `src/cli_main.cpp`
- 審查窗格的 `review.open`、`review.revert` 與回應須保留 `requestToken`；還原須附 `head` 並檢查檔案是否在審查後改變。逐行草稿在主機確認收到使用者訊息前不可清除；localStorage 不可用時須保留記憶體草稿並提示。
- 桌面版和 CLI 版功能要對齊。只做其中一邊時，要在回覆裡說明另一邊為什麼不需要。CLI 不能依賴 WebView2，也不要在 CLI 保留整段畫面或完整歷史，完成的訊息寫進終端機捲動紀錄後就放掉。
- 設定都在 `settings.json`（`settings_`），預設值在 `App::ensureProfileDefaults` 補上，舊檔案缺欄位也要能開。
- 模型與思考程度只能從後端回報目錄選擇；沒有能力資料時使用後端預設，不能猜 ID 或支援值。保留舊對話手動設定，模型／程度可獨立固定。多代理節點固定值、日常模式、獨立意見及 API／本地模型不被自動覆蓋。規則見 `docs/adaptive-routing.md`。
- Gemini 帳號查詢須同時支援分離 core 與新版 bundled ESM SDK，只從所選安裝的入口圖載入，拒絕其他專案或上層套件。不得執行 CLI 主入口來猜登入，也不得為檢查帳號發送模型提示詞。
- Gemini 使用官方 npm CLI 的 Google OAuth。免費層與 Google One 個人帳號已於 2026-06-18 停止支援 Gemini CLI；帳號檢查須用官方 CLI 的設定請求辨識 `UNSUPPORTED_CLIENT` 並顯示 `retired`，不能只看健康檢查就宣稱可用，額度查詢須綁定設定請求回傳的專案。移除 API 計費環境並強制 `oauth-personal`。方案只顯示 Google 回報名稱，額度缺值顯示未知，不用 Token 推算。沒有官方模型目錄時只允許預設模型。無介面計畫模式可能自動放寬權限，CCA 的 `plan` 不啟動 Gemini；`ask`／`edit` 不自動升成 YOLO。Gemini 工具輸出不含可驗證的 shell 結束碼，不能據此通過必要測試閘門。
- 主題只接受目錄 ID，不接受任意 CSS。`config/themes.json` 與原生 token 經產生器共用至桌面、CLI 和更新視窗；預覽不保存，失敗須恢復已保存主題。變更目錄或原生 token 後執行 `tools/make_themes.py`。主題裝置只讀產生的 token（含 `--theme-a1`～`--theme-a6` 與字型堆疊），共用規則以 `:where()` 降權；只用 Windows 內建字型。
- 頭像替換或移除，必須確認設定檔保存成功後才刪除舊圖。個人資料用 `persistProfile`、群組頭像用 `persistGroupAvatar`；保存失敗須保留舊參照與裁切內容。
- 徽章持有資料只讀 `%APPDATA%\CCA\badges.json`，沒有此檔也一定有成員徽章。不得依顯示名稱授予開發者徽章；`badges.display` 只能調整已持有徽章的排序與隱藏，保存成功後才確認。徽章不是權限或線上身分認證。
- 群組圖標順序由主機 `groupOrder()` 正規化，`groups.reorder` 必須包含當下所有且只含群組 ID，原子保存成功後才更新。訊息活動不能重排圖標；拖曳期間保留原生 DOM。刪除最後群組不自動建立替代群組，私訊保持可用。
- 對話資料在 `%APPDATA%\CCA`（`util::appDataDir()`）。桌面版和 CLI 用 `util::lockDataDir()` 互斥，不能同時寫。
- 專案資料夾必須通過 `util::projectFolderProblem`：不能是磁碟根目錄、使用者資料夾本身或系統資料夾。Codex 的 Windows 沙箱會改專案資料夾的 ACL，曾經因為把整個使用者資料夾當工作區而出事；還原方式見 docs/purpose.md 的資料夾界線。
- 子程序一律經 `process.cpp` 建立（放進 Job 物件，CCA 結束時整棵程序樹一起結束），用 `CREATE_NO_WINDOW`，不和 CLI 共用主控台。唯一例外是 `launchUpdateBroker`：經核對的更新程式必須在主程式關閉後繼續運作。宿主 Job 不允許分離時取消更新，不假裝成功。
- 待送指令保存成功後才清空草稿；重啟預設暫停，dispatching 項目改為待確認中斷，不能自動重試。不得用佇列跳過暫停、計畫核准、權限或工作區檢查。損壞的佇列檔保留原樣。
- 上下文來源只能陳述主機可觀測的交付資料；外部 CLI 內部歷史、截斷與檔案內容未知時明示未知。後端工具回報不是主機驗收證據。
- Git 指令一律在背景執行並關閉帳密與編輯器提示（`GIT_TERMINAL_PROMPT=0`、`GCM_INTERACTIVE=never`）。唯一例外是使用者在登入頁按下登入的 `git.login`：允許 Git 認證管理員開自己的視窗或瀏覽器，最多等 5 分鐘。CCA 不提供密碼欄位，也不讀取或保存帳密；`credential.helper` 只在使用者從登入頁要求時設定。錯誤文字裡網址的帳密要先移除才能送到畫面。提交、推送、拉取在 AI 工作時拒絕；主機須依當下狀態重新檢查選取路徑，鎖定與未確認的紅色檔案不能提交，提交只帶選取的路徑。拉取只用 `--ff-only`。金鑰掃描只回報檔案、行號與類型，不重複金鑰內容。
- 額度自動接續預設關閉，只在 CCA 開著時依記錄的 `quota_stop.resumeAt` 執行，次數有上限；不能為了接續而清除使用者的停止或手動選擇。
- 更新檔先核對內容及產品／版本資源，保存資料並取得更新程式就緒確認後才關閉。測試只替換隔離副本；不得對使用者目前的 EXE 試裝。詳見 `docs/workbench.md`。
- 不用 C++ 例外；錯誤用回傳值，使用者看得到的錯誤用 `toast` 或系統訊息。
- 審查者測試守門只採用主機量到的結束碼：指令由使用者在設定輸入（單行、500 字、無控制字元），模型不能改；只有「審查後紅、審查前綠」才還原，指令無法啟動或逾時一律不還原並標示沒有驗證；還原前先保存審查者的結果檢查點。測試用假後端搭真實 git，見 `tests/takeover_test.cpp`。
- MCP 工具釘住：API 代理每次 `mcp_discover`／`mcp_call` 前重新取得工具清單，描述雜湊與核准時不同或沒釘住就阻擋；釘住只能經 `toolserver.pin`，主機以新清單核對使用者看到的雜湊，伺服器指令或參數變更即作廢所有釘住。釘住不證明工具行為安全。
- 特殊版本（Haiku、Sonnet、Opus、Fable）是同一套原始碼以 `CCA_EDITION` 建置的另一條產品路線，版本號自成一列。更新只自動安裝同一路線的新版；切換路線一律由使用者在更新頁或 `/update switch`、`/update return` 確認。特殊版本第一次啟動時先把不支援的設定寫進 vault，寫入成功才停用它們；普通版本（1.0.1-astra 以後）啟動時還原並封存 vault。新增「特殊版本不支援」的設定時，要同時加進 `src/editions.cpp` 的清單。
- Haiku 手機模擬不改 Haiku：只讀安裝資料夾裡、與發行清單核對過的 APK，編輯頁原封不動，橋接由 CCA 在載入前注入；預覽一律在沒有橋接的 WebView。視窗與橋接要和 Android 版同樣守規則（路徑只在專案內、金鑰不進頁面、模型請求只經固定的 HTTPS 端點）。為了讓它能在電腦上開啟而修改 `android/haiku` 是不允許的；差異（Windows 檔名、DPAPI）寫在模擬端。
- CCA 插件只有資料：主題只能用既有形狀與內建主題的裝置、顏色須為 `#rrggbb` 且文字對比至少 4.5:1；模式只能設定白名單欄位，不得設定 bypass 或測試指令；指令不得蓋過內建指令。啟用時記下 `plugin.json` 的 SHA-256，檔案改動即停用。特殊版本不載入插件。
- 多代理交接必須先保存檢查點與任務，再啟動下一位。不能只依模型說法判定修改或測試成功；審查結論須位於結尾，沒有主機取得的測試結束碼就不能通過必要測試閘門。
- API Key 只走工作程序 stdin IPC 與 DPAPI 檔案，不進設定、提示詞或聊天。工具只開放專案內的相對路徑，拒絕連結、憑證及環境檔；本地代理沒有寫入、執行或 MCP 呼叫工具。
- 驗收證據只能由主機工作程序的實際結果建立；須綁定來源與計畫版本。重現必須用相同案例取得舊版指定失敗、新版成功。一般測試不是作業系統沙箱；外部網站也不能只靠本機指紋聲稱已部署相同版本。
- 沙箱使用現有 Docker 的來源副本，網路關閉、不掛宿主工作區，不自動拉取映像。缺少隔離後端時阻擋；不得退回宿主機執行。MCP 是使用者另行核准的宿主工具，不等於沙箱。
- 回覆中的本機檔案連結只經 `file.reveal` 開啟所在資料夾，不直接執行 EXE、腳本或文件。只接受存在的本機磁碟絕對路徑，拒絕 UNC、控制碼及資料流路徑。CLI 以文字顯示路徑，不建立桌面點擊入口。
- 模型輸出一律當成不可信的文字：桌面版經 `MD.render` 跳脫後才能放進 `innerHTML`；CLI 輸出工具文字前要濾掉控制碼。

## 程式碼風格

<!-- cca-review:start -->
**機械規則（`python tools/check_style.py` 會擋）**：UTF-8 不加 BOM（含中文的 `.ps1` 例外，要有 BOM）；LF 換行（`.bat`、`.cmd`、`.ps1` 用 CRLF）；不用 Tab；沒有行尾空白；檔案以換行結尾。`--fix` 可以修編碼標記、換行、行尾空白和行首 Tab；行內 Tab 會回報，但不會自動改掉，以免改動字串內容。

**C++（C++20，MSVC）**
- 4 格縮排，新寫或改到的行不超過 120 欄。格式以 `.clang-format` 為準，但**只格式化你改到的行**（`check_style.py --format` 只檢查有改動的行）。不要為了風格重排整個檔案，那會讓審查看不出真正的改動。
- 命名：型別 `PascalCase`；函式和變數 `camelCase`；類別成員加底線結尾 `settings_`；常數 `kMaxDiff`；命名空間小寫（`util`、`tui`、`term`）。
- `#include` 順序：對應的標頭 → `<windows.h>` 等系統標頭 → 標準函式庫 → 專案標頭，各組之間空一行。
- 檔案開頭寫一兩行英文註解說明這個檔案負責什麼。註解寫「為什麼」，不要逐行翻譯程式在做什麼。
- 建 JSON 用 `J({{"key", value}})`；讀欄位用 `.str()`、`.i64()`、`.boolean()`，缺欄位時有預設值。
- 檔案內部用的小工具新寫時放 `namespace { }`（少數舊程式用 `static`，改到時順手換掉即可）。

**JavaScript（ui/）**
- 2 格縮排，單引號，有分號。沿用 `h(tag, props, ...children)` 建 DOM、`put()` 換內容、`icon()` 畫圖示，不引入框架或 npm 套件。
- 介面狀態放在 `S`，畫面用 `renderXxx()` 重畫；每個從主機來的訊息在 `onMessage` 的 `switch` 處理。

**CSS**：沿用 `ui/app.css` 開頭的 CSS 變數（`--claude`、`--codex`、`--signal`、`--text-muted`…），不要新增字體或寫死顏色。外觀照 [docs/visual-identity.md](docs/visual-identity.md)：切角板而不是圓角、白色主要按鈕、冰藍訊號色只用在焦點與進度、每位隊員固定一個識別色；不要加漸層、玻璃擬態或發光陰影。群組的設定（主刀、模型、權限、資料夾、本地審查員、來回上限、流程圖）放在頻道設定面板 `opsPanel`，不要再加回標題列。

**Python**：工具只用標準函式庫（需要圖片處理的產生器不在這個倉庫）；4 格縮排。

**選用主題例外**：上述切角及石墨色規則適用於 CCA 原生主題。十五款選用主題集中寫在 `ui/themes.css`，可依各自來源使用圓角、紙色、圖樣，以及來源本身的漸層或光暈（Gemini 星芒、Sky 燭光、Tokyo Night 霓虹、萬聖節南瓜光暈），顏色只能從 token 讀取；不得在業務元件硬編配色。原生主題仍不用裝飾漸層或發光陰影。詳細規格及參考來源見 `docs/themes.md`。

**文字**：介面、說明、文件都用台灣繁體中文。去 AI 化規則也適用於 CCA 自己：不寫「值得注意的是」「不僅……更……」這類套話，不在標題放 emoji，不堆形容詞，寫具體的檔名、數字和步驟。公開介紹不以私人性格設定或第三方作品作為產品功能；自訂風格不能影響技術規範。

**審查時特別看**：UI 執行緒規則、新訊息類型四個地方有沒有一起改、桌面與 CLI 是否對齊、測試有沒有用 `CCA_DATA_DIR`、有沒有把模型輸出未經跳脫放進畫面、有沒有關掉使用者正在用的 CCA。
<!-- cca-review:end -->

## 協作規則（CCA 群組）

- 撰寫者先實作；審查者直接修改檔案；複審時確認對方的修改正確、沒有破壞原本功能、符合需求。沒有實質問題就給 `VERDICT: APPROVE`，不要為了改而改。
- 本地審查員的建議可能出錯。採納前要自己開檔案、搜尋或跑測試，回覆裡逐條說明查證了什麼，並提到審查員的名字。
- CCA 會用 `src/guard.cpp` 檢查每則回覆，不符合就退回重寫（預設最多兩次，`settings.json` 的 `guard`）：說明用台灣繁體中文、有角色時每則都用角色的稱呼且不自稱 AI、審查與複審最後一行寫 VERDICT、改了檔案不能寫 APPROVE、APPROVE 前要有讀檔／搜尋／指令紀錄、說測試通過的那一輪要真的執行過指令。
- 測試用的模擬後端回覆很簡短，會被規範審查退回；協定測試裡要關掉 `guard`（見 `tests/takeover_test.cpp`、`tests/cli_smoke.py`），規範審查本身另有測試。
- 使用者說「只提出、不修改」時，就真的不要改檔案。

## Git

- 發行版本順序以 `docs/workbench.md` 為準：`1.0.0- → 1.0.0-oss → 1.0.0-luna → 1.0.0-terra → 1.0.0-sol → 1.0.1- → 1.0.1-oss`（之後 luna、terra、sol、astra），特殊版本另見 `docs/editions.md`；數字版本第四欄是後綴序號（oss 為 1）；後綴版本稱為「重大規範化」，每一版都要有產品發表（`docs/promo.md`）。用 `python tools/make_release.py --next <目前版本>` 查下一版；同步更新 `src/version.hpp` 的文字與數字版本，交付檔名也用完整版本。預覽檔名不能代替版本遞增。交付給使用者的版本用 `make_release.py` 輸出到 `%APPDATA%\CCA\installers\<版本>`，使用者以 CCA 內建更新安裝；特殊版本加 `--edition`。

- 交出前跑 `python tools\check_style.py --format --js` 和相關測試。
- 協作控制改動另跑 `build.bat collaboration-test`、`build.bat takeover-test` 與 `tests/collaboration_browser.cjs`。後者需 Playwright、Edge 與本機 UI 伺服器；原生測試使用獨立資料與真實 Git 測試倉庫，不能拿使用者專案測合併。
- 計畫需先保存再核准、再執行，留言會更新版本並取消核准與驗收；模型完成僅為待驗收。意見模式使用獨立唯讀 session，不繼承原對話記憶，不允許升權。worktree 合併必須比對預覽的來源及目標提交，不能自動暫存、強制覆寫衝突或刪除 worktree。
- 提交訊息用 `類型: 主旨`（`feat:`、`fix:`、`docs:`、`test:`、`chore:`），主旨用英文，例如 `fix: keep model fields when saving the vault`，之後空一行用中文或英文說明原因。一個提交做一件事；不加共同作者（Co-Authored-By）行。
- `build/` 不進版本控制（`.gitignore`），`docs/` 裡指向 `build/` 的驗證連結只在本機有效。
- 只有使用者要求時才提交或推送；不要改寫已經存在的歷史。
- CCA 的回滾檢查點是另外的影子倉庫（`%LOCALAPPDATA%\CCA\snapshots`），和這個專案的 `.git` 互不影響。
