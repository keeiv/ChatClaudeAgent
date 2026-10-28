# 頭像與資料卡驗證（2026-10-24）

這是 0.4.0 的首次驗證紀錄。後續修正了設定檔保存失敗時的頭像保護，最新版 0.4.1 與驗證結果見 [第一輪審查](profiles-review.md)。

接續前任的群組頭像欄位與媒體驗證草稿完成實作，版本改為 0.4.0。這輪由 GPT 實作與自查，Claude 沒有參與審查。沒有提交或推送 Git，也沒有關閉原本的 CCA。

## 已執行

| 項目 | 結果與證據 |
| --- | --- |
| 核心單元 | [352 項通過、0 失敗](../build/profile-review/core.log)，使用隔離 `CCA_DATA_DIR`。 |
| 接手與協定 | [80 項通過、0 失敗](../build/profile-review/takeover.log)，模擬後端。 |
| 頭像原生流程 | [63 項通過、0 失敗](../build/profile-review/native.log)：WIC 完整解碼、裁切代號與目標綁定、失敗與取消保留原圖、替換清理、啟動清理暫存圖、所有成員與群組保存、重開保留頭像與自訂日期、日曆日期驗證。測試自己建立隔離資料目錄。 |
| 裁切幾何與模型對應 | [16 項通過](../build/profile-review/geometry.log)，涵蓋橫圖、直圖、縮放、邊界、量化包、未知與重新訓練變體。 |
| 瀏覽器操作與繪製 | [26 項通過](../build/profile-review/browser.json)：真的上傳測試圖、拖曳縮放並比對匯出像素與 512 × 512 尺寸、取消、錯誤圖片、AI 與群組頭像、日期覆寫與來源、性格切換、雙向 Tab 焦點循環、窄視窗與跨媒體來源裁切。沒有瀏覽器腳本錯誤。 |
| 原有流程圖與桌面腳本 | [40 項元件](../build/profile-review/workflow.log)、[18 項整合](../build/profile-review/desktop.log)通過。 |
| CLI 情境 | [7 組通過](../build/profile-review/cli-smoke.log)：允許、拒絕、設定保存、群組、流程動畫、逾時失敗、跨程序資料鎖。使用新版 CLI 與真實 ConPTY，後端為模擬。產物在 [run-e28h8k33](../build/cli-smoke/run-e28h8k33)。 |
| 風格工具回歸 | [13 項通過](../build/profile-review/style-tests.log)。 |
| 專案風格 | [編碼、換行、改動行格式、JavaScript 語法](../build/profile-review/style.log)通過，執行 `python tools/check_style.py --format --js`；`git diff --check` 通過。 |
| 建置 | [桌面](../build/profile-review/desktop-build.log)、[CLI](../build/profile-review/cli-build.log)成功。實際 PE 版本均為 0.4.0；直接讀取 EXE 資源，確認桌面每個介面檔與磁碟來源一致、含 `profile_media.js`、不含 `mock.js`；CLI 不包含介面資源。 |

已實際查看[資料卡](../build/profile-review/profile-card.png)、[裁切畫面](../build/profile-review/crop-editor.png)、[窄視窗](../build/profile-review/crop-narrow.png)。沿用既有字體、色票與資料卡結構；圓形遮罩用來預覽裁切結果，沒有新增裝飾漸層、發光或行銷文案。

自查修正了無效日期連帶變更名稱、測試遍歷暫存 JSON 的生命週期、窄視窗預覽卡擋住表單，以及資料卡鍵盤焦點跑到背景的問題。模型日期另外區分 Qwen2.5-Coder 的首批與新增尺寸，未知型號不猜日期；來源見 [日期查證](profiles.md#日期查證)。

## 執行檔

- [ChatClaudeAgent-0.4.exe](../build/ChatClaudeAgent-0.4.exe)
- [ChatClaudeAgent-CLI-0.4.exe](../build/ChatClaudeAgent-CLI-0.4.exe)
- [大小與 SHA-256](../build/profile-review/artifacts.json)

目前的工作結束後，由使用者關閉舊版，再啟動新版；共用同一個資料目錄時只能執行一個。原本的對話與設定沿用，CLI 保留只用名稱與性格的操作。

## 尚未驗證

原生 WebView2 視窗及 Windows 檔案選擇器內的完整操作沒有實測。瀏覽器測試使用實際 DOM、Canvas、圖片解碼與模擬主機；跨來源案例重建桌面兩個來源與 CORS 標頭，不能當作原生 WebView2 已測過。原生 WIC、檔案寫入與重開保存則由 C++ 測試獨立驗證。

沒有執行真實訂閱模型協作，也沒有使用使用者的私人圖片、對話或設定測試。`build/` 是本機產物，不進 Git；這份文件連到的測試紀錄與 EXE 只在本機存在。
