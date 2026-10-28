# ChatClaudeAgent 0.3.0 驗證紀錄

日期：2026-10-24，Windows 本機。從既有 0.2.0 程式接續，工作開始時快照版本為 `a46769a`，不是重新建立 CLI。

## 發行檔

| 檔案 | 位元組 | SHA-256 |
| --- | ---: | --- |
| [ChatClaudeAgent-CLI.exe](../build/ChatClaudeAgent-CLI.exe) | 1,537,536 | `BE65E54EEFEEDED7EA2C7D8BAAFBA5448315F667521E2F5A17A9F4E58DF5F4C6` |
| [ChatClaudeAgent-0.3.exe](../build/ChatClaudeAgent-0.3.exe) | 1,436,160 | `85B15879D6ADEF2C3E5F5AC8B698AAA39BFA9280BF23A3AC2C6CE5660E60B641` |

兩個 EXE 的產品版本均為 0.3.0。舊桌面程式仍在執行，因此新版另存為 `ChatClaudeAgent-0.3.exe`，沒有中斷舊工作階段。關閉舊程式後可直接開啟新版；平常的 `build.bat` 仍以 `ChatClaudeAgent.exe` 為預設輸出。

已逐位元組確認新桌面 EXE 包含目前的 `workflow.js`、`index.html` 和 `app.css`。開發用 `mock.js` 沒有嵌入桌面 EXE，CLI 也沒有嵌入流程圖網頁程式。[檢查結果](../build/workflow-preview/resources.json)、[檔案資訊](../build/workflow-preview/artifacts.json)。

## 已執行的檢查

| 檢查 | 結果與範圍 |
| --- | --- |
| 核心與終端渲染器 | [330 通過、0 失敗](../build/workflow-preview/unit.log)。含來源、獨立本地審查、兩份建議回傳、停止／失敗／核准狀態、完成統計保留、事件上限及 24–120 欄中英文排版；其中 3 項檢查選用的預覽輸出。未設定預覽輸出時為 327 項。 |
| 接手與 App 狀態 | [73 通過、0 失敗](../build/workflow-preview/takeover-build.log)。含既有額度接手和停止情境，以及視覺設定驗證、工具與核准狀態更新、一般串流不重送流程、清除對話。 |
| 桌面流程元件 | [34 通過](../build/workflow-preview/dom.log)。含節點重用、單次傳輸、正反方向、生命週期、系統減少動態、背景頁停止和名稱注入防護。 |
| 桌面完整腳本 | [18 通過](../build/workflow-preview/desktop-smoke.log)。實際執行 `icons.js`、`md.js`、`workflow.js`、`mock.js` 和 `app.js`；四位成員完成協作、設定往返、切換聊天室釋放元件，以及舊有進度收合可再次展開。 |
| 原生 CLI | [7 組情境通過](../build/workflow-preview/smoke.log)。真實 ConPTY，含允許、拒絕及原因、多行輸入、模型／性格／權限／視覺設定保存、48×18 縮放、群組交棒、思考與傳輸、逾時必須失敗、跨程序資料鎖。 |
| 終端錄製解碼 | [9 通過](../build/workflow-preview/recording-checks.log)。兩張思考快照的轉圈不同、兩張交接快照的圓點向 GPT 移動、核准控制可見、完成後保留 2 個檔案 +12 行、隱藏流程後權限與說明仍可見。 |
| 建置與語法 | [CLI 建置](../build/workflow-preview/cli-build.log)、[桌面建置](../build/workflow-preview/desktop-build.log) 成功；三個修改的 JavaScript 檔案語法檢查通過，版本差異空白檢查通過。 |

最後一輪 CLI 產物保存在 [run-f4vk4y5z](../build/cli-smoke/run-f4vk4y5z)。所有後端均為模擬，不會使用模型額度或更動真實對話。

已解碼並查看 [送審畫面](../build/workflow-preview/transfer.png)、[完成畫面](../build/workflow-preview/done.png) 和 [四位成員的渲染器樣本](../build/workflow-preview/four-members.png)。前兩張來自原生 CLI 錄製；四位成員樣本使用明確的核心測試事件，不是四個真實模型的執行證據。PNG 以終端格線和 Windows 字體重建，不能代替 Windows Terminal 字體回退或輸入法測試。

## 記憶體樣本

取自同一輪空白群組的 [原始記錄](../build/cli-smoke/run-f4vk4y5z/flow/screen.vt.memory.jsonl)，100×34 欄列、短對話、模擬後端，單次測量。MiB 為 1,048,576 位元組。

| 時點 | CLI 本體工作集 | CLI 私有提交 |
| --- | ---: | ---: |
| 空白群組待命 | 12.02 MiB | 2.71 MiB |
| 思考轉圈 | 13.93 MiB | 3.10 MiB |
| 等待核准 | 14.03 MiB | 3.13 MiB |
| 送審圓點傳輸 | 14.18 MiB | 3.21 MiB |

工作集與私有提交是不同指標，不應相加。AI 子程序、終端機和 Ollama 模型另外計算，這組數字不是整個工作流程的總記憶體，也不是與 0.2.0 的控制變因比較。長對話和真實模型會有不同結果。

## 尚未驗證

工具列出的原生應用程式與瀏覽器皆為空，建立預覽分頁回傳 `Browser is not available: iab`。因此桌面通過的是 DOM、完整腳本和資源嵌入檢查；WebView2 實際繪製、動畫的視覺平順度與窄視窗像素排版尚未實測。CLI 輸入法組字、不同終端字體、真實 Claude／ChatGPT 訂閱後端及真實 Ollama 推論也未在這次執行。

上述為第一版當時的限制。後續審查已透過可用的瀏覽器測試工具補做實繪與動態驗證，並修正窄終端與輸入框問題；原生 WebView2 視窗和真實模型服務仍未實測，詳見 [第一輪審查紀錄](workflow-review.md)。

重跑指令、控制方式及查閱來源見 [協作流程說明](workflow.md)。
