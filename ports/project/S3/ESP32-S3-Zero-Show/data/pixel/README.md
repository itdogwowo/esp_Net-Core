# pixel — 8×8 Show 內容（低亮度呼吸）

> 2026-11-21：原本 `white80`（80% 白）實測太光，64 顆 WS2812B 全白約 3 A，
> 已改成**低亮度呼吸**。現時開機預設是 `breathe`，`white=0`，8-bit 最高約 **23/255**
> （約 9%），64 顆合計約 0.3–0.4 A，USB 供電安全。

## 四層

| 層 | 檔案 | 內容 |
|---|---|---|
| 1. effect | `effects/effects.json` | `breathe`：`math_now`（正弦）、`F=10`、`l_max=384`（12-bit）、`end_Time=256`、`step=1`、`spacing=0`；**256 幀一個完整呼吸循環**（50 fps 約 5.1 s，0 → 23 → 0） |
| 2. map | `map/matrix.json` | id 1 / `matrix`；`full` = 全部 `ws2812` |
| 3. mode | `modes/white80.json` | id 1 / 名稱 **`breathe`**、`mapping=matrix`、`white=0`、`matrix.full × breathe × rgb`；檔名保留 `white80.json` 只為兼容上傳工具，內容已不是 80% 白 |
| 3b. mode | `modes/color.json` | id 2 / `color`、`white=0`、同一個 effect（預留手動切換） |
| 4. show | `registry.json` | `auto_play=true`、`list=["breathe"]`；開機只播第一個 |

## 亮度

- 效果值域是 12-bit；`l_max=384` → 8-bit 約 `(384*4095/4096) >> 4 = 23`。
- `write="rgb"` 寫 R/G/B，W=0；`white=0` 不再混白。
- 所以呼吸最亮約 24/255，離 80% 白的 204 很遠；如仍覺得光，把
  `l_max` 降到 128（8-bit 8）或 64（4）再上傳。
- 不要同時開 `white: 80` 與 `l_max: 4095`；那會回到 3 A 等級。

## 上傳／切換

```bash
python3 tools/show_upload.py --via http://127.0.0.1:8011 --check
python3 tools/show_upload.py --via http://127.0.0.1:8011 --reboot
python3 tools/show_verify.py --via http://127.0.0.1:8011 --max-led 40
```

- 內容改完要重開（`--reboot`）令 `content_task` 重新載入；firmware 不需要重燒。
- `0xF06D` payload `01` 切 `breathe`、`02` 切 `color`（兩者現時同 effect）。
