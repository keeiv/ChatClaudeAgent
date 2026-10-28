# 共用沙盒驗證（0.6.1）

驗證日期：2026-10-25。檢查原專案的檔案清單、Git 差異、沙盒來源、Claude／Codex 啟動設定、事件轉接與內嵌資源。沒有變更開源副本、提交 Git 或結束使用者正在執行的 CCA。

## 環境與啟動

Docker Desktop 已安裝，Docker Client／Server 為 29.9.2，使用 Linux／WSL2 後端；本機已有 `busybox:1.36`。本輪沒有安裝套件或下載映像。

一開始 Docker 引擎沒有執行。啟動後 WSL 的 `/run/host-services/backend.sock` 缺失，Docker API 持續逾時，正常重新啟動也逾時。強制停止這次啟動的 Docker Desktop，僅終止 `docker-desktop` 發行版，再啟動後恢復。沒有終止其他發行版，也沒有重設、刪除 Docker 資料。

## Claude 實際呼叫

使用本機 Claude Code 2.1.308 與 Haiku，透過 `ClaudeSession` 的正式啟動參數、初始化及事件解析執行，未使用假的 Claude 回覆。每次測試建立獨立暫存專案、驗證字串、測試用 `.env` 與 `CCA_DATA_DIR`；未讀取私人對話或原專案圖片。

`--connect-only` 只啟動工作階段及送出 `mcp_status` 控制要求，不送模型提示詞。確認同一個工作階段的 `cca-sandbox` 為 `connected`，且確實提供 `sandbox_status` 與 `sandbox_run`，不以其他 MCP 已連線當作證據。回傳到 CCA 的狀態只含名稱、狀態與工具名稱，排除伺服器環境變數與設定。

取得本次使用者要求測試 Claude 的授權後，再以 `--run` 發送最小驗證提示詞。實際觀察到 `ToolSearch`（工具描述查找）、`mcp__cca-sandbox__sandbox_status`、`mcp__cca-sandbox__sandbox_run`。後者回傳 Docker 隔離、結束碼 0、uid 65534、從來源副本讀出的驗證字串、`env=hidden` 與 `/output/proof.txt`。主機原始驗證檔維持相同內容。

第一次執行時，驗證程式把純工具查找 `ToolSearch` 誤列為不允許，雖然沙盒已成功，整體仍回傳失敗。修正後的重跑成功；另發現每回合初始化沒有工具描述，會覆寫測試報告的連線欄位，已改成保留查證過的連線結果。最後再次完整執行，整體結束碼為 0，並獨立讀回 JSON 核對各項條件。三次均未執行宿主命令工具。

紀錄：[不送提示詞的連線測試](../build/sandbox-claude-connect.log)、[第一次執行](../build/sandbox-claude-live-first.log)、[修正後的實際呼叫](../build/sandbox-claude-live.log)。這些是本機驗證連結，不隨原始碼發布。

最後一次的 [結構化證據](../build/sandbox-live-evidence.json) 記錄初始化、連線、兩項工具、真實結束碼與來源未變；不使用模型的口頭自述判定成功。

重跑方式：

```powershell
.\build.bat sandbox-live
.\build\sandbox_live.exe <claude.exe完整路徑> --connect-only
# 下一條會使用訂閱額度，須先取得授權。
.\build\sandbox_live.exe <claude.exe完整路徑> --run busybox:1.36
```

初始化、MCP 連線及執行均有逾時；非沙盒工具需要核准時會拒絕，不能靠主機執行指令補成成功。

## 首輪修正與回歸

| 範圍 | 結果與證據 |
| --- | --- |
| 共用沙盒 | [15 項測試](../build/sandbox-mcp-validation.log)，本輪無跳過。包括真實容器與真實 stdio MCP 子程序。 |
| 容器隔離 | 實測 uid 65534、capabilities 為 0、無法連外、來源與根目錄不可寫；看不到 `.env`、`.git`、Docker socket 或專案外測試檔。來源內容保持不變，輸出可取回。 |
| 失敗傳遞 | 真實容器 `exit 17` 經 MCP 回傳 `isError: true` 及原結束碼；Claude／Codex 的沙盒事件保留命令與數值結束碼，交接及必要測試閘門可採用。模型聲稱成功的文字不能代替結束碼。 |
| 輸入與輸出 | 非物件參數、錯誤欄位、無效映像及過長命令會拒絕；錯誤 RPC 參數不會中斷下一個要求。單個文字輸出只讀取 8 KiB 加一個截斷判定 byte，不先載入整個檔案。 |
| 清理 | 無效請求在複製來源前拒絕；來源超限、執行逾時或其他例外都移除半成品。MCP 完成後清除來源副本；API 工作者複製 PNG 證據後也清理暫存工作，保留正式產物。 |
| API 工作程序 | [24 項測試](../build/sandbox-runtime-validation.log)，涵蓋憑證、路徑、唯讀權限、核准綁定、MCP 與產物清理。使用本機替身，不消耗 API 額度。 |
| 原生回歸 | [核心 518 項](../build/sandbox-core.log)、[多代理 57 項](../build/sandbox-pipeline.log)，均為 0 失敗。多代理使用本機 HTTP 與 CLI 替身。 |
| 介面邏輯 | 桌面整合 18、流程 DOM 40、裁切 16 項，合計 74 項通過。沒有操作原生 WebView2。 |
| CLI | [七組 ConPTY 情境](../build/sandbox-cli-smoke.log)全部通過。首次設定情境曾把 `/flow compact` 與 `/exit` 當成多行訊息；保留[失敗紀錄](../build/sandbox-cli-smoke-first.log)，設定情境獨立重跑及完整重跑均通過，未因這次時序問題改動 CLI 產品程式。 |
| 風格 | [131 個檔案](../build/sandbox-style.log)通過 `--format --js`；風格工具本身 13 項回歸通過。沿用既有介面設計，文件描述具體操作與限制。 |

桌面與 CLI 均以版本 0.6.1 建置：[桌面版](../build/ChatClaudeAgent-0.6.1.exe)、[CLI 版](../build/ChatClaudeAgent-CLI-0.6.1.exe)。[資源與 SHA-256 核對](../build/sandbox-artifacts.json)確認兩版的工作程序及沙盒腳本與原始碼相同；桌面介面逐檔相同，CLI 不含桌面介面，兩版都不含模擬主機。執行中的 0.6.0 保持原樣，待目前工作結束後由使用者關閉，再啟動新版。

## 第二輪複審

前一輪補上的 `connected` 成功條件保留。另以模擬 CLI 重現：兩個工具都成功後，若回合回報錯誤或程序以代碼 7 異常結束，原驗證程式仍會回報 PASS。現在整體成功還需要 `TurnDone` 的狀態為 `completed`，JSON 結果另記錄 `turn_completed`。

新增 `tests/test_sandbox_live.py`，透過真實的原生驗證程式及 stdin／stdout 協定執行五種情境：正常完成、回合失敗、程序異常結束、狀態工具失敗、伺服器未連線。修正前的兩個失敗情境確實得到錯誤的成功結束碼，修正後五項均通過。這組測試使用明確標示的模擬回覆，不呼叫模型或 Docker，不能當作容器隔離證據。

複審另實際重跑：共用沙盒 15 項（包含兩項真實容器／stdio 情境，無跳過）、工作程序 24 項、核心 518 項，以及 Claude 2.1.308 的 `--connect-only`，均通過。[本輪判定回歸紀錄](../build/sandbox-round2-probe.log)供查閱。沒有重新發送訂閱模型提示詞；真實 Claude 呼叫沿用首輪保存且重新核對的證據。

本次只修改測試與說明，兩版 0.6.1 產品執行檔不需重建；重新比對資源與 SHA-256，仍與首輪交付相同。介面及素材未改動，文件維持具體的指令、結果與限制。

## 範圍限制

這次確認的是 Claude 真正呼叫共用沙盒，以及主機測試程式透過同一個 MCP 協定執行真實容器。沒有在這一輪請 GPT 模型自行決定呼叫工具；Codex 的協定與事件轉接由原生回歸測試檢查。原生 WebView2 視窗未操作。

沙盒仍需要已安裝且正在執行的 Docker 與本機映像，只能執行 Linux 容器。Windows 的 MSVC／`build.bat` 使用主機建置，不列為容器測試。缺少引擎或映像時保持阻擋，不會回到宿主執行。

參考：[Claude MCP 文件](https://code.claude.com/docs/en/mcp)、[官方 SDK 的 MCP 狀態控制協定](https://github.com/anthropics/claude-agent-sdk-python/blob/main/src/claude_agent_sdk/_internal/query.py)、[Docker 執行限制](https://docs.docker.com/engine/containers/run/)。
