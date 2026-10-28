# 私訊流程複審

2026-10-24 以快照 `f2dea64` 為基準，接續前一輪的私訊收合修改與審查成果。

## 修正

Claude 私訊曾由 GPT 接手後，下一次要求會建立新的工作回合，對話種類仍是 `claude`。原判斷把這個種類當成 Claude 有參與的證據，即使本次只有 GPT 開工，也會多畫 Claude 待命節點。新增回歸測試先得到兩項失敗，再改為只計算本次已開工或正在輸出的成員。真正由兩位 AI 接手的同次工作仍保留節點與交接。

桌面另加上節點數量防護。單節點的隱藏備援狀態即使帶有未消耗的傳輸事件，也不會建立 Web Animation；這個生命週期測試在修正前失敗。開發用 mock 同步採用實際參與者判斷，避免預覽與正式核心不一致。

沒有更換圖標、字體或色盤。核對 `ui/app.css` 的流程樣式與瀏覽器寬、窄視窗後，本地審查員所稱留白過大、素材過度光滑與無理由霓虹，沒有對應到本輪變更：流程區塊使用 7–9 像素垂直內距、沿用既有中文字體，沒有新增美術素材。低透明度的節點呼吸效果是使用者指定的工作提示。另一份本地建議只依文字描述表示沒有問題，也不能代替實測。

## 驗證

| 檢查 | 本輪結果 |
| --- | --- |
| 核心及終端渲染 | [346 通過、0 失敗](../build/workflow-dm-review/unit.log)。含 GPT 單獨接續 Claude 私訊時不建立節點。 |
| 接手及 App 狀態 | [80 通過、0 失敗](../build/workflow-dm-review/takeover.log)。新增已由 GPT 主導、Claude 未登入的私訊，確認有歷史 Claude 訊息也不產生本次的假節點。 |
| 桌面元件 | [40 通過](../build/workflow-dm-review/dom.log)。含空節點、單節點隱藏與取消傳輸，以及返回群組恢復。 |
| 完整桌面腳本 | [18 通過](../build/workflow-dm-review/desktop-smoke.log)。 |
| 瀏覽器實繪 | [27 通過](../build/workflow-dm-review/browser-checks.json)。私訊流程高度為 0、沒有節點與圓點，返回群組恢復四位成員；原有動態、縮放與輸入框檢查通過。 |
| 真實 ConPTY | [7 組情境通過](../build/workflow-dm-review/smoke.log)，錄製存於 [run-6y64iy5s](../build/cli-smoke/run-6y64iy5s)。涵蓋核准、拒絕、設定、群組、流程、逾時與資料鎖。 |
| 終端錄製 | [11 項通過](../build/workflow-dm-review/recording.log)，含思考轉圈、送審圓點移動、24 欄成員狀態和核准控制。 |
| 建置與資源 | [CLI](../build/workflow-dm-review/cli-build.log)與[桌面](../build/workflow-dm-review/desktop-build.log)建置成功。[資源檢查](../build/workflow-dm-review/resources.json)確認桌面嵌入最新程式與樣式，開發 mock 沒有嵌入。 |

已檢視[私訊畫面](../build/workflow-dm-review/browser-dm.png)、[群組畫面](../build/workflow-review/browser-wide.png)與[送交本地審查的圓點](../build/workflow-review/browser-transfer.png)。瀏覽器使用開發 mock，狀態錯誤與減少動態檢查使用明確測試資料；原生接手測試使用模擬的協定後端。沒有驗證原生 WebView2 視窗、輸入法或真實訂閱服務。

本輪 CLI 本體待命工作集為 12.01 MiB，交接時為 13.93 MiB，見[原始記憶體樣本](../build/cli-smoke/run-6y64iy5s/flow/screen.vt.memory.jsonl)。這是單次短對話，AI 子程序及終端機另計。

## 審查版執行檔

- [ChatClaudeAgent-CLI-review.exe](../build/ChatClaudeAgent-CLI-review.exe)
- [ChatClaudeAgent-workflow-review.exe](../build/ChatClaudeAgent-workflow-review.exe)

產品版本維持 0.3.0，同名審查檔已更新為本輪內容；[檔案大小與 SHA-256](../build/workflow-dm-review/artifacts.json)。未停止使用中的桌面程式，也未覆蓋其執行檔。使用相同資料目錄時，請先關閉舊程式再啟動審查版。
