# Haiku 手機模擬（在電腦上開啟俳句）

> **你已經有桌面版了，建議使用桌面版。** 這個功能只是讓你先看看 Haiku 在手機上的樣子；要在電腦上寫程式，桌面版的 CCA 才是完整的工具。

CCA 1.0.2-oss 與 Fable 0.0.5 起，軟體更新頁的「Android 版本」區多了「在電腦上以手機模擬開啟」：CCA 開一個手機大小的視窗（寬約 412、高最多約 858 個邏輯像素），把 Haiku 的 APK 裡的編輯頁原封不動載入，並由 CCA 扮演 Android 殼層。

## 不改 Haiku

Haiku 沒有為了能在電腦上開啟而改過任何一個字：

- 編輯頁取自安裝資料夾裡的 APK（`assets/www`），由 CCA 在記憶體裡解壓縮，每個檔案核對 CRC；不是複製一份到別處。
- 頁面呼叫的 `HaikuHost` 橋接，由 CCA 在載入前注入；頁面的程式碼仍是它自己的，找得到真正的橋接，不會退回開發用的假主機。
- 開啟前，CCA 先確認這個 APK 是更新頁掃描列出的那一個，且大小與 SHA-256 符合同資料夾 `manifest.json` 的記載，內容裡有 Haiku 的三個頁面檔。

## CCA 扮演的殼層（與 Android 版逐項對照）

| 橋接方法 | 電腦上的做法 |
| --- | --- |
| `projects.*`、`files.*`、`chat.*` | 專案存在 `%APPDATA%\CCA\haiku-sim\projects`；名稱、路徑、2 MiB、400 個檔案的限制與手機相同，另外拒絕 Windows 的裝置名稱（`CON`、`NUL` 等）、結尾的點與空格、非法字元，以及連結（junction） |
| `checkpoint.*` | 每輪一個還原點，最多 20 個，存在 `haiku-sim\checkpoints`；還原時保留對話 |
| `keys.*` | 用 Windows DPAPI 加密存在 `haiku-sim\keys`；頁面只會知道「有沒有設定」，拿不到金鑰 |
| `models.list`、`model.send` | CCA 用 WinHTTP 連 `api.anthropic.com`、`api.openai.com`；金鑰只加在標頭，回應裡出現的金鑰會換成「[已隱藏]」；不跟隨轉址；本文與回應各限 8 MiB；網路呼叫在背景執行緒，視窗不會卡住 |
| `preview.open` | 另開預覽視窗（有「返回編輯」「重新整理」），在**沒有橋接**的 WebView 載入專案檔案；預覽頁送出的橋接訊息沒有接收端 |
| `export.zip` | 匯出到「下載\CCA Haiku」，名稱含時間 |

編輯頁只能留在自己的頁面（離開的導覽會被取消），也不能開新視窗。

## 限制

- 專案與金鑰存在這台電腦，**不會同步到手機**；兩邊各自獨立。
- 這是模擬：觸控、系統的深淺色切換、輸入法行為都以 WebView2 為準，和真機會有差異；沒有真機的 Keystore，金鑰庫換成 DPAPI。
- 手機模擬只在桌面版提供；CLI 沒有視窗，`/update` 只列出 APK 的位置。
- 使用 Haiku 仍要自己的 API 金鑰，依用量向供應商付費；CCA 不經手帳單。

## 驗證

```powershell
.\build.bat haiku-sim-test      # 解壓縮對 zlib、真實 APK、專案、DPAPI、橋接與本機伺服器（不連外）
.\build.bat haiku-window-test   # 真的開出視窗，載入未修改的 APK 頁面，驅動橋接、預覽，並存截圖到 build\
node tests\editions_browser.cjs  # 更新頁的按鈕與建議文字
```

`haiku-window-test` 需要 WebView2 與已建置的 `build\android\CCA-Haiku-0.0.2.apk`；沒有 APK 時略過並註明。
