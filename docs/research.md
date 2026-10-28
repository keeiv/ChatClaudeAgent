# 研究筆記

寫程式之前查的資料，以及這些資料怎麼影響設計。查詢日期：2026-10-23。

## 1. 用訂閱帳號驅動 AI 的唯一實際做法

Claude Pro 和 ChatGPT Plus 的訂閱都**不提供 API 金鑰**。要用訂閱額度寫程式，只能透過官方的程式碼代理：

| | Claude | GPT |
|---|---|---|
| 官方工具 | Claude Code CLI（本機測到 2.1.299） | Codex CLI（本機測到 0.162.1） |
| 程式介面 | `claude -p --input-format stream-json --output-format stream-json` | `codex app-server`（stdio 上的 JSON-RPC） |
| 登入方式 | `claude auth login`（瀏覽器 OAuth） | `account/login/start` 或 `codex login` |

所以 CCA 不直接呼叫模型，而是在背景啟動這兩個 CLI，把它們的事件串流轉成 Discord 風格的訊息。

## 2. 訂閱檢查（已在本機驗證）

**Claude**：`claude auth status --json`。登入後會輸出 `loggedIn`、`authMethod`（`claude.ai` 代表訂閱帳號，`api_key` 代表 API 金鑰）、`subscriptionType`（`pro` / `max` / `team` / `enterprise`）。這幾個欄位是從 claude 執行檔的原始碼字串裡確認的。另外，stream-json 的 `initialize` 控制回應裡也有 `account.subscriptionType`。

**GPT**：app-server 的 `account/read` 會回傳 `{"account":{"type":"chatgpt","planType":"plus"}}`，本機實測拿到的是 `plus`。`PlanType` 的完整列舉：`free, go, plus, pro, prolite, promax, team, self_serve_business_*, business, ent26, enterprise_*, edu, edu_plus, edu_pro, unknown`。

**判斷規則**：Claude 必須是 `claude.ai` 登入，而且方案屬於 pro/max/team/enterprise。GPT 必須是 `chatgpt` 帳號，而且方案不是 free/go/unknown。兩個都通過才能進入主畫面。

## 3. 寫程式的軟體必須讓使用者知道的資訊

整理自 Claude Code、Codex、VS Code Copilot、Cursor 的文件與使用者回報：

1. **權限模式要一直看得到**，尤其是「跳過全部確認」。Claude 官方文件說 `bypassPermissions` 應「只在容器或沒有網路的 VM 裡使用」，而且「無法防禦提示注入」。Codex 把 `danger-full-access` 標成 *Elevated Risk*。
2. **權限請求要具體**：哪個工具、什麼指令、哪個檔案、為什麼要權限。選項至少要有「允許一次 / 本次對話都允許 / 拒絕並說明原因」。模糊的提示會讓使用者過度信任。
3. **檔案變更統計**：每個檔案顯示 `路徑 +新增 −刪除`，可以展開看 diff。一輪結束時要有總計。
4. **正在執行的指令**：指令內容、工作目錄、結束碼、耗時、輸出（可收合）。
5. **用量與額度**：token 用量、上下文使用率、Claude 的等值 API 花費（`total_cost_usd`，是估算值）、5 小時與每週額度的使用率和重置時間。
6. **進度與狀態**：誰正在做事（像 Discord 的「正在輸入…」）、待辦清單、API 重試（`system/api_retry` 的第幾次/共幾次）、錯誤。
7. **隨時可以中斷**：停止按鈕。
8. **可以回復**：Cursor、VS Code、Claude Code 都把檢查點當成核心功能，但也都提醒檢查點不能取代 git。CCA 第一版不做回復，改為在非 git 資料夾時提醒使用者。

## 4. 協定細節（本機實測或從執行檔確認）

### Claude Code stream-json
- 宿主傳 `{"type":"control_request","request_id":..,"request":{"subtype":"initialize"}}`，回應包含 `models`（ModelInfo：`value, displayName, description, supportedEffortLevels`）和 `account`。
- 使用者訊息：`{"type":"user","message":{"role":"user","content":[{"type":"text","text":"..."}]}}`
- 加上 `--permission-prompt-tool stdio` 之後，需要權限時 CLI 會送出 `control_request`，subtype 是 `can_use_tool`，欄位有 `tool_name, input, tool_use_id, permission_suggestions, blocked_path, decision_reason, title, description`。
- 宿主回 `{"type":"control_response","response":{"subtype":"success","request_id":..,"response":{"behavior":"allow","updatedInput":{..}}}}`，拒絕時用 `{"behavior":"deny","message":".."}`。
- `AskUserQuestion` 也走 `can_use_tool`，回答放在 `updatedInput.answers`，key 是問題文字，value 是選項 label。
- 其他控制請求：`interrupt`、`set_model`、`set_permission_mode`。
- 輸出事件：`system/init`、`stream_event`（text_delta）、`assistant`、`user`（裡面的 `tool_use_result.structuredPatch` 用來算 +/-）、`rate_limit_event`、`system/api_retry`、`result`（`total_cost_usd, usage, modelUsage, num_turns, duration_ms`）。
- 權限模式：`default`（手動確認）、`acceptEdits`、`plan`、`auto`、`dontAsk`、`bypassPermissions`。

### Codex app-server（從 `codex app-server generate-json-schema` 產生的規格）
- `initialize` → `initialized` → `thread/start {model, cwd, approvalPolicy, sandbox, developerInstructions}` → `turn/start {threadId, input:[{type:"text",text}], effort}`。
- 通知：`item/started`、`item/completed`（`agentMessage`、`commandExecution`、`fileChange{changes:[{path,kind,diff}]}`、`mcpToolCall`、`webSearch`、`reasoning`）、`item/agentMessage/delta`、`turn/diff/updated`、`turn/plan/updated`、`thread/tokenUsage/updated`、`account/rateLimits/updated`、`turn/completed`、`error`。
- 伺服器請求（需要回覆）：`item/commandExecution/requestApproval`、`item/fileChange/requestApproval`，決定值為 `accept / acceptForSession / decline / cancel`。另有 `item/tool/requestUserInput`。
- 額度：`account/rateLimits/read` → `primary`（300 分鐘視窗）、`secondary`（10080 分鐘＝一週），各有 `usedPercent` 和 `resetsAt`。
- 模型：`model/list` 回傳 7 個模型（gpt-6.1-sol 為預設），每個都附可用的推理強度。
- 沙箱：`read-only / workspace-write / danger-full-access`。核准政策：`on-request / never`（`untrusted` 已淘汰）。

## 5. 插件

| | Claude | GPT（Codex） |
|---|---|---|
| 插件清單 | `claude plugin list --json --available`（本機 3904 個） | app-server `plugin/list`（市集分組） |
| 安裝/移除 | `claude plugin install <id> -s user`、`uninstall` | `plugin/install {pluginName, marketplacePath/remoteMarketplaceName}`、`plugin/uninstall {pluginId}` |
| MCP 伺服器 | `claude mcp add-json -s user`、`claude mcp remove`，清單讀 `~/.claude.json` | `codex mcp add/remove`、`codex mcp list --json` |

兩邊的插件格式不通用，所以介面上每個插件都標「給 Claude」或「給 GPT」。MCP 伺服器可以一次裝給兩邊，但會分開列出。

## 6. 讓介面不像 AI 做的

一般認為「AI 感」的特徵：紫色漸層、Inter 字型、置中主視覺加一個按鈕、三張圓角卡片配小圖示、裝飾用的「Live」脈動點、✨ 星星圖示、到處都是毛玻璃、假名字（John Doe / Acme）、少了錯誤/空白/載入/停用狀態。

建議的解法是**照著一套真實存在的設計系統做**，不要用形容詞去描述風格。CCA 直接照 Discord 深色主題的實際規格：

- 色票：伺服器列 `#1E1F22`、側欄 `#2B2D31`、聊天區 `#313338`、輸入框 `#383A40`、主色 `#5865F2`、綠 `#23A55A`、紅 `#F23F42`、文字 `#DBDEE1` / `#949BA4`
- 尺寸：伺服器列 72px（圖示 48px）、側欄 240px、標題列 48px、頭像 40px、訊息左側縮排 72px
- 元件：用 Discord 的嵌入卡片（左側 4px 色條）放檔案變更和指令；用 Discord 訊息按鈕（藍、灰、紅、綠）處理權限；用「正在輸入…」顯示誰在工作；用 APP 標籤標示 AI
- 字型：Discord 的 gg sans 有版權，改用 Segoe UI 加微軟正黑體，不用 Inter
- AI 頭像只用字母和品牌色，不用 ✨，也不用別人的商標

## 7. 實作時發現的事（本機實測）

- **Codex 的 Windows 沙箱會修改 ACL**：在「工作區可寫」模式下，Codex 會用 `codex-windows-sandbox-setup.exe` 讓 `CodexSandboxUsers` 群組取得工作區和 `%TEMP%` 的寫入權限。如果給它的工作區是使用者資料夾，它會對整個使用者資料夾底下的項目加寫入權限。所以 CCA 會讓 app-server 在 `%LOCALAPPDATA%\CCA\codex-server` 這個空資料夾啟動，不用 `command/exec`，並且拒絕把磁碟根目錄、使用者資料夾、系統資料夾當成專案資料夾。
- **Codex 桌面版開著時沙箱可能失敗**：沙箱設定會驗證 Codex 桌面版的 `node_repl.exe`，檔案被鎖住時（os error 32）指令會失敗，錯誤是 `setup refresh had errors`。CCA 偵測到這個錯誤時會提示。
- **Job 物件要允許脫離**：CCA 把子程序放進 Job 物件，關閉時一起結束。Codex 沙箱需要 `JOB_OBJECT_LIMIT_BREAKAWAY_OK`。
- **Claude 的登入和桌面版分開**：Claude 桌面版自己管理登入，所以內建的 claude.exe 單獨執行時是未登入。CCA 需要另外執行 `claude auth login`。
- **協作的停止條件**：審查方「同意且沒有修改」時，只有在對方上一輪不是「有疑慮」的情況下才算雙方都沒意見。否則要交回去讓提出疑慮的一方確認。

## 8. 自訂風格、日常模式與思考程度（2026-10-23 第二輪）

**Ultracode**：Claude Code 2.1.299 接受 `--effort ultracode`（`--help` 沒有列出）。從執行檔字串確認，它等於 XHigh 再開啟「standing dynamic-workflow orchestration」，會分派子代理處理大型任務，需要支援的模型，用量最高。Codex 的 `model/list` 則直接列出每個模型的思考程度，gpt-6.1-sol 最高到 `ultra`（自動分派子任務）。

**最省額度的模型**：Codex 的協定沒有價格資訊，模型名稱裡的 luna／mini／nano 是較小、較快的系列；Claude 用 Haiku。日常模式依序找這些名稱，找不到就用預設模型，使用者也可以在設定裡指定。

## 9. 回滾、本地模型與用量（2026-10-23 第三輪）

**回滾聊天**：Claude Code 2.1.299 支援 `--resume <session> --resume-session-at <message uuid> --fork-session`，從某則助手訊息接續並分出新的 session。CCA 記下每輪最後一則助手訊息的 uuid，回滾時接到目標訊息之前的最後一個 uuid；如果目標是第一輪，就開新的 session。Codex app-server 有 `thread/revert {threadId, beforeTurnId}`，把那一輪和之後的回合從對話移除。

**回滾工作**：Claude Code 的 `/rewind` 只在互動模式可用，Codex 沒有檔案檢查點，所以 CCA 自己做。做法參考 Cursor 和 VS Code 的檢查點：每輪開始前用獨立的 git 倉庫（`--git-dir` + `--work-tree`）存一次，還原時先存「回滾前的狀態」，刪掉之後新增的檔案，再 `checkout <commit> -- .`。

**本地模型推薦**：用 DXGI 讀每張顯示卡的 DedicatedVideoMemory，用 GlobalMemoryStatusEx 讀記憶體。規則是：模型大小加 1.5 GB 放得進顯示記憶體就全跑在 GPU；放不下但不超過顯示記憶體加 35% 系統記憶體就標「勉強」（部分卸到 CPU，會慢）；沒有獨立顯示卡時，大小加 2 GB 不超過系統記憶體的 70% 才列出。候選清單是 Ollama 上適合審查程式的模型（qwen3-coder 30B、devstral 24B、gpt-oss 20B、qwen3 14B／8B／4B、qwen2.5-coder 14B／7B／3B）。這台電腦（RTX 5070 Ti Laptop 12 GB、15 GB 記憶體）推薦 qwen3:14b。

**為什麼本地模型只能建議**：小模型審查程式時誤報率高，常把正確的程式說成有錯。所以它們拿到的是 CCA 讀好的 diff 和檔案（沒有工具、不能改檔），建議要附上「信心」。交給 Claude 和 GPT 時附帶規則：必須讀檔、跑測試或引用程式碼證明後才能採納，否則要說明為什麼不採納。

**每日用量**：每輪結束時把 token 記到 `%APPDATA%\CCA\usage.json` 的當天欄位。圖表是堆疊長條，三個顏色用 dataviz 驗證工具在 #232428 背景上檢查過色盲區分度和對比。

## 來源

- Claude Code headless / stream-json：https://code.claude.com/docs/en/headless
- Claude 權限模式：https://code.claude.com/docs/en/permission-modes
- Agent SDK 權限與提問：https://code.claude.com/docs/en/agent-sdk/user-input
- Codex 核准與沙箱：https://learn.chatgpt.com/docs/agent-approvals-security
- Codex exec JSONL 整理：https://codex.danielvaughan.com/2026/04/08/codex-exec-jsonl-reference/
- VS Code 代理信任與檢查點：https://code.visualstudio.com/docs/copilot/concepts/trust-and-safety
- 代理 UX 信任模式：https://pub.towardsai.net/designing-for-ai-agents-8-ux-patterns-that-build-trust-in-2026-713a0c926933
- 核准邊界：https://dev.to/hefty_69a4c2d631c9dd70724/ai-agents-need-approval-boundaries-not-autonomy-theater-182j
- Cursor 檢查點：https://developertoolkit.ai/en/cursor-ide/advanced-techniques/checkpoints-branching/
- AI 介面同質化：https://prg.sh/ramblings/Why-Your-AI-Keeps-Building-the-Same-Purple-Gradient-Website
- AI slop 特徵：https://github.com/Laith0003/ux-skill/wiki/How-to-detect-AI-slop-in-your-design
- 常見 AI 介面樣式：https://noqta.tn/en/blog/ai-design-slop-overused-ui-patterns-fix-2026
- Ollama API：https://github.com/ollama/ollama/blob/main/docs/api.md
- 本機：`claude --help` 與執行檔字串（`--resume-session-at`）、`claude --help`、`claude auth status --json`、`codex app-server generate-json-schema`、`account/read`、`model/list`、`plugin/list` 的實測輸出
