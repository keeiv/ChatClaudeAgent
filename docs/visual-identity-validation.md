# 0.8.0 視覺與協作控制驗證

日期：2026-10-25，Windows 本機。這次依據 `visual-identity.md` 補完視覺與審查細節，並實作使用者追加的五項協作功能。保留原先未提交的工作；開始前的備份在 `%LOCALAPPDATA%\CCA\codex-backups\identity-20261025-191245`。

## 交付內容

- 石墨灰、切角、白色主要按鈕與冰藍訊號的一致化；登入、帳號、選單、鍵盤焦點與減少動態。
- 審查檔案篩選、鍵盤切換、保留程式碼行、跨重新開啟的逐行意見與草稿；主機確認送達才清除。還原需比對審查時的檔案版本，避免覆蓋較新的編輯。
- 可編輯、留言、版本核准、另行執行與人工逐項驗收的計畫。
- 跨對話收件匣，可返回原對話；已讀不會略過必要核准。
- 可編輯 Markdown 關注點，指定一般互審的成員或多代理流程的審查位置。
- 2 至 4 位模型的獨立唯讀意見、額外共識整理回覆、原文保存及待核准計畫提案。
- 按任務的 Git worktree 隔離、真實衝突預演、比對兩側提交後明確合併。

操作與限制見 [協作控制](collaboration.md)。

## 原生檢查

| 命令 | 實際結果 | 驗證重點 |
| --- | --- | --- |
| `build.bat test` | 557 passed, 0 failed | 原有核心、CLI 渲染、權限、任務與審查工具 |
| `build.bat collaboration-test` | 52 checks, 0 failures | 計畫版本、過期核准、儲存失敗保留、人工驗收、中斷重啟、關注點、收件匣、意見失敗／停止、真實 Git 合併與衝突 |
| `build.bat takeover-test` | 110 checks, 0 failures | 真實子程序協定、獨立意見與唯讀設定、保留原 session、共識結構、核准後執行計畫、待驗收、接手與審查還原 |
| `build.bat pipeline-test` | 60 checks, 0 failures | 原有多代理、主機證據、API 本機 HTTP fixture 與持久交接 |
| `python tests/test_check_style.py` | 13 tests, OK | 格式工具本身的回歸 |

子程序測試以測試執行檔模擬 Claude 與 Codex 的協定，沒有呼叫官方推論服務、付費 API 或本地模型。Git 測試真的建立倉庫、提交、分支和 worktree，驗證無衝突合併、不同任務保留、衝突拒絕、過期預覽拒絕及未提交變更保護。

## 瀏覽器與終端機

使用專案既有 Playwright 與本機 Edge；UI 伺服器在 `http://127.0.0.1:5208/`，瀏覽器透過 `#mock` 模擬主機。執行前設定 `CCA_UI_URL` 與可找到 Playwright 的 `NODE_PATH`，沒有新增發行相依。

| 測試 | 實際結果 |
| --- | --- |
| `node tests/collaboration_browser.cjs` | 19 checks passed：五項新功能、核准與執行分開、提案不自動執行、文字跳脫、可選區段不顯示 null、窄視窗 |
| `node tests/visual_identity_browser.cjs` | 18 checks passed：審查草稿、送出確認、焦點、鍵盤、過期回應、窄視窗與減少動態 |
| `node tests/platform_browser.cjs` | 14 checks passed |
| `node tests/pipeline_models_browser.cjs` | 27 checks passed |
| `node tests/desktop_smoke.cjs` | 18 checks passed |
| `node tests/workflow_ui.cjs` | 40 checks passed |
| `node tests/profile_media.cjs` | 16 checks passed |
| `python tests/collaboration_cli.py --exe build/ChatClaudeAgent-CLI-0.8.0.exe` | 12 checks passed：真實 ConPTY、存檔、意見子程序與 Git worktree |
| `python tests/cli_smoke.py --exe build/ChatClaudeAgent-CLI-0.8.0.exe` | 7 scenarios passed：允許、拒絕、設定、協作、流程、逾時、跨程序資料鎖 |

瀏覽器模擬只能驗證互動與排版；真實 Git／檔案保存／子程序協定另外由原生與 CLI 測試確認。瀏覽器視覺檢查不等於 WebView2 上的人工輸入法或輔助科技測試。

已檢視的截圖（`build/` 不進版本控制，連結僅供本機）：

- [計畫與驗收](../build/ui-shots/plan-after.png)
- [統一收件匣](../build/ui-shots/inbox-after.png)
- [多模型意見](../build/ui-shots/opinions-after.png)
- [worktree 預演與合併](../build/ui-shots/workspace-after.png)
- [審查意見草稿](../build/ui-shots/review-notes-after.png)

## 建置與資源

另存輸出，不關閉或替換正在執行的 CCA：

- [桌面版 ChatClaudeAgent-0.8.0.exe](../build/ChatClaudeAgent-0.8.0.exe)
- [CLI ChatClaudeAgent-CLI-0.8.0.exe](../build/ChatClaudeAgent-CLI-0.8.0.exe)

`python tools/verify_resources.py --desktop build/ChatClaudeAgent-0.8.0.exe --cli build/ChatClaudeAgent-CLI-0.8.0.exe --output build/identity-validation/resources.json` 比對內嵌 UI、runtime、沙盒程式與來源，確認沒有把 `mock.js` 打包。二進位雜湊與大小在 [資源驗證](../build/identity-validation/resources.json)，原生建置與測試輸出在 `build/identity-validation/*.log`。

交付前執行 `python tools/check_style.py --format --js` 與 `git diff --check`；C++ 只格式化變更範圍。不提交、不推送，不對使用者的真實 Git 倉庫執行測試合併。

## 未宣稱完成的範圍

沒有測試真實訂閱登入後的推論品質、額度成本或真實 Ollama 的意見整理；沒有新增 API 供應商作為意見模式成員、Windows 通知中心深層連結、自動解衝突或自動清理 worktree。這些不影響本版五項功能的既定操作路徑，也不以模擬結果替代它們的驗證。
