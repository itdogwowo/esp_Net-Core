# esptool-js（vendored）

- 來源：<https://github.com/espressif/esptool-js>
- 取用方式：`https://unpkg.com/esptool-js/bundle.js`（單檔，pako 等依賴已內聯）
- 版本：以取用當日 unpkg latest 為準（2026-11-18；308.5 KB）
- 授權：Apache-2.0（見 `LICENSE.txt`，vendored 必須保留）

## 檔案

| 檔 | 用途 |
|---|---|
| `bundle.js` | **ES module**（尾段 `export {…}`）。唔可以當 classic script 用。 |
| `loader.js` | 我們的薄 shim：`import * as` 之後放上 `window.esptooljs`，並發 `esptooljs-ready` 事件。 |
| `LICENSE.txt` | Apache-2.0 原文。 |

`index.html` 用 `<script type="module" src="/static/vendor/esptool-js/loader.js"></script>` 載入。

## 為什麼唔直接改 bundle.js

`bundle.js` 係 minified build 產物，手改之後冇辦法重現／升級。本專案的做法：

- **想改行為** → 在 `static/js/60-flash.js` 外面包一層：
  - 自訂 reset 序列（`resetConstructors`）
  - chip 白名單／拒絕邏輯
  - 進度、錯誤訊息中文化
  - 燒錄前後的序列埠交接（`/api/port/release` → 燒 → `/api/port/reclaim`）
- **真係要改內部** → clone 上游、`npm install && npm run build`，再把新的
  `bundle.js` 貼返呢度（同時更新上面的版本／大小）。

## 已知限制（來自上游 README）

- 建基於 **Web Serial**：只有 Chrome／Edge 89+（Android Chrome 需 polyfill）。
- **不**提供 ELF→bin 轉換，亦冇 `espefuse.py`／`espsecure.py` 對應功能。
- 需要 **user gesture** 才能 `navigator.serial.requestPort()`。

所以本專案把它定位為**可選的第三條燒錄路徑**（另外兩條：NC4 OTA、Python esptool）。
