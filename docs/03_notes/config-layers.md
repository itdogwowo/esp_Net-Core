# 配置分層：一個層一個檔案

> **狀態**：重新規劃（2026-11-02）。程式目前只有程式常數 + NVS，還沒有 config loader。
> **參考**：mp_Net-Core 的 `config.json`（一個檔 24 個 section：System / Buffer / SPI / I2C / UART / TFT / WS2812 …）
> **相關**：[decode-routes.md](decode-routes.md)、[writing-a-task.md](writing-a-task.md)、[component-layout.md](component-layout.md)

---

## 0. 先分清楚：能不能換 binary

| 層 | 內容 | 換它要重 build？ | 放哪 |
|---|---|---|---|
| **L1 硬體事實** | flash 大小/頻率、PSRAM、CPU 頻率、有沒有 ETH、console | ✅ 一定要 | `ports/<chip>/<board>/board.json` + `sdkconfig.defaults` |
| **L2 能力碎片** | ETH、NC4 over WS、NC4 over UDP | ✅ 要 | `ports/_profiles/*.defaults` |
| **L3 執行期** | 腳位覆蓋、通道與解碼路線、IP、任務參數、效果開關 | ❌ 同一份 binary | **隨身碟 `config/*.json`** |

判準：**換了它需不需要重燒 firmware？** 需要 → L1/L2；不需要 → L3。

---

## 1. 為什麼分檔，而不是一個大 config.json

舊專案是一個 5.8 KB 的 `config.json`，24 個 section。那在 MicroPython 行得通，因為
`ConfigManager` 是唯一的讀寫者，所有模組共用一個 bus。C 版不行，理由有三個：

1. **每個層的讀取時機不同**：硬體要腳位才能 init；NC4 分配要等硬體起來才知道有哪些通道。
   一個檔會逼所有人同步在同一個時間點讀。
2. **一個壞檔不該拖垮全部**：現在 `json.loads` 失敗就整個檔沒了。分檔之後，
   `network.json` 壞掉只影響 IP，效果照跑。
3. **擁有者要清楚**：`hardware.json` 只有硬體層讀；`system.json` 只有系統層讀。
   改網路的人不會碰到像素設定，也不用怕存檔時重排別人的 section。

還有一個好處：**firmware 只讀不寫**（只有第一次開機寫預置）。
使用者用隨身碟改，firmware 不會跟使用者搶檔案。

---

## 2. 檔案清單

```text
/data/                       ← 隨身碟（FAT，卷標 NETCORE）
├─ config/
│  ├─ hardware.json          腳位與 bus（硬體層 init 前讀）
│  ├─ driver.json            PHY / DMA / TFT / USB 參數（driver init 前讀）
│  ├─ system.json            身份、開機、NC4 通道與解碼路線（硬體起來後讀）
│  ├─ network.json           IP / mask / gw / DHCP
│  └─ tasks.json             任務 core / prio / stack 覆蓋
└─ pixel/                    ★ 效果自己的樹（見 §5.1）
```

| 檔案 | 擁有者 | 讀的時機 | 缺檔 / 壞檔 |
|---|---|---|---|
| `hardware.json` | 硬體層 | **硬體 init 之前** | 用 `board.json` 的板級預設 |
| `driver.json` | driver 層 | driver init 之前 | 用 driver 內建預設 |
| `system.json` | 系統層（main） | **硬體起來之後**（分配要看實際硬體） | 只啟動硬體實際支援的通道 |
| `network.json` | 網路層 | ETH/WiFi 起來前 | 用內建 IP 或 DHCP |
| `tasks.json` | 任務管理器 | 任務登記前 | 用程式裡宣告的 core/prio |
| `pixel/`（目錄） | fx | 掃描 `pixel/effects/` | 不啟用效果 |

---

## 3. 讀取規則（每層自己讀，不中央統一載入）

```c
#include "nc_cfg.h"

nc_cfg_t *cfg = nc_cfg_open("system");     // /data/config/system.json
if (cfg == NULL) {
    use_defaults();                        // 缺檔 → 預設，不是錯誤
    return ESP_OK;
}
const cJSON *root = nc_cfg_root(cfg);
int ver = nc_cfg_version(cfg, 1);          // 版本不符 → 預設 + warning
...
nc_cfg_close(cfg);
```

規則：

1. **缺檔 → 預設**，不是錯誤。使用者刪掉任何一個檔都要能開機。
2. **壞檔 → log + 預設**，該層停用或退化，其他層照跑。
3. `_comment` 一律忽略；`version` 不符 → 預設。
4. 一個層只讀自己那個檔，**不跨檔讀別人的 section**。
5. firmware **不寫**（第一次開機寫預置除外）；例外是「補值」與「補條目」，判準是**用戶有沒有機會知道**：
   - `cID` 空白 → 填 MAC 推導值（用戶看不到 MAC）；
   - `tasks.json` 缺固件內建的任務 → 補在下面（用戶看不到固件裡有什麼）；
   - `system.json` 的 `routes` 缺一條新線路 → 補 `{in: 線路, out: []}`（線路是程式建的）。
   一律只加不改、放在用戶設定之後。原則是「補值」——
   例如 `cID` 空的就填 MAC 推導值，寫回時**只換那一個值**（token patch），
   你的縮排、鍵順序、`_comment` 位置都不會被動，舊檔留 `.bak`。

---

## 4. 開機順序（誰在什麼時候讀）

```text
① NVS init
② hardware.json → 硬體 init（GPIO/SPI/I2C/UART/ETH PHY；缺檔用 board.json）
③ driver.json   → driver init（ETH DMA、USB、TFT…）
④ 硬體起來了，讀分配：
     system.json  → engines[] / services[] / nc4.links[] / routes[]
     network.json → IP
     tasks.json   → 任務覆蓋
⑤ 宣告（記帳）  看②③的實際結果 + ④的分配
⑥ 資源 carve    nc_plan_commit(當下可用量)
⑦ NC4 通道啟動  建 transport → 建 bus → 掛 decode 引擎
⑧ 任務啟動     nc_task_start_all() → nc_task_boot() 依 layer
⑨ pixel/（掃 `pixel/effects/*.fxe`，attach 時才載入）
```

**為什麼 ④ 在 ②③ 之後**：分配是「把通道/引擎/緩衝放到實際存在的硬體上」。
ETH PHY 沒 link、UART 沒接，就不該照 config 硬開一條 link。
**為什麼 ② 之前就有 config**：腳位是 init 的輸入，所以 `hardware.json` 由硬體層自己讀，
不跟 ④ 的分配混在一起。

---

## 5. 各檔 schema

### `system.json`（System 層）

```jsonc
{
  "version": 1,
  "cID": "0xF5F9",
  "frame_interval_ms": 33,
  "num_pixels": 676,

  "nc4": {
    "links": [                                  // 通道：只講怎麼收
      { "name": "CDC", "kind": "cdc", "cfg": { "rx": { "mode": "pull", "region": 8205, "chunk": 1024, "budget": 8 } } },
      { "name": "UART", "kind": "uart", "cfg": { "rx": { "mode": "pull", "region": 1024, "chunk": 512, "budget": 8 } } },
      { "name": "TCP", "kind": "tcp", "cfg": { "port": 6000, "rx": { "mode": "push_frame", "region": 16410, "chunk": 8192, "budget": 8, "slots": 8, "slot_size": 8205 } } }
    ],
    "routes": [                                 // 誰解、要不要轉出去（router 永遠生效）
      { "in": "nctx",     "out": ["decode0"] },   // 本機 → 自己
      { "in": "outbound", "out": [] },            // 本機 → 外
      { "in": "CDC",      "out": ["decode0"] },
      { "in": "UART",     "out": ["decode1"] },
      { "in": "TCP",      "out": ["decode0", "UART"] }
    ]
  }
}
```

`routes[].out` 同時接受**通道**（轉發）與 **decode 引擎**（本地執行），跟舊專案的
`out: ["self"]` 同一個語意（新專案的本機來源叫 `nctx`）；`decodeN` 有人指向才開（N = 核心編號），**不需要宣告引擎**。
`fxN` 由 `.fxe` manifest 的 `core` 推導，也不能放進 `out`（它不吃幀）。
完整規則見 [routing.md](routing.md)。

### `hardware.json`（硬體層）

```jsonc
{
  "version": 1,
  "_gpio_convention": "（見下）",

  "ETH":  { "enable": 1, "mdc": 31, "mdio": 52, "ref_clk": 50,
            "phy_type": "IP101", "phy_addr": 1,
            "spi": -1, "cs": -1, "rst": -1, "int": -1 },
  "PIN":  { "enable": 0, "list": [ { "GPIO": null, "label": "tft_dc", "mode": "OUT", "initial": 0 } ] },
  "SPI":  { "enable": 0, "list": [ { "baudrate": 80000000, "phase": 0, "polarity": 0,
                                     "GPIO": { "sck": null, "mosi": null, "miso": null }, "data_pins": [] } ] },
  "I80":  { "enable": 0, "list": [ { "data": [], "wr": -1, "cs": -1, "freq": 80000000 } ] },
  "SDIO": { "enable": 0, "list": [ { "freq": 40000000, "width": 4,
                                     "GPIO": { "sck": null, "cmd": null, "data": [-1,-1,-1,-1] } } ] },
  "XL9555": { "enable": 0, "bus": { "type": "i2c", "index": -1 }, "addr": "0x20",
              "list": [ { "IO": 0, "label": "LCD_RST", "mode": "OUT", "initial": 1 } ] }
}
```

`ETH` 是特例：不掛在任何匯流排上，整段放最頂，不拆去 driver。
DMA 大小（1600）與環數（16/16）**不在 config**——那是 build-time，見 `ports/_profiles/eth-rmii.defaults`
（512 → 1.9 MB/s；1600 配 16/16 → 7.4–8.1 MB/s；32/32 反而令 UL 掉到 3.65，見 [p4-bringup.md](p4-bringup.md)）。
`XL9555` 在這層註冊；但 **I80 面板的 rst / bl 是用 `TFT.xl9555` 子樹填「IO 編號」**（0 / 1），
不是 label——這是舊專案 `_pins_note` 就定下的規則。`TFT.pins` 只放 `PIN` 的 label（dc / cs / rst / bl）。

### `driver.json`

```jsonc
{
  "version": 1,

  "USB":    { "enable": 0, "msc": 0, "cdc": 0 },
  "TFT":    { "enable": 0, "bus": { "type": "spi", "index": -1 }, "driver": "ST7789",
              "width": 240, "height": 320, "rotation": 0, "color_order": "RGB",
              "invert": 0, "pixel_format": "RGB565_BE",
              "pins": { "dc": "tft_dc", "cs": "tft_cs", "rst": "tft_rst", "bl": "tft_bl" },
              "xl9555": { "rst": 0, "bl": 1 } },
  "SDcard": { "enable": 0, "bus": { "type": "sdio", "index": 0 }, "mount": "/sd", "slot": 0 },
  "PCM5102": { "enable": 0, "list": [ { "bus": { "type": "i2s", "index": -1 }, "GPIO": { "xsmt": null } } ] }
}
```

**引用匯流排一律用 `bus = { type, index }`**：`type` 選種類（spi / i2c / uart / i2s / sdio / i80），
`index` 選第幾個實例（0 = 該類型 `list` 的第一個，-1 = 未接）。
不用 `spi_id` 那種 id——因為 id 是使用者自己填的，index 跟註冊次序一致，不容易對錯。

`USB` 排最前：它不掛在任何匯流排上，跟 `ETH` 在 hardware 的地位對稱。

**舊專案的硬體節怎麼分到這兩層**（照抄時的對照表）：

| 舊 section | 新家 | 備註 |
|---|---|---|
| `PIN` / `SPI` / `I2C` / `I80` / `UART` / `I2S` / `PWM` / `ENC` | `hardware.json` | 匯流排與腳位本身 |
| `SDcard` → `SDIO` | `hardware.json` | 只留匯流排（腳位、freq、width）；卡本身在 driver |
| `Network.lan`（GPIO + driver + phy） | `hardware.json` → `ETH` | 整段搬，不拆；特例放最頂 |
| `XL9555` | `hardware.json` | I2C 上註冊，IO 變成 label 給 driver 用 |
| `TFT` / `SDcard` / `WS2812` / `APA102` / `PCA9685` | `driver.json` | 用 `bus.index` 引用 hardware |
| `Audio` / `PCM5102` / `GT1151Q` / `HUSB238` / `uartMotor` | `driver.json` | 同上 |
| `USB` | `driver.json` | 排最前（不掛匯流排） |

**量測值不進 config**：已經量到「最好」的數字，用戶不需要調、也不應該調——
寫在 firmware 或 build-time，真的要改的人要重新量。

| 數字 | 現在的值 | 放哪 | 為什麼不能隨便動 |
|---|---|---|---|
| ETH DMA buffer / 環數 | 1600 / 16 / 16 | `ports/_profiles/eth-rmii.defaults` | 512 → DL 1.9 MB/s；32/32 → UL 掉到 3.65 |
| `read_chunk`（CDC / UART / TCP） | 1024 / 512 / 8192 | firmware 預設 | 一次讀太少會多繞幾圈，直接掉吞吐 |
| `rx.region` | 8205 / 1024 / 16410 | 跟 `NC4_MAX_FRAME` 綁 | 改了就是改 plan 的記憶體宣告 |
| `rx.slots` / `slot_size` | 8 × 8205 | firmware 預設 | 記憶體 vs 掉幀的取捨，已量過 |

對照之下，**部署之間真的會不同**的才進 config：哪幾條通道、埠號、routes、IP、腳位、enable。

### `network.json`

四種連線方式並列：**有線（lan）/ 無線（wifi）/ 藍牙（bt）/ ESP-NOW**。
`priority` 決定同時啟用時誰先（照舊專案）。

```jsonc
{
  "version": 1,
  "_comment": "Network 層：有線 / 無線 / 藍牙 / ESP-NOW。只放連線參數，不決定誰來解碼。",
  "hostname": "",                        // DHCP 主機名 / mDNS；空 = 用 cID 推導（待實作）
  "priority": ["lan", "wifi"],           // 同時啟用時的偏好順序

  "lan": {
    "enable": 1, "driver": "RMII", "phy_type": "IP101", "phy_addr": 1,
    "gpio": { "mdc": 31, "mdio": 52, "ref_clk": 50 },
    "ip": { "mode": "static", "addr": "10.10.1.50", "mask": "255.255.0.0", "gw": "10.10.1.1" }
  },

  "wifi": {
    "enable": 0, "mode": "sta",          // sta | ap | apsta
    "ssid": "", "ssid_pw": "", "timeout_s": 300,
    "ip": { "mode": "dhcp" }             // dhcp | static
  },

  "bt": {
    "enable": 0, "profile": "none",      // none｜hid（鍵盤手把）｜spp（序列埠）｜a2dp（音樂）｜hfp（通話）｜gatt（自訂 BLE）
    "fast": 0,                            // 0 = 省電（長間隔）｜1 = 低延遲（短間隔、2M PHY）
    "name": "",                           // 空 = 用 hostname
    "pin": "",                            // 空 = Just Works（雙方免輸入）；有內容 = 只取數字，4–6 位
    "bonding": 1                          // 記住金鑰：配一次，之後自動重連
  },

  "espnow": {
    "enable": 0, "channel": 0,           // 0 = 跟隨 WiFi
    "encrypt": 0, "pmk": "",
    "peers": [ { "mac": "AA:BB:CC:DD:EE:FF", "lmk": "" } ]
  }
}
```

> 這四種連線方式怎麼選、每個參數什麼意思、哪些板子支援哪一種：
> 見 [connectivity.md](../02_guides/connectivity.md)。

**能力是 build-time 決定的**：config 只能「打開」這個 build 已經編進去的東西。
- WiFi / BT / ESP-NOW 要靠 `ports/_profiles/*.defaults`（例如 `wifi`、`bt`、`espnow`）編進 image；
  沒有那塊能力時，`enable: 1` 只會在 log 說「這個 build 不支援」。
  （這三個 profile 目前尚未建立。）
- **Classic / BTDM 只有初代 ESP32 有**：S3 與 C 系列只有 BLE，P4 連 radio 都沒有。
  所以「用哪個協定」不是使用者填的欄位，而是 `profile` ＋ 板子能力推導出來的。
- **ESP-NOW 依賴 WiFi radio**（P4 沒有 WiFi，所以 P4 的 `espnow` 永遠不可用）。
- **BT 與 WiFi 共用 radio**，同時開要開 coex（見 profile）。
- `ssid_pw` / `pmk` / `lmk` / `pin` 是秘密：之後照舊專案的做法搬到 NVS，JSON 只留空字串。

**`profile` × 板子能力 → 實際協定**（`fast` 只在雙模板上影響 BLE／Classic 的取捨）：

| 能力 | `profile` | `fast` | 實際 |
|---|---|---|---|
| dual（初代 ESP32） | `a2dp` / `hfp` | 任意 | Classic（只有它有） |
| dual | `hid` / `spp` | 1 | Classic（低延遲） |
| dual | `hid` / `spp` | 0 | BLE（省電） |
| ble（S3） | `hid` / `gatt` | 任意 | BLE |
| ble | `a2dp` / `hfp` / `spp` | 任意 | log 不支援 |
| none（P4） | 任意 | 任意 | log 不支援 |

**PIN 的解析規則**（沒有 `pairing` 欄位，模式由 `pin` 決定）：

| `pin` 的內容 | 結果 |
|---|---|
| `""` | **Just Works**：雙方都不用輸入（手機 OS 可能仍跳一次配對確認） |
| `"1234"` | 4 位 → 照用 `1234` |
| `"12345"` / `"123456"` | 5／6 位 → 照用 |
| `"12-34"` | 只取數字 → `1234` |
| `"dfgd321fgjf55"` | 數字 `32155`（5 位）→ **自動寫回 `"32155"`** |
| `"1234567"` | 多於 6 位 → 取前 6 位 `123456` |
| `"12"`（不足 4 位） | 視為沒設定 → 寫回 `""` + log 警告 |
| `"abcd"`（沒有數字） | 視為沒設定 → 寫回 `""` + log 警告 |

- **正規化**：每次開機讀到 `pin` 就整理一次，結果和原字串不同就寫回檔案
  （`bt.pin` 的路徑用 token patch，排版不動）。不足 4 位會寫回 `""`。
- **Classic（SPP）**：legacy PIN 就是這 4 位。
- **BLE**：passkey 是 **6 位**數字，韌體把 4／5 位的 PIN 左補 0 到 6 位
  （例：`"1234"` → 對端輸入 `001234`；`"123456"` → 直接 6 位）。
  這是 BLE 協定的固定寬度，不是我們多存了什麼。

### `tasks.json`

> 只列**租戶任務**；核心任務永遠唔會寫入（見 [task-model.md](task-model.md)）。

```jsonc
{
  "version": 1,
  "tasks": [
    { "name": "hello", "level": 6, "affinity": [1, 0] }
  ]
}
```

- `level`：`-1` = 唔啟動；同一 level 內按註冊次序。
- `affinity`：`[core0, core1]`，只可以有一個 `1`；`[0,0]` = 唔跑。
- 缺席的條目：開機會自動補寫（只加不改）。

### 5.1 `pixel/`：效果自己的樹

效果不是「一個設定檔」——它是**資產**（程式 + 映射 + 模式），所以自己一棵樹：

```text
/data/pixel/
├─ effects/          效果檔 *.fxe（載入器掃這個目錄）
├─ map/              像素映射（實體排列 → 邏輯索引）
└─ modes/            模式 / 預設（開機跑哪個、參數）
```

規則：

- **檔案系統就是註冊表**：`ls pixel/effects/` 就知道有哪些效果，沒有另一張總表。
- 每個 `.fxe` 自帶 manifest（kind / core / fps / px / cmds），載入器驗它，不靠外部設定。
- `map/`、`modes/` 的 schema 等效果引擎落地再定；現在只載入 + 驗證。
- **UI 暫時不做**（本來要放 `ui.json`，取消）。

>`pixel/` 的檔案由使用者用隨身碟管理；firmware 只讀，`ASSET_RELOAD` 才重新掃描。

---

## 6. 程式對應

| 東西 | 位置 | 狀態 |
|---|---|---|
| `nc_cfg_open/root/version/close` | `firmware/components/sys/config/` | ❌ 還沒有 |
| 各層自己的 parser | 各 component（`sys/store`、`sys/transport`、`main`） | ❌ 還沒有 |
| 出廠預置（每個檔一份） | `ports/<chip>/<board>/data/config/*.json` | ❌ 現在只有一個 `config.json`（321 B，只有 System + Network.lan） |
| 效果樹 | `/data/pixel/`（`effects/`、`map/`、`modes/`） | 🟡 載入器掃 `pixel/effects/`（路徑本輪改） |
| NVS 覆蓋 | `net/rx_core`、`net/rx_split`、`cid` … | ✅ 已有（優先於 config） |

**優先序**：NVS（執行期指令寫的）> `config/*.json`（使用者編的）> 程式預設。

---

## 7. 遷移步驟

| 步 | 做什麼 | 驗收 |
|---|---|---|
| C0 | 建 `sys/config`：開檔 → cJSON → 關檔；缺檔/壞檔回 NULL | host test：三種檔案狀態 |
| C1 | `system.json` 的 `nc4.links` 先接（唯讀）；`routes[]` 等兩個 decoder 落地才生效 | 改 `slots` 重開機生效 |
| C2 | `hardware.json` / `driver.json` 接掉 `main.c` 的寫死值 | 換腳位不用改 code |
| C3 | `network.json` / `tasks.json` | IP 與 core 可由檔案覆蓋 |
| C4 | 出廠預置改成多檔（`ports/<board>/data/config/`） | 新板首次開機長出 `config/` 目錄 |
| C5 | `pixel/` 樹（`effects/`、`map/`、`modes/`） | 拖拉 `.fxe` 進 `pixel/effects/` 就生效 |

---

## 8. 一個原則

**每一層只讀自己的檔案、只用自己懂的欄位、缺檔就用預設。**
中央不解析、不轉發、不幫別人驗證 —— 這樣任何一個檔壞掉，都只是那一層退化。
