# I80 腳位 schema：稀疏 `data`／自動 8↔16-bit／自動 dummy

> **狀態**：設計稿（2026-11-26），**未改任何 code**。實作前要先答文末 §9 的幾個問題。
>
> 相關：[pixel-output-backends.md](pixel-output-backends.md)（後端矩陣與 RMT／PARLIO 待做）、
> `docs/02_guides/configuration.md`（配置四層）、`AGENTS.md` §9。

## 0. 為什麼要有這份

Matrix 板只**真的接了一支** LED 資料腳（GPIO14 = D0），但 LC2026 的 `I80` 段要寫 8 支 data
＋ `wr` ＋ `dc`，全部是「I80 硬性要求的 dummy」，看起來像一堆亂七八糟的魔法數字。
用家要求（2026-11-26）：

1. `data` 接受**稀疏寫法**：`[14, -1, -1, …]`（`-1` = 未用／冇輸入）；
2. `data` 接受**只寫真正接燈那支**：`[14]`；
3. 固件**自動決定 8-bit 還是 16-bit**（由有效腳數／lane 數決定）；
4. 順便問：`wr`／`dc` 可不可以**唔寫**。

另外用家提到 FastLED「好像直接使用 GPIO0」——見 §8：FastLED 單腳是靠 **RMT**，不是 I80。

## 1. 現況（as-implemented，2026-11-26）

### 1.1 解析：`nc_cfg_load.c` 的 `apply_i80()`

| 規則 | code 行為 |
|---|---|
| `enable=false` | `n_i80 = 0`（整段當冇） |
| `data[]` 每個元素 | 要 **0..NC_CFG_GPIO_MAX(54)**；`<0` 或 `>54` → `st->invalid++` 並**丟掉該元素** |
| 元素數 | 必須剛好 **8 或 16**（`e.n_data != 8 && != 16` → **整條 entry 丟棄**） |
| `wr`／`dc` | `get_pin_opt()`（`-1` = 未設）；**任何一個 <0 → `defaulted++` + `continue`（整條不建立）** |
| `cs` | 選填（`-1` 可以） |
| `bus_width` | **由 `len(data)` 推出**（唔另外寫） |

### 1.2 驅動：`nc_i80_out.c` 的 `nc_px_backend_i80()`

- `bus_width` = 8／16（`cfg->bus_width==16 ? 16 : 8`）。
- `lanes = cfg->lanes`（來自 `driver.json` 的 `LED.lanes`），clamp 到 `≤ bus_width`。
- **全部 `bus_width + 2` 支腳**都要 `0..48`、**不可重複**、**不可踩保留腳**
  （`nc_hw_reserved()`：S3 = 26–37、19／20、43／44、0／3／45／46；**GPIO3 只准當 lane 線**，
  例外寫在 `nc_i80_out.c` 的 `allow_strap`）。
- dummy 腳被其他 owner claim 只 `WARN`，不擋整條 lane。
- `pins[]` 的次序：`D0..D(width-1)`、`DC`、`WR`（→ `dc_gpio_num = pins[width]`、
  `wr_gpio_num = pins[width+1]`）。

### 1.3 硬件事實（不可繞）

LCD_CAM 的 **i80（8080）模式只有 8-bit 或 16-bit**，每個 PCLK 打一個 bus word：

- **WR = write strobe = WS2812 的 bit clock**，冇 WR 一個 bit 都出唔到；
- **DC** 由 `esp_lcd` I80 panel IO 要求（本專案 `dc_levels={0,0,0,0}`，永遠低，WS2812 不用它）；
- 所以 **「I80 只用一支腳」在硬件上做不到**，只能自動補 dummy（本設計），
  或者換 **RMT** 後端（§8）。

> 補充：有兩條腳位來源。有 `driver.json` 的 `LED` 段 → 走 `hardware.json` 的 `I80`
> （本設計的範圍）；冇 → 走 **Kconfig 的 `CONFIG_NETCORE_PIXEL_I80_PINS` 字串**
> （例如 SuperMini。那條路 **不改**，仍然要寫齊 8／16 個數）。

## 2. 設計目標／非目標

**目標**

- G1 `data` 兩種寫法都受理：稀疏（`-1`／省略尾部）同密集（8／16 個真腳）；
- G2 8-bit／16-bit **自動**，由「需要幾條 lane」決定，唔由「寫了幾個槽」決定；
- G3 `wr`／`dc` 可以唔寫（自動揀），有寫就用寫的；
- G4 **唔靜默**：自動揀了什麼腳、為什麼、寬度＝幾，一律 log（鐵律 9）；
- G5 **舊檔案零改動**：現時 repo 內全部 `data` 都冇 `-1`（2026-11-26 掃過：Matrix 8 支、
> Zero 8 支、其餘空／disable），所以舊寫法行為不變；
- G6 想指定 dummy 腳時有得指定（`dummy` 欄位），否則用內建池。

**非目標**

- 不做「I80 1-bit」——硬件冇這個模式；
- 不改 Kconfig 的 `CONFIG_NETCORE_PIXEL_I80_PINS` 字串語義；
- 不在此做 RMT／PARLIO 後端（各自獨立 Task）。

## 3. 語義規則（提案）

```jsonc
// ① 稀疏：16 個槽，只有 D0 真腳
"data": [14, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1]
// ② 只寫有效腳（尾部可以完全省）
"data": [14]
// ③ 全都唔寫（等於「用預設／Kconfig」，或直接 enable=false）
// ④ 想自己指定 dummy（可選）
"list": [ { "data": [14], "dummy": [38, 39, 40, 41, 42, 47, 48], "wr": 1, "dc": 2 } ]
```

| 概念 | 規則 |
|---|---|
| 有效腳 | `data` 中 **`>= 0` 的元素**（保持原有次序） |
| `-1` | **未用**（跳過；唔再當「錯值丟棄」） |
| 省略 | `data` 短過 8／16 → 尾部視為未用（唔再因為長度 ≠8／16 而丟棄） |
| `n_used` | 有效腳數量；`0` → **硬錯**（enable=true 但冇一支腳） |
| **`lanes`** | `min(n_used, driver.json` 的 `LED.lanes`)`；`n_used < LED.lanes` → **WARN**（唔夠腳） |
| **`bus_width`** | `lanes > 8 ? 16 : 8`（＝由需要幾條 lane 決定，**唔係**由槽數決定） |
| 補 dummy 數量 | `bus_width − n_used`（≥0） |
| `data` 長度上限 | ≤ 16（`NC_CFG_I80_DATA_MAX`）；>16 → 硬錯 |
| 每個真腳 | 仍要 0..48、唔重複、唔踩保留腳（沿用現有檢查） |

> **重要**：用家寫「16 個槽、只有 1 支有效」→ `bus_width = 8`（因為 `lanes=1`）。
> 想要 **16-bit** 一定要有 **9 條以上有效 lane**（`lanes > 8`）。這點要寫明，
> 否則會出現「我寫了 16 個槽但只有 8-bit」的疑惑。

## 4. 自動 dummy／`wr`／`dc`

### 4.1 池（pool）優先次序

1. `I80.list[].dummy[]`（新可選欄位，`hardware.json`；runtime 可改）；
2. `I80.list[].wr`／`dc` 有寫就用寫的，**冇寫／寫 -1** 時由池揀；
3. 內建 **per-chip 安全池**（固件常數）。S3 草案：

```text
S3 池（次序＝優先）：1, 2, 4, 5, 6, 7, 8, 9, 10, 13, 15, 16, 17, 18, 21, 22
排除：26–37（flash/PSRAM）、19/20（USB）、43/44（console）、0/3/45/46（strapping）、
      11/12（Matrix 板 IMU I2C，避免撞）、14（Matrix 的 LED，避免被當 dummy）
```

- 揀腳時要跳過：已用（`data` 有效腳）、重複、保留腳、已被其他 owner claim
  （`nc_hw_claim()` 會告訴我們；失敗就換下一支）。
- 排序**固定**（同一份輸入 → 同一組 dummy），方便維運對照。
- 16-bit 需要 16＋2 支腳；S3 池只有 16 支 → 要 16-bit 時池可能不夠，**唔夠就硬錯**
  （唔靜默變成 8-bit，因為 8-bit 會少 lane）。

### 4.2 log（G4）

```text
I80[0]：data 有效 1 支（GPIO14，lane 0）；LED.lanes=1 → bus 8-bit；
        dummy 4,5,6,7,8,9,10；wr=GPIO1（自動）、dc=GPIO2（自動）
```

## 5. 16-bit：條件與代價

| | 8-bit | 16-bit |
|---|---|---|
| 觸發 | `lanes ≤ 8` | `lanes ≥ 9` |
| 需要的腳 | 8 data + WR + DC = 10 | 16 data + WR + DC = 18 |
| 每筆 DMA | `units × 1` | `units × 2` |
| 同一 px 數的傳輸時間 | 一樣（PCLK 決定 tick 數） | 一樣 |
| 實機驗證 | ✅ 已上板（Matrix 64 px、50 fps） | ⚠️ 從未實機驗過 |

## 6. 相容性（2026-11-26 掃描）

| 檔案類型 | 現況 | 新 schema 影響 |
|---|---|---|
| Matrix 類（wiring＋4 個 project） | `data` 8 支真腳、`wr=1`、`dc=2` | **不變**（8 支真腳 → `n_used=8`、`lanes=min(8,1)=1`、`bus_width=8`） |
| Zero（wiring） | `data` 8 支、`wr=2`、`dc=4` | 不變 |
| C3／P4／N16R8／Zero-Show 類 | `data: []`、`wr=-1`、冇 `dc` | 現在：整條被丟棄（`defaulted`）。新 schema：仍然「冇有效腳 → 唔建立」，但**要 log 清楚**（唔好靜靜當 disable） |
| SuperMini／P4-WIFI6-DB／Mini-Nano | `I80.enable=0` | 不變（走 Kconfig 路徑） |
| 全部 repo 檔 | **冇任何 `data` 用 `-1`** | `-1` 由「錯值」變「未用」→ 零回歸風險 |

## 7. 錯誤 vs 警告語義（鐵律 9）

| 情況 | 級別 | 行為 |
|---|---|---|
| `enable=true` 但 0 支有效腳 | **錯誤** | 該條 I80 不建立 + `ESP_LOGE`（現時只 `defaulted++`，太靜） |
| `data` >16 個元素 | **錯誤** | 整條不建立 + log |
| 真腳重複／>48／保留腳 | **錯誤** | 整條不建立 + log（現有行為） |
| `lanes > n_used` | **警告** | 照用 `n_used` 條 lane，log「唔夠腳」 |
| dummy 池用完（補唔夠） | **錯誤** | 整條不建立（**唔會**偷偷降成 8-bit 少 lane） |
| dummy 腳被其他 owner 佔用 | **警告** | 換池內下一支；池全滿才錯 |

## 8. 跟 FastLED 的對照（用家提問；2026-11-26 對過原始碼）

| FastLED 後端 | 實際外設 | 腳位 | 我們 |
|---|---|---|---|
| **RMT**（S3 預設） | RMT TX | **任意 1 支 GPIO，冇 dummy** | ❌ 未實作（見 [pixel-output-backends.md](pixel-output-backends.md) §4） |
| **「I2S」driver（S3）** | **LCD_CAM（i80）** | 1–16 lane，**冇 dummy data 腳**（見 §8.1） | ⚠️ 同一個外設，但我們走 `esp_lcd`，要 8／16＋WR＋DC |
| LCD driver（新版，S3） | LCD_CAM i80，3-word/bit（6 B/bit） | 同上（1–16 lane） | ⚠️ 同上 |
| I2S1 parallel（**classic ESP32 才有**） | 真 I2S1 平行模式 | 24 lane（GPIO 0–39 窗） | ❌ 不適用（S3 冇這個模式） |
| PARLIO | PARLIO | P4／C5／C6／H2（**S3 冇**） | ❌ |

> **更正（2026-11-26）**：之前把「S3 限 GPIO 8–23」寫進這份稿係錯的 —— 那是
> **classic ESP32 的 I2S 平行腳位窗**。S3 的 LCD_CAM 走 GPIO matrix，腳位有彈性
> （我們 Matrix 用 14＋38–48＋1／2 就係證明）。

### 8.1 FastLED 為何可以「1 支腳、冇 dummy」——它不用 `esp_lcd`

FastLED 的 S3 實作在 `src/platforms/esp/32/drivers/i2s/channel_driver_i2s.h`，檔頭寫得很清楚：

```text
/// @brief I2S LCD_CAM implementation of ChannelEngine for ESP32-S3
/// uses ESP32-S3's LCD_CAM peripheral (via I2S mode) ...
/// - 1-16 WS2812/WS2812B LED strips (parallel output via data bus)
struct I2sChannelEngineConfig {
    int data_gpios[16];      ///< GPIO numbers for data lanes D0-D15
    int num_lanes = 0;       ///< Active lane count (1-16)
    u32 pclk_hz = 2400000;   ///< Pixel clock
};
```

- 它是 **自己寫 LCD_CAM 寄存器**（`i2s_lcd_cam_peripheral_esp`、transpose16x1），
  **只用 `data_gpios[0..num_lanes-1]` 真的 route GPIO**，未用的 data lane 唔 route，
  所以**冇 dummy 腳**；DC 也不需要（它不經 `esp_lcd` 的 panel IO）。
- 我們現在的 `drv_i80` 走 **`esp_lcd_new_panel_io_i80()`**：`data_gpio_nums[8/16]`、
  `wr_gpio_num`、`dc_gpio_num` **全部都要真腳**，所以才有 7 支 dummy＋WR＋DC。
- 兩個實作**用的是同一個硬件**（LCD_CAM i80，每個 PCLK 打一個 bus word），差別只在
  驅動層：**`esp_lcd` 幫你管好但要求齊腳；直接寄存器可以只 route 用到的 lane**。

### 8.2 所以「1 支腳」在 S3 有三條路（本設計只是第 3 條的一半）

| 路線 | 腳位 | 改動 | 風險／限制 |
|---|---|---|---|
| **(a) RMT 後端** | 任意 1 支｜S3 只有 **4 個 TX channel**（>4 條鏈要時分／fallback） | 新 `drv_rmt`＋adapter（[pixel-output-backends.md](pixel-output-backends.md) §4 已排） | IDF 官方 API、最成熟 |
| **(b) 自寫 LCD_CAM 驅動**（FastLED 路線） | 1–16 lane、**冇 dummy**、可重用現有 bus-word codec | 新 `drv_lcd_cam`（直接寫 `LCD_CAM` 寄存器）＋`nc_i80_out` 換底層 | 工作量最大、要自己處理时序／DMA／中斷；但保留 16 lane |
| **(c) `esp_lcd` I80 自動補 dummy**（本設計稿） | 仍要 8／16＋WR＋DC（自動揀） | 只改 config 層 | 最小、零回歸；**唔會**減少實際佔用腳位 |

- 結論：本設計是 **I80 的語義糖衣**（讓你寫 `[14]` 就搞定），**唔會**令佔用腳位變少。
  要真正「零 dummy」→ (a) 或 (b)。(a) 完成後 `data` 的稀疏語義可以原封不動沿用
  （有效腳 = lane 腳；`data:[14]` 就係 1 條 RMT 鏈）。
- 用家的直覺（「FastLED 好像直接一支腳、背後其實是 I80」）**兩半都對**：
  S3 上 FastLED 那個驅動確實是 LCD_CAM（＝我們的 I80）；它做到單腳是**繞過 esp_lcd**，
  不是硬件有 1-bit I80 模式。

### 8.3 換成 (b) 要改幾多？**畫波（codec）唔使改**（✅ 2026-11-26 已實作，見 status §7.254）

答案：**畫波／配對／環／任務全部唔使改**，因為 buffer 契約冇變——codec 出的是
「每個 tick 一個 bus word」的連續 byte stream，`drv_i80` 只是把這段交給硬件：

```text
pixel_encode_task → nc_px_codec_* → 一格 = units × (bus_width/8) bytes（已轉置）
        │
nc_i80_out.c  submit(slot)          ← 只改「腳位怎麼接」這一段
        │
drv_i80（要換的就是這一層）            ← esp_lcd → 直接寫 LCD_CAM 寄存器
        │
DMA → D0.. 資料線（只 route 有用途的 lane）→ WS2812
```

要動的三處：

| # | 位置 | 改什麼 |
|---|---|---|
| 1 | `firmware/components/driver/drv_i80/nc_i80_drv.c`（或新 `drv_lcd_cam/`） | 換底層：`esp_lcd_new_i80_bus`／`esp_lcd_new_panel_io_i80`／`esp_lcd_panel_io_tx_color` → 自己設 LCD_CAM 時鐘分頻、`lcd_ctrl`、GPIO matrix（**只 route 有效 lane**）、DMA 描述符、完成中斷。**介面 `install/submit/done/pclk` 保持不變**，上層唔知換了 |
| 2 | `nc_i80_out.c` | 腳位接駁：`pins[width+2]`（data＋DC＋WR 全部要真腳）→ `lane_pins[lanes]`（只列真正接燈的腳）；claim 也只 claim 這幾支。`bus_width` 8／16 照舊（≤8 lane → 8-bit；9–16 → 16-bit） |
| 3 | config 層（本設計稿 §3–§4） | 稀疏 `data`、`-1`＝未用、`lanes`、自動寬度；`wr`／`dc` 消失（新驅動不需要） |

**唔使改**：`nc_px_codec_*`（12-bit→8-bit→波形→轉置）、`nc_px_layout_*`、
環（`nc_buf` slot_bytes 照舊）、`pixel_*_task`、`render` 節拍、`0xF06B`／`0xF071` 統計。

**但唔係零風險**，要自己補返 esp_lcd 本來幫我們做的四件事：

1. **PSRAM 槽的 cache writeback**（現時靠 `esp_lcd` 的 `tx_color`；新 driver 要自己
   `esp_cache_msync()`。internal 槽不受影響）
2. **完成中斷**：esp_lcd callback → 自己開 LCD_CAM interrupt（`done_order` 那段邏輯可保留）
3. **PCLK 分頻**：esp_lcd 由 `pclk_hz` 算；新 driver 要自己算（FastLED 有對應
   `solveI2sClockDivider()` 可參考），Matrix 要 2.5 MHz
4. **`lcd_ctrl` 的 8／16-bit 設定與 WR**：i80 模式的 WR（PCLK）可以**唔 route**
   （FastLED 就是咁），但要在板上證實資料線照樣按 PCLK 更新

驗證方式：host test 只能覆蓋 config 層；**硬件路徑一定要上板**（`0xF06B` 逐 px hex
＋肉眼／示波器睇 T0H／T1H）。建議分兩階段落：**① 1 lane、8-bit、2.5 MHz（Matrix 64 px）
先通** → **② 才做 8／16 lane 與多條鏈**。

### 8.4 設計紅線：I80／LCD_CAM 唔可以「異化」成只for燈（2026-11-26 用家指示）

> **2026-11-26 更新（階段①b 已完成）**：我們自己的驅動已經**完整 LCD 化**——
> `nc_i80_drv_submit_cmd(slot, lcd_cmd, bytes)`（cmd phase＋param phase）、
> `cmd_bits`、DC 四段電平、CS assert／de-assert；`wr`／`dc`／`cs` 全部**可選**
> （≥0 才 route，唔填 -1 唔影響 LED 模式），唔會像 esp_lcd 那樣缺一就整條唔建立。
> 面板**未有硬件驗證**（未上板），文件明講。見 status §7.255。

用家明確要求：**這個驅動要具備真熒幕（LCD 面板）顯示能力，不要把它異化；兩邊的驅動要
互相參考、可合成。** 所以：

| 規則 | 意思 |
|---|---|
| **直驅唔取代 esp_lcd** | LCD 面板（要 DC／CS／cmd phase／多 device）繼續用 `esp_lcd` i80 panel IO；直驅只係多一條「只 data lane」模式。Kconfig 二選一，但兩者都實作同一個 `nc_i80_drv_*` 契約 |
| **cfg 保持通用** | `nc_i80_drv_cfg_t` 有可選 `wr_pin`／`dc_pin`／`cs_pin`（≥0 才 route）。LED 模式全部 -1（唔需要真腳），面板模式照 i80 規範 route |
| **共用輸出層** | `render_task` 係公用輸出層（AGENTS §10.1）：將來 LCD 內容同 pixel 環走同一個 encode／submit／reclaim 節奏；直驅／esp_lcd 都只係「硬件尾巴」，上面唔知 |
| **唔可以偷偷削功能** | 直驅現階段**未做** cmd phase／多 device／CS 仲裁；呢啲寫明「面板請用 esp_lcd 路徑」，唔可以靜默降級（鐵律 9） |

換句話講：直驅係 I80 的**第二個實作**（為咗只 route 用到的 lane），唔係「LED 專用驅動」；
將來面板要走直驅時，補 cmd phase／CS 就係同一個檔案加分支，唔使另起一套。

## 9. 待拍板（實作前要答）

1. **`lanes` 的來源**：`min(n_used, LED.lanes)`（提案）還是「`data` 有效腳數就是 lanes，
   完全忽略 `LED.lanes`」？（前者對 SuperMini 這類「寫齊 8 支但只有 6 lane」最安全）
2. **`wr`／`dc` 可否完全省略**：提案「可以，自動揀＋log」。還是要「必須寫，缺就硬錯」？
3. **內建 S3 池的內容**：上面 §4.1 那 16 支 OK 嗎？要不要把 11／12（IMU）也算進去？
4. **GPIO0／45／46（strapping）**：要不要像 GPIO3 一樣開放成「只准當 lane／dummy 用」
   的例外（FastLED 可以，我們現時擋）？
5. **16-bit 觸發**：`lanes > 8`（提案）還是「`data` 寫滿 16 個槽就 16-bit」？
   （後者會令「寫 16 個 -1」意外變成 16-bit，我唔建議）
6. **要唔要順便排 §8.2 路線 (b)（自己寫 LCD_CAM 驅動，1–16 lane 零 dummy）**？
   它是唯一同時做到「多鏈」＋「冇 dummy」的路，但要直接操作 LCD_CAM 寄存器
   （FastLED 用 `i2s_lcd_cam_peripheral_esp` 那套）。若只做 (a) RMT，S3 只 4 個 TX
   channel 就係上限。

## 10. 實作影響面（未開工，供估工作量）

| 檔案 | 改什麼 |
|---|---|
| `firmware/components/sys/config/nc_cfg_load.c` | `apply_i80()`：稀疏解析、`n_used`、自動寬度、叫池補 dummy／wr／dc |
| `firmware/components/sys/config/include/nc_cfg_types.h` | `nc_i80_cfg_t` 加 `n_used`（lanes） |
| `firmware/components/sys/hw/nc_hw.c`（或新 `nc_hw_pool.c`） | per-chip dummy 池＋「揀下一支可用腳」helper |
| `firmware/components/tasks/render_task/nc_render_task.c` | `i80_src()` 用 `n_used`／自動寬度（`led->lanes` 仍然保留 clamp） |
| `tools/ncportgen.py` ＋ `ports/hardware/*/wiring.json` | 預置可以寫稀疏；`sync_hardware_presets.py` 重生成 |
| `tools/hosttest/cfg_test`（或 `pxScan_test`） | 新 case：`[14]`／`[14,-1…]`／`[]`／`wr` 省略／16-bit 邊界 |
| `docs/02_guides/configuration.md`、`status.md` | schema 文件＋實測數字 |

> 路線 §8.2 (b)（自寫 LCD_CAM）另計：新 `firmware/components/driver/drv_lcd_cam/`＋
> `nc_i80_out.c` 換底層（`esp_lcd` → 直接寄存器），並要自己處理 DMA／中斷／reset。
> 它與本表**不衝突**：本表的 config 語義（稀疏 `data`、自動寬度）在 (b) 之後仍然適用。

**上板驗證計劃**（Matrix，release 映像）：live `hardware.json` 改 `"data":[14]`（其餘省）
→ reboot → 睇 log「I80[0]：有效 1 支…8-bit」→ `status tasks` 四任務 running →
`0xF06B` 有 frame、`0xF071` 50 fps／drops=0 → 再試 `wr`/`dc` 都省。

### 8.5 「用舊驅動邏輯驗證」（冇面板的做法）＋對照結果（2026-11-26）

用家問：冇 LCD 面板，可唔可以直接看舊驅動（IDF `esp_lcd` i80）的邏輯來驗證？
可以，而且照做之後**真的找出兩個差異並修正**。逐項對照（來源：
`$IDF/components/esp_lcd/i80/esp_lcd_panel_io_i80.c`）：

| 環節 | IDF `esp_lcd` | 我們直驅 | 判定 |
|---|---|---|---|
| bus clock／reset | `PERIPH_RCC_ACQUIRE_ATOMIC` + `lcd_ll_enable_bus_clock/reset_register` | 相同 | ✅ |
| clk source／分頻 | `esp_clk_tree_enable_src` + `esp_clk_tree_src_get_freq_hz` + 群除頻 `LCD_PERIPH_CLOCK_PRE_SCALE(2)` | **今次已改成照 IDF 由 clk tree 讀頻率**（原本硬編 80 MHz） | ✅ 修正 1 |
| PCLK prescale | `resolution / pclk`，1..`LCD_LL_PCLK_DIV_MAX`(64) | 相同（80 MHz / 32 = 2.5 MHz） | ✅ |
| idle／edge | `set_clock_idle_level(!idle_low)`、`set_pixel_clock_edge(active_neg)` | 相同（false／false） | ✅ |
| 8/16-bit | `set_dma_read_stride(bus_width)` | 相同 | ✅ |
| RGB off／color convert off／always_out | 三個都有 | 三個都有 | ✅ |
| byte swizzle | `set_swizzle_mode(AB2BA)`，逐 device `enable_swizzle(param_bits>bus_width)` | **今次補**：明確 `enable_swizzle(false)`（原始 byte 流唔要 swap） | ✅ 修正 2 |
| phase cycles | `(cmd_cycles, 0, data?1:0)`；`cmd_cycles=ceil(cmd_bits/bus_width)` | 相同算法；另加 phase 形狀快取（只省重寫次數，值不變） | ✅ |
| blank cycles | `(1,1)` | 相同 | ✅ |
| reset + FIFO reset | 每筆做 | 每筆做 | ✅ |
| 啟動次序 | `gdma_start` → `esp_rom_delay_us(4)` → `lcd_ll_start` | 相同 | ✅ |
| DC 電平 | 由 io_config，切 device 時重設 | install 時一次設定（未有切 device） | ⚠ 單面板等價 |
| CS | 多 device 接 peripheral signal；單 device 無 CS | 軟件 GPIO：start assert、完成 ISR de-assert | ⚠ 行為等價，實作不同（要示波器驗） |
| 完成中斷 | `esp_intr_alloc_intrstatus` + level + clear | 同一 pattern（另用 `esp_intr_enable` gating） | ✅ |
| 多筆 async | `trans_queue`＋`done_queue`（真 pipelining） | 自己 ring queue＋單筆 in-flight | ⚠ 簡化（LED 幀距 20 ms 夠用；面板高刷新率會少咗管線） |
| PSRAM cache | `tx_color` 做 C2M msync | `submit` 自己做 | ✅ |
| 多 device 切換 | 有 | 未有（明講，唔靜默） | ❌ |

**結論**：LED 路徑（= 已上板驗證那條）與 IDF 的序列**逐項等價**；面板專屬部分
（cmd phase 公式、DC 電平、CS 語義）**靜態對照一致**，但實際時序仍要真面板＋示波器才算「驗證過」。
今次對照直接促成兩個修正（clk tree 讀頻率、明確關 swizzle），上板回歸：49.9 fps、drops=0、
rainbow 64 色（fw 3e55192-dirty、app 6567ee93）。

**仍然未做**：多 device 切換、真 pipelining（面板高刷新率才需要）、示波器量 cmd/DC/CS。
