# ports/ — 板子層（設定，不是程式）

`firmware/components/` 分「這是什麼程式」；`ports/` 分「**這塊板子要什麼**」。

原則：**晶片 = 一個 build，硬體板 = 一份描述，project = 一組選擇。**

---

## 1. 兩層：硬體板 vs project

```text
ports/
├─ hardware/<chip>/<board>/     這塊板「有什麼」
│  ├─ board.json                 F.spec   晶片／flash／PSRAM／CPU／console／能力
│  ├─ wiring.json                F.wiring 腳位、匯流排、板載晶片（**firmware 預設的唯一來源**）
│  ├─ driver.json                F.device 掛在匯流排上的裝置（build-time；runtime 逐段接）
│  ├─ sdkconfig.defaults         F.build  硬體事實（flash/PSRAM/CPU/console/ETH/WiFi）
│  ├─ partitions.csv             這塊 flash 的分割表（可選；沒有就用 firmware/partitions.csv）
│  └─ README.md
│
├─ project/<chip>/<board>/      這塊板「跑什麼」
│  ├─ project.json               F.project  name／hardware 指回哪塊板／profiles／seed 清單
│  ├─ sdkconfig.defaults         F.build   項目能力（ws/udp…）產生的碎片
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
| **L2 能力碎片** | ETH／WiFi（硬體能力）；NC4 over WS／UDP（項目能力） | ✅ 要 | `hardware/profiles`／`project/profiles` → `_profiles/*.defaults` |
| **L3 執行期** | gpio、服務開關、像素數、網路位址、任務 core | ❌ 同一 binary | `project/data/config/*.json` → 裝置 `/data/config/` |

> **PSRAM、flash 大小、flash 頻率永遠不可能用 config.json 換**——它們在 bootloader 與
> 記憶體初始化階段就決定了。寫進 runtime config 只會被 `tools/board.py check` 擋下來。

---

## 3. 日常操作

```bash
python3 tools/board.py list              # 硬體板 × project × 能力
python3 tools/board.py check             # 驗 board.json ↔ sdkconfig、兩層 profile、seed
python3 tools/board.py sync              # 改 _profiles/ 或 profiles 之後重跑
python3 make.py build ESP32-P4-ETH       # 用硬體板名（自動配同名 project）
python3 make.py build P4/ESP32-P4-ETH    # 用 <chip>/<board>
python3 make.py build <project 名>       # 用 project 名（hardware 由 project.json 決定）
python3 make.py boards                   # 同 board.py list
```

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
  "profiles": ["eth-rmii"],          // 只放硬體能力；ws/udp 在 project
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

> `wiring.json` 裡的 `SPI`／`I2C`／`I80`／`I2S`／`ENC`／`XL9555` 目前**只保留在 seed**，
> 還沒有對應的 typed struct；`ncportgen` 會把它們原樣放進 `preset_hardware.json`，
> 等 driver 有型別再逐段接。已 typed 的是 `ETH`／`SDIO`／`PIN`／`PWM`／`UART`。

---

## 6. 能力碎片的規矩

- 碎片只放「**同一個原因而存在的多行設定**」（ETH 一定要配 DMA + lwIP 視窗）。
- 碎片裡不要出現任何 L1 事實（型號、腳位、頻率）。
- 改碎片 → `board.py sync` → 所有宣告它的板子／project 一起更新；牆上的數字只有一份。
- `sdkconfig.defaults` 檔尾那段是**產生出來的**，手改會在下次 `sync` 被蓋掉；
  這塊板子獨有的東西請寫在 marker 區塊上方。
- **硬體能力**（eth-rmii、wifi-hosted-c6）寫在 `hardware/board.json` 的 `profiles`；
  **項目能力**（nc4-ws、nc4-udp）寫在 `project/project.json` 的 `profiles`。
  build 時的合併次序：`firmware` → `hardware` → `project`。

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
| 15 塊板子並存 | 目前 5 塊，`board.py new` 隨時加 | 沒 driver 支撐的板子不要先開空殼 |

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
