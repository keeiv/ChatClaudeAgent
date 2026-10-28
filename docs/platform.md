# 多代理平台（0.6.2）

本輪升級原專案，沒有改動旁邊的開源副本，也沒有提交或推送 Git。既有私訊、雙方群組、性格、插件及頭像功能保留。群組未設定 `pipeline` 時仍走原本流程。

## 沿用與新增

原核心已具備 `App` 的 UI 執行緒、Claude Code／Codex 協定轉接、影子 Git 檢查點、本地唯讀工具、規範審查及用量事件。交接時還有未完成的 `task_ledger` 與 `claude_usage`；本輪接續修正，沒有重建這些系統。

實施順序是：持久交接及額度查詢 → 順序代理與審查閘門 → API 工作程序 → MCP／隔離執行 → 聊天室圖片 → 相容性驗證。新增模組如下。

| 模組 | 責任 |
| --- | --- |
| `src/task_ledger.*` | 原始需求、驗收標準、交接、測試與版本索引。 |
| `src/pipeline.*` | 每位代理的模型、角色、會話、順序、預算與審查停止條件。 |
| `src/runtime.*` | 沿用 `ChildProcess` 的 Job 物件及 JSON-lines IPC，背景事件回到 UI 執行緒。 |
| `src/platform.cpp` | 供應商／MCP 管理、任務控制、WIC 圖片驗證與保存。 |
| `tools/agent_runtime.py` | Python 標準函式庫實作的 API 轉接、DPAPI、受限檔案工具、MCP stdio 及 Docker。 |
| `ui/platform.js` | 流程排序、API／工具設定、任務狀態、圖片縮圖與預覽。 |

## 啟用流程

桌面版在群組側欄按「多代理流程 › 設定」。可加入、移除與排序 2 至 32 個位置，每個位置有獨立 `id`、模型及角色。Claude Code、Codex、API 都能撰寫、審查或驗證並修改程式；Ollama 位置維持唯讀。第一位必須撰寫，並至少有一位審查或驗證者。

模型直接以下拉選單選擇，不需要輸入模型 ID。Claude Code 與 Codex 使用登入後取得的清單，也可選「使用 CLI 預設模型」；Ollama 列出本機已安裝的模型，角色顯示為唯讀。按「更新清單」可重新查詢。

API 先選供應商，CCA 會自動查詢該 API 的 `/models` 並填入供應商的預設模型，沒有預設值時填入第一個。可再從同一份清單切換模型。清單載入失敗或為空時會顯示原因並允許重試；目前沒有可用模型時無法儲存。切換供應商會清除前一家的選擇，已儲存但不再列出的模型會保留顯示，必須重新選擇才可儲存。模型清單更新不會重建名稱與預算欄位。

CLI 維持以 JSON 匯入流程；桌面版儲存的模型選擇與 CLI 共用，不需要在終端機重設。

完整循環預設上限為 3，可調 1 至 20。完整路由使用所有位置；簡單路由只走第一位撰寫者及第一位審查者。自動路由對不超過 1,200 UTF-8 bytes 且未命中安全、權限、資料、遷移、API 等風險字詞的需求採簡單路由。這是本機規則，沒有額外模型分類費用；可手動選完整路由。

任務中可暫停、繼續或取消，也能在停止執行後重新審查。暫停保留目前代理、版本與工作區鎖；取消釋放鎖。重開程式會把仍在執行的任務標為中斷，使用者選「接續任務」後才啟動代理。重新審查從審查者開始，不重跑撰寫者，也不重置同一任務的 API 預算。

需補充或修正需求時，先暫停再傳送訊息。補充會寫進同一個任務的交接索引，保留原始需求，並優先傳給後續代理；既有工作檔案及任務 ID 保留，審查與測試狀態重新驗證。工作中直接傳送補充會提示先暫停。

CLI 以 `/pipeline 檔名.json` 匯入設定，`/pipeline off` 回到原流程。例如：

```json
{
  "nodes": [
    {"id": "author", "name": "Claude", "backend": "claude", "role": "write", "model": ""},
    {"id": "reviewer", "name": "GPT", "backend": "codex", "role": "review", "model": ""},
    {"id": "verifier", "name": "Claude 驗證", "backend": "claude", "role": "verify", "model": ""}
  ],
  "maxCycles": 3,
  "routing": "full",
  "requireTests": true,
  "acceptance": "保留既有行為，執行專案測試，沒有未解問題才完成"
}
```

模型留空時由官方 CLI 使用預設值。不同 Codex 位置可填不同模型 ID；是否可用以該 CLI 或 API 回報為準，Codex 型號名稱不代表 API 一定有同名模型。

其他 CLI 指令有 `/pause`、`/continue`、`/stop`、`/rereview`、`/resume`、`/task-history [序號]`、`/sandbox`、`/provider-models id`、`/provider-remove id`、`/toolserver-test id`、`/toolserver-pin id 工具…`（1.0.1-sol）與 `/toolserver-remove id`。群組另有 `/test-guard 指令|on|off`，設定審查者測試守門。

## 記憶、版本與審查

任務保存在資料目錄的 `tasks/<conv_id>.json`。完整原始需求及每次交接保存在 `task-history/<task_id>/request.json`、`1.json`、`2.json` 等索引；紀錄後補欄位時另留舊版本。工作記憶只保留最新 40 筆，畫面顯示最新 10 筆，較早內容按需讀取。未解問題會保留索引，摘要有固定 byte 預算。

`CCA_DATA_DIR` 同時隔離設定、對話與 `local/` 下的影子 Git、快取。工作區互斥使用路徑對應的 Windows 核心物件，仍能阻擋不同資料目錄的 CCA 同時改同一個專案。

每筆交接含 `task_id`、`agent_id`、`role`、`base_revision`、`head_revision`、`changed_files`、`diff_reference`、`summary`、`issues`、`test_results`、`next_action`。模型的完整報告另存在 `report`，供查閱修改函式、理由及決策。修改檔案和版本取自執行前後的實際影子 Git，不採信文字自述。

交接提示傳任務摘要與版本引用，不重送整份對話或程式碼。API／本地代理以 `task_history` 取索引、`task_read` 分段讀完整需求或紀錄、`git_diff` 查實際修改、`read_artifact` 讀同任務圖片。Claude Code／Codex 也會收到索引及影子倉庫位置。

審查結尾須有 `REVIEW: PASS`、`REVIEW: NEEDS_FIX` 或 `REVIEW: BLOCKED`；既有 `VERDICT` 放在最後一行。內文引用 PASS 不算通過。PASS 仍須有本輪查證紀錄，API／本地審查須真的讀過 Git 差異；有檔案修改不能直接 PASS。啟用規範審查時仍檢查語言、角色及測試宣稱，重試受上限與預算限制。

預設完成前必須有主機取得的測試結束碼 0。命令名稱用來辨識測試／建置／檢查，模型回覆「通過」不能代替執行證據。修改後撤銷先前的通過狀態，失敗或沒有結束碼的測試也不能過閘門。使用者可明確取消必要測試要求；整合測試的部分案例會關閉此要求，另有案例驗證缺少測試會阻擋完成。

相同工作區以解析後的路徑鎖定，不能由另一個任務同時寫入。交接及重啟後若實際工作區與已保存版本不同，就停止並要求使用者檢查。按繼續才採用目前版本，先前審查及測試狀態會撤銷。重複相同問題而沒有修改會停止；循環上限仍是最後一道限制。

回滾檔案後撤銷完成及測試狀態，重新建立代理會話，保留回滾版本與理由。回滾聊天會清掉被撤回任務的目前記憶，不能拿未撤回的模型會話繼續引用已刪內容。任務結果保存失敗也不會標記完成。

## API 與權限

原本 Claude Code／Codex 的訂閱認證保持原樣。API 供應商在「設定 › 多代理與 API 工具」另外建立，支援 Responses 與 Chat Completions 相容協定，模型清單由 `/models` 取得；能力只有供應商回報時才收錄，未知影像能力須自行確認。

工作程序需要現有的 Python 3.10 以上，沒有自動安裝套件。API Key 經 stdin IPC 傳入，使用 Windows DPAPI 加密在 `provider-credentials/<id>.bin`，綁定服務網址；不進設定、提示詞或聊天。HTTP 重新導向不帶金鑰重送，正式 API 只允許 HTTPS，loopback HTTP 供本機服務及測試使用。

每位 API 代理有累計 Token／估算費用上限。費率由使用者填寫，沒有費率就標示金額未知，只能限制 Token。不同模型費率不同時，分別建立供應商設定。額度以供應商回報的用量結算，可能有單次請求超出剩餘額度的差額；無法把本機估算當成供應商的帳務硬上限。429 及暫時性 5xx 有有限次退避，沒有無限重試。

API 與本地代理只有列出的工具。檔案工具拒絕絕對路徑、穿越、連結、`.env*` 及憑證位置。本地代理的工具清單沒有寫入、執行或 MCP 呼叫；即使模型自行要求，也會在執行端拒絕。規劃模式禁止寫入與執行，詢問模式的寫入需核准，而且核准綁定目前呼叫。

## MCP、隔離與圖片

新增 MCP 管理用於 API 代理，採 stdio JSON-RPC，按需取得工具描述。Claude Code／Codex 仍使用原本插件頁的 MCP 設定。管理頁可配置、啟停、發現與測試既有伺服器；必須先確認來源、原生執行檔、參數及允許工具，不會自動跑 `npx` 或安裝套件。CLI 用 `/toolserver-import 檔名.json` 匯入，輸入 `ALLOW` 才核准。

1.0.1-sol 起，核准的工具還要「釘住」描述：工作程序記下名稱、描述、參數格式與註記的 SHA-256，之後每次發現與呼叫前重新取得清單比對，不同或沒釘住的工具會被阻擋。做法與限制見 [審查者測試守門與 MCP 工具描述釘住](reviewer-guard.md)。

MCP 是另行核准的宿主程序，不具有 Docker 隔離保證。API 工作者每次呼叫宿主工具仍需核准，伺服器清單及權限不能在多代理任務執行中更改。瀏覽器 MCP 回傳的 PNG 會自動拆成產物，不把整串 base64 重送進模型文字。

`sandbox_run` 使用現有 Docker 映像，`--pull=never`、無網路、唯讀根檔案系統、非 root、移除 capabilities，另有 CPU／記憶體／程序數／逾時限制。只掛載受篩選的來源副本及本次輸出資料夾，不掛載原工作區、家目錄或憑證。來源副本上限 32 MiB，單檔上限 5 MiB；依賴須已存在映像內。這組參數適用 Linux 容器，沒有 Windows Sandbox、VM 或一般 WSL 的替代執行路徑。

沙箱命令輸出有大小上限；逾時及取消會清理對應容器。缺少 Docker 或現有映像時回報阻擋，不改在宿主機執行。Claude Code／Codex 原有工具仍遵守各自原本的權限與沙箱政策，不能把它們的宿主測試稱為 Docker 隔離測試。

API 代理可用 `publish_artifact` 提交專案內 PNG；CLI 代理在完成測試後輸出一行 `CCA_ARTIFACT: .cca-artifacts/截圖.png`。圖片經主機限制路徑、大小及 WIC 完整解碼後複製到媒體目錄，連同任務、時間、來源、操作、結果與錯誤欄位保存。只有路徑的提交會標示未知結果，不假造成功。相同圖片不重複保存。

聊天室會直接顯示縮圖，點擊放大，側欄保留該任務圖片歷史；影像能力啟用後，後續 API 代理能讀同一份圖片。CLI 保持文字介面，顯示圖片引用，預覽由桌面版處理。

## Claude 與 GPT 共用的沙箱

Codex 在 Windows 的沙箱常因 Codex 桌面版鎖住 `node_repl.exe` 而啟動失敗（見 [研究紀錄](research.md)），GPT 就一直回報沙箱壞掉。CCA 現在把同一套 Docker 沙箱做成 MCP 伺服器 `tools/sandbox_mcp.py`，同時交給兩邊：

- Codex：`thread/start` 與 `thread/resume` 的 `config.mcp_servers["cca-sandbox"]`，`default_tools_approval_mode` 設為 `approve`，`tool_timeout_sec` 150（預設 60 秒短於一次執行的 120 秒上限）。
- Claude Code：啟動時寫一份 `--mcp-config` 檔到 `%LOCALAPPDATA%\CCA\prompts`，並以 `--allowedTools mcp__cca-sandbox` 略過逐次核准；Claude 結束時刪掉該檔。
- 兩邊都會在角色指示裡看到一段說明，包含沙箱的限制與「指令環境失敗時先試 sandbox_run」。

工具有兩個。`sandbox_status` 回報 Docker 引擎版本與本機已有的映像；`sandbox_run` 接受 `image` 與參數陣列 `command`，沿用 `run_sandbox` 的全部限制。每次呼叫用獨立的容器名稱（`cca-mcp-<工作階段>-<亂數>`），結束後刪除來源副本；`/output` 底下最多 8 個、每個 8 KiB 的文字檔會附在結果裡。映像不存在、引擎沒啟動、沒有 Docker 時，回傳說明原因的錯誤，不改在宿主機執行。

啟動條件：`PATH` 上有 Python（非 WindowsApps 的商店捷徑）與 `docker`，而且 `settings.json` 的 `sandboxShare` 不是 `false`（預設 `true`）。缺任何一項就不註冊，GPT 看不到壞掉的工具。安裝 Docker 後要重開 CCA，新的 `PATH` 才會生效。每日對話（沒有專案資料夾）不提供。桌面版與 CLI 版共用同一段核心，兩邊行為一致。

限制：

- 只有 Linux 容器。CCA 自己的 Windows 建置與測試（`build.bat`、MSVC）不能在裡面跑，那類結果仍以主機實際執行為準。
- 映像要先 `docker pull`，沙箱不會自己下載。
- 工具由 CCA 預先核准，不會跳核准卡。沙箱本身沒有網路、不掛載宿主工作區，才敢這樣做。
- 規劃與唯讀模式也能用，因為它只讀來源副本、不改專案。
- CCA 被強制結束時，執行中的容器可能撐到命令自己結束或 120 秒逾時；正常結束與取消都會清掉。

## Claude 額度

在訂閱與用量開啟「即時查詢 Claude 額度」，或 CLI `/claude-usage on`。Claude 工作中每 20 秒查詢，待命每 60 秒，手動更新也有 5 秒最短間隔。23% 這類低用量也會更新，不等 90% 警告。畫面顯示查詢時間與失敗原因，失敗保留上次數值。

這是輪詢，沒有逐 token 的官方推播保證。官方 [status line 文件](https://code.claude.com/docs/en/statusline) 描述互動式資料；目前輪詢使用的 OAuth usage 端點不是穩定公開 API，可能變動。只讀 Claude Code 已保存的登入權杖、只連 Anthropic 官方 HTTPS，不替它更新或輪替權杖。測試只使用隔離的模擬登入檔及 loopback 服務。

## 驗證與限制

測試命令與本輪結果見 [驗證紀錄](platform-validation.md)。API／CLI 協作測試使用本機替身服務及子程序，不能當成真實訂閱或 API 帳戶已實測。原生 WebView2 視窗也需要另外驗證。

0.6.1 已補測 Claude Code 2.1.308 經正式轉接器載入 `--mcp-config`，並真正呼叫共用沙盒。容器隔離、失敗結束碼、輸出上限與清理的實際證據見 [沙盒驗證](sandbox-validation.md)。本輪沒有請 GPT 模型自行選擇工具，也沒有操作原生視窗；這些範圍不能當成已驗證。

參考：[Responses 工具呼叫](https://developers.openai.com/api/docs/guides/function-calling)、[MCP stdio 協定](https://modelcontextprotocol.io/specification/2025-03-26/basic/transports)、[Docker 執行參數](https://docs.docker.com/engine/containers/run/)、[Windows Sandbox 配置](https://learn.microsoft.com/en-us/windows/security/application-security/application-isolation/windows-sandbox/windows-sandbox-configure-using-wsb-file)。本輪選擇現有 Python 標準函式庫與可選 Docker，沒有另建 JavaScript 打包系統。
