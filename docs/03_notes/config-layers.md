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
5. firmware **不寫**（第一次開機寫預置除外）；唯一例外是「補值」——
   例如 `cID` 空的就填 MAC 推導值，寫回時**只換那一個值**（token patch），
   你的縮排、鍵順序、`_comment` 位置都不會被動，舊檔留 `.bak`。

---

## 4. 開機順序（誰在什麼時候讀）

```text
① NVS init
② hardware.json → 硬體 init（GPIO/SPI/I2C/UART/ETH PHY；缺檔用 board.json）
③ driver.json   → driver init（ETH DMA、USB、TFT…）
④ 硬體起來了，讀分配：
     system.json  → NC4 decode[] / links[]
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
  "_comment": "身份、開機行為、NC4 通道與解碼路線。CPU 與緩衝是系統資源。",
  "cID": "0xF5F9",
  "debug_level": 3,
  "frame_interval_ms": 33,
  "boot_strict": 1,

  "nc4": {
    "decode": [
      { "name": "decode0", "core": 0, "prio": 4, "budget": 8 },
      { "name": "decode1", "core": 1, "prio": 4, "budget": 8 }
    ],
    "links": [
      { "name": "CDC",  "kind": "cdc",  "rx": { "decode": "decode0" } },
      { "name": "UART", "kind": "uart", "rx": { "decode": "decode1" } },
      { "name": "TCP",  "kind": "tcp", "port": 6000,
        "rx": { "mode": "push_frame", "decode": "decode0", "slots": 4, "slot_size": 8205 } },
      { "name": "WS",   "kind": "ws",  "port": 8080,
        "rx": { "mode": "message", "decode": "decode1", "max_msg": 24576 } },
      { "name": "UDP",  "kind": "udp", "port": 6001,
        "rx": { "mode": "datagram", "decode": "decode1", "max_msg": 1472 } }
    ]
  }
}
```

### `hardware.json`（硬體層）

```jsonc
{
  "version": 1,
  "PIN":  { "list": [ { "id": 16, "mode": "out", "level": 0 } ] },
  "SPI":  { "list": [ { "id": 0, "mosi": 11, "sclk": 12, "miso": 13 } ] },
  "I2C":  { "list": [ { "id": 0, "sda": 8, "scl": 9, "freq": 400000 } ] },
  "UART": { "list": [ { "id": 1, "tx": 7, "rx": 8, "baud": 921600 } ] },
  "ETH":  { "mdc": 31, "mdio": 52, "ref_clk": 50 }
}
```

### `driver.json`

```jsonc
{
  "version": 1,
  "ETH": { "phy_type": "IP101", "dma_buf": 1600, "rx_desc": 16, "tx_desc": 16 },
  "USB": { "enable": 1, "msc": 1, "cdc": 1 },
  "TFT": { "enable": 0, "driver": "st7789", "width": 240, "height": 320 }
}
```

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
    "enable": 0, "mode": "ble",          // ble | classic | both
    "name": "",                          // 空 = 用 hostname
    "pin": "",                           // 空 = Just Works（雙方免輸入）；有內容 = 取前 4 位數字當 PIN
    "bonding": 1                         // 記住金鑰：配一次，之後自動重連
  },

  "espnow": {
    "enable": 0, "channel": 0,           // 0 = 跟隨 WiFi
    "encrypt": 0, "pmk": "",
    "peers": [ { "mac": "AA:BB:CC:DD:EE:FF", "lmk": "" } ]
  }
}
```

**能力是 build-time 決定的**：config 只能「打開」這個 build 已經編進去的東西。
- WiFi / BT / ESP-NOW 要靠 `ports/_profiles/*.defaults`（例如 `wifi`、`bt`、`espnow`）編進 image；
  沒有那塊能力時，`enable: 1` 只會在 log 說「這個 build 不支援」。
- **ESP-NOW 依賴 WiFi radio**（P4 沒有 WiFi，所以 P4 的 `espnow` 永遠不可用）。
- **BT 與 WiFi 共用 radio**，同時開要開 coex（見 profile）。
- `ssid_pw` / `pmk` / `lmk` / `pin` 是秘密：之後照舊專案的做法搬到 NVS，JSON 只留空字串。

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
  （`bet.pin` 的路徑用 token patch，排版不動）。不足 4 位會寫回 `""`。
- **Classic（SPP）**：legacy PIN 就是這 4 位。
- **BLE**：passkey 是 **6 位**數字，韌體把 4／5 位的 PIN 左補 0 到 6 位
  （例：`"1234"` → 對端輸入 `001234`；`"123456"` → 直接 6 位）。
  這是 BLE 協定的固定寬度，不是我們多存了什麼。

### `tasks.json`

```jsonc
{
  "version": 1,
  "override": [
    { "name": "decode0", "core": 0, "prio": 4, "stack": 6144 },
    { "name": "tcp_nc4_rx", "core": 1, "prio": 10 }
  ]
}
```

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
| C1 | `system.json` 的 `nc4.links` 先接（唯讀） | 改 `slots` 重開機生效 |
| C2 | `hardware.json` / `driver.json` 接掉 `main.c` 的寫死值 | 換腳位不用改 code |
| C3 | `network.json` / `tasks.json` | IP 與 core 可由檔案覆蓋 |
| C4 | 出廠預置改成多檔（`ports/<board>/data/config/`） | 新板首次開機長出 `config/` 目錄 |
| C5 | `pixel/` 樹（`effects/`、`map/`、`modes/`） | 拖拉 `.fxe` 進 `pixel/effects/` 就生效 |

---

## 8. 一個原則

**每一層只讀自己的檔案、只用自己懂的欄位、缺檔就用預設。**
中央不解析、不轉發、不幫別人驗證 —— 這樣任何一個檔壞掉，都只是那一層退化。
