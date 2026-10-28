# Gemini Google 帳號、私訊、群組與流程審查（1.0.1-oss）

## 1.0.1-oss：帳號停用的判斷、額度查詢與一般流程

**額度查不到的原因。** 官方 Gemini CLI 文件寫明：免費層與 Google One 帳號的 Gemini CLI 已於 2026-06-18 由 Antigravity CLI 取代。這類帳號的健康檢查（`loadCodeAssist` 的 `HEALTH_CHECK`）仍回報「Gemini Code Assist for individuals」，但官方 CLI 實際設定使用者時送出的請求會得到 `ineligibleTiers: [{ reasonCode: "UNSUPPORTED_CLIENT" }]`，額度端點則回 403。1.0.0 只做健康檢查，所以把帳號顯示為「已通過」、剩餘額度「暫時未知」。

1.0.1-oss 照官方 CLI 的 `setupUser` 再送一次同樣的設定請求（只讀，不會建立專案或開通）：
- 回報 `UNSUPPORTED_CLIENT` 時，狀態改為「個人帳號已停用」（`retired`），說明 Google 的變更，帳號卡提供「查看官方說明」與「改用其他帳號登入」，不再顯示可用。
- 仍受支援的帳號，改用與官方 CLI 相同、綁定專案與方案的伺服器查詢 `retrieveUserQuota`；Google 回 403 時明示「沒有 Gemini Code Assist 授權」，其他錯誤仍標為未知。
- 測試以本機替身 SDK 涵蓋停用、授權不足、查詢失敗與正常回傳，不使用真實帳號。

**私訊。** 每個安裝都有「Gemini」私訊（`dm-gemini`），和 Claude、GPT 的私訊一樣保存工作階段（`--resume`），並支援日常模式、權限、專案資料夾、清除對話與停止。Gemini CLI 沒有模型目錄，模型與思考程度一律顯示「Gemini CLI 預設模型」；`/model`、`/effort` 在 CLI 會說明原因，不猜測模型名稱。帳號未通過或已停用時，輸入框直接顯示原因。

**一般群組。** 頻道設定新增「Gemini 成員」開關（CLI：`/gemini on|off`）。加入後群組依「主刀 → 搭檔 → Gemini」的順序輪替：Gemini 是第三位審查者，可直接修改檔案。共識改為「最新修改的作者以外，每位仍可工作的成員都同意，且沒有未回應的疑慮」；兩人群組的行為與以前相同。

**額度不足時跳過。** 群組的「額度用完時」預設為「跳過，交給下一位」：
- 已知額度用完（Claude 的額度事件、GPT 回報 100% 的視窗、Gemini 各模型剩餘 0、或帳號已停用）的成員，輪到時直接跳過，聊天出現黃色「跳過」標記並寫明原因，每位成員每輪只標記一次。
- 某一輪因額度被拒（錯誤代碼或訊息含用量上限、quota、RESOURCE_EXHAUSTED 等）時，該輪改交給輪替中的下一位；Claude 仍沿用原本附交接紀錄的接手方式。沒有回報重置時間時，十五分鐘內不再派給該成員。
- 只剩一位成員能工作時，由它完成並自查，不宣稱其他成員已審查。
- 全部成員都沒有額度時強制停止：任務狀態保存為「上次中斷」，記錄停止時間、第幾輪、哪一階段、已跳過的成員、已修改的檔案與「下一步由誰做什麼」。聊天顯示紅色的強制停止摘要與「額度恢復後接續任務」按鈕，任務紀錄卡也寫明停止原因；接續時從記錄的下一步開始。
- 多代理流程採同樣規則：沒有額度的節點標記跳過並略過，該審查者不算未通過，但至少要有一位審查者實際通過；所有審查者都被跳過時暫停，全部節點都沒有額度時強制停止並保存游標與下一步。
- 設定 → 額度與自動接續可在額度恢復後自動接續強制停止的任務，見 [版本控制與額度自動接續](git.md)。
- 私訊沒有其他成員可接手：Claude 私訊仍依設定交給 GPT；其他情況顯示「額度不足」與已保留的需求。

## 1.0.0-sol 與 1.0.1- 的設計

在「訂閱與用量」的 Gemini 卡片檢查或登入。需要 Node.js 與官方 npm 安裝的 `@google/gemini-cli`，執行檔預設從 PATH 或 `%APPDATA%\npm\gemini.cmd` 尋找，也可在執行檔設定指定。CCA 不自動安裝 CLI。

按「登入」後會開啟官方 CLI 視窗，選擇 **Login with Google** 完成登入，登入後直接按「重新檢查」，不必先關閉 Gemini 視窗。帳號能否使用以官方 CLI 與 Google 帳號回報為準；2026-06-18 起免費層與 Google One 個人帳號不再能用 Gemini CLI（見上節）。Google 密碼與 OAuth 權杖不進入 CCA 設定或聊天。

1.0.0-sol 修正 npm CLI 0.65.0 的打包相容性：新版只提供 bundle，沒有舊版的獨立 core 套件。CCA 會從所選安裝的實際入口追蹤 SDK 匯出；不執行 CLI 主程式、不載入舊的孤立區塊，也不搜尋目前專案的套件。仍保留舊版分離 SDK 的支援。

套件不相容、網路／登入驗證未完成、Google 服務拒絕與額度未知分開處理。Google 驗證已通過但剩餘額度查不到時，登入仍顯示已通過；服務檢查遭拒時顯示「已登入，服務檢查未通過」，不宣稱能開始模型任務。遇到不確定錯誤不清除憑證，也不把原始 SDK 例外或 HTTP 標頭傳進介面。

群組的「頻道設定 → 多代理流程 → 新增 Gemini 審查者」會加入可讀取、修改檔案的第二位審查者，並選擇完整流程，避免簡單路由略過第二位審查者。可組成 Claude 撰寫 → GPT 審查修正 → Gemini 審查修正；實際差異會交回下一輪，直到必要審查與測試閘門滿足。各節點分開保存工作階段，停止、暫停及回滾也會處理 Gemini。

1.0.1- 在「設定 → Gemini」提供顯示名稱、裁切頭像、橫幅、日期與性格。點流程成員或訊息頭像可開資料卡並編輯；性格庫支援 Gemini 專用與通用設定，下一輪會傳入實際 Gemini 子程序。角色資料由本機管理，發行版沒有指定角色。

桌面流程圖依設定順序排列，超過三個節點時換列；每個節點保留獨立 ID，同一 Gemini 的多個節點不會一起亮起。成員列表、訊息與交接並列節點別名和目前個人名稱；側欄另顯示參與帳號的額度。CLI 的流程、`/name`、`/persona` 也支援 Gemini。停止 Gemini 時只結束該節點並保存任務停止狀態，不改寫 GPT 的訊息。

Gemini 尚未提供 CCA 可使用的模型目錄，因此目前使用官方 CLI 的預設模型，不接受猜測的模型 ID。它沿用專案及群組權限；`edit` 對應 `auto_edit`，`ask` 對應 `default`，只有使用者原先選擇 `bypass` 才用 `yolo`。無介面模式無法回覆互動權限卡，需確認的工具會遭拒絕；不能為了跑測試自行放寬權限。官方無介面 `plan` 可能轉入 YOLO，所以 CCA 的唯讀規劃模式會阻擋 Gemini。

方案與各模型剩餘額度透過已安裝 CLI 的官方 SDK 查詢 `loadCodeAssist`、`retrieveUserQuota`，不發送模型提示詞。帳號檢查成功後每 60 秒更新，也可手動更新。SDK 版本不相容、帳號未啟用、查詢失敗或欄位缺漏，均明示未知或錯誤；不把一般 Gemini 網頁方案猜成 CLI 方案。CLI 每輪 Token 另行累計，與 Google 剩餘額度分開顯示。

串流工具結果沒有獨立的 shell 結束碼，不能把工具文字中的「測試成功」當成必要測試證據；需要測試的流程仍須取得主機驗證證據。退出碼非零或沒有終端 `result` 都判定該輪失敗。

CLI 使用 `/gemini-login` 登入、`/status` 查看方案與額度、`/pipeline 檔案.json` 載入包含 `backend: "gemini"` 的流程。桌面及 CLI 共用相同代理與持久任務。

```json
{
  "routing": "full",
  "maxCycles": 3,
  "requireTests": false,
  "nodes": [
    { "id": "author", "name": "Claude", "backend": "claude", "role": "write" },
    { "id": "reviewer", "name": "GPT", "backend": "codex", "role": "review" },
    { "id": "reviewer-gemini", "name": "Gemini", "backend": "gemini", "role": "review" }
  ]
}
```

上述範例關閉必要測試閘門，適合檢查純文字工作；程式開發應依驗收需求設為 `true`，取得主機證據後才能驗收。

官方來源：[登入](https://geminicli.com/docs/get-started/authentication/)、[串流協定](https://geminicli.com/docs/cli/headless/)、[無介面計畫模式](https://geminicli.com/docs/cli/plan-mode/)、[額度查詢實作](https://github.com/google-gemini/gemini-cli/blob/main/packages/core/src/code_assist/server.ts)、[額度與方案（含 2026-06-18 個人帳號變更公告）](https://geminicli.com/docs/resources/quota-and-pricing/)。
