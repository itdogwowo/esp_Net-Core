/* esptool-js loader（把 ES module 掛上 window）
 *
 * 為什麼要這一層：`bundle.js` 是 **ES module**（尾段係 `export {…}`），
 * 而本專案全部係傳統 `<script>`（唔用打包、唔用 import map）。
 * 所以在呢個 module 裡 import 完，再明顯地放上 `window.esptooljs`，
 * 其他 classic script（60-flash.js）就用 `window.esptooljs` 拿到全部 export。
 *
 * 版本：vendor 自 https://unpkg.com/esptool-js/bundle.js（Apache-2.0，見 LICENSE.txt）
 * 想改內部行為：請改上游再重新 vendor（bundle 係壓縮產物，唔適合手改）；
 * 或者在本專案的 60-flash.js 外面包一層（reset 序列、錯誤處理、chip 判斷都在那層）。
 */
import * as esptooljs from "./bundle.js";

window.esptooljs = esptooljs;
// 讓 UI 知道「頁面燒錄」真的可用（bundle 載入成功，唔係 404）
window.dispatchEvent(new CustomEvent("esptooljs-ready", {
  detail: { version: esptooljs.version || null },
}));
