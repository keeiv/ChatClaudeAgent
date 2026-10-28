# 頭像與成員資料卡

桌面版點聊天室上方的群組名稱，進入「群組資料」更換或移除群組頭像。自己的頭像在「設定 › 我的個人資料」，AI 的頭像在設定側欄各自的個人資料；Gemini 與本地審查員也適用。群組的頭像獨立保存，各 AI 的頭像則用在所有對話。

選擇 PNG、JPG、GIF 或 WebP 後會開啟裁切畫面。拖曳、方向鍵和縮放滑桿都可以調整；Shift 加方向鍵移動較多。圓框是顯示範圍，按「儲存頭像」才替換舊圖，取消或 Esc 會保留原圖。輸入上限 10 MB、4,000 萬像素，輸出是 512 × 512 靜態 PNG，GIF 裁切後也會變成靜態圖。既有 GIF 頭像可繼續顯示，橫幅不經裁切。

點群組右側成員或訊息旁的頭像可開資料卡。AI 的自介取自目前性格的「簡介」，沒有性格時顯示工作職責。資料卡的模型依目前聊天室設定及後端已回報的模型判定；本地成員依設定的 Ollama 模型判定。

「加入時間」預設使用該模型的發布日期；在 AI 個人資料可另外指定日期，清空就恢復預設。這個欄位不是加入某個群組的實際時間。未收錄的模型顯示「未收錄發布日期」，不會用今天或其他模型的日期代替。每筆預設日期都有可點閱的來源。

CLI 延續只設定名稱與性格的需求，不載入頭像、不新增圖片選單；共用核心會保留桌面版存下的頭像和日期。

## 日期查證

2026-10-24 查閱下列一手資料。模型表在 `src/model_dates.cpp`，前端使用核心隨 `boot` 傳來的同一份表。

| 模型 | 日期 | 來源 |
| --- | --- | --- |
| Claude Fable 5.1 | 2026-09-15 | [Anthropic 公告](https://www.anthropic.com/claude-fable-and-mythos-5-1) |
| Claude Opus 5.5 | 2026-09-22 | [Anthropic 公告](https://www.anthropic.com/claude-opus-5-5) |
| Claude Sonnet 5.5 | 2026-09-28 | [Anthropic 公告](https://www.anthropic.com/claude-sonnet-5-5) |
| Claude Haiku 5.5 | 2026-10-07 | [Anthropic 模型公告索引](https://www.anthropic.com/news/models) |
| GPT-5.6 Sol / Terra / Luna | 2026-07-09 | [OpenAI API 更新紀錄](https://developers.openai.com/api/docs/changelog) |
| GPT-6 Astra | 2026-09-03 | [OpenAI API 更新紀錄](https://developers.openai.com/api/docs/changelog) |
| GPT-6 Sol / Luna | 2026-09-22 | [OpenAI API 更新紀錄](https://developers.openai.com/api/docs/changelog) |
| GPT-6.1 Sol | 2026-09-29 | [OpenAI API 更新紀錄](https://developers.openai.com/api/docs/changelog) |
| Llama 3.1 | 2024-07-23 | [Meta 公告](https://ai.meta.com/blog/meta-llama-3-1/) |
| Qwen2.5-Coder 1.5B / 7B | 2024-09-19 | [Qwen 首次發布](https://qwenlm.github.io/blog/qwen2.5-coder/) |
| Qwen2.5-Coder 0.5B / 3B / 14B / 32B | 2024-11-12 | [Qwen 新增尺寸公告](https://qwenlm.github.io/blog/qwen2.5-coder-family/) |
| Qwen3 | 2025-04-29 | [Qwen 公告](https://qwenlm.github.io/blog/qwen3/) |
| Dolphin 3.0 Llama 3.1 8B | 2024-12-29 | [作者模型庫的 createdAt](https://huggingface.co/api/models/dphn/Dolphin3.0-Llama3.1-8B)；這是上傳日，資料卡會明確標示 |

Claude 的短別名會依當前對應型號解析；已回報的實際型號優先。Ollama 已知尺寸與量化包可對應到原始模型，重新訓練或未收錄的變體保持未知；例如 `qwen3:4b-thinking-2507` 不會套用首批 Qwen3 的日期。未來換新模型時應重新查證來源並更新表格和測試。

## 實作與驗證入口

選圖時核心建立暫存副本及一次性裁切代號，並保留目標。前端 `image.commit` 只能回傳該代號和 PNG，不能改寫頭像的對象。核心用 Windows WIC 完整解碼、檢查尺寸後才存檔；取消、關閉或下次啟動會清理暫存圖。圖片放在資料目錄的 `media/`，設定只存檔名；群組頭像存於 `conversations.json`，個人頭像和日期存於 `settings.json`。

0.4.1 起，設定檔寫入成功後才刪除舊圖；如果無法保存，會保留原設定與裁切內容，可以修正資料目錄的寫入問題後重試。移除頭像或刪除群組也會先確認設定可以保存。最新版執行檔與回歸結果見 [第一輪審查](profiles-review.md)。

```powershell
.\build.bat profile-test
node tests\profile_media.cjs
node tests\workflow_ui.cjs
node tests\desktop_smoke.cjs
python tools\check_style.py --format --js
```

`tests/profile_browser.js` 是提供給 Playwright 瀏覽器測試工具的函式，不能當作 `node` 測試執行。先以 `$env:PORT='5198'; node tools/serve_ui.js` 啟動預覽，再由 `browser_run_code_unsafe` 的 `filename` 執行該檔。它在瀏覽器產生三色測試圖，檢查裁切後的像素、大小、取消、錯誤圖片、日期、性格和窄視窗，截圖放在 `build/profile-review/`。這些是開發用模擬資料，不會編進 EXE，也不會使用模型額度。

原生 WebView2 視窗與使用者真實圖片仍需與瀏覽器預覽的驗證分開記錄；本輪結果見 [驗證紀錄](profiles-validation.md)。

Gemini 在多代理流程中使用相同的個人資料與性格，成員列表、訊息頭像和流程圖會顯示目前名稱。節點別名與個人名稱不同時兩者並列；同一 Gemini 後端的不同節點分別保留交接與執行狀態。Gemini 尚無獨立私訊入口，模型仍使用官方 CLI 預設，不提供未驗證的模型或思考程度選項。CLI 的 `/name`、`/persona` 可選 Gemini；圖片編輯仍在桌面版操作。
