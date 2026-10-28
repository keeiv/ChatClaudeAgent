# CCA 1.0 圖標設計

研究與製作日期：2026-10-26。範圍為 Windows EXE 圖標、軟體內標誌、CLI 點陣與可重建的原始資產。

## 研究

優先查閱官方指南、官網與官方程式庫中的圖檔。以下「觀察」是對實際輪廓的分析，並非替品牌宣稱設計寓意。第三方圖檔只存於忽略版本控制的 `build/brand-1.0/`，不隨 CCA 發布。

| 來源 | 觀察或指南重點 | 對 CCA 的取捨 |
| --- | --- | --- |
| [OpenAI 品牌指南](https://openai.com/brand/) | 標誌有固定比例與留白；基本幾何構成一致的輪廓 | 使用固定母版與留白，不借用 Blossom 結構 |
| [Claude 官網](https://claude.com/)與 [Anthropic 商標指南](https://www.anthropic.com/legal/trademark-guidelines) | 官網圖標是暖橘色放射形；指南要求比例、可讀性與清楚的周圍空間 | 保留清楚的單一輪廓，不採橘色放射線 |
| [Google Design：Gemini](https://design.google/library/gemini-ai-visual-design) | 基本幾何與負空間可延伸到整體產品 | 讓 CCA 輪廓延續既有切角，不採星芒或漸層 |
| [Grok 品牌資源](https://x.ai/legal/brand-guidelines)與其連結的[官方 App Store 圖片](https://apps.apple.com/us/app/grok-ai/id6670324846) | 小尺寸以環形缺口與斜向形狀維持識別 | 採單色辨識原則，不借用圓環與斜線組合；下載品牌 ZIP 時回傳 403，視覺依據為官方商店圖片 |
| [DeepSeek 官方程式庫](https://github.com/deepseek-ai/DeepSeek-V3)及其引用的[標誌 SVG](https://raw.githubusercontent.com/deepseek-ai/DeepSeek-V2/main/figures/logo.svg) | 藍色鯨魚剪影和字標分工，圖形單獨仍可辨認 | CCA 圖標與字標分開輸出，不增加吉祥物細節 |
| [Qwen 官方程式庫](https://github.com/QwenLM/Qwen3)及其引用的[標誌 PNG](https://qianwen-res.oss-accelerate-overseas.aliyuncs.com/logo_qwen3.png) | 紫色折帶、重複方向與中央留白構成輪廓 | 學習形狀一致性，不借用折帶星形或紫色主色 |
| [Microsoft 應用程式圖標](https://learn.microsoft.com/en-us/windows/apps/design/iconography/app-icons)、[製作規格](https://learn.microsoft.com/en-us/windows/apps/design/iconography/app-icon-construction)與 [Win32 圖標](https://learn.microsoft.com/en-us/windows/win32/uxguide/vis-icons) | 小尺寸可辨認，並提供接近實際顯示尺寸的圖標資源 | ICO 寫入 15 張原生尺寸，檢查 16 像素與高 DPI |
| [Apple 圖標指南](https://developer.apple.com/design/human-interface-guidelines/app-icons/)、[圖標設計講座](https://developer.apple.com/videos/play/wwdc2017/822/)與[資產實務](https://developer.apple.com/app-store/asset-best-practices/) | 簡單、可辨認、跨用途保持同一身分 | 同一母版產生 SVG、ICO 與點陣；維持 CCA 原有平面樣式 |

## 定案：接續

比較過銜接、對位框、方形折合三種輪廓。定案採兩個相向的 C，中段連續，外側各留一個開口。它對應 CCA 的工作交接，也能在沒有字標的情況下成立。

- 32 × 32 母版，45 度切角，單色填滿；不使用外框補細節。
- 標誌 `#E9EDF1`，深色版本 `#0C0E11`，圖標底板 `#13161A`。這些值對應目前介面中的文字與石墨底色。
- 底板左上、右下切角；沒有漸層、光暈、立體材質或裝飾性星芒。
- CCA 字標是同樣端點與切角的自繪輪廓，不加入新字體依賴。
- 代理保留各自的識別色；產品標誌不再混合供應商顏色。

## 實作與核對

全部資產由不公開的圖標產生器輸出。檔案用途與留白見 [資產說明](../assets/logo/README.md)。

- 桌面與 CLI EXE 以相同 `assets/cca.ico` 建置。ICO 尺寸為 16、20、24、30、32、36、40、48、60、64、72、80、96、128、256。
- 軟體標題列、伺服器列、系統訊息使用 `ui/icons.js` 的同一圖形；登入頁使用 `ui/logo.svg`。
- CLI 從母版取樣成 16 × 16 點陣，以半格字元顯示為 16 欄、8 列，配色為介面文字白。
- `tools/verify_resources.py` 逐張比較兩個 EXE 中的 RT_ICON 圖片與來源 ICO，並核對內嵌 UI 和工作程序。它不依賴 Explorer 快取來判斷是否換圖。

本機檢查產物位於 `build/brand-1.0/`，不進版本控制。這次沒有變更使用者資料格式、模型設定或既有對話。

## 驗證紀錄

| 檢查 | 結果 |
| --- | --- |
| `build.bat test` | 557 項通過，0 項失敗 |
| `tests/cli_smoke.py --exe build/ChatClaudeAgent-CLI-1.0.0.exe` | 7 種 ConPTY 情境通過；資料與假後端均隔離 |
| `tests/desktop_smoke.cjs` | 18 項桌面整合檢查通過 |
| Edge 實際畫面 | 標題列、側欄、登入 SVG、關於頁 1.0.0 顯示正常，沒有頁面執行錯誤；已人工檢視截圖 |
| EXE 內嵌資源 | 兩個 EXE 的 15 張圖標逐位元組符合來源，桌面 UI 與工作程序符合來源，沒有內嵌 mock.js |
| Windows 版本資源 | 兩個 EXE 的 FileVersion、ProductVersion 均為 1.0.0，數字版本 1.0.0.0 |
| 重建一致性 | 再次執行圖標產生器，全部輸出 SHA-256 不變 |
| 風格與差異檢查 | `check_style.py --format --js` 及 `git diff --check` 通過 |

檢查未呼叫付費模型，也未關閉原本執行中的 CCA。桌面截圖使用瀏覽器假資料；CLI 標題預覽由實際 ConPTY 紀錄還原，完整原始紀錄保存在 `build/cli-smoke/run-rtnwzgp_/`。

| 交付檔案 | SHA-256 |
| --- | --- |
| `build/ChatClaudeAgent-1.0.0.exe` | `498c5a8446898c3df10499c6f9a15dd8006f90ebf62e413a783f9f783a693f54` |
| `build/ChatClaudeAgent-CLI-1.0.0.exe` | `bc911f225b6e9b28eb2958a724ccdb0869a846a2cdbc8a0fe7b108f0da27754d` |

各尺寸與字標預覽 · 桌面畫面 · 登入畫面 · CLI 實際標題 · 資源核對結果
