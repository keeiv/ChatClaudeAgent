# 頭像功能第一輪審查（0.4.1）

2026-10-24 重新讀取目前檔案與差異，由 GPT 查證、修正與測試。Claude 沒有參與本輪審查，沒有提交或推送 Git，執行中的 CCA 沒有被關閉。

## 修正

0.4.0 只確認 PNG 寫入成功，就刪掉舊圖並回報完成，頭像參照所在的設定檔留待之後保存。設定檔無法寫入時，重開後會指向已刪除的舊圖片。

在隔離資料目錄鎖住 `settings.json.tmp` 與 `conversations.json.tmp` 重現後，[原版故障測試出現 13 項失敗](../build/profile-round1/native-baseline.log)，包括後續重開保存的連帶失敗。

修正版先保存新的頭像參照，成功後才刪除舊圖、回報完成。保存失敗時還原記憶體中的參照、刪除未使用的新 PNG，保留原圖和裁切內容，並允許用同一個裁切重試。個人資料、橫幅和移除頭像使用相同的保存順序；刪除群組也會先確認群組清單可以寫入，失敗時保留群組、訊息和頭像。群組頭像不會建立空白的個人資料位置。

`persistProfile`、`persistGroupAvatar` 在 `src/profile.cpp`，群組清單保存由 `src/app.cpp` 的 `saveConversations` 共用，沒有新增介面訊息。CLI 沿用共用核心，仍只設定名稱與性格。

## 本地建議查證

- `ui/app.css:45` 的字體沿用 gg sans、Segoe UI、微軟正黑體及 Noto Sans TC，這輪沒有新增 Inter。檢查新增 CSS 與實際畫面，沒有 hero、三張等寬卡片或裝飾漸層。
- CSS 的三處 gradient 是既有長文收合遮罩與單選鈕圓點。裁切圓框和暗化邊角用來預覽頭像範圍；沒有新增霓虹、材質或美術素材，不因這些功能用途要求換風格。
- `src/app.cpp` 的 `kDailyNote` 在日常模式加入模型指令，內容限制主動讀檔與執行工具，屬於實際功能文案，保留。
- `data:image/png;base64` 是 Canvas 匯出圖片的編碼，不是圖像生成提示詞；材質、構圖和光線的建議不適用。`mock.js` 的示例資料只供測試，已檢查不會編入 EXE。新增提示與資料卡沒有標題 emoji、行銷套話或虛構評價。

## 本輪驗證

| 檢查 | 結果 |
| --- | --- |
| 原生頭像與持久化 | [83 項通過](../build/profile-round1/native.log)，新增設定寫入失敗、重試、移除及刪除群組保護。 |
| 核心與接手 | [352 項核心](../build/profile-round1/core.log)、[80 項接手與協定](../build/profile-round1/takeover.log)通過；隔離資料與模擬後端。 |
| 瀏覽器操作 | [26 項通過](../build/profile-round1/browser.json)，重新操作裁切、資料卡、取消、日期、自介、鍵盤與跨來源圖片。 |
| 前端與工具 | [16 項裁切與日期](../build/profile-round1/geometry.log)、[40 項流程圖](../build/profile-round1/workflow.log)、[18 項桌面整合](../build/profile-round1/desktop.log)、[13 項風格工具回歸](../build/profile-round1/style-tests.log)通過。 |
| 建置 | [桌面](../build/profile-round1/desktop-build.log)、[CLI](../build/profile-round1/cli-build.log)成功；PE 版本都是 0.4.1。 |
| CLI 回歸 | [7 組真實 ConPTY 情境通過](../build/profile-round1/cli-smoke.log)，含允許、拒絕、設定、群組、流程、逾時及資料鎖；後端為模擬，錄製在 [run-9sckubzx](../build/cli-smoke/run-9sckubzx)。 |
| 執行檔資源 | [直接讀取 PE 資源](../build/profile-round1/artifacts-check.log)，桌面介面與磁碟來源相同，包含裁切元件；CLI 沒有介面資源，兩版都不含模擬主機。 |
| 專案風格 | [改動行格式、JavaScript 語法、編碼與空白](../build/profile-round1/style.log)通過；`git diff --check` 通過。 |

原生 WebView2 視窗、Windows 圖片選擇器及真實訂閱協作仍沒有實測。瀏覽器操作與 C++ 的 WIC、檔案保存測試是分開執行的證據，不能宣稱整個原生桌面流程已測過。

## 修正版執行檔

- [ChatClaudeAgent-0.4.1.exe](../build/ChatClaudeAgent-0.4.1.exe)
- [ChatClaudeAgent-CLI-0.4.1.exe](../build/ChatClaudeAgent-CLI-0.4.1.exe)
- [大小與 SHA-256](../build/profile-round1/artifacts.json)

目前的工作結束後，關閉舊版再啟動 0.4.1。原本的對話、圖片與設定沿用，測試沒有使用使用者的私人資料。`build/` 裡的證據與 EXE 是本機產物，不進 Git。
