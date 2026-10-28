# 介面主題（1.0.1-sol）

「使用者設定 → 介面主題」在普通版本共有十六款：CCA 原生、OpenAI Codex／Anthropic Claude／Google Gemini 各一組亮暗版本、Tokyo Night、urban legend、The Sky、Infinite，1.0.1-oss 新增的 Connecting 與 Blessing，以及 1.0.1-sol 新增的 CCA 自製主題：2026 萬聖節（亮、暗兩款）與追獵行動。選擇後先預覽，按「套用主題」才保存；取消、關閉設定或保存失敗時恢復已保存的主題。方向鍵、Home、End 可切換預覽。資料保存在本機 `settings.json`。

1.0.1-oss 以前的主題只換配色與圓角，畫面結構完全相同，實際看起來像同一個模板換色。這一版每款主題都從原作取幾個可辨認的「裝置」：使用者訊息的形狀、區段標記、控制項、選取狀態，以及一個背景圖樣。群組、頻道、任務控制和驗證流程的位置不變，Claude、GPT、Gemini 與本地審查員也保留各自的識別色。

## 各主題的裝置

| 主題 | 從原作取的裝置 | 配色重點 |
| --- | --- | --- |
| CCA 原生 | 不變：切角板、24px 點陣、冰藍訊號、指揮官斜紋警示帶 | 石墨灰、白色主要動作 |
| OpenAI Codex 亮／暗 | 使用者訊息是灰色泡泡、回覆滿版；輸入框是有陰影的膠囊；區段標題前是 CLI 提示符 `>_`；不畫點陣與裝飾線 | 純灰階，主要動作是黑（亮）或白（暗） |
| Anthropic Claude 亮／暗 | AI 回覆改用襯線字、行距 1.75；使用者訊息放在燕麥色紙片上；Claude 的名字與區段標題前有陶土色星芒 ✻；頻道與群組標題用襯線 | 象牙紙面 `#faf9f5`／暖炭 `#262624`，陶土 `#c96442`／`#d97757` |
| Google Gemini 亮／暗 | 四角星形區段標記；藍、紫、玫瑰星芒漸層只在輸入框聚焦、輸入中提示、時間軸進行中與歡迎標題出現；選取項目是膠囊；輸入框圓角 28px | 白底 `#f0f4f9` 容器、選取 `#d3e3fd`／近黑 `#131314`、選取 `#004a77` |
| Tokyo Night | 頻道標題做成編輯器分頁，頂端藍線；區段標題像註解 `//`；階段與 APP 標籤套語法色；輸入提示列變成等寬狀態列；伺服器列選取項有霓虹光暈 | 原作色盤 `#1a1b26`、`#7aa2f7`、`#bb9af7`、`#7dcfff`、`#e0af68` |
| urban legend（NTE） | 官網的貼紙風：亮色按鈕與卡片有墨色粗框與硬陰影，按下會往陰影方向沉；標題用 Bahnschrift 窄體大寫；選取頻道是斜貼的黃色貼紙；聊天區有網點，後方是一行斜體空心字「URBAN LEGEND」 | 靛灰夜色、奶油黃 `#fde390`、青藍 `#4fe5fb`、桃紅 `#f1558e` |
| The Sky | 伺服器列是由深到淺的天空漸層、按鈕是圓形雲白；標題列深青；頻道頂端有雲層漸層；標題細體加字距；選取是晚霞粉；聚焦、主要動作與輸入中提示帶燭光暈；聊天區有稀疏星點 | 奶油紙色 `#fffcf4`、深青 `#2e7090`／`#163f52`、晚霞粉 `#fce9e5`、燭光 `#f2b45c` |
| Infinite（暗區突圍：無限） | 區段標題用方括號 `[ … ]`、窄體大寫；歡迎標題是警示黃；次要按鈕是細框方形；成員欄左緣是官網分頁器那樣的刻度尺；聊天區有很淡的掃描線 | 近黑 `#121418`、鐵鏽橙 `#ca4d31`、警示黃 `#e3cf3d` |
| Connecting | 訊息沿著頭像欄的一條纜線排列，每位發言者是線上的節點；頻道標題下是六色訊號線；區段標記是六色短條；輸入框聚焦時接上六色邊框；伺服器列選取標記也是六色 | 深夜藍 `#121933`、青綠 `#33ccbb` 與六色 |
| Blessing | 頻道標題是撒了六色彩紙的蜜桃色帶，下緣是蛋糕花邊；區段標題前是一根點亮的蠟燭；面板像賀卡，有虛線車縫；使用者訊息是蜜桃色泡泡；歡迎區灑彩紙 | 奶油 `#fffaf3`、蜜桃 `#fbe3cf`、覆盆子 `#b8355c` 與六色 |
| 2026 萬聖節（亮／暗） | 頻道標題下緣是南瓜燈的鋸齒笑口；區段標題前是一顆小南瓜；聊天區右上角掛一角蜘蛛網；使用者訊息是帶淡紫框的對話泡泡；英文標題用手寫體（Ink Free，中文落回微軟正黑體）；暗色版的目前頻道圖標與南瓜透出橙光 | 亮：奶油 `#fff6e6`、南瓜橙 `#f08a1c`、夜紫 `#6b2fa0`；暗：夜紫黑 `#170d20`、南瓜橙 `#ff8a1f`、史萊姆綠 `#8be04e` |
| 追獵行動 | 切角開在右上與左下，和原生 CCA 的左上右下相反；卡片頂緣是紅線；區段標題前是準星；頻道標題下緣是紅色斜紋；聊天區有淡淡的斜線網格，右下角有「追獵行動」描邊字；使用者訊息左側有紅條 | 近黑 `#0f0b0c`、警戒紅 `#ff2a3d`／`#d11a2a`、警示琥珀 `#ffb000` |

主題設定頁的預覽卡也畫出各自的裝置（`>_`、✻、四角星、分頁藍線、空心字、天空漸層、方括號、六色線、彩紙、南瓜、斜紋），卡片下方一行說明這些裝置。

字體只使用 Windows 內建字型：Codex 用 Segoe UI Variable，Claude 回覆用 Georgia／Cambria（中文落回微軟正黑體），Tokyo Night 標籤用 Cascadia Mono，NTE 與 Infinite 標題用 Bahnschrift。沒有加入字型檔、遠端字體、背景影片、角色圖、歌詞或外部 CSS。

## 研究與設計取捨

查閱日期：2026-10-27。以下分開記錄可觀察的特徵與 CCA 的取捨；主題是 CCA 的詮釋，不宣稱還原第三方產品。

- **NTE**：在官方網站以瀏覽器讀取實際樣式，主色為奶油黃 `rgb(253,227,144)`、青藍 `rgb(79,229,251)`、桃紅 `rgb(241,85,142)`，深色底 `rgb(29,29,29)`，字體為 Bahnschrift；按鈕是粗描邊加硬陰影的貼紙造型，頁尾有斜體空心的大字。舊版的酸綠不在官網配色裡，這一版改掉。
- **暗區突圍：無限**：官方網站底色 `rgb(8,10,13)`、文字淺灰、強調色是鐵鏽橙 `rgb(202,77,49)`，標題是警示黃的做舊字，文字常用方括號「[ … ]」，側邊有刻度式分頁器，按鈕是細框方形。舊版的軍綠出自遊戲內畫面，官網的簡報風格更好辨認，這一版改用官網。
- **Sky**：官方網站是奶油色 `rgb(255,252,244)` 頁面、深青 `rgb(46,112,144)` 與 `rgb(22,63,82)`、晚霞粉 `rgb(252,233,229)`，頂端是雲層漸層，平台按鈕是有柔和陰影的白色圓形，字體是細的圓體。美術訪談提到光（玩家與聖所的光）是冷色場景裡唯一的暖色，因此燭光只用在需要注意的地方。
- **Claude**：Anthropic 的象牙 `#faf9f5`、暗色 `#141413`、陶土 `#d97757` 等值出自第三方整理的品牌資料，未找到 Anthropic 官方公開的完整色票，因此只取各來源一致的數值；襯線回覆、燕麥色使用者訊息與星芒是 Claude 產品可觀察的特徵。
- **Gemini、Codex、Tokyo Night**：分別依官方產品頁、官方桌面文件與原作色盤。
- **2026 萬聖節與追獵行動**：兩者是 CCA 自己的設計，沒有第三方來源，`sources` 為空。萬聖節以節日常見的南瓜燈、蜘蛛網與橙紫配色組成，亮暗兩版共用同一組裝置；追獵行動依使用者要求取紅黑配色與「另類切角」，把原生的左上右下切角翻成右上左下，用反向切角、準星與斜紋區別於原生主題。兩款都沒有引用第三方作品或角色。
- **Connecting 與 Blessing**：都是 halyosy 的「ニコニコラボ」合唱曲。Blessing 於 2014-03-11 發表，是繼 Smiling 之後的第二首，以生日祝福為題，作者說明想鼓勵在成長途中迷路的人；Connecting 於 2014-12-05 發表，是合輯「Download feat. 初音ミク」與「Upload feat. Vocalist」的主題曲，系列第三首。兩首的 VOCALOID 版都由初音未來、鏡音鈴、鏡音連、巡音流歌、KAITO、MEIKO 六位合唱。CCA 用「六色」代表多人合唱，對應群組裡多位 AI 接力；六色取自六位歌聲常見的代表色印象，沒有找到 Crypton 公開的正式色碼，因此不稱為官方色。主題不含角色圖、歌詞或音訊。

## 來源

- OpenAI：[桌面文件](https://learn.chatgpt.com/docs/app)。
- Anthropic：[官方網站](https://www.anthropic.com/)、[產品介紹](https://claude.com/product/overview)。
- Google：[Gemini](https://gemini.google.com/)、[Windows 桌面版介紹](https://blog.google/innovation-and-ai/products/gemini-app/gemini-app-now-on-windows/)。
- Tokyo Night：[原作與色盤](https://github.com/tokyo-night/tokyo-night-vscode-theme)。著作權聲明保留在 [Tokyo Night 授權](../assets/themes/tokyo-night-LICENSE.txt)。
- NTE：[官方網站](https://nte.perfectworld.com/en/)、[Steam 頁面](https://store.steampowered.com/app/4508340/)。
- Sky：[官方網站](https://www.thatskygame.com/)、[Steam 頁面](https://store.steampowered.com/app/2325290/)、[80 Level 美術訪談](https://80.lv/articles/interview-a-deep-dive-into-the-art-of-sky-children-of-the-light-with-thatgamecompany)。
- 暗區突圍：無限：[官方網站](https://www.arenabreakoutinfinite.com/en/)、[Steam 頁面](https://store.steampowered.com/app/2073620/)。
- Connecting：[VocaDB](https://vocadb.net/S/68841)、[ニコニコ大百科](https://dic.nicovideo.jp/a/connecting)、[ニコニコ動画](https://www.nicovideo.jp/watch/sm25056423)。
- Blessing：[VocaDB](https://vocadb.net/S/50703)、[ニコニコ動画](https://www.nicovideo.jp/watch/sm23304692)。

## 共用實作

`config/themes.json` 是唯一來源：每款主題有 `palette`（基本色、最多六個 `accents`、`font`／`display`／`reading` 字型堆疊、可選的 `ink` 主要動作文字色）、`shape`、`motif` 說明與來源。`tools/make_themes.py` 結合原生 CCA 的基本 token，產生 `ui/theme_catalog.js` 與 `src/theme_catalog.hpp`；`accents` 變成 `--theme-a1`～`--theme-a6`，字型堆疊變成 `--font`、`--theme-display`、`--theme-reading`。修改後執行產生器，`--check` 檢查兩邊一致，建置會先執行檢查。

`ui/appearance.js` 套用 token 與 `data-theme`、`data-shape`、`data-scheme`。各主題的裝置寫在 `ui/themes.css`，只讀上述 token，不寫死顏色；共用規則用 `:where()` 降低權重，讓個別主題能覆寫。訊息元素帶 `data-author`，主題用它區分使用者與 AI 的訊息外觀。`src/themes.cpp` 驗證主題 ID 並原子保存，不接受任意 CSS。

CLI 使用 `/theme` 列目錄、`/theme connecting` 等指令切換，沿用語意色；終端機不畫圓角、圖樣或六色線。獨立更新視窗沿用所選主題的背景、文字與訊號色。

## 驗證

- `build.bat appearance-test`：目錄十六款、主題 ID 往返、原生配色可解析、設定保存與失敗復原。
- `node tests/appearance_browser.cjs`（需要 Playwright 與介面開發伺服器）：十六款預覽、鍵盤操作、保存邊界、窄視窗、文字與成員色對比至少 4.5:1，以及每款主題的裝置確實出現（例如 Codex 的 `>_`、NTE 的空心字、Connecting 的纜線）。測試畫面存在忽略的 `build/oss-upgrade/screenshots/`。
