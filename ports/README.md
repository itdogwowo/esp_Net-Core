# ports/ — 板子層（設定，不是程式）

`firmware/components/` 分「這是什麼程式」；`ports/` 分「**這塊板子要什麼**」。

原則：**晶片 = 一個 build，硬體板 = 一份描述，project = 一組選擇。**
命名只用一種寫法：**代號 `<CHIP>-<Identity>`（目錄名／`name`／project 名）
＋ 身份 `brand`／`model`／`rev`（人類可讀）**。改名要保留 `aliases`；詳見 §10。

---

## 1. 兩層：硬體板 vs project

```text
ports/
├─ hardware/<chip>/<board>/     這塊板「有什麼」
│  ├─ board.json                 F.spec   晶片／flash／PSRAM／CPU／console／能力
│  ├─ wiring.json                F.wiring 腳位、匯流排、板載晶片（**firmware 預設的唯一來源**）
│  ├─ driver.json                F.device 掛在匯流排上的裝置（build-time；runtime 逐段接）
│  ├─ sdkconfig.defaults         F.build  硬體事實（flash/PSRAM/CPU/console/ETH）
│  ├─ partitions.csv             這塊 flash 的分割表（可選；沒有就用 firmware/partitions.csv）
│  └─ README.md
│
├─ project/<chip>/<board>/      這塊板「跑什麼」——runtime 設定＋（選用）project 層 build 覆寫
│  ├─ project.json               F.project  name／hardware 指回哪塊板／seed 清單
│  └─ data/config/               F.preset  出廠執行期預置（system/network/tasks）
│
├─ _profiles/                   能力碎片（可組合、可重用）
├─ _chips/                      別名 profile（ST7789／GT1151Q…）
└─ hardware/_chips/             晶片能力（cores／USB；ncportgen 讀它）
```

**判準：**

| 問題 | 放哪 |
|---|---|
| 換這塊板要重新 build 嗎？ | 要 → `hardware/`（board.json／wiring／sdkconfig／partitions） |
| 是「這塊板有哪些腳／晶片」嗎？ | 是 → `hardware/wiring.json`／`driver.json` |
| 是「這個項目要開哪些通道／任務／IP」嗎？ | 是 → `project/`（project.json／data/config） |
| 同一塊板要跑兩個不同項目？ | 開兩個 `project/<chip>/<name>/`，`hardware` 都指向同一塊 |

> 同一塊硬體可以有多個 project；`project.json` 的 `"hardware"` 是必要欄位。
> 沒有 project 也可以 build（只有硬體預設、沒有 runtime seed），但正式出貨應該有。

---

## 2. 三層：哪些能「換板不改程式」，哪些不能

| 層 | 例子 | 換板要不要重新 build | 放哪 |
|---|---|---|---|
| **L1 硬體事實** | flash 大小 / 頻率 / 模式、PSRAM、CPU、有冇 Ethernet、console | ✅ 要 | `hardware/board.json` + `hardware/sdkconfig.defaults` |
| **L2 能力碎片** | ETH／WiFi／NC4 over WS／UDP | ✅ 要 | `hardware/board.json` 的 `profiles` → `_profiles/*.defaults`（**project 唔參與 build**） |
| **L3 執行期** | gpio、服務開關、像素數、網路位址、任務 core | ❌ 同一 binary | `project/data/config/*.json` → 裝置 `/data/config/` |

> **PSRAM、flash 大小、flash 頻率永遠不可能用 config.json 換**——它們在 bootloader 與
> 記憶體初始化階段就決定了。寫進 runtime config 只會被 `tools/board.py check` 擋下來。

> **WiFi 預設唔開，開關在 project 層**：`Kconfig NETCORE_WIFI` 預設 `n`。
> 要開就在 **`project/<chip>/<board>/sdkconfig.defaults`** 加 `CONFIG_NETCORE_WIFI=y`
> （原生 radio；P4 改用 `wifi-hosted-c5`／`wifi-hosted-c6` 碎片）。
> 冇寫 = esp_wifi 完全唔 link：省 flash，亦省 **~28 KB 靜態 BSS** ＋ **~100 KB
> 執行期 internal**（libphy／libbb／net80211 的緩衝 —— 即使 `network.json` 的
> `wifi.enable=0` 唔會呼叫 `esp_wifi_init`，靜態部分仍然佔住 SRAM）。
> `hardware/board.json` 的 `features.wifi` 只描述「這塊晶片有沒有 radio」，不是開關。
> 完整操作（兩道閘、驗證、踩過的坑）見 [configuration.md](../docs/02_guides/configuration.md) §2。

---

## 3. 日常操作

```bash
python3 tools/board.py list              # 硬體板 × default project × 能力
python3 tools/board.py projects          # 每塊硬體板掛了哪些 project（default 會標）
python3 tools/board.py check             # 驗 board.json ↔ sdkconfig、project 結構、seed
python3 tools/board.py sync              # 改 _profiles/ 或 profiles 之後重跑
python3 make.py build ESP32-P4-ETH       # 硬體 + default project（冇 default = 純硬體）
python3 make.py build ESP32-P4-ETH --project Demo   # 指定 project（短名可）
python3 make.py build ESP32-P4-ETH --no-project     # 明確唔要 project
python3 make.py build P4/ESP32-P4-ETH    # 用 <chip>/<board>
python3 make.py build <project 名>       # 用 project 名（hardware 由 project.json 決定）
python3 make.py boards                   # 同 board.py list
```

### 3.1 建置時的 project 選擇（2026-11-16）

**正常情況只揀硬體**；project 係可選的「用戶 config」——只換 `data/config` 預置，
唔會改 build 能力（Kconfig／profiles 全部屬 hardware）。

| 情況 | 行為 |
|---|---|
| `build <硬體板>`、有 default project | 硬體 ＋ default project |
| `build <硬體板>`、冇 default project | **純硬體**（冇 project seed；用 firmware 內建最小骨架） |
| `build <硬體板> --project <名>` | 指定 project；`--project demo` 會命中 `<硬體板>-Demo` |
| `build <硬體板> --no-project` | 就算有 default 都用純硬體 |

- **default 判定**：`project.json` 的 `"default": true`；否則**同名 project**
  （`ports/project/<chip>/<board>/`）就係慣例上的 default。每塊板最多一個。
- 建 project：`python3 tools/board.py new --project Demo --hardware ESP32-P4-ETH`
  → 自動命名 `ESP32-P4-ETH-Demo`（同一 chip 下唔會撞名）；加 `--default` 就設為該板 default。
- **非 default** 的 build 用獨立 build dir 與 artifacts 名：
  `<硬體板>__<project 目錄>`（例如 `ESP32-P4-ETH__ESP32-P4-ETH-Demo.bin`），
  唔會蓋走 default 的 image。`--no-project` 而塊板有 default 時叫 `<硬體板>__bare`。

新增一塊硬體板（會同時建 project 骨架）：

```bash
python3 tools/board.py new \
  --chip S3 --board ESP32-S3-Matrix \
  --flash 16 --flash-freq 80 --psram 8 --psram-mode octal --psram-speed 80 \
  --cpu 240 --console usb-serial-jtag --eth none --wifi
```

只替既有硬體加一個 project：

```bash
python3 tools/board.py new --project demo --hardware P4/ESP32-P4-ETH
```

**不需要改任何 shell 腳本或 CMake。**

---

## 4. `hardware/board.json`（F.spec）

機器可讀的板子身份證；`check` 用它對照真正的 Kconfig：

```jsonc
{
  "name": "ESP32-P4-ETH",
  "chip": "esp32p4",
  "flash": { "size_mb": 32, "freq_mhz": 40, "mode": "dio" },
  "psram": { "size_mb": 32, "mode": "hex", "speed_mhz": 200 },
  "cpu_mhz": 360,
  "console": "uart0",
  "profiles": ["eth-rmii"],          // 所有 build 能力都在這裡（eth/wifi/ws/udp）
  "features": { "eth": "rmii-ip101", "wifi": false, "usb": false },
  "verified": "2026-11-02",
  "notes": "rev1.3 樣品；flash 只能 40 MHz、CPU 上限 360 MHz"
}
```

`check` 會抓的錯（都是實際會讓你燒壞一小時的錯）：

- `board.json` 說 16 MB，`sdkconfig.defaults` 設 8 MB
- `board.json` 說有 PSRAM，`sdkconfig` 沒開 `CONFIG_SPIRAM`
- `board.json` 說沒有 Ethernet，`sdkconfig` 卻開了 `CONFIG_ETH_ENABLED`
- 宣告了能力碎片但沒 `sync`
- 把 `flash_size` / `psram` 這種 build-time 的東西寫進 project 的 `data/config/*.json`
- 共通 `firmware/sdkconfig.defaults` 偷渡板子事實

還有一個會靜默咬人的：**`firmware/sdkconfig` 存在的時候，改 `sdkconfig.defaults` 不生效**。
`check` 會比對 mtime 提醒你，`make.py build` 則會自己重設。

---

## 5. `hardware/wiring.json`（F.wiring）：firmware 預設的唯一來源

以前 firmware 把 `wiring.json` 原文嵌進 image，開機再 cJSON 解析；同一份資料有
兩份（JSON 文字 + struct），而且註解與重複的字典鍵全部進 flash。

現在 build 時由 `tools/ncportgen.py` 做一次「字典 → 數值」：

```text
wiring.json ──ncportgen──► nc_port.c（static const nc_hardware_cfg_t）
                        └► preset_hardware.json（/data/config/hardware.json 的 seed）
```

| JSON | struct | 省什麼 |
|---|---|---|
| `"mode":"OUT"` / `"IN"` | `0` / `1` | 每次比較不再 `strcmp` |
| `"pull":"UP"` / `"DOWN"` | `1` / `2` | 同上 |
| `"phy_type":"IP101"` | `NC_CFG_PHY_IP101` | `char[12]` → 1 byte |
| `"iface":"eth"` / `"service":"nc4"` / `"mem":"psram"` | enum | 每個 link 省 22 B |
| `_comment` 整段 | 不進 firmware | 這塊板省 ~1.5 KB flash |

生成物是 **C 的 designated initializer**，不是 JSON：沒有解析、沒有 cJSON、
沒有重複的鍵名；alignment 由編譯器按真實型別處理。
`nc_cfg_defaults_hardware()` 直接回傳這份 `static const`。

`preset_hardware.json` 仍然存在——它是給人看的（USB 插上去就看得到可以改的欄位），
也是 `hardware.json` 的第一次 seed。用戶改 `/data/config/hardware.json` 只覆蓋差異；
firmware 不會因為用戶檔缺失而失去硬體預設。

> `wiring.json` 裡的 `I2C`／`I2S`／`ENC`／`XL9555` 目前**只保留在 seed**，
> 還沒有對應的 typed struct；`ncportgen` 會把它們原樣放進 `preset_hardware.json`，
> 等 driver 有型別再逐段接。已 typed 的是 `ETH`／`SDIO`／`PIN`／`PWM`／`UART`／`I80`／`SPI`。

### 5.1 `hardware/driver.json`（F.device）：掛在匯流排上的裝置

**一句話規矩：腳位／匯流排本身只寫在 hardware 層（`wiring.json` → 生成
`nc_hardware_cfg_t`，runtime 可由 `/data/config/hardware.json` 覆蓋）；`driver.json` 只寫
「邊個裝置、掛邊條 bus、裝置參數」。** 呢個就係「先建硬件，再建用硬件的東西」。

```text
wiring.json（硬件：I80 的 D0..D7／WR／DC 喺邊支腳）
      │  ncportgen（build 時）→ nc_hardware_cfg_t.i80[]（nc_port.c 的 static const）
      │ ／ 開機時 /data/config/hardware.json 同段覆蓋（唔使重 build）
      ▼
driver.json（裝置：backend、bus index、幾條 lane、每條幾顆、格式、時序）
      │  ncportgen → nc_led_cfg_t
      ▼
render 任務：nc_cfg_hardware()->i80[led.bus] 取腳位 ＋ nc_led_cfg_t 取參數
```

`driver.json` 引用匯流排一律用 `bus = { "type", "index" }`：`type` 選種類
（`spi` / `i2c` / `uart` / `i2s` / `sdio` / `i80`），`index` 選該類型 `list` 的第幾個
（`-1` = 未接）。裝置本身的型號／能力用 `profile` 指去 `ports/_chips/<name>.json`。

#### 燈鏈（LED 段）

```jsonc
// ports/hardware/S3/ESP32-S3-Matrix/driver.json —— 只有裝置參數
"LED": {
  "enable": 1,
  "backend": "i80",                            // i80 / spi / direct / mock（rmt／parlio 未實作）
  "bus": { "type": "i80", "index": 0 },        // → hardware 層 I80[] 的第 0 條
  "lanes": 1,                                  // 前幾條資料線真的接燈
  "px": 64,                                    // **每條 lane** 幾顆（不是總數）
  "format": "ws2812",                          // ws2812／ws2812w／ws2815／ws2801／apa102
  "timing": { "pclk_hz": 2500000, "bit_cells": 3, "t0h": 1, "t1h": 2 }
}
```

對應的硬件（腳位）在 `wiring.json`（**舊專案原形狀**，`bus_width` 由 `data` 長度推出）：

```jsonc
"I80": {
  "enable": 1,
  "list": [
    { "data": [14, 38, 39, 40, 41, 42, 47, 48],   // D0..D7（8 支 = 8-bit；16 支 = 16-bit）
      "wr": 1,                                    // I80 硬性要求；送燈時是 dummy
      "dc": 2,                                    // esp_lcd 必要的 D/C（LED 也是 dummy）
      "cs": -1,                                   // LCD 面板的 CS；燈鏈 = -1
      "freq": 80000000 }                          // LCD 的 PCLK；LED 用 LED.timing.pclk_hz
  ]
}
```

| 欄位 | 意思 | 唔寫會點 |
|---|---|---|
| `backend` | 用邊個 driver | `enable=1` 就一定要寫 |
| `bus` | 引用 hardware 層的哪條匯流排 | `i80`／`spi` 一定要 |
| `lanes` | 幾條資料線真的接燈（1..data 支數；SPI 只可以 1） | 預設 1 |
| `px` | **每條** lane 幾顆 | `enable=1` 就一定要 |
| `format` | 線材格式（決定 GRB／GRBW／RGB 同每顆幾個 byte） | `enable=1` 就一定要 |
| `timing` | 波形時序；`pclk_hz`／`bit_cells`／`t0h`／`t1h` | 預設用該格式的內建波形 |

三種狀態唔可以混：

| `present` | `enable` | 行為 |
|---|---|---|
| 冇 LED 段 | — | render 用 `CONFIG_NETCORE_PIXEL_*`（未遷移的板子行為不變） |
| 有 | `0` | 明確唔用燈 → 只留 mock lane，**唔會**退回 Kconfig；腳位可以未定（`index:-1`） |
| 有 | `1` | device 參數由 JSON 決定；**腳位由 `nc_cfg_hardware()` 取** |

> **腳位改錯有兩條路**：改 `wiring.json` 再 build（改預設），或者插 USB 改
> `/data/config/hardware.json` 的 `I80` 段（runtime 覆蓋，唔使重 build）。
> `I80` 由 2026-11-24 起真的會被 `nc_cfg_apply_hardware()` 讀（以前只係死段）。
>
> **project 覆寫就讓位**：`ports/project/<...>/sdkconfig.defaults` 一旦寫了任何
> `CONFIG_NETCORE_PIXEL_*`（外接燈具屬 project），board 的 LED 段會自動讓位、render 用
> Kconfig。規則由 `tools/ncportgen.py` 在 build 時判定（`--project` 參數），
> `tools/board.py check` 會 warn 講明邊個贏。理由：外接燈鏈的腳位是**那組燈具**的事，
> 唔應該寫死在一塊通用開發板上。

#### 常見錯誤（`board.py check` 會擋）

| 症狀 | 真正原因 |
|---|---|
| `drivers.json`（多個 s）| 工具鏈只認 `driver.json`，成個檔冇人讀 |
| `driver.json` 出現 `I80`／`SPI`／`I2C`／`UART`／`PIN` 段 | 那是 hardware 層，搬去 `wiring.json` |
| `driver.json` 出現 `GPIO` | 腳位只可以出現一次：hardware 層 |
| `I80` 寫 `bus_width` | 由 `data` 長度決定，唔可以另寫一個（會漂移） |
| `LED.order` 同 `format` 唔一致 | 線材次序由格式決定，`order` 只會被對帳 |
| `wiring.json` 與 `sdkconfig` 的 `NETCORE_PIXEL_I80_PINS` 唔一致 | 兩個真相；改到一樣為止 |



---

## 6. 能力碎片的規矩

- 碎片只放「**同一個原因而存在的多行設定**」（ETH 一定要配 DMA + lwIP 視窗）。
- 碎片裡不要出現任何 L1 事實（型號、腳位、頻率）。
- 改碎片 → `board.py sync` → 所有宣告它的板子一起更新；牆上的數字只有一份。
- `sdkconfig.defaults` 檔尾那段是**產生出來的**，手改會在下次 `sync` 被蓋掉；
  這塊板子獨有的東西請寫在 marker 區塊上方。
- **所有 build 能力**（`eth-rmii`、`wifi-hosted-c6`、`nc4-ws`、`nc4-udp`…）都寫在
  `hardware/board.json` 的 `profiles`；project 層冇 `profiles`。
- build 時的合併次序：`firmware/sdkconfig.defaults` → `hardware/sdkconfig.defaults`
  → `project/sdkconfig.defaults`（後者勝）。
- **project 主要是 runtime 設定**（`project.json` ＋ `data/config/*.json`）；但同一塊硬體
  可以外接不同的東西（LED 燈鏈、外掛板），那組腳位是 build-time 事實 —— 所以 project
  **可以選擇性**帶一份 `sdkconfig.defaults`，只覆寫外接裝置（例如 `NETCORE_PIXEL_*`）。
  硬體事實（flash／PSRAM／CPU／console／分區表／target）一律唔准寫入 project，
  `board.py check` 會報錯。呢份檔改完要 `--fresh` 才會重新套用。
- project 的 `data/config` 會作為 `F.preset` 嵌進 image，所以換 project（換預置內容）
  仍然要重 build 一次。

---

## 7. 出廠預置的兩個坑

**① 不走 `fatfs_create_partition_image`。** MSC 用 wear levelling，WL 會把分區開頭拿去放自己的
metadata，預先燒在 offset 0 的 FAT 映像會對不上 → 碟顯示「沒有檔案系統」。
現在的做法：firmware 內嵌 `config.json` → 第一次開機自動格式化 → 寫入預置 → 交給 host。

**② `idf.py flash` 會蓋掉使用者改過的內容。** 所以：

- 開發 / 日常 → `app-flash`（只換 app，不動碟）
- 出廠 / 要重置 → `flash`（連預置一起蓋回去）

預置不是保證：使用者可以刪掉 `config.json`。**缺檔必須能開機**，不能因此進不了 RUN。

---

## 8. 跟 MicroPython 版（mp_Net-Core）的對照

MPY 的 `ports/<chip>/<board>/` 是**部署 delta**：`slave/` 是基礎，port 用「同路徑覆蓋」
放 `main.py`、`action/*.py`、`config.json`、`schedule.json`——因為 MicroPython 的程式
本身就是檔案系統上的檔案。

C 版**刻意不照抄**：

| MPY | C 版 | 為什麼 |
|---|---|---|
| `slave/` | `firmware/` | 一樣：乾淨基礎 |
| port 可放 `*.py` 覆蓋程式 | ❌ 只能放設定 | 程式要編譯；per-board code = per-board build |
| port 的 `config.json` 覆蓋 `slave/config.json` | `project/data/config/*.json` 是出廠預置，runtime 屬使用者 | 同一份 FAT，不疊層 |
| 15 塊板子並存 | 目前 7 塊，`board.py new` 隨時加 | 沒 driver 支撐的板子不要先開空殼 |

MPY 那邊 15 個 port 有一半靠的是「同一個 Python 基礎 + 不同 config」；C 版把這件事
做得更硬：不只要 config，還要 `board.json` 把**不可 runtime 的部分**釘死在 build 期。

---

## 9. 什麼不該放這裡

- 程式 → `firmware/`
- 給人看的說明 → `docs/`
- 主機端工具 → `tools/`

分層理由見 [docs/03_notes/component-layout.md](../docs/03_notes/component-layout.md) §4；
port 兩層的完整設計與 OTA／分割表問答見
[docs/03_notes/ports-layout.md](../docs/03_notes/ports-layout.md)。

---

## 10. 命名規範（2026-11-16 定案）

板名會同時出現在目錄、`make.py build <名>`、`artifacts/`、開機 log 與文件。
以前同一塊 DFRobot 板目錄叫 `ESP32-P4-C6`、文件叫「Firebird」、官方叫
「FireBeetle 2 ESP32-P4」——三種寫法，搜尋與對帳都會斷。
所以名稱分兩層：**代號（穩定、機器用）** 與 **身份（人類用）**。

### 10.1 代號：`<CHIP>-<Identity>`

- `<CHIP>`：`ESP32` / `ESP32-S2` / `ESP32-S3` / `ESP32-C3` / `ESP32-C5` /
  `ESP32-C6` / `ESP32-C61` / `ESP32-H2` / `ESP32-P4`。**晶片永遠在最前**；
  目錄層已經按晶片分，但名字仍然帶晶片，`make.py build <名>` 一眼就知燒哪顆。
- `<Identity>` 按以下優先次序取名：

  | 順位 | 情況 | 例 |
  |---|---|---|
  | 1 | **官方／品牌產品名**（去掉重複的晶片詞） | Waveshare `ESP32-S3-Matrix`；DFRobot `FireBeetle 2 ESP32-P4` → `ESP32-P4-FireBeetle2`；Espressif `ESP32-C3-DevKitM` |
  | 2 | **通用／白牌板的社群叫法**，或整塊板就叫模組代號 | `ESP32-S3-SuperMini`、`ESP32-S3-Mini-Nano`、`ESP32-S3-N16R8` |
  | 3 | **自家工程板**：用用途 | `ESP32-P4-ETH` |

- 只用 ASCII 英數與 `-`；每個 `-` 分隔的詞用大寫字母開頭
  （`ESP32-S3-Mini-Nano`、`FireBeetle2`）。中文、空格、底線、小寫開頭一律不接受。
- **品牌不放最前**（`FireBeetle2` 這類已經成為產品名一部分的除外）：
  品牌寫 `board.json` 的 `brand`，不靠目錄名表達。
- **版本／批號不進名字**：`v1.0`、`rev1.3`、樣品 → `board.json` 的 `rev`／`notes`。
- **project 名 = 目錄名**；同一塊硬體多個 project → `<硬體板>-<用途>`
  （例：`ESP32-P4-ETH-Soak`）。

### 10.2 身份：人類可讀（不參與解析）

`board.json` 可選欄位：

| 欄位 | 作用 | 例 |
|---|---|---|
| `brand` | 品牌；白牌寫 `null` | `"DFRobot"` |
| `model` | 原廠產品名（可以含空格） | `"FireBeetle 2 ESP32-P4"` |
| `rev` | 板本／批次 | `"v1.0"` |

`board.py list` 顯示代號；要知品牌／型號就看 `board.json` 或該板 `README.md`。

### 10.3 改名規則：`aliases`

改名 = 目錄名、`name`、project 名一齊改；**舊名一定要寫進 `aliases`**
（`board.json` 與 `project.json` 都可以放）：

```jsonc
"name": "ESP32-P4-FireBeetle2",
"aliases": ["ESP32-P4-C6", "P4-C6", "ESP32-P4-FireBeetle"]
```

- `make.py build ESP32-P4-C6`、`board.py check P4-C6` 仍然解析得到。
- `aliases` 只影響查詢，不進 build 路徑、不改 `-DPORT`／`-DPROJECT`、不影響
  `artifacts/<board>.bin` 的名字（那用 `name`）。
- 改名要同步更新 `docs/` 的**現行**文件；帶日期的歷史紀錄
  （`docs/03_notes/*-2026-*.md`）保留原名，由
  [ports-layout.md](../docs/03_notes/ports-layout.md) 的對照表補。

### 10.4 現有板子

| 代號 | 品牌 | 產品名 | 舊名／別名 |
|---|---|---|---|
| `ESP32-C3-DevKitM` | Espressif | ESP32-C3-DevKitM | — |
| `ESP32-P4-ETH` | （自家） | rev1.3 ETH 樣品 | — |
| `ESP32-P4-FireBeetle2` | DFRobot | FireBeetle 2 ESP32-P4 | `ESP32-P4-C6`、`P4-C6` |
| `ESP32-S3-Matrix` | Waveshare | ESP32-S3-Matrix | — |
| `ESP32-S3-Mini-Nano` | （白牌） | ESP32-S3 Mini Nano（N16R8） | `S3-Mini-Nano` |
| `ESP32-S3-N16R8` | （白牌） | ESP32-S3 N16R8 | — |
| `ESP32-S3-SuperMini` | （白牌） | ESP32-S3 SuperMini | — |

### 10.5 現有硬體設定（2026-11-16 整理）

| 代號 | 晶片 | Flash | PSRAM | CPU | Console | 能力碎片 | project seed |
|---|---|---|---|---|---|---|---|
| `ESP32-C3-DevKitM` | esp32c3 | 4MB@40 DIO | — | 160 | uart0 | — | hardware／system／network／tasks |
| `ESP32-P4-ETH` | esp32p4 | 32MB@40 DIO | 32MB hex@200 | 360 | uart0 | eth-rmii／nc4-ws／nc4-udp | hardware／system／network／tasks |
| `ESP32-P4-FireBeetle2` | esp32p4 | 16MB@40 DIO | 32MB hex@200 | 360 | uart0 | eth-rmii／wifi-hosted-c6 | hardware／system／network／tasks |
| `ESP32-S3-Matrix` | esp32s3 | 4MB@80 DIO | — | 240 | none（OTG MSC＋CDC log） | — | hardware／system／network／tasks |
| `ESP32-S3-Mini-Nano` | esp32s3 | 16MB@80 DIO | 8MB octal@80 | 240 | none（OTG MSC＋CDC log） | — | hardware／system／network／tasks |
| `ESP32-S3-N16R8` | esp32s3 | 16MB@80 DIO | 8MB octal@80 | 240 | usb-serial-jtag | — | hardware／system／network／tasks |
| `ESP32-S3-SuperMini` | esp32s3 | 4MB@80 DIO | 2MB quad@80 | 240 | usb-serial-jtag | — | hardware／system／network／tasks |

> **USB 的配置地圖**（晶片 → 板子 → 用戶、每顆晶片幾條通道、S3 開 MSC 的完整步驟）
> 見 [docs/03_notes/usb-layers.md](../docs/03_notes/usb-layers.md)。
>
> **S3 的 CPU 預設一律 240 MHz（最高）**；要降頻就改該板 `board.json` 的 `cpu_mhz`
> 同 `sdkconfig.defaults` 的 `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_*`，然後
> `make.py build <board> --fresh`（sdkconfig 有 cache，不 fresh 不會生效）。
> 這是 **build-time 預設**；執行期熱切換（MicroPython `machine.freq()` 那種）目前未實作，
> 要開 `CONFIG_PM_ENABLE` ＋ `esp_pm_configure()`，並補 UART／SPI／LEDC 等時鐘補償。
>
> `driver.json` 目前 P4-ETH／Matrix／Zero／N16R8／SuperMini 有（SuperMini 全 `enable=0`，係
> schema 示範）；C3-DevKitM／Mini-Nano／P4-FireBeetle2／P4-WIFI6-DB 冇掛外部裝置，
> check 的 warning 是預期（不是缺檔錯誤）。**只有 `LED` 段真的會被 firmware 讀**（見 §5.1）。
> 晶片能力（cores／USB）在 `ports/hardware/_chips/`，
> 它的 `usb.link` 必須與 `nc_cfg_chip.c` 的 fallback 一致（`usb_jtag0`／`cdc0`）。

### 10.6 自動檢查

`python3 tools/board.py check` 會驗：代號格式、晶片前綴、`name` 與目錄名一致、
`aliases` 格式與撞名。`python3 tools/board.py new --board …` 會直接拒絕不合公約的名字，
並支援 `--brand` / `--model` / `--rev` / `--alias`。
