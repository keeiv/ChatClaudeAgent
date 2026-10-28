# CCA 插件（1.0.1-astra）

CCA 插件是一份 `plugin.json`，可以為 CCA 加入三種東西：介面主題、對話模式、輸入框指令。插件只有資料，CCA 讀取、驗證後使用，不執行插件裡的任何程式。Claude Code 與 Codex 自己的插件（MCP、技能）仍在原本的「插件」頁管理，兩者互不相干。

## 安裝與啟用

1. 每個插件放在 `%APPDATA%\CCA\plugins\<資料夾>\plugin.json`。資料夾第一次用到時自動建立，設定頁的「CCA 插件 → 開啟插件資料夾」會直接打開它。
2. 「重新掃描」後，插件出現在清單裡，預設**未啟用**。有錯誤的插件會列出每一項錯誤，不能啟用。
3. 「檢視並啟用」會列出插件要加入的全部內容：主題、模式改動的每一項設定與附給 AI 的指示全文、每個指令的範本。勾選確認後才啟用。
4. 啟用時 CCA 記下 `plugin.json` 的 SHA-256。之後只要檔案被修改，插件就自動停用，標示「內容已改變」，必須重新檢視。

CLI 對應的指令：`/plugins` 列出插件與錯誤，`/plugins enable ID` 印出全部內容後輸入 `ALLOW` 啟用，`/plugins disable ID`、`/plugins scan`、`/plugins open`；`/plugin-mode 模式ID` 套用模式，`/plugin-mode off` 清除（`/mode` 仍是權限選單）。

範例：[examples/plugins/night-shift/plugin.json](../examples/plugins/night-shift/plugin.json)。把 `night-shift` 資料夾複製到插件資料夾即可試用。

## 格式

```json
{
  "schema": 1,
  "id": "night-shift",
  "name": "夜班",
  "version": "1.0.0",
  "author": "MoriTeahouse",
  "description": "說明",
  "themes": [],
  "modes": [],
  "commands": []
}
```

- `id`：小寫英數字與連字號，2 到 40 字，不可與其他插件重複。`name` 必填。
- 每個插件最多 8 款主題、8 個模式、16 個指令；`plugin.json` 不超過 64 KiB；最多讀取 32 個插件資料夾。
- 文字欄位不可含控制字元；指示與範本可以換行。

### 主題

```json
{ "id": "ember", "name": "餘燼", "scheme": "dark", "shape": "slash", "base": "hunt",
  "palette": { "rail": "#060504", "side": "#0b0908", "chat": "#100d0b", "panel": "#0b0908", "input": "#1a1512",
               "hover": "#221b16", "selected": "#33210f", "line": "#3d2f24", "text": "#eee4da", "muted": "#b3a294",
               "signal": "#ff8a3d", "brand": "#c8551d", "ink": "#ffffff", "accents": ["#ff8a3d"],
               "display": "Bahnschrift, \"Segoe UI\", sans-serif" } }
```

- 顏色一律是 `#rrggbb`；`text` 與 `muted` 對 `chat` 的對比必須至少 4.5:1，否則不能啟用。
- `shape` 只能選 CCA 既有的形狀：`cut`、`soft`、`paper`、`round`、`editor`、`sticker`、`sky`、`tactical`、`signal`、`ribbon`、`pumpkin`、`slash`。
- `base` 可指定一款內建選用主題（例如 `hunt`、`claude`、`blessing`），插件主題就沿用它的裝置（切角、區段標記、背景圖樣），只換顏色。沒有 `base` 時只改顏色與形狀。原生 `cca` 不能當 `base`。
- `display`、`reading` 只能列出已安裝的字型名稱（英數字、空格、逗號、引號與連字號），不能放 `url()` 或其他 CSS。
- 插件主題的 token 由 CCA 用與 `tools/make_themes.py` 相同的規則從 `palette` 算出；亮色主題的成員與狀態色會換成紙面上讀得清楚的版本。主題的 ID 是 `p.<插件ID>.<主題ID>`，在「介面主題」裡和內建主題一起出現，CLI 用 `/theme p.night-shift.ember` 套用。插件停用時改顯示 CCA 原生主題，選擇仍保存，重新啟用後恢復。

### 模式

```json
{ "id": "strict", "name": "嚴格審查", "scope": "group",
  "settings": { "permission": "ask", "maxRounds": 10, "regressionGuard": true },
  "instructions": "每一項修改都要說明它修正的具體失敗……" }
```

- `scope`：`group`、`dm` 或 `any`（預設）。
- `settings` 只接受：`permission`（`plan`、`ask`、`edit`；**不能**是 `bypass`）、`maxRounds`（1–20）、`lead`、`autoFallback`、`geminiMember`、`localMembers`、`regressionGuard`、`daily`（只在私訊有效）。插件不能設定測試指令，測試守門要等使用者自己填了指令才會生效。
- 在頻道設定的「插件模式」選擇後，設定立即套用到這個對話，`instructions` 會附在每一輪送給 AI 的說明裡，前面標明來自哪個插件模式，並註明不能改變工具權限、測試要求或 VERDICT 規則。取消模式只停止附加指示，不會把設定改回去。

### 指令

```json
{ "name": "security", "description": "以安全審查的角度檢查", "scope": "any",
  "template": "請以安全審查的角度檢查以下範圍：{{input}}" }
```

- 在輸入框打 `/security 登入流程` 送出，主機把它換成範本，`{{input}}` 換成指令後面的文字；範本裡沒有 `{{input}}` 時，文字接在範本後面。對話紀錄裡保存的是換好的全文。
- 名稱不能與 CLI 內建指令相同（例如 `help`、`theme`、`update`、`plan`），兩個插件同名時只有先載入的那個有效。桌面版裡以斜線開頭、但不是插件指令的文字照原樣送出；CLI 把斜線開頭的輸入當成指令，不認得時會提示。

## 安全界線

- 插件沒有程式碼、沒有 CSS、沒有網路與檔案權限；能改變的只有上面列出的資料。
- 主題只用 CCA 已有的裝置，顏色經過格式與對比檢查。
- 模式的指示是使用者看過並核准的文字，仍屬於提示詞的一部分：它能影響 AI 的做法，但 CCA 的規範審查、權限與測試閘門照常執行。
- 檔案改動即停用，避免「核准後偷換內容」。
- 特殊版本不載入插件；普通版本的插件核准會保存起來，回到普通版本時恢復（見 [版本路線](editions.md)）。

## 驗證

`build.bat edition-test` 以真實資料夾測試掃描、驗證錯誤、雜湊核准、主題 token、模式範圍、指令展開與檔案改動後停用；`node tests/editions_browser.cjs` 在 Edge 檢查設定頁、檢視對話框、插件主題與模式選單。
