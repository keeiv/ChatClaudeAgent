## 這個變更解決什麼問題

<!-- 一兩句話。連到相關的 issue：Fixes #123 -->

## 怎麼解

<!-- 重點的設計選擇，以及你沒有採用的做法與原因。 -->

## 測試

<!-- 勾掉你跑過的；沒跑的寫原因。沒有驗證就寫「沒有驗證」。 -->

- [ ] `python tools/check_style.py --format --js`
- [ ] `python tools/run_native_tests.py`（或相關的目標：`...`）
- [ ] `python tests/run_browser_tests.py`（改了 `ui/`）
- [ ] `build.bat memory-test`，必要時加 `CCA_SANITIZE=1`（改了解析或外部輸入的程式）
- [ ] `python tests/extreme_env.py`（改了保存、路徑、錯誤處理）
- [ ] 新增或更新了測試，且在修正前會失敗

## 檢查清單

- [ ] 提交訊息是 `類型: 說明`，一個提交做一件事
- [ ] 桌面版與終端機版都處理了（或在上面說明為什麼只需要一邊）
- [ ] 新增訊息類型時，核心分派、`ui/app.js`、`ui/mock.js` 都改了
- [ ] 模型輸出沒有未經跳脫就放進畫面
- [ ] 沒有提交金鑰、個人路徑、建置輸出或二進位檔
- [ ] 文件與 AGENTS.md 已隨行為更新

## 截圖

<!-- 介面變更請附前後對照。 -->
