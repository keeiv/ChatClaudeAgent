# 多代理平台驗證索引

目前版本為 0.6.2。0.6.1 的共用 Docker 沙盒及 Claude 實際呼叫紀錄見 [沙盒驗證](sandbox-validation.md)，包含實際環境、命令、失敗原因、修正與剩餘限制。

平台回歸使用隔離的 `CCA_DATA_DIR`、本機 HTTP 替身及原生子程序；不把它們當作真實模型自行協作的證據。核心紀錄位於 [sandbox-core.log](../build/sandbox-core.log)，多代理紀錄位於 [sandbox-pipeline.log](../build/sandbox-pipeline.log)。建置產物與內嵌資源核對則記在 [sandbox-artifacts.json](../build/sandbox-artifacts.json)。

這些 `build/` 連結只在本機工作區有效。真實 Claude 沙盒測試已獨立記錄；原生視窗、真實多模型的完整協作與帳務仍須分別驗證。

## 模型選單（2026-10-25）

多代理流程的 Claude、Codex、API 與 Ollama 模型改為下拉選單。API 自動查詢所選供應商，使用預設或第一個模型；清單變動會保留尚未儲存的名稱及預算，不會沿用其他供應商的選擇。來源未列出的舊模型保留顯示，要求使用者選擇有效項目後再儲存。

本輪實際執行：

| 檢查 | 結果與證據 |
| --- | --- |
| 原生多代理 | [60 項，0 失敗](../build/pipeline-models-review/pipeline-test.log)。新增本機 HTTP 模型探索、供應商及請求識別、錯誤回覆與 Python 無法啟動的案例。資料與 DPAPI 金鑰均使用隔離暫存目錄。 |
| 模型選擇瀏覽器 | [27 項](../build/pipeline-models-review/browser.json)，使用既有 Playwright 與 Edge，驗證訂閱清單、API 自動填入、切換供應商、過期回覆、失敗重試、空清單、舊模型、唯讀角色、排序、儲存及窄畫面。 |
| 平台瀏覽器 | [14 項](../build/platform-review/browser.json)，涵蓋流程編輯、API 設定、圖片與窄畫面。 |
| 介面邏輯 | `desktop_smoke.cjs` 18 項、`workflow_ui.cjs` 40 項、`profile_media.cjs` 16 項。 |
| CLI 回歸 | [7 組 ConPTY 情境通過](../build/pipeline-models-review/cli-smoke.log)，後端為模擬，沒有使用訂閱額度。 |
| 風格工具 | `test_check_style.py` 13 項；`check_style.py --format --js` 檢查 133 個檔案。 |
| 建置與資源 | [桌面](../build/pipeline-models-review/desktop-build.log)、[CLI](../build/pipeline-models-review/cli-build.log)均成功，PE 版本為 0.6.2；[內嵌資源核對](../build/pipeline-models-review/artifacts.json)與原始碼一致，兩版均不含假主機。 |

新增原生訊息案例第一次未通過，原因是測試主機沒有開啟 `uiReady_`，收不到管理回覆。已在隔離測試中開啟接收並重跑，沒有藉由 `ready` 啟動真實 CLI 認證；最終 60 項檢查均通過。

原生測試的模型清單來自 `provider_fixture.py` 的本機 HTTP 服務；瀏覽器使用可控制延遲及錯誤的假主機。沒有使用真實訂閱額度或 API Key，也沒有安裝套件。介面沿用原有字體、品牌色與表單；[畫面](../build/pipeline-models-review/model-selects.png)已檢視。

重跑瀏覽器檢查前，先用 `node tools/serve_ui.js` 啟動開發伺服器；另一個終端機將 `CCA_UI_URL` 設為該伺服器網址（預設 `http://127.0.0.1:5178/`），再執行 `node tests/pipeline_models_browser.cjs` 與 `node tests/platform_browser.cjs`。需要既有的 Playwright 套件及 Edge；此流程不會自動安裝。
