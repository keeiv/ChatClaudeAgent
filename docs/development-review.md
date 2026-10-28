# 開發指南與風格工具複審

2026-10-24 以 `main` 的首個提交 `c618c2f` 為基準。提交已存在，審查開始時工作區乾淨，沒有設定遠端；`build/` 未被追蹤，WebView2 SDK 的換行屬性為 `-text`。

## 查證與修正

`CLAUDE.md` 使用 `@AGENTS.md` 引入共用指南，Codex 的對話設定有傳入專案工作目錄。本地審查員由共用 `App::localReviewPrompt` 讀取標記區塊，桌面與 CLI 都走這個核心。修訂後標記內容為 2,548 bytes，包含最後的架構審查重點，沒有被 3 KiB 上限截掉。原生測試覆蓋沒有檔案、有標記、長檔截斷及 `CLAUDE.md` 備援。

風格工具的新增回歸測試重現了以下問題，再進行修正：

- Git 將中文路徑加引號、轉義後，已追蹤檔案會漏掉改動行；新檔會嘗試開啟帶引號的錯誤路徑。現在使用 NUL 分隔的檔名，逐檔解析行號，不解析顯示用的路徑標頭。
- 指定 `--js` 或 `--format`，但必要工具不存在時，原本仍回傳成功。現在回傳失敗；缺少 Git 比對基準或讀取差異失敗也會明確回報。
- 規範禁止 Tab，但原工具略過 Markdown；單獨的 CR 換行也沒有檢出。現在會回報，並可修正 CR 與行首 Tab；行內 Tab 保留並要求手動處理，以免改動字串。

新增 `tests/test_check_style.py`，僅使用 Python 標準函式庫。測試在暫存目錄建立自己的 Git 倉庫與測試提交，不更動 CCA 的 Git 索引或歷史。實際呼叫 clang-format 的情境確認中文路徑會被檢查，舊有未改行不會被重排。

`AGENTS.md` 與 README 已同步必要工具、錯誤行為與回歸測試指令。

## 本輪驗證

| 檢查 | 結果 |
| --- | --- |
| 風格工具回歸 | [13 項通過](../build/guides-review/style-tests.log)，包含真實 Git 與 clang-format 的整合情境。 |
| 專案風格 | [94 個檔案沒有問題](../build/guides-review/style.log)，使用 `--format --js`。 |
| 核心單元 | [352 項通過、0 失敗](../build/guides-review/unit.log)，使用隔離的 `CCA_DATA_DIR`。 |
| 接手與子程序協定 | [80 項通過、0 失敗](../build/guides-review/takeover.log)，使用模擬後端。 |
| 桌面流程元件與整合 | 40 項與 18 項通過。沒有新增介面或訊息類型。 |
| 兩版建置 | [CLI](../build/guides-review/cli-build.log)與[桌面](../build/guides-review/desktop-build.log)成功；原生來源在本輪測試與建置期間的雜湊一致。 |

去 AI 化檢查：將目前介面與素材比對前輪快照 `f2dea64`，`ui/app.js`、`app.css`、`index.html` 及四個 SVG 除空白、BOM、換行之外，內容相同；應用圖示 PNG、ICO 與 `ui/logo.png` 的位元組完全相同。本輪文件寫實際指令、限制與測試結果，沒有新增版型、字體、特效或美術素材。

審查期間工作區另外出現 `src/app.cpp` 的 `settings.set` 驗證改動。本輪沒有編輯或回復該檔案；上述原生測試以它出現後的檔案重新編譯、執行。既有 `c618c2f` 與專案索引保持不變，本輪修正未提交或推送。

沒有執行會消耗訂閱額度的測試，也沒有驗證真實模型是否遵從指南。檔案入口與提示詞讀取的驗證不能保證模型每次都遵守規範。

## 審查版執行檔

- [ChatClaudeAgent-CLI-guides-review.exe](../build/ChatClaudeAgent-CLI-guides-review.exe)
- [ChatClaudeAgent-guides-review.exe](../build/ChatClaudeAgent-guides-review.exe)

使用替代檔名建置，沒有關閉 CCA 程序。產品版本維持 0.3.0，[檔案大小與 SHA-256](../build/guides-review/artifacts.json)。
