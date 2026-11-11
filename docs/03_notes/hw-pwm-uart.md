# PWM／UART／PIN：硬體設定與驅動（照舊專案）

> 2026-11-10。這頁記錄 `hardware.json` 的 PIN／PWM／UART 三段怎樣被讀、
> 對應的 IDF 驅動，以及上板結果。舊專案參考：
> `slave/driver/pin_drv.py`、`pwm_drv.py`、`uart_drv.py`。

---

## 1. 設定從哪裡讀

`hardware.json`（build-time wiring + 用戶覆蓋）→ `nc_cfg` typed struct →
`nc_cfg_hardware()` → 驅動。

```json
{
  "PIN":  { "enable": 1, "list": [ { "GPIO": 14, "label": "test_in", "mode": "IN", "pull": "UP" } ] },
  "PWM":  { "enable": 1, "list": [ { "GPIO": 21, "freq": 1000, "resolution": 12, "duty": 0, "dStay": 2048, "label": "test_pwm" } ] },
  "UART": { "enable": 1, "list": [ {
      "id": 1, "baudrate": 115200, "rxbuf": 4096, "txbuf": 4096,
      "en_settle_ms": 1,
      "GPIO": { "tx": 47, "rx": 48, "en": -1 }
  } ] }
}
```

- `PIN`：舊專案 `{GPIO,label,mode,pull,initial}`；由 `hw_gpio` owner 在 `on_start` 套用，
  不是 `main.c` 直接 `gpio_config`。
- `PWM`：舊專案只有 `GPIO`；新增可選 `freq`／`resolution`／`duty`／`label`。
  **預設 12-bit：duty 0–4095**（以前 MicroPython 的 10-bit 只是假解析度）。
  `dStay` = 停止／歸零時回到的中性值（舊專案 dStay；燈 0、motor 2048）。
  預設 0；範圍 0–max_duty。
- `UART`：照舊專案：`id`／`baudrate`／`rxbuf`／`txbuf`／`en_settle_ms`／`GPIO{tx,rx,en}`。
  `en >= 0` = RS485 DE；`-1` = 純 UART。

`null` 欄位 = 未接；`enable=0` = 整段不建立。兩者都不會令 parse 失敗（計 `defaulted`）。

---

## 2. 驅動

### PIN

- `components/tasks/hw_gpio` 在 `on_start` 讀 `nc_cfg_hardware()->pin[]`，
  逐支 `gpio_config`，設 output initial 或 input pull。
- 同一個 owner 發佈 snapshot；`0xF036 HW_SNAP` 看得到 levels。
- 已被 SD／PWM／UART claim 的腳位會被 owner 拒絕（`nc_hw_pin_claimed_by_other`）。

### PWM（LEDC）

- `components/driver/drv_pwm`：`nc_pwm_drv_install()`，最多 8 通道。
- 同一 `(freq, resolution)` 共用一個 LEDC timer；不同組合用下一個 timer（最多 4）。
- `duty` 以 resolution bits 為單位；`nc_pwm_drv_set()` 會 clamp。
- **起始位置歸零**：每個通道 `hpoint = 0`，每個 LEDC 週期由 timer zero 開始。
- **停止／歸零用 dStay**：`nc_pwm_drv_stay(idx)` = `duty dStay` + `update_duty`。
  `0xF03C PWM_STAY` 可測；`0xF03D PWM_ZERO` 才是真正 0。
  所以「停止」不是一律 0：LED 的 dStay=0，motor 的 dStay=2048（0x80 死區停）。
- 預設 `freq=1000`、`resolution=12`（duty 0–4095）；`resolution` 仍可選 1–14。

### UART／RS485

- `drv_uart` 的 `nc_uart_drv_cfg_t` 新增 `en_pin`／`en_settle_ms`／`baud`。
- `en >= 0` 時，`nc_uart_drv_write()`：
  1. `en=1`
  2. 等 `en_settle_ms`（預設 1 ms；0 ms 實測會吃 start bit）
  3. `uart_write_bytes`
  4. `uart_wait_tx_done` 等 FIFO 排空
  5. 再等最後 1 byte 離開 shift register（10 bits / baud）
  6. `en=0` 回接收
- 這是舊 `_Rs485Uart` 的同步版本；未做 listen-before-talk（舊版有）。
- `id` = UART port；`rxbuf`／`txbuf` 直接餵 IDF driver。
- `main` 的 `uart_owner_init()` 先找 `hardware.json` 中 enabled 的 UART；
  測試映像才 fallback 7/8 迴路。

---

## 3. 上板結果（A 板，S3）

設定：`PIN 14 IN pull-up`、`PWM 21 1 kHz/12-bit`、`UART1 47/48 115200`。

```json
{"ready":1,"port":1,"baud":115200}        // 0xF03B UART_INFO
{"count":1,"duty":[0]}                    // 0xF03A PWM_INFO（12-bit）
{"levels":16384,...}                      // pin14 = 1（pull-up）
{"idx":0,"count":1,"duty":512,"err":"ESP_OK"}   // 0xF039 PWM_SET
```

- UART driver 真的安裝成功（port1／115200）。
- PWM 通道建立；12-bit duty 可設 **4095／2048／0**。
- PIN 14 input pull-up 在 snapshot 讀到 1。
- 設定經 **OTA 換韌體後仍然保留**（`nc_boot` 不再因 fw_hash 變更格式化 data）。

---

## 4. 測試指令

| Cmd | 用途 |
|---|---|
| `0xF039 PWM_SET` | `[idx][duty_lo][duty_mid][duty_hi]`（duty 0–4095） |
| `0xF03C PWM_STAY` | `[idx]`；回到 config 的 `dStay` |
| `0xF03D PWM_ZERO` | `[idx]`；真正 duty 0 |
| `0xF03A PWM_INFO` | `{"count":N,"duty":[...]}` |
| `0xF03B UART_INFO` | `{"ready":..,"port":..,"baud":..}` |
| `0xF036 HW_SNAP` | GPIO snapshot（PIN 段） |

---

## 5. 尚未 typed／未驅動

`hardware.json` 仍有以下段只會被計 unknown，未有 typed parser／driver：

- `SPI`、`I2C`、`I80`、`I2S`、`ENC`、`XL9555`
- `TFT`／`PCM5102`／`PCA9685` 等 driver.json chip 段（runtime 未讀）

下一步：先做 SPI／I2C（很多 chip 依賴），再做 I80／I2S；`driver.json` 的
user override → runtime merge 也要補。
