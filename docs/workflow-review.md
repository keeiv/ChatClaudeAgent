# 0.3.0 第一輪審查與修正

2026-10-24 接續審查。工作開始時的快照為 `2c2e54b`；檔案與第一版完成狀態一致，沒有查到前一輪中斷前留下的新修改。沿用第一版的配色、圖標與原生 CLI 架構。

## 查出的問題

1. 24 欄的精簡流程把成員塞在同一行，第二位主要成員或本地成員會被截掉。新增的姓名／狀態可見性測試在修改前出現兩項失敗。現在空間不夠時換行，保留每位成員的狀態；長自訂名稱仍會依可用欄寬截短。核准與輸入照舊優先。
2. 桌面將受邀成員清單當成傳輸事件的重設條件，移除或加回成員會重播剛顯示過的圓點；只邀一位本地成員也仍有通往缺席成員的分支。現在只在對話或工作回合改變時重設事件紀錄，分支依受邀成員建立。重播問題已先由失敗測試重現。
3. 瀏覽器實測發現，縮小視窗時先量輸入框、才收起側欄，會用過窄的暫時寬度計算高度；空白輸入框因此膨脹到 242 像素。改成側欄完成切換後再量測。已驗證空白框恢復單行高度，三行文字仍可正常擴張，清空後再縮回。

本地審查員 1 的 runner 錯誤沒有產生有效審查結果，不能視為同意。本地審查員 2 提到的紫色漸層、hero、公版三張卡片、Inter 和行銷假資料，沒有對應檔案位置；查閱這次的差異、流程 CSS、字體設定與實際畫面後，未找到這些問題。呼吸亮度與沿線圓點是使用者明確指定的效果，因此保留。

## 本輪驗證

| 檢查 | 結果 |
| --- | --- |
| 核心與終端渲染器 | [342 通過、0 失敗](../build/workflow-review/unit.log)，含 3 項預覽輸出檢查；一般執行為 339 項。 |
| 接手與 App 狀態 | [73 通過、0 失敗](../build/workflow-review/takeover.log)。 |
| 桌面流程元件 | [37 通過](../build/workflow-review/dom.log)，含更動成員不重播與單一本地分支。 |
| 桌面完整腳本 | [18 通過](../build/workflow-review/desktop-smoke.log)，包含進度收合、設定保存和視圖釋放。 |
| 真實 ConPTY | [7 組情境](../build/cli-smoke/run-tv8pyrou)通過，新增工作中切換至 24×18、再恢復寬視窗的錄製。 |
| 錄製解碼 | [11 通過](../build/workflow-review/recording.log)，確認窄視窗的兩位主要成員、權限和說明都可見。 |
| 瀏覽器實繪 | [24 通過](../build/workflow-review/browser-checks.json)：真正的 CSS 轉圈、Web Animation 圓點位移、節點不重疊、收合、780×780 與低高度視窗、空白與多行輸入、減少動態，以及核准／暫停／失敗時不傳輸。 |
| 打包 | 新 CLI 與[桌面建置](../build/workflow-review/desktop-build.log)成功；[資源檢查](../build/workflow-review/resources.json)確認桌面嵌入最新流程、程式、樣式和入口，開發 mock 未嵌入。 |

已查看 [桌面寬視窗](../build/workflow-review/browser-wide.png)、[桌面窄視窗](../build/workflow-review/browser-narrow.png)、[交接圓點](../build/workflow-review/browser-transfer.png)與 [24 欄終端](../build/workflow-review/narrow-terminal.png)。瀏覽器測試程式為 [tests/workflow_browser.js](../tests/workflow_browser.js)，搭配 `PORT=5197` 的 `tools/serve_ui.js`，可由 Playwright 的 `browser_run_code` 載入執行。

這次可用的瀏覽器測試工具補足了第一版未實際繪製畫面的缺口。瀏覽器後端仍是 mock，核准、暫停與失敗的動畫檢查使用明確測試狀態；沒有使用真實模型額度，也未驗證原生 WebView2 視窗、輸入法組字或官方訂閱服務。

CLI 本體在本輪空白群組待命的工作集為 11.88 MiB，交接動畫時為 13.77 MiB，AI 子程序另計。[原始樣本](../build/cli-smoke/run-tv8pyrou/flow/screen.vt.memory.jsonl)。這是單次短對話測量，不代表所有情況，也不能用來比較兩版的效能差距。

## 審查版執行檔

- [ChatClaudeAgent-CLI-review.exe](../build/ChatClaudeAgent-CLI-review.exe)
- [ChatClaudeAgent-workflow-review.exe](../build/ChatClaudeAgent-workflow-review.exe)

兩個檔案的產品版本仍為 0.3.0。[大小與 SHA-256](../build/workflow-review/artifacts.json)。沿用替代輸出路徑，未覆蓋第一版發行檔，也未停止既有桌面工作階段。關閉佔用同一資料目錄的舊程式後，可直接啟動審查版。

以上雜湊與測試數字是該輪紀錄。同名審查執行檔已在[私訊流程複審](workflow-dm-review.md)後更新，最新檔案資訊與驗證結果以該紀錄為準。
