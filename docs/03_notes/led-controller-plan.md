# LED Controller 規劃（C 版，無 OOP）

> 2026-11-10。**性質：規劃／記錄，尚未實作。**
> 目前沒有 LED 燈帶／16-lane 硬件可測，所以這一頁先把契約、backend、量測閘門寫清楚；
> 不在沒有硬件的情況下盲寫 I80 backend。
> 相關：[hw-pwm-uart.md](hw-pwm-uart.md)（PWM 是低層，不是 LED Controller）、
> [buffers.md](buffers.md)（led.frame／led.wave）、[driver-layer.md](driver-layer.md)。

---

## 1. 為什麼是 `nc_led`，不是把 PWM 叫 LED Controller

| 層 | 職責 | 現在的狀態 |
|---|---|---|
| `drv_pwm` | 一個 GPIO 的 LEDC duty | ✅ 已做（12-bit、dStay） |
| `nc_led`（controller） | N 顆 LED、frame、show／clear／dStay、顏色順序 | ⏳ 本規劃 |
| `pixel` / effect | 算 frame（layout／modes／effects） | ⏳ 未做 |
| `drv_i80` | 16-lane WS2812／SK6812 波形 | ⏳ 未做（等硬件） |

PWM 是「單通道、沒有 frame、沒有 GRBW」；LED Controller 是「整條燈帶」。
PCA9685 可以是其中一個 backend，但 PWM 不是 controller 本身。

---

## 2. C 的界面（沒有 class）

用 `struct + ops`，和 `nc_bus`／`nc_task`／`nc_fx_backend` 同一種做法：

```c
typedef struct nc_led nc_led_t;

typedef struct {
    esp_err_t (*init)(nc_led_t *led);
    esp_err_t (*show)(nc_led_t *led);
    esp_err_t (*clear)(nc_led_t *led);          // 回填 dStay
    esp_err_t (*set_pixel)(nc_led_t *led, int idx, uint32_t rgbw);
    esp_err_t (*set_brightness)(nc_led_t *led, uint8_t bri);
} nc_led_ops_t;

struct nc_led {
    const char *name;          // "led0"
    const nc_led_ops_t *ops;
    void *ctx;                 // backend 私有
    uint16_t count;            // LED 數
    uint16_t d_stay;           // 停止／歸零的中性值（舊 dStay）
    uint8_t  order;            // RGB / GRB / GRBW…（backend 解釋）
};
```

- `show()` 只負責把目前 frame 交給 backend；不計算效果。
- `clear()` 回填每個 pixel 的 `d_stay`（12-bit 或 8-bit 由 backend 定）。
- `set_pixel()` 是方便 API；大量更新可以直接拿 frame buffer（見 §3）。
- Register 方式：`nc_led_register(&led)`，之後 pixel task 用名字找；
  不需要 C++、不需要 inheritance。

---

## 3. Frame／buffer 契約

| buffer | where | 大小 | 用途 |
|---|---|---|---|
| `led.frame` | **P** | 38,400 × 2 | 16×600×4 B GRBW；render 寫、show 讀 |
| `led.wave` | **S** | 28,800 × 2（24-bit）或 38,400 × 2（32-bit） | I80 DMA 波形；**S3 I80 只讀 internal** |
| `pixel.tile` | S | 16 KB | 多 pass 工作區 |

- `show()` 之前要 flush PSRAM frame（msync 約 0.4–0.5 ms）。
- I80 DMA 完成才可以還 frame slot（`nc_buf_get_end`）。
- `d_stay` 是「停止／熄燈」的中性值：LED 0、motor 2048（12-bit）。

---

## 4. Backend 順序

| Backend | 狀態 | 什麼時候做 |
|---|---|---|
| `mock` | ⏳ 可先做 | 無硬件；host test 驗 interface／dStay／double buffer |
| `i80` 16-lane WS2812／SK6812 | ⏳ 等硬件 | 1 lane 先量時序，再 16 lane |
| `spi` APA102 | ⏳ 之後 | 有 SPI 燈帶 |
| `pca9685` | ⏳ 之後 | I2C PWM 燈 |

**S3 I80 硬規則**：
- DMA 只可以讀 internal SRAM → `led.wave` 一定要 S。
- GPIO **33–37 是 N8R8 octal PSRAM**，16-lane 排線一定要避開。
- 16 lane 是並行，不會縮短每條 lane 的串列時間。

---

## 5. 50 FPS 閘門（等硬件才可量）

| 協定 | 600 顆／lane | 上限 | 50 FPS？ |
|---|---:|---:|---|
| WS2812 24-bit | 18.0 ms | 55.5 FPS | ✅（2 ms 餘裕） |
| SK6812 GRBW 32-bit | 24.0 ms | 41.7 FPS | ❌ 標準 800 kbit/s；要超頻或 ≤500 顆 |

**未拍板**：16×600 是 24-bit 還是 GRBW 32-bit。這個決定會直接改變 backend 編碼和 50 FPS 結論。

量測閘門（有硬件才做）：
1. 1 lane × 600：示波器看 30 µs／LED、reset gap、第一顆／最後一顆。
2. 16 lane × 600：frame cadence、p99 ≤ 20 ms、dropped=0。
3. 另一核 PSRAM copy／CPU stress 同時跑，量 `show()` 的 submit／wire／fill。
4. 用 `0xF030` 的合成 I80 DMA 結果當參考（submit 70–122 µs、wire 1.9–7.3 ms）。

---

## 6. 分階段（沒有硬件時只做 L0）

| 階段 | 內容 | 硬件需求 |
|---|---|---|
| **L0** | `nc_led` interface＋mock＋host test（frame／dStay／double buffer） | 無 |
| **L1** | `nc_led_i80` 1 lane × 600，示波器驗時序 | 燈帶＋邏輯分析儀 |
| **L2** | 16 lane × 600，50 FPS／stress | 16 lane 燈板 |
| **L3** | Pixel domain（layout／modes／effects）接 `nc_led` | L2 |
| **L4** | Audio／video scene 綁定 | L3 + audio |

**現在只建議做 L0**；L1 之後要真硬件，因為 I80 時序、level shifter、電源、GRBW 都會在真機才暴露。

---

## 7. 與網絡的次序

WiFi 憑證／自動連線已完成；S3 的 NC4 over TCP／UDP 仍未做。
建議 **L0（LED interface）→ 網絡 transport → L1**，還是 **L0 → L1 → 網絡**：

- 若你要先看到燈：L0 → L1。
- 若你要先用手機／PC 經 WiFi 控制：L0 → 網絡 → L1。

目前沒有 LED 硬件，先做 L0 與網絡都不會被硬件阻塞；但 **L0 很小**，可以先做。

---

## 8. 未定

1. 16×600 是 WS2812 24-bit 還是 SK6812 GRBW 32-bit？
2. 50 FPS 是否硬需求？若是 GRBW，需超頻或減到 500 顆／lane。
3. 16 lane 的實際 GPIO 排線（避開 33–37、SD、USB、console）。
4. Level shifter／電源／每 lane 保險絲。
5. `dStay` 對 LED 是 0；motor 是 2048；PCA9685 是否用 12-bit？
