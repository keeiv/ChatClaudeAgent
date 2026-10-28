<p align="center">
  <img src="assets/logo/cca-app-256.png" alt="ChatClaudeAgent 產品圖標" width="160">
</p>

<h1 align="center">ChatClaudeAgent（CCA）</h1>

<p align="center">
  以工作交接、交叉審查與主機驗證為核心的 Windows 開發工作區。<br>
  桌面版與原生 C++ 終端機版，共用同一個核心。
</p>

<p align="center">
  <a href="LICENSE">GPL-3.0</a> ·
  <a href="docs/editions.md">版本路線</a> ·
  <a href="docs/changelog.md">更新紀錄</a> ·
  <a href="AGENTS.md">開發指南</a>
</p>

## 版本路線

普通版本是這個倉庫。另有定位不同的特殊版本，各自一個倉庫，版本號各自從 0.0.1 起算（細節見 [版本路線](docs/editions.md)）：

| 版本 | 圖標 | 狀態 | 授權 | 倉庫 |
| --- | --- | --- | --- | --- |
| 普通版本 CCA | <img src="assets/logo/cca-app-64.png" width="40" alt="CCA"> | 1.0.1-astra | 開源（GPL-3.0） | 這個倉庫 |
| 俳句 Haiku | <img src="assets/editions/haiku/icon-64.png" width="40" alt="Haiku"> | 0.0.2，Android | 開源（GPL-3.0） | [ChatClaudeAgent-Special](https://github.com/keeiv/ChatClaudeAgent-Special/tree/haiku) 的 `haiku` 分支 |
| 十四行詩 Sonnet | <img src="assets/editions/sonnet/icon-64.png" width="40" alt="Sonnet"> | 開發與規劃中 | 開源（GPL-3.0） | [ChatClaudeAgent-Special](https://github.com/keeiv/ChatClaudeAgent-Special/tree/sonnet) 的 `sonnet` 分支 |
| 大師巨作 Opus | <img src="assets/editions/opus/icon-64.png" width="40" alt="Opus"> | 開發與規劃中 | 開源（GPL-3.0） | [ChatClaudeAgent-Special](https://github.com/keeiv/ChatClaudeAgent-Special/tree/opus) 的 `opus` 分支 |
| 寓言 Fable | <img src="assets/editions/fable/icon-64.png" width="40" alt="Fable"> | 0.0.3 | 閉源，免費提供 | 不公開 |

## 概覽

CCA（ChatClaudeAgent）是以工作交接、交叉審查與主機驗證為核心的 Windows 開發工作區，提供桌面介面與原生 C++ CLI。

CCA 起於作者使用 OpenAI 為 Claude Code 開發的 Codex 插件時，對雙模型協作流程仍有未被滿足的需求。長任務需要反覆整理進度；修改、審查、權限與驗收分散；切換代理後，容易遺漏已做的工作、尚未完成的要求與審查依據。

開發目的，是讓多階段的開發工作能持續交接、獨立審查、追查來源，並由使用者掌握流程：

- **交接有依據**：帶入實際檔案變更、指令狀態、需求修正與待辦，保留上下文截斷的提示。
- **審查可核對**：不同模型交叉檢查；主機記錄測試結果、來源指紋與計畫版本，讓完成聲明有可查的依據。
- **操作有界線**：權限、計畫核准、變更檢視、隔離工作區與回滾由使用者選擇，不能由模型的說法代替授權。
- **模型與表達可替換**：沿用已有的 CLI 登入、本機專案與另行設定的 API；性格是可選的本機資料，不綁定角色，也不改變技術規範。

不同模型達成一致或測試通過，仍不能保證不存在錯誤。CCA 保存可檢查的過程與結果，具體目標、已完成能力及界線見 [開發目的](docs/purpose.md)。

1.0.0 統一 Windows EXE、桌面介面與 CLI 的圖標。新標誌以兩個相向銜接的 C 為輪廓，沿用石墨灰、單色與切角；圖標內含 15 種原生尺寸。[設計與參考](docs/brand-1.0.md)。同版補上近一年 Token 活動熱力圖、CLI 用量查詢，以及不編入角色內容的本機性格資料檔。

0.9.0 新增需求對應的主機驗證證據、相同案例的修正前後重現，以及可操作的網站預覽與元素留言。[操作與限制](docs/quality.md)。

0.8.0 補上工作計畫的編輯、版本留言、核准與逐項驗收；跨對話收件匣；可編輯 Markdown 審查關注點；2 至 4 位模型的獨立唯讀意見與共識提案；以及按任務建立 Git worktree、預演衝突與確認合併。桌面版入口在側欄「任務控制」及頻道設定；CLI 使用相同核心。[操作與限制](docs/collaboration.md) · [驗證紀錄](docs/visual-identity-validation.md)。

0.6.0 新增可選用的多代理流程：群組側欄按「多代理流程 › 設定」，可排序 Claude Code、Codex、API 及唯讀本地模型；預設最多三次完整循環。既有群組保持原本流程，私訊與插件沿用。操作、API 計費、共享記憶、MCP 權限、截圖與隔離環境限制見 [多代理平台](docs/platform.md)。

0.6.1 補上共用沙盒的結束碼、輸出上限與暫存資料清理；Claude 實際呼叫及容器隔離結果見 [沙盒驗證](docs/sandbox-validation.md)。

0.6.2 的多代理流程改用模型下拉選單；API 依所選供應商自動載入及填入模型，失敗時可重試，不需要輸入模型 ID。

## 功能

- **私人訊息**：分別和 Claude、GPT 一對一對話。
- **協作群組**：可選 Claude 或 GPT 主刀，另一位負責審查與修改，再由主刀複審。到達輪數上限（預設 6 輪）會先停下來問你。
- **群組導覽**：每個協作群組在最左側有獨立圖標，選取後顯示群組的對話與工具入口。私訊保留在首頁；群組可拖曳、用選單或鍵盤調整固定順序，CLI 使用 `/groups`。[操作說明](docs/group-navigation.md)。
- **用你自己的訂閱**：透過官方 Claude Code 和 Codex CLI 執行；登入可用的 Claude 訂閱或 ChatGPT 訂閱即可使用對應模型。GPT 主導時，Claude 無法參與也能獨立完成。
- **額度接手**：預設啟用 Claude 額度耗盡時由 GPT 接手。等 Claude 該輪停止後，附上原始需求、使用者修正、部分回覆、指令狀態、檔案變更與待辦，再繼續工作。一般網路錯誤與用量警告不會觸發。GPT 自查完成會明確標示 Claude 沒有參與審查。
- **插件商店**：替 Claude 或 GPT 安裝插件，每個插件都標明是給哪個 AI 的；MCP 伺服器可以一次裝給兩個。
- **自訂工作風格**：可自行設定代理的名稱與回覆方式；技術品質、權限與協作格式優先於個人設定。
- **個人資料**：桌面版可自訂群組、自己和 AI 的頭像，先裁切再儲存；點成員可看模型發布日期與依性格顯示的自介。新頭像存成靜態 PNG，橫幅保留 GIF 支援；CLI 只設定名稱與角色性格。操作與日期來源見 [頭像與資料卡](docs/profiles.md)。
- **日常模式**：在私訊標題列打開，改用最省額度的模型（Claude 用 Haiku、GPT 用 luna 系列）和最低思考程度聊天，不需要專案資料夾，改檔案或執行指令前會先問你。
- **回滾**：在任一則訊息的選單按「回滾」，可以分別選「回滾工作」（把專案檔案還原到那則訊息之前）和「回滾聊天」（刪除之後的訊息，Claude 和 GPT 也會忘記那之後的內容）。還原前會先存一個檢查點，按「復原檔案」可以回來。
- **本地審查員（最多兩個）**：用 Ollama 在你的電腦上跑模型，邀請進群組後只做唯讀審查。它們用三個唯讀工具（讀檔、列資料夾、搜尋，只限專案資料夾）自己讀檔，每條建議都要附「檔案:行號」和逐字原文；CCA 拿原文去比對檔案，對不上的建議直接刪掉，只有對得上的才轉交 Claude 和 GPT，而他們採納前仍要自己取證。本地模型不能改檔案、不能執行指令，也沒有私訊。不支援工具呼叫的模型不能當審查員。在 設定 › 本地模型 會依你的顯示卡和記憶體推薦模型，可以直接下載、刪除、指定給兩個位置。
- **審查窗格**：標題列的審查按鈕或協作摘要的「審查這次的變更」，打開從檢查點到現在的差異，可切換「這一輪、這次任務、整個對話」。可以還原整個檔案或單一段落（還原前先存檢查點，聊天裡的系統訊息可以復原），也可以在任一行留言，最後一次交給主刀或私訊的 AI 處理。CLI 用 `/review`、`/revert 檔案`、`/undo-revert`。
- **互審時間軸**：群組側欄列出這次工作每一輪的隊員、結論與修改行數，點一下跳到那則訊息。
- **審查草稿與保護**：可篩選變更檔案，以方向鍵切換；逐行意見和未完成文字會保存在本機。送出後等主機確認收到才清除；檔案在審查後又被修改時，還原會先擋下，避免覆蓋較新的內容。CLI 的 `/revert` 有相同保護。介面規則與驗證見 [視覺識別](docs/visual-identity.md)。
- **頻道設定**：標題列只顯示一行摘要（主刀、權限、資料夾）；主刀、模型、額度用完時的接手、權限、資料夾、本地審查員、來回上限與流程圖都在頻道設定面板。
- **規範審查**：每則 AI 回覆結束時，CCA 用程式（不是提示詞）檢查：說明是否為台灣繁體中文、有沒有用角色的稱呼、有沒有自稱 AI 而出戲、審查是否寫了 VERDICT、APPROVE 之前有沒有真的讀檔或跑指令、說「測試通過」的那一輪有沒有真的執行過指令、本地審查員的建議有沒有逐條查證。不符合就附上違規清單退回同一個 AI 重寫，最多兩次；仍不符合時，群組會暫停讓你選「再退回一次」或「接受並繼續」，私訊會把回覆標成「未通過規範」。每次交給 AI 的訊息開頭也會附上一張規則卡。
- **Token 活動**：在「訂閱與用量」查看近一年的日曆熱力圖，可切換每日、截至當日的近 7 日或已保存紀錄累計；滑鼠、鍵盤及點選均可查看數值。保留最近 14 天的詳細長條與表格；API 及本地流程也納入。CLI 使用 `/activity daily|weekly|total`。[統計範圍](docs/activity.md)。
- **個人徽章**：七款身分與版本紀念圖形，所有安裝內建成員徽章。個人資料展示六枚，可從目錄查看完整收藏、自行排序與隱藏；CLI 使用 `/badges`。本機授予不提供工具權限或線上認證。[操作與資料範圍](docs/badges.md)。
- **多個群組**：每個群組可以指定不同的專案資料夾。
- **思考程度**：每個對話可以選模型加思考程度，例如「Opus 5.5 Ultracode」「GPT-6.1-Sol Max」。Claude 有 Low／Medium／High／XHigh／Max／Ultracode（動態工作流程，會分派子代理），GPT 有 Low 到 Max 和 Ultra。

## 需要先安裝

| | 安裝方式 | 登入 |
|---|---|---|
| Claude Code | PowerShell：`irm https://claude.ai/install.ps1 \| iex` | `claude auth login`（或在 CCA 裡按「登入 Claude」） |
| Codex CLI | 先裝 Node.js，再 `npm install -g @openai/codex` | `codex login`（或在 CCA 裡按「登入 ChatGPT」） |

桌面版使用 Microsoft Edge WebView2 顯示介面。CLI 不需要 WebView2，建議在 Windows Terminal 執行；字體沿用終端機設定，中文使用終端機的字體回退。

## 執行

```bash
build\ChatClaudeAgent.exe
```

第一次開啟會檢查兩個訂閱。之後選一個專案資料夾就可以開始。

### 終端機版本

執行 `build.bat cli` 建置，再用 `build\ChatClaudeAgent-CLI.exe` 啟動。也可以指定 `claude`、`gpt`、`group` 或群組名稱，例如 `build\ChatClaudeAgent-CLI.exe group`。與桌面版共用對話資料，同一份資料只能由一個程式開啟。

- `/help` 查看所有指令；`/chat` 切換對話，`/cd 路徑` 選專案。
- `/lead gpt`、`/lead claude` 選主刀；切換會帶入近期工作上下文。工作進行中使用 `/takeover`，先停止 Claude，再讓 GPT 接續。
- `/fallback on`、`/fallback off` 控制自動接手。額度耗盡後維持 GPT 主導；額度恢復後可用 `/lead claude` 明確重試。
- `/show` 查看上一則回覆的完整指令輸出與 diff，`/model`、`/effort` 設模型與思考程度，`/permission` 選權限。
- `/name` 改名稱，`/persona` 選用或建立性格，`/humanize on` 開啟去 AI 化規則。
- `/flow` 控制協作流程圖，`/motion off` 關閉動態。思考會轉圈，撰寫會呼吸亮起，送審時圓點沿線移動；已邀請的本地成員也顯示在圖上。桌面版有相同流程圖與收合控制，詳見 [協作流程與調查](docs/workflow.md)。
- 權限請求使用方向鍵或數字選擇，Enter 確認、Esc 拒絕；Shift+Enter 換行，Esc 中斷，連按兩次 Ctrl+C 離開。

CLI 只重畫輸入、選單與尚未完成的回覆，已輸出的內容交給終端機的捲動紀錄。`/status` 可看本體的工作集與私有記憶體；官方 AI 子程序、終端機和 Ollama 另外計算，長對話仍會增加核心保存紀錄所需的記憶體。完整操作、設計來源與測試方式見 [CLI 說明](docs/cli.md)。

圖標由不公開的產生器輸出，CCA 本身使用單色，代理各自的識別色保留在介面內。SVG、PNG、ICO 與終端機點陣版的用途見 [圖標說明](assets/logo/README.md)。資料目錄與 `CCA_` 環境變數保留原名，現有對話可繼續使用。

交接紀錄有長度上限，省略處會標明。模型必須重新查閱實際檔案和指令結果；上下文不足時應詢問使用者。接手沿用對話權限，不會自動提高權限。GPT 無法啟動時，群組會暫停並保留交接提示供重試。

`build.bat takeover-test` 以模擬的 Claude/Codex 子程序驗證交接，不使用真實模型額度。

## 權限模式

| 模式 | Claude | GPT（Codex） |
|---|---|---|
| 規劃模式 | `plan` | 唯讀沙箱 |
| 每次詢問 | `default`，修改和指令都先問 | 唯讀沙箱，需要時詢問 |
| 自動編輯（預設） | `acceptEdits` | 工作區可寫，需要時詢問 |
| 跳過所有確認 | `bypassPermissions` | 完全存取、不詢問 |

「跳過所有確認」要先在對話框勾選「我了解風險」才會開啟，開啟時輸入框上方會一直顯示紅色警告。

CCA 不允許把磁碟根目錄、使用者資料夾、文件／桌面／下載的根目錄或系統資料夾當成專案資料夾，因為 AI 會取得專案資料夾的寫入權限。

### GPT 在 Windows 上的沙箱

Codex 在「規劃／每次詢問／自動編輯」模式下會在 Windows 沙箱裡執行指令。如果 Codex 桌面版同時開著，它可能鎖住自己的檔案，讓沙箱啟動失敗（訊息是 `setup refresh had errors`）。CCA 會在對話裡提示。解法是關閉 Codex 桌面版再試，或改用「每次詢問」，讓 GPT 的指令經你核准後在沙箱外執行。

## 你會看到的資訊

- 每個檔案的新增／刪除行數（`src\cart.ts +9 −2`），點開看 diff；每輪結束有總計。
- 執行的指令、工作目錄、結束碼、耗時、輸出。
- 權限請求：哪個 AI、要做什麼、為什麼；可以允許一次、本次對話都允許、拒絕並說明原因。
- 待辦清單、「正在輸入／正在思考」、API 重試次數、錯誤。
- token 用量、上下文使用率、Claude 的等值 API 估算、5 小時和每週額度與重置時間。

這些項目是怎麼決定的，見 [docs/research.md](docs/research.md)。

## 本地模型

1. 安裝 [Ollama](https://ollama.com/download)。CCA 需要時會自動啟動 `ollama serve`。
2. 設定 › 本地模型：看推薦清單（依 GPU 顯示記憶體和系統記憶體判斷放得下的模型，標「推薦」的是最適合的那個），按下載。
3. 選好兩個位置的模型，在群組右側的「本地審查員」勾選要邀請的人。

本地模型的上下文是 16k token。CCA 只送 12 KB 以內的 diff 摘要，檔案內容由審查員自己用 read_file 讀（每次最多 300 行，每輪合計約 28 KB）。可用記憶體不夠時 Ollama 會拒絕載入，CCA 會提示你關掉一些程式或換小一點的模型。

審查員必須支援 Ollama 的工具呼叫（設定頁的模型清單會標示「不支援讀檔」）。實測（2026-10-24）：qwen3:8b 會照規則讀檔並引用原文；llama3.1:8b 常常不呼叫工具、自己編出檔案內容，引用全部對不上而被移除，不建議當審查員；dolphin 系列不支援工具。

## 檢查點

每次 AI 開始一輪（有選專案資料夾的對話）之前，CCA 會把專案資料夾存進 `%LOCALAPPDATA%\CCA\snapshots\<對話>.git`。這是另外的 git 倉庫，不會碰到專案自己的 `.git`。`node_modules`、`build`、`.git` 等資料夾不會存。需要電腦上有 git。

## 開發規則

替 CCA 寫程式之前先讀 [AGENTS.md](AGENTS.md)：檔案地圖、建置與測試、架構規則和程式碼風格都在裡面。Claude Code 經由 `CLAUDE.md` 讀它，Codex 直接讀它；CCA 會把其中「審查重點」那一段交給本地審查員（任何專案的 `AGENTS.md` 都可以用 `<!-- cca-review:start -->`／`<!-- cca-review:end -->` 標出要給本地模型看的部分）。

交出前執行：

```bash
python tools/check_style.py --format --js
```

它檢查編碼、換行和空白（`--fix` 會自動修正），用 `.clang-format` 檢查改動過的 C++ 行，並檢查 JavaScript 語法。

指定的工具或 Git 比對基準缺少時會回傳失敗。行內 Tab 需要手動處理，`--fix` 不會改動可能屬於字串的內容。風格工具的回歸測試可用 `python tests/test_check_style.py` 執行；測試只建立暫存檔與隔離 Git 倉庫，不更動專案的索引或提交。

## 建置

需要 Visual Studio Build Tools 2022（「使用 C++ 的桌面開發」）和 Node.js（只用來產生資源檔）。

```bash
build.bat
```

```bash
build.bat test
```

`build.bat test` 跑單元測試。`build.bat live` 會產生 `build\live.exe`，用真的 CLI 做端對端測試（會用掉少量訂閱額度）：

```bash
build\live.exe codex
```

```bash
build\live.exe group --fake-claude
```

`build\live.exe persona --fake-claude` 會檢查性格、日常模式和最省模型有沒有正確傳給 Claude Code（`tests/fake_claude.js` 是模擬的 Claude CLI，不需要登入）。

`build.bat pty` 建置 CLI 測試器，接著執行 `build.bat takeover-test`、`python tests/cli_smoke.py`，可用模擬後端驗證終端機輸入、權限決定、設定、縮放與群組交棒，不使用訂閱額度。

若舊版仍在執行，可用 `CCA_DESKTOP_OUT` 或 `CCA_CLI_OUT` 指定另一個建置輸出路徑。

### 修改介面

介面在 `ui/`，建置時會編進 exe。開發時可以不重新編譯：

```bash
node tools\serve_ui.js
```

用瀏覽器開 http://localhost:5178 會載入 `ui/mock.js` 的模擬資料。也可以讓 exe 直接讀資料夾裡的介面檔：設定環境變數 `CCA_UI_DIR=<專案>\ui`，再加 `CCA_MOCK=1` 就會顯示模擬資料。

## 架構

```
ui/            介面（HTML/CSS/JS，無外部套件），Discord 深色主題
src/main.cpp   Win32 無邊框視窗 + WebView2，介面從 exe 資源讀取
src/cli_main.cpp 終端機介面、輸入編輯、指令與權限選單
src/cli_render.* 中文欄寬、Markdown、訊息與點陣標誌
src/term.*     Windows Console 輸入與 VT 輸出
src/app.*      對話、訊息模型、存檔、UI 指令分派
src/group.cpp  協作流程（撰寫 → 審查 → 複審 → 直到雙方同意）
src/claude_agent.*  驅動 `claude -p --input-format/--output-format stream-json`
src/codex_agent.*   驅動 `codex app-server`（JSON-RPC）
src/auth.cpp   找 CLI、檢查訂閱方案、登入
src/plugins.cpp  插件與 MCP 伺服器
src/personas.* 自訂工作風格與額外指令
src/profile.cpp  名稱／頭像／橫幅、性格指派、日常模式的模型選擇
src/snapshot.* 檢查點（獨立的 git 倉庫）
src/rollback.cpp 回滾聊天／工作、復原
src/hardware.* 偵測 GPU／記憶體、推薦本地模型
src/ollama.*   Ollama HTTP API、本地審查員的對話
src/local.cpp  本地模型設定、下載、群組裡的唯讀審查
src/local_tools.* 本地審查員的唯讀工具、引用比對
src/review.*   審查窗格：檢查點差異、逐段還原
src/guard.*    規範審查（語言、角色、VERDICT、取證）、規則卡、退回訊息
src/takeover.* 額度接手判斷與交接紀錄
src/process.*  子程序（管線、Job 物件，關閉 CCA 時整棵程序樹一起結束）
```

對話紀錄存在 `%APPDATA%\CCA`，WebView2 資料在 `%LOCALAPPDATA%\CCA`。

### 工作台、版面與更新

工作台提供 CCA 交付需求與規則的來源紀錄，以及可編輯、排序、取消的待送指令佇列。版面設定包含閱讀、協作、審查預設與可調整的欄寬。桌面與 CLI 共用版本檢查和獨立更新程式；更新來源是資料目錄下自動建立的 `installers` 資料夾，遠端發行站目前未啟用，亦不提供遠端網頁。

操作、版本後綴（重大規範化）、發行清單及失敗復原方式見 [工作台與軟體更新](docs/workbench.md)。

## 授權

CCA 以 [GNU GPL v3](LICENSE) 授權。`third_party/` 內的 Microsoft WebView2 SDK 依它自己的授權條款，不在 GPL 範圍內。

使用 Claude Code、Codex 等第三方命令列工具時，須遵守各服務自己的條款；CCA 不代為取得、也不轉賣任何訂閱或額度。
