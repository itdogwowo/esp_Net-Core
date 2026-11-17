# CPU 頻率：build-time 預設 vs 執行期（未來）

> 2026-11-16。現階段：**S3 全部用 240 MHz（最高）**，暫時以最寬鬆的性能跑；
> 執行期調頻是未來測試，未實作。

---

## 1. 現在：開機預設是 build-time

| 設定 | 檔案 | 作用 |
|---|---|---|
| `cpu_mhz` | `ports/hardware/<chip>/<board>/board.json` | 給人／`board.py check` 看的板子事實 |
| `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_*` | 同板 `sdkconfig.defaults` | 真正決定 bootloader／app 開機頻率 |

目前 S3 四塊板一律 240：

| 板 | CPU |
|---|---:|
| ESP32-S3-Matrix | 240 |
| ESP32-S3-Mini-Nano | 240 |
| ESP32-S3-N16R8 | 240（由 160 改） |
| ESP32-S3-SuperMini | 240 |

要降頻（例如為了功耗／溫度）：

1. 改 `board.json` 的 `cpu_mhz`
2. 改 `sdkconfig.defaults` 的 `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_*`
3. `python3 make.py build <board> --fresh`

**不 fresh 不會生效**：`artifacts/build/<board>/sdkconfig` 會 cache 舊值，改了
`sdkconfig.defaults` 也不會重新套用。

---

## 2. 為什麼 runtime config 現在改不到

- `/data/config/*.json` 沒有 `cpu_mhz` 欄位；
- `sdkconfig` 沒有 `CONFIG_PM_ENABLE`；
- code 沒有呼叫 `esp_pm_configure()`／clock API。

所以「現在改 config」只能改 build-time 預設，不是執行期切換。

---

## 3. MicroPython 為什麼可以 `machine.freq()`

MPY 是 runtime 直接改 clock（IDF 的 PM／clock API），不是改 config 檔。
C firmware 一樣做得到，但要自己處理：

- `CONFIG_PM_ENABLE=y` ＋ `esp_pm_configure({max_freq_mhz, min_freq_mhz,
  light_sleep_enable})`；或
- 較低層 `rtc_clk_cpu_freq_set_config()`（private，ABI 不保證）。

**CPU/APB 一改，以下全部要跟**：

- UART baud divisor（console／NC4 UART）
- SPI／I2S／LEDC／RMT 時鐘源
- flash／PSRAM timing（IDF 會處理，但要驗）
- WiFi 會 hold `ESP_PM_APB_FREQ_MAX` lock，未必即時降頻

---

## 4. 未來測試（未做，先寫下來）

### 方案 B：boot-time config（建議先做）

- `system.json` 加 `cpu_mhz`（0 = 用 build 預設；160／240）。
- B03（capability）在 drivers 初始化前套用。
- 需要 `CONFIG_PM_ENABLE=y`；`light_sleep_enable=false`。
- 驗收：同一 firmware 改 config → 重開機頻率變；UART log 正常；WiFi 正常。

### 方案 C：runtime 熱切換（MPY 式）

- 開 PM/DFS，執行中 240↔160↔80。
- 需要 per-driver 時鐘補償；切換瞬間可能 glitch。
- 驗收：連續切換 100 次，UART／SPI／LEDC／WiFi 不掉；量電流與溫度。

### 測試矩陣（建議）

| 項目 | 160 | 240 |
|---|---|---|
| UART log（115200） | 正常、無亂碼 | 正常 |
| WiFi ping／TCP | 0% loss | 0% loss |
| SPI／LEDC 波形 | 頻率正確 | 頻率正確 |
| 開機時間 | 量測 | 量測 |
| 電流／溫度 | 量測 | 量測 |

> 現階段不做：先用 240 把 NCM／USB 做完，再回來做調頻。

---

## 5. 結論

- **現在**：S3 全部 240，build-time；要降頻改 `board.json`＋`sdkconfig.defaults`＋`--fresh`。
- **未來**：boot-time config（B）優先；runtime 熱切換（C）等有需求再做。
- 不要在「現在的 `/data/config`」找 CPU 頻率，它不在那裡。
