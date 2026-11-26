# pixel — Matrix 8×8 LED Textkit 的 JSON 燈效

> 板載 64 顆 WS2812B 串成一條鏈（資料腳 **GPIO14**），8×8 = **64 px、1 條 lane**。
> I80 的 D0 = GPIO14 真的接燈；D1..D7／WR／DC 是 I80 硬性要求的 dummy（不要接東西）。
>
> **燈效 100% 由這裡的 JSON 決定，唔使改 C、唔使重燒 firmware。**
> 改完 JSON → 上傳去裝置 `/data/pixel` → reboot（或 `0xF06F` reload）就生效。

## 1. 四層（firmware 開機掃這四層）

| 層 | 檔案 | 內容 |
|---|---|---|
| effect | `effects/effects.json` | 波形程式（`math_now`＝正弦）＋播放參數（step／spacing／offset／speed） |
| map | `map/matrix.json` | 一組 group：`full` = 全部 64 顆 `ws2812` |
| mode | `modes/*.json` | 邊個效果寫邊個 group、寫邊條通道（`r`／`g`／`b`／`rgb`…） |
| show | `registry.json` | 播放清單（`auto_play` ＋ `list`） |

一個 mode 最多 8 項；一個 effect 最多 16 段；值域全部 **12-bit（0..4095）**。

## 2. 三個 mode（開機播 `rgb_breath`，`0xF06D` 切）

| # | mode | 效果 | 做法 |
|---|---|---|---|
| 1 | `rgb_breath` | **RGB 呼吸（變色）** | 三個效果各寫 `r`／`g`／`b`，正弦相位相差 120° → 顏色沿色環轉；三相之和恆定，所以總亮度唔會閃 |
| 2 | `rainbow` | **彩虹循環** | 同 1 一樣的 120° 三相，但每個效果多一個 `spacing=4`（每顆燈多一個空間相位）→ 64 顆攤開剛好 1 個色環，`step=1` 令它每幀漂 1/4 顆（約 5.1 s 一圈） |
| 3 | `white_breath` | **亮度呼吸（真正變光暗）** | 一個效果、`write: rgb` → 三個通道同步，亮度 0→23/255→0，160 幀（約 3.2 s）一個循環 |

> 想「染色呼吸」（固定顏色、只變光暗）：抄 `white_breath`，把 `write` 拆成 `r`／`g`／`b`
> 三項、三項引用同一個 effect，再各自乘上想要的比例（例如 r 384／g 256／b 128 ＝ 暖橙）。

## 3. 條式（改參數前先睇）

```
正弦：sin01 = sin(2π · phase)；value = l_lim + (sin01 · (l_max − l_lim)) >> 12   （12-bit）
相位：phase = (phi·16 + step_phase · g) & 0xFFFF
       step_phase = (65536 · F) / 10 / end_Time      → F=10、end_Time=N 時，一個正弦週期 = N 幀
時間：g0 = ((t / speed) · step + offset) % total      → 每幀 g 前進 step 格
空間：第 i 顆燈的 g = (g0 + i · spacing) % total
```

- 50 fps 之下：`end_Time = 160` → 3.2 s；`end_Time = 256` → 5.1 s。
- `phi`：4096 = 360°，所以 **1365 ≈ 120°**、2730 ≈ 240°。三個通道各差 120° 就會顏色輪轉。
- `spacing`：`total / spacing` ≈ 一條鏈上幾個色環。**64 顆想 1 個色環 → `spacing = 256/64 = 4`**。
- `step`：彩虹漂移速度。`step = spacing` 時每幀漂一整顆（快）；`step=1` 最慢。

## 4. 「想改咩」對照表

| 我想要 | 改邊個 |
|---|---|
| 更光／更暗 | `l_max`（12-bit）。**注意電流**：見下節。 |
| 呼吸快／慢 | `end_Time`（幀）。160 = 3.2 s、80 = 1.6 s。 |
| 彩虹密／疏 | `spacing`。4 ≈ 1 圈／64 顆；2 ≈ 2 圈；8 ≈ 半圈。 |
| 彩虹漂快／慢 | `step`（大 = 快），或者 `speed`（大 = 慢）。 |
| 換方向 | `reverse: true`。 |
| 加第四個通道（RGBW 燈） | 多一個效果寫 `w` 通道（`write: "w"`）。 |
| 加新 mode | 抄一個 `modes/*.json`、改 `id`／`name`／`index`，再喺 `registry.list` 加入。 |

> ⚠ 一個 mode 最好**寫齊 R／G／B**（三項 `r`+`g`+`b`，或者一項 `rgb`）。layout 只會寫
> 它覆蓋到的通道，冇寫的通道會留住上一個 mode 的值（切 mode 之後會殘留顏色）。

## 5. 亮度／電流（USB 供電）

- `l_max = 384` → 8-bit 約 `(384·4095/4096)>>4 ≈ 23`／255（約 9%）。
- 三相模式任一瞬間三條通道合計 ≈ 1.5 × 單通道（正弦 120° 之和恆定 1.5×峰值），
  所以 64 顆大約 **50–80 mA**，USB 500 mA 安全。
- 想推光：`l_max` 每加倍電流約加倍。`l_max = 1536`（8-bit 96）64 顆約 0.3 A，要留意供電。
- **唔好**同時把 `l_max` 拉到 4095 又 `white: 80`（會回到幾安培等級）。

## 6. 本機驗證 → 上傳 → 生效

```bash
# ① 本機驗 JSON（唔使板）：parse＋end_Time 嚴格遞增＋mode 引用的 group／effect 存在
python3 tools/show_upload.py \
    --src ports/project/S3/ESP32-S3-Matrix-LED-Textkit/data/pixel --check

# ② 上傳（WebMaster REST；預設 http://127.0.0.1:8011）
python3 tools/show_upload.py --via http://127.0.0.1:8011 \
    --src ports/project/S3/ESP32-S3-Matrix-LED-Textkit/data/pixel

# ③ 令 firmware 重新載入（reboot；或用 0xF06F reload）
python3 tools/show_upload.py --via http://127.0.0.1:8011 --reboot
#   或者： python3 tools/nc4.py --port <PORT> reboot

# ④ 睇下有冇載入成功
python3 tools/nc4.py --port <PORT> status tasks     # 四條 pixel 任務 level=4／running=1
python3 tools/nc4.py --port <PORT> raw 0xF06B 00    # 預覽（ShowFrame）
python3 tools/nc4.py --port <PORT> raw 0xF071 00    # 管線統計（fps／apply µs／drops）
```

- 上傳只管 `/pixel/{effects,map,modes,registry}`，**唔會碰** `/data/config`。
- 缺檔／壞 JSON 唔會 boot loop：paint 會 log warning 並填黑，修好再 reload 即可。
- `show_upload.py` 的 `--src` 預設是 Matrix-Show 的目錄，記得帶 `--src`。

> ⚠ **`/data/config/system.json` 的 `links[].msc` 要 0（MSC 關）**：
> `msc:1` 會把 FAT 交給 USB host（Windows 見到 `NC-xxxx` 磁碟機），之後裝置自己
> **讀唔到** `/data/pixel` → `0xF06F` reload 會「掃唔到 /data/pixel/effects」並 fallback
> 每幀填黑，`show_upload.py` 也會 `err_busy`。
> 要「改 JSON → reload → 即刻見效」就保持 `msc:0`；`msc:1` 只適合純粹由 PC 拖檔案
> 進去、然後重開機給裝置重新讀一次。

## 7. 為什麼要開 `CONFIG_NETCORE_CONTENT`

Textkit 原本只有輸出層（I80／encoder／render），沒有生產者 → 燈唔會亮。
本 project 的 `sdkconfig.defaults` 現在有：

```
CONFIG_NETCORE_CONTENT=y              # pixel_effect_task（算值流）＋ pixel_paint_task（寫環）
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192  # paint 的 on_start 要解析 map／mode
CONFIG_NETCORE_PIXEL_PACK_PSRAM=y     # Show 的 I80 packed 波形固定 PSRAM
```

改完要 `--fresh`（sdkconfig 已存在時 IDF 不會重套 defaults）：

```bash
IDF_PATH=$HOME/esp/esp-idf python3 make.py build ESP32-S3-Matrix \
    --project ESP32-S3-Matrix-LED-Textkit --fresh
python3 tools/ota.py --port <PORT> \
    artifacts/ESP32-S3-Matrix__ESP32-S3-Matrix-LED-Textkit.bin
```

之後改燈效**只改 JSON**，唔使再重燒。
