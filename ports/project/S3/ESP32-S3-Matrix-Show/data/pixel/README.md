# pixel — 8×8 Show 內容（80% 白 ＋ 20% 效果）

呢個目錄係 **Show 內容的主機端來源**（會上傳到裝置的 `/data/pixel`）。
內容共四層，firmware 開機時由 `nc_px_layer_load_all("/data/pixel")` 一次載入；
64 顆 WS2812（8×8 matrix）串成一條鏈。

```text
data/pixel/                 →  裝置上
├─ effects/effects.json     →  /pixel/effects/effects.json   （第 1 層：波形 program）
├─ map/matrix.json          →  /pixel/map/matrix.json        （第 2 層：群組排列）
├─ modes/white80.json       →  /pixel/modes/white80.json     （第 3 層：mode，white=80）
├─ modes/color.json         →  /pixel/modes/color.json       （第 3 層：mode，white=0）
├─ registry.json            →  /pixel/registry.json          （第 4 層：播放清單）
└─ README.md                   （只留主機；工具唔會上傳呢個檔）
```

## 四層的分工

| 層 | 檔案 | 內容 | 在這裡的意思 |
|---|---|---|---|
| 1. effect | `effects/effects.json` | `colorwave`：兩段 program（`math_now` F=10 end_Time=128 → `square_wave_now` F=17 end_Time=256）＋空間參數 `step=3`／`spacing=7`／`offset=11`／`speed=1`／`reverse=false` | 12-bit（0..4095）波形；`end_Time` 嚴格遞增，最後一段 = 波長 256 幀 |
| 2. mapping | `map/matrix.json` | id 1／name `matrix`；group `full` = `{"type":"ws2812","sel":":"}` | 群組 `full` = 整條 64 顆燈鏈 |
| 3. mode | `modes/white80.json`、`modes/color.json` | id 1 `white80`（`white: 80`）、id 2 `color`（`white: 0`）；兩者都 `mapping: matrix`、`map: [{group: "matrix.full", effect: "colorwave", write: "rgb"}]` | 委派：邊個 group 用邊個 effect、寫邊幾個通道，再加白度 |
| 4. show | `registry.json` | `auto_play: true`、`list: ["white80", "color"]` | 開機自動播 list 第一個 mode（white80） |

每幀資料流：`registry 選 mode → mode.mapping 找 map → 逐 entry 找 effect → 建 plan →
program 算 12-bit 值 → write 散入 R/G/B/W 四通道 → white mix → 8-bit → 送燈`。

## 80% 白混合公式

firmware 讀 mode 的 `"white"`（0..100，預設 0），對**每個像素的四個通道都做**：

```text
out = (out * (100 - w) + 255 * w) / 100        # 整數除法；四通道 R/G/B/W 都做
```

- `w = 80`：每通道底 = `255 * 80 / 100 = 204`，再疊 20% 效果。
  即 8×8 睇落係一片白（四通道都有 204 以上），但效果仍然在白色上面輕微流動。
- `w = 0`：完全唔混白，輸出就係效果原本的顏色。

例：效果某一刻係 `R=0, G=255, B=0`，`mode` 的 `white=80`、`write="rgb"`：

| 通道 | 效果值 | 混 80% 白後 |
|---|---|---|
| R | 0 | `0*20/100 + 204 = 204` |
| G | 255 | `255*20/100 + 204 = 255` |
| B | 0 | `204` |
| W | 0（`write:"rgb"` 只寫 RGB，W 保持 0） | `0 + 204 = 204` |

所以 `white80` 睇落係「白底 80% ＋ 20% colorwave」；`color`（white=0）係純 colorwave。
`write:"rgb"` 的 W 通道本身由效果寫 0，混白之後 W 亦會變 204 —— 四通道都偏白是預期行為。

## 如何切換 mode

裝置開機時按 `registry.json` 的 `list` 順序取第一個 mode 播（`auto_play: true`）：

- 想開機由 `color` 開始：把 `registry.json` 改成
  `{"version":1,"auto_play":true,"list":["color","white80"]}`。
- 想改白度：直接改 `modes/white80.json` 的 `"white"`（0..100），例如 50 = 一半白。
- 想換效果組合：改 mode 的 `map[]`（`group`／`effect`／`write`；`write` 可為
  `r`/`g`/`b`/`w`/`ww`/`rgb`/`rgbw`/`wwww`；有需要可加 `range`，Python slice 語義）。
- mode／registry 是**開機載入**：改完上傳後重開裝置（`--reboot`）才會生效。

上傳（只會 PUT 上面 5 個檔，唔會刪或覆蓋其他 `/pixel`、`/data` 檔案）：

```bash
python3 tools/show_upload.py --via http://127.0.0.1:8011            # 上傳
python3 tools/show_upload.py --via http://127.0.0.1:8011 --reboot   # 上傳後重開
python3 tools/show_upload.py --check                                # 只做本機 JSON 檢查
```

`--base /pixel`（預設）控制裝置目標目錄；`--port` 直接開 CDC 未支援，請用 `--via`。
上傳工具會先驗 effects／map／modes／registry：JSON parse、`end_Time` 嚴格遞增、
mode 引用的 group／effect 名存在、`white` 在 0..100。

> **目錄前提**：`/data/pixel`、`/pixel/effects` 由 firmware seed；`/pixel/map`、`/pixel/modes`
> 亦要存在先寫得入（FILE_BEGIN 唔會自動 mkdir 父目錄）。ESP32-S3-Matrix-Show 的 firmware
> 現在兩邊都會建：`main.c` 的 `seed_dirs()`（每次開機）＋ show_task 的
> `ensure_pixel_dirs()`；如果上傳 map／modes 見到 `err_write_fail`，多數係燒咗舊映像／
> 未 boot 過，可先用 MSC 模式（USB 磁碟）手動建目錄。
