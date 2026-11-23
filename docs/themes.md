# 介面外觀（1.0.2-luna）

「設定 → 外觀」有兩款：**CCA 暗色**（預設）與 **CCA 亮色**。兩款共用同一套版面、間距、成員識別色與狀態語意，只換明暗。選擇後先預覽，按「套用」才保存在本機 `settings.json`；取消、關閉設定或保存失敗時恢復已保存的外觀。方向鍵、Home、End 可切換預覽。CLI 用 `/theme cca` 或 `/theme cca-light`。

1.0.2-luna 以前的十六款選用主題（各家產品、遊戲與節日主題）已經移除。設定檔裡留著舊主題 ID 時，CCA 啟動後改用 CCA 暗色，不會報錯。

## 資料來源

- `ui/app.css` 開頭的 `:root` 是暗色的完整 token。
- `config/themes.json` 列出兩款外觀；亮色只寫底色、線條、文字、訊號與主要動作幾個顏色（`palette`），其餘 token 由 `tools/make_themes.py` 推導，包括在淺色底上加深的成員色與狀態色。
- 產生器輸出 `ui/theme_catalog.js`（桌面）與 `src/theme_catalog.hpp`（CLI 與更新視窗）。改了 `:root` 或 `config/themes.json` 之後執行 `python tools/make_themes.py`；CI 會用 `--check` 確認產生檔沒有過期。

## 對比

`tests/appearance_browser.cjs` 在真正的 Edge 裡切換每一款外觀，檢查正文、次要文字、Claude 與 GPT 名稱對對話底色，以及主要按鈕文字對按鈕底色的對比都至少 4.5:1。

## CCA 插件的配色

插件可以加入自己的配色（見 [CCA 插件](plugins.md)）。插件配色只換顏色，版面與元件和內建外觀相同；舊插件檔裡的 `base` 欄位仍被接受，但不再有作用。
