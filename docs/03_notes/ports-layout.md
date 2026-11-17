# ports 兩層：hardware（有什麼）＋ project（跑什麼）

> 2026-11-16 定案。舊的 `ports/<chip>/<board>/` 一律搬到
> `ports/hardware/<chip>/<board>/`；runtime 預置搬到 `ports/project/<chip>/<board>/`。

---

## 1. 為什麼要分

舊目錄把三種東西放在一起：

```text
ports/P4/ESP32-P4-ETH/
├─ board.json          硬體事實
├─ wiring.json         硬體接線
├─ driver.json         板載晶片
├─ sdkconfig.defaults  建置事實 + 能力碎片
└─ data/config/        出廠執行期預置（system/network/tasks/hardware）
```

問題不是「檔案太多」，而是**同一塊板只可以有一個「項目」**：

- `system.json` 的 `links[]` / `routes[]` 是「這個部署要收哪幾條線」——項目決定，不是硬體。
- `tasks.json` 是「這個項目要跑哪些任務」——項目決定。
- `network.json` 的 IP 是「這個現場」——項目／部署決定。
- `wiring.json` 的腳位是「這塊板焊成怎樣」——硬體決定，換項目不變。

分成兩層之後，同一塊硬體可以掛多個 project，各自帶自己的 runtime 預置；
build 能力只由硬體板決定（project 唔會改 Kconfig）。

---

## 2. 目錄與所有權

```text
ports/
├─ hardware/<chip>/<board>/
│  ├─ board.json              F.spec    晶片 / flash / PSRAM / CPU / console / features
│  ├─ wiring.json             F.wiring  腳位、匯流排、板載晶片（firmware 預設唯一來源）
│  ├─ driver.json             F.device  匯流排上的裝置（build-time）
│  ├─ sdkconfig.defaults      F.build   硬體事實 + 硬體能力碎片
│  ├─ partitions.csv          這塊 flash 的分割表（可選）
│  └─ README.md
├─ project/<chip>/<board>/     **只有 runtime 設定，冇 build 設定**
│  ├─ project.json            F.project name / hardware / seed
│  └─ data/config/*.json      F.preset  system / network / tasks
├─ _profiles/                 能力碎片（eth-rmii / nc4-ws / nc4-udp / wifi-hosted-*）
├─ _chips/                    別名 chip profile（ST7789 / GT1151Q …，driver.json 引用）
└─ hardware/_chips/           晶片能力（cores / USB），ncportgen 讀它產生 nc_cfg_chip_t
```

`project.json`：

```jsonc
{
  "version": 1,
  "name": "ESP32-P4-ETH",
  "hardware": "P4/ESP32-P4-ETH",   // 必要：指回硬體層
  "seed": ["system", "network", "tasks"],
  "default": true                  // 可選：`build <硬體板>` 冇 --project 時用這個
}
```

### 2.1 命名（2026-11-16 定案）

目錄名／`name`／project 名 = **`<CHIP>-<Identity>`**；品牌、型號、板本寫
`board.json` 的 `brand`／`model`／`rev`，不進目錄名。改名要留 `aliases`。
完整規則、現有板子清單與檢查方式見 [ports/README.md §10](../../ports/README.md)。

**2026-11-16 改名**：`ESP32-P4-C6` → **`ESP32-P4-FireBeetle2`**
（DFRobot FireBeetle 2 ESP32-P4／DFR1172；舊名與 `P4-C6` 都寫進 `aliases`，
`make.py build ESP32-P4-C6` 仍然解析得到）。同日新增 `ESP32-S3-Mini-Nano`
（通用 S3-N16R8 白牌板）。

### 能力碎片放哪一層

| 碎片 | 層 | 理由 |
|---|---|---|
| `eth-rmii` | hardware | 板上有沒有 EMAC／PHY 是硬體事實 |
| `wifi-hosted-c6` | hardware | 外掛 C6 是板載硬體 |
| `nc4-ws` / `nc4-udp` | hardware | 2026-11-16 修正：**project 唔再有 build 設定**；同一塊板要 WS／UDP 就開在硬體板的 profiles（重 build 一次，所有 project 共用同一個 binary） |

> **project 只放 runtime 設定**（`project.json` ＋ `data/config/*.json`）。換部署 = 換
> project 的 data/config，不需要重 build；要改 build 能力 = 改 hardware 的 profiles 並重 build。

build 的 `SDKCONFIG_DEFAULTS` 次序：

```text
firmware/sdkconfig.defaults → hardware/sdkconfig.defaults
```

後者勝。`tools/board.py sync` 只會把 hardware 的 `profiles` 寫進
`hardware/sdkconfig.defaults` 檔尾 marker 區塊；project 層冇 build 設定可以同步。

---

## 3. build 時怎麼指定

```bash
python3 make.py build ESP32-P4-ETH                  # 硬體 + default project
python3 make.py build ESP32-P4-ETH --project Demo    # 指定 project（短名可）
python3 make.py build ESP32-P4-ETH --no-project      # 純硬體
python3 make.py build P4/ESP32-P4-ETH     # <chip>/<board>
python3 make.py build <project 名>        # project 名 → 由 project.json 找硬體
python3 make.py build --all               # 每塊硬體板各建一次（各自 default project）
```

解析規則（`tools/ncbuild/env.py::resolve_board`）：

1. 查詢先找 `ports/hardware/`：`name`、目錄名、`<chip>/<board>`。
2. 再找 `ports/project/`：`name`、目錄名、`<chip>/<board>`。
3. 命中 project → 讀 `project.json.hardware` 找硬體板。
4. 命中硬體板 → 找同名 project；沒有就找 `hardware` 指向它的 project。
5. 兩層都找不到 → 列出可用名字，**不猜**（猜錯就是燒錯板）。

CMake 收到：

```text
-DPORT=P4/ESP32-P4-ETH          # ports/hardware/ 之下
-DPROJECT=P4/ESP32-P4-ETH       # ports/project/ 之下；可以空
-DIDF_TARGET=esp32p4
```

---

## 4. firmware 預設：從 JSON 變成編譯期常數

以前：

```text
wiring.json ──EMBED_FILES──► firmware image ──開機 cJSON parse──► struct
hardware.json（完整副本）──EMBED_FILES──► 同一份 image
```

同一份資料有兩份（JSON 文字 + struct），而且 `_comment`、重複的 `"mode"`／
`"type"`／`"GPIO"` 鍵全部進 flash。

現在：

```text
wiring.json ──tools/ncportgen.py（build 時）──► nc_port.c
                                            └► preset_hardware.json（seed）
```

`nc_port.c` 是 static const：

```c
static const nc_hardware_cfg_t s_hw = {
    .version = NC_CFG_SCHEMA_VERSION,
    .eth = { .present = true, .enable = true, .mdc = 31, .mdio = 52,
             .ref_clk = 50, .phy_addr = 1, .phy_type = NC_CFG_PHY_IP101 },
    .n_sdio = 0,
    .sdio = { { 0 } },
    ...
};
const nc_hardware_cfg_t *nc_port_hw_defaults(void) { return &s_hw; }
```

- `nc_cfg_defaults_hardware()` 回傳它（weak symbol；host test 沒有生成檔時退回 `k_hw`）。
- 用戶 `/data/config/hardware.json` 仍然逐欄位覆蓋它；**沒有用戶檔也不會失去硬體預設**。
- `app_main` 用 `nc_port_chip()`（由 `hardware/_chips/<chip>.json` 產生）取代
  `nc_cfg_chip_find(CONFIG_IDF_TARGET)`；`nc_cfg_chip.c` 只留作 fallback。

### 字典 → 數值

`ncportgen.py` 在 build 時把 JSON 的字典對標成 C enum：

| JSON | C | 省什麼 |
|---|---|---|
| `"mode":"OUT"` / `"IN"` | `0` / `1` | 不用每次 `strcasecmp` |
| `"pull":"UP"` / `"DOWN"` | `1` / `2` | 同上 |
| `"phy_type":"IP101"` | `NC_CFG_PHY_IP101` | `char[12]` → 1 byte |
| `"iface":"eth"` / `"service":"nc4"` / `"mem":"psram"` | enum | 每個 link 22 B |
| `_comment` | 不進 firmware | 每塊板 ~1 KB |

link 的三個字典欄位（`iface` / `service` / `mem`）在 runtime 的用戶 JSON 仍然寫字串，
但 parse 一次就只存 1 byte；`nc_cfg_iface_name()` 等只給 log／STATUS 用。

**已經 typed 的 hardware 段**：`ETH`、`SDIO`、`PIN`、`PWM`、`UART`。
其餘（`SPI`、`I2C`、`I80`、`I2S`、`ENC`、`XL9555`）目前只留在
`preset_hardware.json` 給人看，還沒有 struct；等 driver 有型別再逐段接。
`driver.json` 也還是 build-time 資料，runtime 未讀。

---

## 5. 還有哪些「未結構化」的地方

| 位置 | 現況 | 建議 |
|---|---|---|
| `system.json` 的 `port` 與 `cfg.port` | 2026-11-16 已正規化：新檔只寫 `port`；舊 `cfg.port` 仍然讀，兩者不一致會計 invalid | ✅ 已完成；新 seed 全部用 canonical |
| `kind` 的複合字串（`eth_tcp` / `wifi_udp`） | 2026-11-16 新格式用 `kind:"tcp", iface:"eth"`；舊字串仍然拆開接受 | ✅ 已完成；冇 `name` 時生成的名字仍是 `eth_tcp0` |
| `rx` 的位置 | 新格式直接在 link 下面；舊 `cfg.rx` 仍然讀 | ✅ 已完成 |
| `network.json` 的 lan / wifi / bt / espnow | 仍然是各處 ad hoc 讀（`nc_wifi.c`、`nc_net_lines.c`、`nc_net_raw.c`、`main.c`），沒有單一 typed struct | 下一步：`nc_cfg_network()` ＋ 一個 parser；先由 LAN IP 與 WiFi 兩段開始 |
| `driver.json` 的 `bus:{type,index}` | build-time 資料，runtime 未讀；每個裝置重複一個小字典 | 有 driver 型別後改成 `bus_type` + `bus_index` 兩個 u8，並 embed 成 generated 表 |
| `routes[].out[]` | 字串（link 名 / `decodeN`），每列 4×16 B | 下一步可改成 `{type, index}`；但 router 目前靠字串，先不動 |
| `tasks.json` 的 `name` | 字串 24 B × 20 | 任務名是程式註冊的，可用 id；但用戶要看得懂，暫留 |
| `_comment` | 每個 JSON 都一大段 | 只留在 `F.preset`（給人看）；不要進 firmware 的 generated C |

數字化的判準：**這個欄位在 runtime 會不會被比較／儲存很多份？**
會 → enum；只是給人看的 → 留字串。

---

## 6. OTA 空間與分割表（回答「OTA 要唔要預先燒錄？」）

### 6.1 分割表本身不能用 OTA 改

ESP32 的分割表是 flash `0x8000` 的一個資料結構，由 **bootloader 開機時讀**。
`esp_ota_*` 只會：

1. 在**已存在的** `ota_0` / `ota_1` 分區寫 image；
2. 把「下次開機用哪個 slot」寫進 `otadata`。

它**不會**改 `0x8000` 的分割表，也不會建立新的分區。所以：

- **OTA 不可以重新指定分割表。** 要改 offset／size，一定要 `idf.py flash`
  或 `make.py flash <board> --full`（bootloader + 分割表 + app）。
- 反過來說：**OTA slot 不需要「預先燒錄」**。空白的 flash 第一次用 USB／UART
  燒一次 bootloader + 分割表 + app 就有 OTA 能力；之後的 image 由 OTA 寫進另一個 slot。
- 理論上可以在 runtime 擦寫 `0x8000`，但那是拿 bootloader 的信任開玩笑：
  寫壞就是無法開機、只能手動進 download mode。本項目不做。

### 6.2 「4 MB × 2 很浪費」其實不花成本

`ota_0` / `ota_1` 各 4 MB 只是**位址空間**，不是 RAM、不是開機時間、不是磨損。
目前 image 約 0.6 MB，4 MB 是 6 倍餘裕。**縮小 slot 只會換來 flash，但換不到任何 runtime 好處；而 slot 太小是災難**（OTA image 放不下 → 只能拆線重燒）。

真正浪費的是：**P4-ETH 是 32 MB flash，卻用了 16 MB 的分割表**，一半 flash 從來沒被使用。
2026-11-16 已補 `ports/hardware/P4/ESP32-P4-ETH/partitions.csv`：

```text
nvs       0x9000     0x6000
otadata   0xf000     0x2000
phy_init  0x11000    0x1000
ota_0     0x20000    0x400000     ← 與 16 MB 表相同，永不移動
ota_1     0x420000   0x400000     ← 與 16 MB 表相同
data      0x820000   0x1700000    ← 由 7 MB 增到 23 MB
```

套用要 `--full`；`data` 的 offset 不變，舊 FAT 仍然在同一個位置。
要真正用到 23 MB，格式化一次 `/data`（會清空）即可。

### 6.3 怕「隨項目添加而後移」：釘住 app，資料放最後

分割表的 offset 是絕對的。避免將來搬家的唯一方法：

1. **app slot 的 offset 一開始就釘死**（`0x20000` / `0x420000`）；
2. **data 放最後**，只增長、不移動；
3. app slot 的大小現在就決定好（4 MB），不要之後改。

這樣「加項目」只會加資料，不會動到 app。真有一天 image 超過 4 MB：

- 一定要重燒分割表（OTA 做不到）；
- 如果 data 在 app 後面，data 會被搬 → 先備份 `/data`，或把 data 放 app 前面
  （代價是 app 以後不能長大）。

本項目選「app 先、data 後」：app 的可預測性比 data 的搬家成本重要。

### 6.4 現在的 OTA 安全網

- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` **未開**：推壞 image 會卡住，要手動救。
  下一步是開 rollback + `esp_ota_mark_app_valid_cancel_rollback()`。
- `tools/ota.py` 走 NC4 0x22xx；網絡 OTA 走 HTTP `POST /ota`（只限可信 LAN）。
- `make.py flash` 預設 `app-flash`（不動碟）；`--full` 才換 bootloader／分割表。

---

## 7. 驗收清單

- [x] `python3 tools/board.py list` 看到 **7 塊硬體板 × 7 個 project**
      （2026-11-16 新增 S3-Mini-Nano、P4-C6 改名 FireBeetle2）
- [x] `python3 tools/board.py check` 0 error（4 個「冇 driver.json」warning，都係 USB-only 板）
- [x] `tools/ncportgen.py` 對每塊板產生 `nc_port.c` / `preset_hardware.json`
- [x] `python3 tools/board.py resolve ESP32-P4-C6`／`P4-C6`／`S3-Mini-Nano` 都由 alias 命中
- [x] project 層冇 `sdkconfig.defaults`／`profiles`（`board.py check` 會擋）
- [x] `idf.py build` 兩個 target（IDF 6.1：P4-ETH `0xff040`、S3-N16R8 `0x118e90`）
- [x] 2026-11-16 本機 IDF 6.0.3：`ESP32-S3-Mini-Nano` `0x114f90`（1.1 MB）、
      `ESP32-P4-FireBeetle2` `0xfd5f0`（1013.5 KB，75% free）
- [x] project 選擇：default／`--project Demo`／`--no-project` 三種都 build 過；
      preset 真係嵌入 firmware（`nm` 見到 `_binary_preset_*`；default `0xfcff0`、
      Demo `0xfad60`、bare `0xfacd0`）
- [ ] 上板：`hardware.json` 缺失時仍用 generated 預設開機
- [ ] 上板：P4-ETH `--full` 後 `/data` 仍掛得起來
