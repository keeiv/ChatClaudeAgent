# Cursor 帳號、Grok 等模型、私訊與群組審查（1.0.2-terra）

CCA 透過官方 Cursor Agent CLI（`cursor-agent`）使用你自己的 Cursor 帳號，所以 Cursor 方案裡的模型都能用，包括 Grok 系列（例如 `grok-4.7-high`、`cursor-grok-4.6-medium`）。Cursor 和 Gemini 一樣以「每輪啟動一次無介面 CLI」的方式工作，可以開私訊、加入一般群組擔任審查者，或當多代理流程的節點。

## 安裝與登入

1. 依 Cursor 官方說明安裝 CLI（Windows PowerShell）：`irm 'https://cursor.com/install?win32=true' | iex`，說明在 <https://cursor.com/docs/cli/installation>。
2. 在「訂閱與用量」的 Cursor 卡片按「登入」（CLI：`/cursor-login`）。CCA 開啟官方 CLI 的 `login`，登入在瀏覽器完成；登入視窗結束後 CCA 自動重新檢查。
3. 帳號卡顯示帳號、方案（`about --format json` 的 `subscriptionTier`）、CLI 版本與執行位置。

CCA 預設使用 `%LOCALAPPDATA%\cursor-agent\versions` 裡最新、同時有 `node.exe` 與 `index.js` 的版本資料夾（與官方啟動器挑選版本的規則相同）。設定 → 執行檔可以改填安裝資料夾、版本資料夾或 `index.js`。CCA 直接用該版本附帶的 `node.exe` 執行 `index.js`，不經過 `.cmd`／PowerShell 啟動器，多行提示詞與含中文的專案路徑不會被 cmd.exe 改寫。

帳號檢查只執行三個唯讀指令：`status --format json`、`about --format json`、`models`，不送出任何提示詞。檢查與每次執行都會移除 `CURSOR_API_KEY`、`CURSOR_AUTH_TOKEN` 與 `NODE_OPTIONS`，只用登入的帳號計費；CLI 若回報改用 API 金鑰，聊天會出現警告。

## 模型

模型清單只來自 `agent models` 對這個帳號的回報，CCA 不猜測或補上模型 ID：

- 每一行「`id - 顯示名稱`」成為一個選項；顯示名稱去掉零寬字元與多餘空白，`(default)`／`(current)` 標記另外保存。ID 原樣保留，開頭是 `-` 或含空白、引號的行會被忽略，避免被當成命令列旗標。
- 思考程度包含在模型 ID 裡（`-low`、`-high`、`-xhigh`、`-fast`），所以 Cursor 沒有獨立的思考程度設定，也不參加自動選擇模型。
- 新的 Cursor 私訊沒有預設模型，必須先選一個；選單把 Grok 系列排在最前面，可以輸入文字篩選。CCA 不改用 CLI 的 `auto` 或帳號目前的模型。
- 選擇時主機會再比對一次清單，清單沒有的 ID 會被拒絕。訊息標頭顯示 CLI 實際回報的模型名稱。

## 權限

無介面的 Cursor CLI 不能逐項詢問，規則以官方文件與 CLI 原始碼為準（見 [驗證](#驗證)）：

| CCA 權限 | Cursor 的做法 |
| --- | --- |
| 規劃模式 | 不啟動。無介面執行會直接寫入工作區檔案，無法保證唯讀。 |
| 每次詢問 | 不啟動。無法逐項詢問。 |
| 自動編輯 | 不加 `--force`：工作區內的修改直接寫入，允許清單以外的指令、MCP 與網路存取會被拒絕；你在 Cursor 開了沙箱時，能在沙箱內執行的指令會直接執行（Cursor 的規則）。 |
| 跳過所有確認 | 加 `--force`。 |

Cursor 自己的設定也能讓無介面模式不經詢問執行，CCA 在「自動編輯」下會檢查兩處，不符合就拒絕啟動：

- 使用者設定 `cli-config.json`（`CURSOR_CONFIG_DIR`、`XDG_CONFIG_HOME\cursor` 或 `%USERPROFILE%\.cursor`，與 CLI 的找法相同）的 `approvalMode` 不是 `allowlist`（例如 `unrestricted`、`auto-review`）。請在 Cursor 改回 `allowlist`，或在 CCA 改用「跳過所有確認」。這個檔案裡你自己允許的指令（例如 `Shell(ls)`）照 Cursor 的規則執行。
- 專案資料夾或任何上層資料夾的 `.cursor\cli.json` 在 `permissions.allow` 放了 `Read(...)` 以外的項目。CLI 會把這個檔案深度合併到使用者設定上（陣列整個取代你的允許清單），而 `--trust` 不再詢問，所以一個倉庫可以讓 `Shell(*)` 不經詢問執行。專案檔只能設定 `permissions`，裡面的 `approvalMode` 會被 CLI 丟掉。

兩個設定檔無法解析時同樣拒絕。

日常模式需要逐項詢問，所以 Cursor 私訊不提供。每次執行都加 `--trust`（信任你在 CCA 選的專案資料夾）與 `--workspace <專案資料夾>`。信任標記由 CLI 寫在 `%USERPROFILE%\.cursor\projects\<路徑>\.workspace-trusted`，不會出現在專案裡，也不會被當成 AI 的修改。

## 執行與輸出

CCA 以 `-p --output-format stream-json` 啟動，提示詞從標準輸入傳入（CLI 沒有命令列提示詞時會讀到 EOF），不受命令列長度限制。私訊用 `--resume <對話編號>` 接續；多代理流程的每個節點各自保存編號。

- 不使用 `--stream-partial-output`：官方文件寫明它會在工具呼叫前與回合結束時重送已輸出的文字。每個 `assistant` 事件是兩次工具呼叫之間完整的一段，CCA 把每段放成獨立的文字區塊，順序和工具區塊交錯一致。
- `tool_call` 以工具種類為鍵（`shellToolCall`、`readToolCall`、`editToolCall`……），結果以變體為鍵（`success`、`failure`、`rejected`、`permissionDenied`……）。沒見過的工具顯示為「其他」，不當成讀檔。被拒絕的動作會寫明「需要確認的操作不會執行」。
- 指令的結束碼只取 CLI 自己的 shell 執行器回報的數字；沒有回報時不補 0，因此不能據此通過必要測試閘門。
- 回合只有在程序結束碼是 0、而且收到 `result` 事件、`is_error` 為 false 時才算完成；中途結束或沒有 `result` 都算失敗，錯誤訊息附上 CLI 標準錯誤的最後幾行。
- Token 用量取 `result.usage`，沒有時加總每次 `usage` 事件。Cursor 不提供剩餘額度，CCA 顯示「沒有額度資料」，只有在某一輪真的回報額度錯誤時才把 Cursor 當成額度不足並跳過。
- 停止：無介面 CLI 沒有中斷請求，CCA 結束該程序，這一輪標為已中斷。

## 群組與多代理流程

**一般群組。** 頻道設定新增「Cursor 成員」（CLI：`/cursor on|off`），群組依「主刀 → 搭檔 → Gemini → Cursor」輪替；Gemini 不在時 Cursor 是第三位審查者。群組的 Cursor 模型在頻道設定選（CLI：`/model` 後選 Cursor）。帳號沒通過、還沒選模型，或群組權限是規劃／每次詢問時，輪到 Cursor 會標示「跳過」並寫明原因。

**多代理流程。** 「多代理流程 → 新增 Cursor 審查者」或把節點後端設為「Cursor 帳號（Grok 等模型）」。節點模型只能選 Cursor 回報的清單，預設帶入清單裡第一個 Grok 模型，儲存前可改。

## 驗證

- 輸出格式、無介面的寫入與拒絕規則、`approvalMode`：官方文件 <https://cursor.com/docs/cli/reference/output-format>、<https://cursor.com/docs/cli/headless>、<https://cursor.com/docs/cli/reference/permissions>。
- 標準輸入當提示詞、`stream-json` 的事件欄位、`approvalMode` 的可能值與設定資料夾的找法：CLI 版本 2026.11.19-f005e9d 的原始碼（`build-prompt.ts`、print 模式的事件輸出、`cursor-config/paths`）。
- 專案 `.cursor\cli.json` 的合併方式、它只接受 `permissions`、信任標記的位置：同一版 CLI 原始碼（`cursor-config/schema`、`.workspace-trusted`）。
- 實機（不送提示詞）：這個帳號的 `agent models` 回報 284 個模型，CCA 的解析規則全部保留，其中 22 個是 Grok；以 CCA 的實際參數、空的標準輸入執行，CLI 接受所有參數並回報「No prompt provided」，專案資料夾的 `git status` 沒有任何變化。
- `.\build.bat cursor-test`：以真實子程序替身涵蓋事件轉換、標準輸入、環境變數移除、`--force` 只在完全存取出現、規劃／每次詢問／`unrestricted`／專案允許 `Shell(*)`／壞掉的設定檔被拒、只含 `Read(...)` 的專案設定可以執行、模型 ID 不能夾帶旗標、版本資料夾挑選、帳號與模型清單解析，以及經過主機的完整私訊一輪（檢查點、事件、對話編號保存）。
- `.\build.bat takeover-test`：Cursor 加入一般群組後在 GPT 之後審查並達成共識；群組權限是「每次詢問」時 Cursor 被標示跳過、沒有啟動。
- `.\build.bat pipeline-test`：Cursor 當第二位可修改檔案的審查者，清單外的模型不啟動；真實檔案修改、獨立的對話編號與用量、訊息身分、讀檔紀錄算作 PASS 的查證；中途停止只關閉 Cursor 的訊息。
- `tests\cursor_browser.cjs`、`tests\cli_smoke.py`：桌面版的私訊、Grok 優先的模型選單與篩選、群組成員、流程節點與帳號卡；CLI 的 `/cursor on` 與 `/status`。
- 以上都不使用 Cursor 額度。沒有驗證：真實帳號的一輪模型回覆（會使用 Cursor 額度），以及 `--mode plan` 能否保證唯讀（因此不提供）。
