# 個人徽章

研究與實作日期：2026-10-26。徽章用來呈現本機資料的身分、參與和版本紀念，不提供工具權限，也不是伺服器驗證的線上帳號系統。

## Discord 目前的展示方式

Discord [2026-10-21 更新的官方說明](https://support.discord.com/hc/en-us/articles/44393096706583-How-to-Manage-Your-Discord-Profile-Badges) 已加入徽章目錄和展示設定。徽章位於個人資料名稱附近；手機入口在名稱下方。展示區最多六枚，其他持有徽章仍能從目錄查看。所有人可隱藏可隱藏的徽章，Nitro 使用者可拖曳重排；Nitro 徽章需保持可見。新取得或升級的徽章預設移到展示區前方。官方沒有公開非 Nitro 預設排序的完整優先表，因此不能把舊網路文章的固定順序當作目前規則。手機徽章目錄仍是部分使用者可用的實驗。

已下載並檢視該文章的[展示位置示意](https://support.discord.com/hc/article_attachments/44421918716823)，確認為靠近名稱的小圖形列。CCA 參考位置、六格展示與目錄的分工，不沿用 Discord 的徽章圖形、稀有度或付費條件。所有 CCA 使用者都能重排及隱藏。

圖形製作另依 [GitHub Primer](https://primer.style/octicons/design-guidelines/) 的原生尺寸與留白原則，以及 [JetBrains 圖標規範](https://plugins.jetbrains.com/docs/intellij/icons-style.html) 的平面輪廓與像素對齊。CCA 自身規範以 [視覺識別](visual-identity.md) 為準。

## 七款徽章

| ID | 名稱 | 圖形依據 |
| --- | --- | --- |
| `developer` | CCA 開發者 | 白色程式括號與銅色斜線，保留開發工具的簡潔輪廓 |
| `community-developer` | 社群開發者 | 銅色與青綠雙人，加上共用基線；與先前獨立社群圖標分開重繪 |
| `origin` | CCA 原初 | 最早期 EXE 的橘綠交環，改成切角雙環 |
| `familiar` | CCA 熟識 | 1.0.0 以前的橘色上段、淺藍側段與圓點、綠色下段 |
| `present` | CCA 今時 | 目前相向銜接的 C，重新配置為 16／24 像素輪廓 |
| `pioneer` | 先鋒支持者 | 銅色前進旗與白色前向符號，紀念早期參與 |
| `member` | 成員 | 鋼藍單人輪廓，所有安裝內建 |

歷代圖標實際取自 `build/CCA-review.exe`、`build/ChatClaudeAgent-0.9.0.exe`、`build/ChatClaudeAgent-1.0.0.exe` 的 PE 圖標資源，沒有執行舊程式。比較圖：[本機歷代來源](../build/badges/research/history.png)。最早可取得的 `CCA-review.exe` 為交環；沒有據此推測一個不存在的版本日期。

每款有 16／24 像素、彩色／單色版本，共 28 個 SVG。16 像素不是直接縮小大圖：人物頭部與環形留白分別調整。所有圖形均為路徑／圓形，沒有漸層、發光、濾鏡、字體或外部資源。大型預覽：[圖形校樣](../build/badges/design-proof.png)。

## 位置與使用

- 左下角點擊本機個人資料 → 名稱區徽章列 → 點擊徽章或「＋N」開啟目錄。
- 完整個人資料頁和設定中的個人資料預覽也顯示徽章。
- 設定 → 我的個人資料 → 管理徽章，可查看完整目錄與取得方式。
- 目錄內選擇已持有徽章，可隱藏、顯示或向前／向後移。前六枚未隱藏徽章顯示在資料卡，剩餘仍屬持有。
- 儲存前只是草稿，取消不會變更已保存設定。主機保存失敗會保留草稿供重試，原設定不變。
- CLI 使用 `/badges` 查看；`/badges hide origin`、`/badges show origin`、`/badges first member` 調整同一份展示設定。CLI 以文字與狀態標籤呈現，不依賴圖形終端或 WebView2。

AI 成員不繼承本機使用者的徽章。開啟目錄、檢視和調整不會呼叫模型或傳送通知。

## 本機授予與資料範圍

無授予檔時，核心仍會提供成員徽章。其他六枚只來自 `%APPDATA%\CCA\badges.json`：

```json
{
  "version": 1,
  "profile": "local-profile",
  "grants": ["developer", "community-developer", "origin", "familiar", "present", "pioneer"]
}
```

`profile` 是本機資料的註記，不會根據名稱比對身分。將顯示名稱改成 `developer` 不會增加徽章。此檔不包含在 EXE 或公開設定範例；[中性範例](../config/badges.example.json) 的授予清單為空。此設計沒有簽章、遠端權威或防竄改保證，能修改本機資料的使用者可自行變更紀念徽章；徽章因此不參與任何權限判定。

授予檔於啟動時載入，最多 64 KiB。版本不符、重複 ID、未知 ID、格式錯誤及讀取失敗均拒絕整份檔案，顯示錯誤並保留原檔，仍有成員徽章。顯示偏好放在 `settings.json` 的 `badgeDisplay.order`、`badgeDisplay.hidden`；核心只接受已持有的完整排序與有效隱藏清單，使用原子寫入。設定檔缺少或損壞的偏好會在顯示時補齊，不會授予額外徽章。

本次依使用者要求，僅在 developer 的實際本機資料新增六枚授予；加上內建成員，共七枚。其他安裝不會繼承。沒有開放社群申請、貢獻審核或通知機制。

## 驗證

原生個人資料測試使用獨立 `CCA_DATA_DIR`，涵蓋啟動、重新載入、檔案大小與格式、名稱不授予徽章、偏好不能授予徽章、保存失敗回復及請求識別。瀏覽器測試 `tests/badges_browser.cjs` 使用真實 Edge 和模擬主機，涵蓋預設成員、七枚目錄、六格展示、排序、隱藏、取消、錯誤重試與窄視窗。CLI 的 ConPTY 測試檢查同一份偏好保存。執行結果及畫面位於本機 `build/badges/`，不隨 Git 發布。

本次實際通過：147 項原生個人資料檢查、30 項徽章 Edge 檢查、9 組 CLI ConPTY 情境；既有桌面整合 18 項、工作流程 DOM 40 項、Token 活動與性格庫 Edge 21 項也通過。徽章加入初期的共用核心單元測試為 573 項通過，後續授予格式與展示變更由原生個人資料測試覆蓋。全部測試未使用真實模型額度。

資產重建 SHA-256 一致，28 個 SVG 只含路徑與圓形。彩色主體相對 `#181B20` 石墨底的對比值為 7.31～14.67；這是指定底色的圖形檢查，不代表整個介面取得任何無障礙認證。桌面與 CLI 另以 `tools/verify_resources.py` 核對內嵌資源；圖形目錄及個人資料卡畫面均已在 Edge 檢視。
