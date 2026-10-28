# ChatClaudeAgent 協作流程

CLI 和桌面版使用同一份工作狀態。原本的雙方協作顯示 Claude、GPT 與已邀請的本地審查員；可配置的多代理流程改依設定順序顯示，每個節點有獨立 ID，支援 Gemini 及同一後端的多個節點。名稱沿用目前個人資料，節點別名不同時一併顯示。CLI 不載入頭像、瀏覽器或圖表套件。

私訊只有一位 AI 參與本次工作時，不顯示流程圖。兩位 AI 在同一次工作中接手才顯示交接；接手後的新要求若只有 GPT 回覆，流程仍保持隱藏，不會因為對話原本屬於 Claude 而補上一個待命節點。

多代理桌面圖每列最多三個節點，依順序折返排列；交接線只連接已設定的相鄰節點，動態圓點仍以主機實際交接為準。Gemini 不再共用 GPT 的訊息或思考狀態。同一個 Gemini 後端放在不同階段時，只顯示當前節點忙碌。CLI 依序列出節點及狀態，終端空間不足時保留目前節點摘要。

## 看圖與控制

| 畫面 | 代表的工作 |
| --- | --- |
| 節點轉圈 | 後端回報思考中、建立檢查點或載入本地模型。 |
| 節點呼吸亮起 | 撰寫、審查或使用工具。CLI 改變字元亮度，桌面改變節點邊緣的透明度。 |
| 圓點沿線移動 | 已開始下一位成員的回合。每次交接只播一次，約 1.3 秒；不會為了特效延遲後端。 |
| 黃色狀態 | 等你核准、回答或協作暫停；流程圖停止傳輸。核准與提問卡片保持可見。 |
| 勾號／叉號／停止 | 該成員完成、失敗或被中斷。輪數和檔案增刪統計在協作完成後保留到下次工作。 |

本地審查員分別收到主刀的變更，兩人的建議再交給主要審查員。圖形不會把「主刀 → 本地 1 → 本地 2」畫成資料來源。沒有設定模型的受邀位置會標示「未設定模型」。本地模型仍依既有排審規則執行，流程圖不會自行啟動、下載或額外呼叫模型。

CLI 的 `/flow` 開啟選單，或直接輸入：

```text
/flow auto       寬視窗顯示連線，空間不足時顯示文字狀態
/flow compact    固定顯示精簡狀態
/flow off        隱藏圖形
/motion off      關閉呼吸燈、轉圈和傳輸圓點
/motion on       重新啟用動態，仍尊重 Windows 的動畫設定
```

CLI 完整圖形需要至少 68 欄和足夠的空白列。核准、提問、指令選單和多行輸入優先使用畫面空間。底列優先保留權限與 `/help`，長路徑會截短。

桌面版把流程放在聊天標題下方，可按「收合」和「動態開啟／減少動態」。標題右側的流程按鈕可隱藏、恢復圖形。視窗太窄或高度不足時自動顯示文字狀態。兩版共用 `settings.json` 的 `visual.mode` 與 `visual.motion`。Windows 減少動態的系統偏好優先，CLI 在啟動時讀取，桌面版會隨偏好改變更新。

流程只追蹤這次執行期間的工作。重開程式不重播過去的傳輸，聊天紀錄仍照常保存。切換聊天室不會累積計時器或保留離開聊天室的動畫。

## 調查與設計取捨

以下資料於 2026-10-24 查閱，接續 [0.2.0 CLI 調查](cli.md#設計調查)。動態的用途是辨認誰在工作、資料交給誰；沿用淡藍色、Claude 橘、GPT 綠及現有兩個本地成員的識別色。未更換字體與品牌圖標。

| 來源 | 採用的做法 |
| --- | --- |
| [Command Line Interface Guidelines：進度與輸出](https://clig.dev/#robustness) | 長工作持續顯示進度，工具細節和錯誤仍可查閱；保留 `/show` 和終端捲動紀錄。 |
| [Microsoft：VT 控制序列](https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences) | 使用原生文字、真彩色和游標更新；不加終端網頁層。 |
| [Microsoft：SystemParametersInfoW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-systemparametersinfow) | CLI 讀取 `SPI_GETCLIENTAREAANIMATION`，系統停用動畫時使用靜態狀態。 |
| [W3C：Animation from Interactions](https://www.w3.org/WAI/WCAG22/Understanding/animation-from-interactions.html) | 使用者可關閉非必要動態，關閉後保留狀態文字與交接方向。這不是整個程式的 WCAG 符合性宣告。 |
| [MDN：prefers-reduced-motion](https://developer.mozilla.org/en-US/docs/Web/CSS/Reference/At-rules/@media/prefers-reduced-motion) | 桌面依照系統偏好減少動態，CSS 與 JavaScript 都處理。 |
| [web.dev：高效能動畫](https://web.dev/articles/animations-guide) | 桌面動畫使用 `opacity` 和 `transform`，不逐幀變更版面大小，也不加入無條件 `will-change`。 |
| [MDN：Element.animate](https://developer.mozilla.org/en-US/docs/Web/API/Element/animate) | 使用短生命週期的 Web Animations 圓點，結束或切換視圖時移除。 |
| [MDN：Page Visibility API](https://developer.mozilla.org/en-US/docs/Web/API/Page_Visibility_API) | 隱藏視窗時停止傳輸動畫，回到視窗不重播已消耗的事件。 |
| [LinkeDOM 專案](https://github.com/WebReflection/linkedom) | 僅在開發測試使用 DOM；不隨桌面或 CLI 發行。 |

## 實作與驗證

核心只保留最近 8 筆真正的交接事件、每位成員本輪的訊息 ID 及完成統計，動畫不會追加紀錄。CLI 只在工作中更新動態，閒置不持續重畫。桌面重用節點，沒有流程圖專用的逐幀 JavaScript 計時器。

測試使用隔離資料目錄和模擬 Claude／Codex 後端，不使用真實模型額度。四位成員的來源與返回建議另外由核心、終端渲染器和桌面完整腳本驗證。這些測試不代表已驗證官方訂閱服務或真實 Ollama 推論。

```powershell
.\build.bat test
.\build.bat cli
.\build.bat takeover-test
.\build.bat pty
python tests/cli_smoke.py

# 下列套件只用於測試與錄製解碼。
npm install --prefix build/workflow-qa --no-audit --no-fund --ignore-scripts linkedom@0.18.12
npm install --prefix build/cli-qa --ignore-scripts @xterm/headless@5.5.0 @xterm/addon-unicode11@0.8.0
node tests/workflow_ui.cjs
node tests/desktop_smoke.cjs
node tests/workflow_recording.cjs build/cli-smoke/run-實際目錄/flow
```

`workflow_recording.cjs` 驗證真實終端錄製中的轉圈、圓點位置、核准卡片、完成統計及隱藏效果。檔案位於每次測試輸出的 `build/cli-smoke/run-*`。`tests/replay_cli.cjs` 和 `tests/render_cli.py` 可將快照解碼為文字和 PNG。

需要檢查四位成員的終端渲染器時，可設定 `CCA_TEST_WORKFLOW_OUT` 後執行 `build/tests.exe`；這會額外輸出 `four.vt`、兩張快照位置與狀態 JSON，內容明確使用測試事件。桌面瀏覽器預覽可執行 `node tools/serve_ui.js`，開啟 `http://localhost:5178/#flow`，以模擬後端示範四位成員。

本次執行結果、記憶體樣本和發行檔見 [0.3.0 驗證紀錄](workflow-validation.md)。

後續的窄終端修正、成員異動處理與瀏覽器實繪檢查見 [第一輪審查紀錄](workflow-review.md)。

私訊接手後的節點判斷與隱藏驗證見 [私訊流程複審](workflow-dm-review.md)。
