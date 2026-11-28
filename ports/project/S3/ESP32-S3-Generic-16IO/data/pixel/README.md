# ESP32-S3-Generic-16IO 功能測試

16 條獨立 WS2812 lane，每條 **16×16＝256 px**；frame = 4,096 px。
所有效果／mode 都係 canonical RGBW 4 通道；效果層只出 12-bit 值，顏色由配對層 `write` 決定。

## 四個 mode（用 0xF06D 切換）

| id | mode | 每塊 panel 的內容 | 效果類型 |
|---|---|---|---|
| **2** | `led16grid4`（預設） | **4 段**（每段 64 px）各自不同顏色＋效果 | math_now／square_wave_now／pulse_wave／keep |
| 1 | `led16test` | **1 整塊**，空間波浪（蛇形行走） | 全部六種：math_now／square_wave_now／pulse_wave／pulse／keep／starter（starter=黑） |
| 3 | `led16grid16` | **16 段**（每段 16 px）各自不同顏色／效果 | math_now／square_wave_now／pulse／keep |
| 4 | `led16sync` | **整塊同步呼吸**（spacing=0） | math_now／square_wave_now／pulse_wave／keep |

> 「4 段／16 段」係**鏈上線性分段**（group sel 的 px 範圍），唔一定係 2×2／4×4 幾何格；
> 實際排法取決於 panel 的 chain 次序（row-major／serpentine）。

## 顏色

- 顏色由 mode 的 `write` 決定：`r`／`g`／`b`／`rgb`（白）。
- 單通道 write 會把其餘 3 個通道填 0；配對層保證每個 pixel 永遠係完整 RGBW。

## 切換 mode

```bash
# 0xF06D payload = mode id（led16grid4=2, led16test=1, led16grid16=3, led16sync=4）
python3 tools/nc4.py --port <PORT> raw 0xF06D 02
```

`registry.json` 的 `auto_play=true` 開機播第一項 `led16grid4`。

## 亮度

`l_max` 64–512／4096（1.5–12.5%），避免 16×256 全亮電流；房間暗少少會清楚啲。
