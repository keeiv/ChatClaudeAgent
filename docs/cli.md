# ChatClaudeAgent CLI

CLI 與桌面版共用 C++ 的對話、權限、存檔和協作核心。CLI 不建立 WebView2，也不載入頭像或橫幅圖檔；名稱與角色性格用 `/name`、`/persona` 設定。

## 開啟與操作

在專案資料夾開啟 Windows Terminal，執行 `ChatClaudeAgent-CLI.exe` 的完整路徑。可附上 `claude`、`gpt`、`group` 或既有群組名稱。`--help` 和 `--version` 可直接輸出到管線，不會啟動 AI。

第一次開啟會沿用現有對話；沒有設定專案的對話會使用目前資料夾。已選過專案的對話保留原設定，使用 `/cd .` 才會切換。

| 指令 | 用途 |
| --- | --- |
| `/chat`、`/new 名稱` | 切換私訊或群組、建立群組 |
| `/groups`、`/groups up`、`/groups down` | 依固定順序列出群組、將目前群組向前或向後移動；與桌面共用排序 |
| `/cd 路徑` | 設定專案，支援空白路徑與 `~` |
| `/model`、`/effort` | 選模型及思考程度 |
| `/auto on\|off [claude\|codex]` | 同時開關該代理的自動模型及思考程度 |
| `/auto model\|effort on\|off [claude\|codex]` | 分別調整兩項自動偏好；手動模型選擇只關閉對應項目 |
| `/auto preview 需求` | 預覽目前主刀的選擇原因，不呼叫模型 |
| `/theme`、`/theme claude` | 列出主題或套用指定 ID，與桌面共用保存設定 |
| `/permission` | 選權限模式 |
| `/lead claude`、`/lead gpt` | 選主刀手 |
| `/takeover`、`/fallback on` | 手動接手；額度耗盡的成員自動跳過（私訊由 GPT 接手 Claude），全部用完時強制停止並記錄下一步 |
| `/gemini on`、`/gemini off` | 群組邀請或移除 Gemini，擔任第三位審查者 |
| `/git`、`/git select`、`/git commit 訊息`、`/git commit-push 訊息` | 版本控制：以黃／綠／紅標示變更，選檔、提交並推送；`/git commit! 訊息` 在確認不是金鑰後強制提交 |
| `/git push`、`/git pull`、`/git fetch`、`/git auto fetch on`、`/git auto pull on` | 推送、快轉拉取、擷取與自動同步 |
| `/git never 規則`、`/git allow 規則` | 新增或移除永遠不提交規則；不加規則時列出全部 |
| `/git login`、`/git identity 名稱 e-mail` | 遠端需要登入時開啟 Git 認證管理員的登入視窗；設定提交身分，加 `--repo` 只套用到目前倉庫 |
| `/resume-auto on`、`/resume-auto off` | 所有 AI 額度用完而停止的任務，在額度恢復後自動接續 |
| `/name`、`/persona` | 改名稱、選用或建立角色性格 |
| `/activity daily\|weekly\|total` | 查看最近 14 個日期的每日、近 7 日或已保存紀錄累計用量 |
| `/about` | 查看開發目的與設計原則 |
| `/badges` | 查看本機持有與未取得的七款徽章，含展示／隱藏／收藏狀態 |
| `/badges hide\|show\|first ID` | 隱藏、顯示或將已持有徽章排到第一位，與桌面共用展示設定 |
| `/humanize on` | 啟用去 AI 化，保留桌面版已設定的補充規則 |
| `/flow auto`、`/flow compact`、`/flow off` | 完整、自動精簡或隱藏協作流程圖 |
| `/motion on`、`/motion off` | 控制呼吸燈、思考轉圈和交接圓點 |
| `/show` | 印出最近一則 AI 回覆的工具輸出、diff 與文字 |
| `/status` | 看登入狀態、用量和本體記憶體，重新檢查登入 |
| `/inbox` | 列出所有對話的核准、提問、計畫與完成通知 |
| `/plan`、`/plan import JSON檔` | 檢視計畫的版本與步驟，或匯入新草稿 |
| `/plan-comment 文字`、`/plan-approve`、`/plan-run` | 留言、核准目前版本、另行執行；操作前先用 `/plan` 檢查 |
| `/plan-accept step-1` | 人工驗收指定步驟 |
| `/opinions claude,codex 問題`、`/opinions` | 蒐集獨立唯讀意見，或檢視回覆與摘要 |
| `/opinions-stop`、`/opinion-plan` | 停止蒐集，或把提案存成待核准計畫 |
| `/focus`、`/focus codex security` | 檢視 Markdown 關注點與指定情形，或指定成員關注點 |
| `/focus-import JSON檔` | 匯入完整關注點清單（陣列內為 id、name、text） |
| `/workspace create`、`/workspace check`、`/workspace merge` | 建立 worktree、預演合併、合併剛檢查且版本相符的已提交變更 |
| `/stop`、`/continue`、`/end` | 停止執行、繼續或結束暫停的協作 |
| `/help`、`/exit` | 看完整指令、保存並離開 |

Enter 送出，Shift+Enter 或 Ctrl+J 換行，Tab 補完斜線指令。方向鍵可選單或找回輸入紀錄。權限請求用方向鍵或數字選擇，Enter 確認；Esc 拒絕後可填原因。Esc 也可中斷正在執行的工作。

建議至少 80 欄、24 列。字體由終端機決定；Windows Terminal 可用 Cascadia Mono 搭配中文字體回退。CLI 以雙欄處理中日韓文字，表情符號的組合字寬仍可能隨終端機而不同。

插件安裝、Ollama 模型下載、每日用量圖表與回滾目前仍由桌面版操作。CLI 可邀請已在桌面版設定好的本地唯讀審查員。

計畫、意見、關注點與 worktree 的完整流程及 JSON 範例見 [協作控制](collaboration.md)。合併操作保留分支和 worktree；同一隔離對話不能改成另一個專案，請另建對話。

## 記憶體與資料

完成的顯示行只輸出一次，CLI 不保存整張畫面的渲染樹。後續更新只重畫輸入、選單和未完成內容，閒置時不持續重畫。終端機原生捲動紀錄仍可使用，`/show` 會把明細印到捲動紀錄。

對話核心會載入保存的訊息，記憶體會隨紀錄量增加。`/status` 的數字來自 `GetProcessMemoryInfo`，工作集是目前在實體記憶體中的頁面，私有用量是該程序的私有提交量；兩者不應相加。Claude Code、Codex、終端機及本地模型須另計。[Microsoft 欄位定義](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters_ex)。

對話繼續放在 `%APPDATA%\CCA`。桌面版與 CLI 對同一資料目錄有互斥鎖；請先離開其中一個，再開另一個。`CCA_DATA_DIR` 可指定獨立測試資料；同一目錄的相對路徑、尾端斜線與目錄別名會共用鎖。

## 設計調查

2026-10-23 查閱官方文件後，保留既有 CLI 方向：

| 查閱來源 | 本專案採用的做法 |
| --- | --- |
| [Codex CLI](https://learn.chatgpt.com/docs/codex/cli) | 名稱、工作目錄、模型與狀態以文字呈現；用細邊框區分輸入區。 |
| [Codex 指令與快捷鍵](https://learn.chatgpt.com/docs/developer-commands?surface=cli) | `/` 指令選單、模型與權限選擇、方向鍵輸入紀錄。CCA 的指令範圍由自身功能決定。 |
| [Claude Code 互動模式](https://code.claude.com/docs/en/interactive-mode) | 工作狀態、Esc 中斷、Ctrl+C 退出提示，以及獨立查看工具明細的入口。 |
| [Claude Code 終端機設定](https://code.claude.com/docs/en/terminal-config) | 換行同時保留 Shift+Enter 與 Ctrl+J，避免只依賴一種終端機鍵盤協定。 |
| [Claude Code 全螢幕渲染](https://code.claude.com/docs/en/fullscreen) | 查閱替代緩衝區與可見訊息渲染的取捨。CCA 使用主緩衝區，讓終端機保存捲動紀錄。 |
| [Microsoft VT 控制序列](https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences) | UTF-8、游標移動、清除局部區域與真彩色。工具文字中的控制碼過濾後才輸出。 |
| [Microsoft ConPTY](https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session) | 用真正的偽終端機驗證輸入、縮放和輸出；讀寫管線分開處理。 |
| [xterm.js 編碼](https://xtermjs.org/docs/guides/encoding/) | 測試把錄製的 UTF-8 位元組交給終端機解碼器，確認中文與控制序列重播。 |

配色沿用專案既有的 Claude 橘與 GPT 綠，再以淡藍色表示 CCA 的輸入和選取狀態；這是本專案的識別配色。圖標採用 BOT. 的色塊間隙、切角和圓點，詳細規格在 [圖標說明](../assets/logo/README.md)。

## 重跑驗證

```powershell
.\build.bat test
.\build.bat cli
.\build.bat pty
.\build.bat takeover-test
python tests/cli_smoke.py
```

單元測試涵蓋中英文欄寬、Markdown、串流穩定行、權限、控制碼過濾與資料鎖。接手測試涵蓋額度耗盡、手動停止、GPT 自查和不可用情況。CLI smoke 使用真實 ConPTY、隔離資料目錄和模擬後端，檢查允許、拒絕、設定保存、多行輸入、縮放、群組交棒與測試逾時失敗。

每輪錄製、記憶體樣本和保存後的 JSON 留在 `build/cli-smoke/run-*`。不會修改使用者對話，也不會使用模型額度。模擬後端不驗證官方服務的登入、用量限制或真實模型品質。

可在瀏覽器用 `tests/pty_view.html` 重播，或安裝開發用解碼器，把指定快照轉成文字與 PNG。這些套件不隨 CLI 發行：

```powershell
npm install --prefix build/cli-qa --ignore-scripts @xterm/headless@5.5.0 @xterm/addon-unicode11@0.8.0
node tests/replay_cli.cjs <screen.vt> 100 34 approval build/cli-preview/approval
python tests/render_cli.py build/cli-preview/approval.json
```

PNG 使用 Consolas 和微軟正黑體重建錄製畫面，可查排版、文字及選單；字型回退、滑鼠選取和輸入法組字仍需在實際終端機確認。

2026-10-23 的建置、測試結果及記憶體樣本見 [0.2.0 驗證紀錄](cli-validation.md)。0.3.0 新增的工作流程、動態控制與調查來源見 [協作流程說明](workflow.md)。

## 工作台與更新

- `/context`：查看本機實際組合的交付文字與規則；外部 CLI 內部歷史標記未知。
- `/queue [add 文字|edit ID 文字|move ID 順位|remove ID|retry ID|pause|resume]`：管理持久待送指令，順位從 1 起算。忙碌期間一般輸入自動加入佇列。
- `/update [check|install|cancel|open|auto on|off]`：掃描自動建立的本機更新資料夾、安裝已核對的安裝包、取消這次自動更新、用檔案總管開啟資料夾，或切換自動更新。遠端發行站未啟用，`source` 只會回覆這一點。例行掃描不輸出，只有 `/update` 之後或出現可安裝版本、排程、錯誤時才顯示。

桌面欄寬不影響終端機；CLI 的閱讀寬度與字型由終端機控制。完整邊界與失敗行為見 [工作台與軟體更新](workbench.md)。

Gemini 的顯示名稱與性格可透過 `/name`、`/persona` 選取 Gemini 設定，與桌面共用本機資料。多代理流程依節點順序列出實際狀態；終端高度不足時保留目前節點摘要。Gemini 只使用 CLI 預設模型，不列入 `/model`、`/effort` 的可選目錄。啟動時加上 `gemini` 直接開 Gemini 私訊（例如 `ChatClaudeAgent-CLI.exe gemini`）；聊天中被跳過的成員以「» 跳過」一行標示，全部額度用完的強制停止摘要以紅色方塊標示。
