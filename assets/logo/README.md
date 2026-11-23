# CCA 圖標

標誌是兩道相向的弧：淺色是撰寫，天藍是交回來的審查。設計說明見 [CCA 圖標](../../docs/brand-1.0.md)。

| 檔案 | 用途 |
| --- | --- |
| `cca-mark.svg`／`.png` | 淺色標誌、透明背景，用於深色底 |
| `cca-mark-dark.svg` | 深色標誌，用於淺色底 |
| `cca-wordmark.svg`／`.png` | 標誌與 CCA 字樣，深色版本 |
| `cca-wordmark-light.svg`／`.png` | 同一組字標的淺色版本 |
| `cca-app.svg`／`.png` | 加上鋼藍圓角底板的程式圖標；PNG 為 1024 × 1024 |
| `cca-app-*.png` | 16 至 256 像素的各尺寸輸出，對應 ICO 內的 15 張圖 |
| `cca-terminal-preview.png` | 16 × 16 雙色點陣的放大示意；CLI 使用半格字元，佔 16 欄、8 列 |

```powershell
# 由不公開的圖標產生器重建
```

產生器不公開。它同時輸出 `assets/cca.ico`、`ui/logo.svg`、`ui/logo.png`、`ui/icons.js` 的 `logo` 與 `src/logo_art.hpp`。
