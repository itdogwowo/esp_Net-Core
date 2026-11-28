# LED 系列（`LED_*`）：照 Net-Core 現有 controller section 讀

> 2026-11-28（第二版，按用家修正）。**狀態：LED_WS2812 讀取＋render 接線已實作**（見 status §7.310）；
> LED_PWM／LED_PCA9685／LED_APA102 只有 config 形狀預留，backend 未做。
> 用家指示：唔好另設中央 `LED` 設定／system 設定；照 Net-Core 其他控制器管理器
> 的 `{enable, list:[...]}` 形狀，逐段讀就得。舊專案參考：
> `mp_Net-Core/slave/config.json` 的 `WS2812`／`APA102`／`PCA9685`／`PWM`，
> 同 `slave/driver/*_drv.py` 的讀法。

---

## 1. 原則

1. **一個 controller 一段**：`LED_PWM`、`LED_WS2812`、`LED_PCA9685`、`LED_APA102`。
2. 每段只有 `{ "enable": 0|1, "list": [ ... ] }` —— 同 `WS2812`／`APA102`／
   `PCA9685`／`PWM` 一模一樣。
3. **冇中央 `LED` block**、冇 `fps`／全域 `brightness`／全域 `d_stay` 呢類系統設定；
   `d_stay`（中性值）屬於每個 list item。
4. 腳位仍然只喺 `hardware.json`；`list[]` 嘅 item 用 `bus:{type,index}` 引用
   （I80／SPI／PWM 已型別化；PCA9685 要等 I2C 型別化）。
5. **timing 唔寫入 config**：選咗 driver（`LED_WS2812`／`LED_APA102`…）就由
   driver 內建該格式的 timing／波形；config 只寫實際接線＋幾何＋中性值。
6. **一個 list item = 一條線**（同舊 Net-Core `WS2812.list` 一樣，每條 GPIO 一項）；
   `bus.index` 就係「第幾條線」：S3 只有一個 I80 → index 0..N-1 對應 data pin；
   將來 RMT → index = RMT channel。**冇 `lanes`**。
5. 中央 class 係**code 層**（`nc_led` ops）；config 只係四段 leaf 設定。

---

## 2. `driver.json` 形狀（簡化版）

```jsonc
"LED_PWM": {
  "enable": 0,
  "list": [
    { "bus": { "type": "pwm", "index": 0 }, "px": 1, "d_stay": 0 }
  ]
},

"LED_WS2812": {
  "enable": 1,
  "list": [
    { "bus": { "type": "i80", "index": 0 }, "px": 256, "order": "GRB", "d_stay": 0 },
    { "bus": { "type": "i80", "index": 1 }, "px": 256, "order": "GRB", "d_stay": 0 }
  ]
},

"LED_APA102": {
  "enable": 0,
  "list": [
    { "bus": { "type": "spi", "index": 0 }, "px": 300, "order": "BGRW", "d_stay": 0 }
  ]
},

"LED_PCA9685": {
  "enable": 0,
  "list": [
    { "bus": { "type": "i2c", "index": 0 }, "address": ["0x40"],
      "px": 16, "d_stay": 2048 }
  ]
}
```

- `list` 長度上限 16（I80）；每條線只寫實際接了的線，唔需要填滿所有 data 腳。
- 每條線 `px` 可以不同；**每條線有自己內容**：
  - canonical frame 長度 = Σ px（每條線一段，offset 累加）；
  - I80 DMA／傳輸時間 = max(px)（最長線；短線完成後出 reset，不佔額外 frame buffer）；
  - 冇寫入 list 的線 = 0 buffer／0 lane。
- 同型多條 = `list` 多個 item（同舊 Net-Core 一樣，每條線一項）；
  total px = 所有 item 的 `px` 加總；driver 按 `bus.type` 分組，同一 I80 的多個 item
  會在一個 transaction 內一齊送。
- **冇 `timing`／`format`／`backend`**：section 名就係 driver；格式、時序、
  reset gap 由該 driver 決定（例如 LED_WS2812 → ws2812 24-bit、800 kHz）；
  要換 timing 就換 driver 或改 driver 內建表，唔係改每個 project 的 JSON。

---

## 3. 怎樣「讀」

照現有 `apply_devices()`／`apply_i80()`／`apply_spi()` 的做法，新增一個
`apply_led_series()`：

```
for each section in {LED_PWM, LED_WS2812, LED_PCA9685, LED_APA102}:
    if present && enable:
        n = 0
        for item in list (最多 MAX):
            parse bus:{type,index}   → 對 hardware 層對應 list 的 index
            parse 該型欄位（px/order/d_stay/address…；**唔 parse lanes／timing**）
            存入 nc_led_<type>_cfg_t item[n++]
        out->led_<type>.present = true
```

- **只讀＋驗證**，唔改 render／pixel 行為；舊 `LED` block 照舊讀（過渡期）。
- `unknown` 統計唔會再將 `LED_*` 當未知 section。
- 讀完之後 `status cfg` 可以報 `dev.led.{pwm,ws2812,pca9685,apa102}.{enable,n}`（下一步）。

---

## 4. 中央 class（code 層，不入 config）

```c
typedef struct nc_led nc_led_t;
typedef struct {
    esp_err_t (*show)(nc_led_t *);
    esp_err_t (*clear)(nc_led_t *);        // 回填 d_stay
    esp_err_t (*set_pixel)(nc_led_t *, uint16_t idx, const uint16_t rgba12[4]);
    esp_err_t (*set_brightness)(nc_led_t *, uint8_t);
} nc_led_ops_t;
```

manager 由四段 parsed list 建立：每 item 一個 `nc_led_t`（type／count／order／d_stay／
ops）；pixel frame 分段交對應 channel。class 只喺 code，唔會出現 `"LED"` 設定段。

---

## 5. 實作進度

**2026-11-28 已實作**：
- `LED_WS2812.list[]` 每 item 一條線、px 可不同；parser／ncportgen 產生 `line_px[]`。
- frame = Σ px；i80 backend DMA = max px；`nc_px_codec_pack_bus_seg*` 逐線 offset／active mask。
- 16×600 獨立內容 encode **10.4 ms**（tests 映像；舊同內容 broadcast 6.45 ms）；
  host 402 checks、舊 pack_bus 測試全部保留。詳見 status §7.311。

1. `nc_cfg_types.h` 加四個 `nc_led_<type>_cfg_t`＋list 上限。
2. `nc_cfg_load.c` 加 `apply_led_series()`（讀 `{enable,list}`、驗 bus index）。
3. `nc_cfg.h` 加 accessor；`status cfg` 選擇性報 count。
4. host test：空段／enable=0／正常 list／bus index 越界 → 對應 stat。
5. render 照舊用現有 `LED` block；LED_* 只係「讀咗入 struct」，等 manager 接。

咁就符合用家所講：「其他控制器管理器已經設定好，現在只需要學識讀這些設定」。
