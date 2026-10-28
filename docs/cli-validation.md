# ChatClaudeAgent 0.2.0 驗證紀錄

日期：2026-10-23。環境：Windows 11 家用版，10.0.26200。

## 發行檔

| 檔案 | 大小（位元組） | SHA-256 |
| --- | ---: | --- |
| `build/ChatClaudeAgent-CLI.exe` | 1,478,656 | `028387cc3f44fa2d4a2cc30261742863639b9df0cf03c56ba3040590e88a5705` |
| `build/ChatClaudeAgent.exe` | 1,398,784 | `bedb14b6f889c0d92738716595d87fd5876a5781fb0209464b3170ded2a7cde9` |

兩個 EXE 的產品名稱皆為 ChatClaudeAgent，版本皆為 0.2.0。已讀取 PE 的圖標群組與圖標資源，16、20、24、32、40、48、64、256 像素的 PNG 內容均與 `assets/cca.ico` 對應項目一致。

重新執行圖標產生器（不公開），逐一比對 SVG、PNG、ICO、`ui/logo.png` 與 `src/logo_art.hpp` 的 SHA-256，內容沒有改變。已檢查 [圖標各尺寸預覽](../build/cli-preview/icon-sizes.png)，保留原有 BOT. 的底板、色塊與圓點比例。

## 測試結果

| 檢查 | 結果 |
| --- | --- |
| `build.bat test` | 230 通過，0 失敗 |
| `build.bat takeover-test` | 64 檢查，0 失敗 |
| `build.bat cli`、`build.bat`、`build.bat pty` | 全部建置成功 |
| `python tests/cli_smoke.py` | 6 組情境通過 |
| JavaScript 語法 | `ui/app.js`、`ui/icons.js`、`ui/mock.js`、資源產生器、模擬 Claude 與重播工具均通過 |
| CLI `--help`、`--version` | 可透過重新導向輸出，不啟動 AI |

六組 CLI 情境包括：允許指令、拒絕並附理由、設定與縮放、群組交棒、測試逾時必須失敗，以及跨程序拒絕重複開啟同一資料目錄。使用獨立資料與模擬後端，未消耗訂閱額度。

設定情境確認名稱、角色性格與規劃權限寫入磁碟。群組情境確認審查者先回覆，再顯示完成摘要；另以反向排序的訊息 ID 重現並檢查同毫秒訊息的送出順序。控制碼測試確認工具文字無法送出清畫面、OSC 標題、鈴聲或換行控制。

本次完整情境資料保存在 `build/cli-smoke/run-xlljyycn/`，每個情境均有 `driver.log`、`screen.vt`、快照位移、視窗尺寸紀錄與保存後的 JSON。

## 記憶體樣本

來自該輪 `allow/screen.vt.memory.jsonl`。空白測試對話，100 欄、34 列；單次樣本，非跨機器效能保證。

| 時點 | CLI 工作集（MiB） | CLI 私有提交（MiB） | 含模擬子程序工作集（MiB） | 程序數 |
| --- | ---: | ---: | ---: | ---: |
| 閒置 | 12.69 | 2.35 | 29.39 | 3 |
| 等待指令核准 | 14.79 | 2.54 | 85.58 | 6 |
| 回覆完成 | 14.87 | 2.58 | 85.75 | 6 |

子程序合計不包含終端機視窗、ConPTY 主機或 Ollama；相加的工作集可能重複計入共享頁面。模擬後端的用量不能代表真實 Claude Code 或 Codex。對話歷史由核心保存，長紀錄會增加本體記憶體。

## 畫面範圍

已將真實 ConPTY 輸出交給 xterm.js 解碼，重建並目視檢查 [權限請求](../build/cli-preview/approval.png)、[群組交棒](../build/cli-preview/group.png) 與 [48 欄權限選單](../build/cli-preview/narrow.png)。權限選項名稱完整可見，中文沒有被切成無效字元，審查回覆位於完成摘要之前。

本輪沒有可用的互動瀏覽器。PNG 是錄製資料的排版重建；Windows Terminal 的實際字型回退、滑鼠選取、輸入法組字，以及真實訂閱後端仍未在本輪實測。重跑方式與官方設計參考見 [CLI 說明](cli.md)。
