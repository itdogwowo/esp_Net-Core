# 配置分層：一個層一個檔案

> **用語（2026-11-16 起）**：本頁每個檔案都有 `F.` 名，之後所有文件、log、對話都用同一組：
> `F.spec`（板卡規格，`hardware/board.json`）、`F.wiring`（板卡接線，`hardware/wiring.json`）、
> `F.device`（板載裝置，`hardware/driver.json`）、`F.build`（建置開關，兩層 `sdkconfig.defaults`／`_profiles`）、
> `F.project`（項目選擇，`project/project.json`）、`F.preset`（出廠預置，
> `project/<board>/data/config/*.json`）、`F.live`（現行設定，裝置 `/data/config/*.json`）、
> `F.default`（內建預設，`nc_cfg_defaults.c`／generated `nc_port.c`／`nc_cfg_chip.c`）。
> **2026-11-16 起 ports/ 已分成 hardware／project 兩層**；完整設計見
> [ports-layout.md](ports-layout.md)。定義與五條規則見 [glossary.md §5](glossary.md)。

> **狀態（2026-11-16 更新）**：`nc_cfg`（`sys/config`）已接三個層：
> `tasks.json`（任務管理器讀 level / affinity / prio，並會補寫）、`system.json`
> （`cID` / `links[]` / `routes[]` / `log_debug_mode` / `usb`）、
> `hardware.json`（generated 預設 ＋ 用戶逐欄位覆蓋；已 typed：ETH／SDIO／PIN／PWM／UART）。
`ports/project/<chip>/<board>/data/config/hardware.json` 現在有一份 **generated snapshot**
（`tools/sync_hardware_presets.py` 由 `wiring.json` 產生），讓用戶在 repo 就看得見具體 JSON；
build 時優先嵌入它。改 `wiring.json` 後要跑一次 sync，否則 snapshot 會舊。
> `network.json` **仍然不是單一 typed 層**：`nc_wifi.c`、`nc_net_lines.c`、
> `nc_net_raw.c`、`main.c` 各自讀自己那一段（wifi / selftest / rawbench / bt.pin）。
>
> **`wiring.json` 不再在 runtime 解析**：build 時由 `tools/ncportgen.py` 編成
> `nc_port.c`（static const `nc_hardware_cfg_t`）＋ `preset_hardware.json`；
> `nc_cfg_defaults_hardware()` 回傳 generated 表，用戶 `/data/config/hardware.json`
> 只覆蓋差異。`nc_cfg_boot_ex()` 的 wiring base 參數已不再由 `main` 使用。
>
> **`driver.json` 仍未接 runtime**：它是 `ports/hardware/<board>/driver.json`
> （build-time，不進 USB），firmware 沒有讀它；改了不會生效。
> `hardware.json` 其餘段（`SPI`／`I2C`／`I80`／`I2S`／`ENC`／`XL9555`）同理，
> 只留在 generated seed，等 driver 有型別才逐個接。
> 下面 §2–§4 有一部分是**目標設計**，不是現況；以本頁頂部 2026-11-16 更新為準。
>
> **2026-11-16 晚 link schema（canonical）**：`kind` 是 composite（`eth_tcp`／`wifi_tcp`…）
> ＋ `index`；config **不寫 `iface`／`service`**；`name` 由 `kind + index` 生成，可 `name` 覆寫。
> 舊 `kind:"tcp", iface:"eth"` 與 `cfg:{port,rx}` 仍然接受；新 seed 已全部正規化。
> `main.c` 另有第 ① 層最小骨架（`k_min_system`／`k_min_network`／`k_min_tasks`），
> 所以 `config/` 四個檔一定存在。

> **參考**：mp_Net-Core 的 `config.json`（一個檔 24 個 section：System / Buffer / SPI / I2C / UART / TFT / WS2812 …）
> **相關**：[decode-routes.md](decode-routes.md)、[writing-a-task.md](writing-a-task.md)、[component-layout.md](component-layout.md)

---

## 0. 先分清楚：能不能換 binary

| 層 | 內容 | 換它要重 build？ | 放哪 |
|---|---|---|---|
| **L1 硬體事實** | flash 大小/頻率、PSRAM、CPU 頻率、有沒有 ETH、console | ✅ 一定要 | `ports/hardware/<chip>/<board>/board.json` + `sdkconfig.defaults` |
| **L2 能力碎片** | ETH、NC4 over WS、NC4 over UDP | ✅ 要 | `ports/_profiles/*.defaults` |
| **L3 執行期** | 腳位覆蓋、通道與解碼路線、IP、任務參數、效果開關 | ❌ 同一份 binary | **隨身碟 `config/*.json`** |

判準：**換了它需不需要重燒 firmware？** 需要 → L1/L2；不需要 → L3。

---

## 0.5 三層預設：內建最小 → 板級出廠 → 用戶

**一句話：房東保證「就算用戶一個 config 都沒有，板子仍然開得機、收得到指令」；
config 只能管租戶。** 但「冇 config 都開得機」不等於「config 目錄可以不存在」——
使用者插上隨身碟要**看得到**該改哪個檔，所以每個層檔都必須存在，而且要 parse 得成功。

同一個層檔，內容可能來自三個地方（優先序由下往上）：

| 層 | 來源 | 誰決定 | 內容 | 什麼時候用 |
|---|---|---|---|---|
| ① **內建最小預設** | 編在 firmware 裡的常數字串 | 韌體 | 只有 `version` + 最少必需欄位（骨架） | 該層**完全沒有**任何來源時 |
| ② **板級出廠預置** | `ports/project/<chip>/<board>/data/config/<layer>.json`（build 時 `EMBED_FILES`） | 這塊板子 | 實際值：腳位、PHY、IP、任務清單 | 檔案不存在、而且這塊板有出廠檔 |
| ③ **用戶檔** | 隨身碟上的 `/data/config/<layer>.json` | 使用者 | 使用者改過的值 | 永遠最高優先（存在就不動） |

**建立規則**（`seed_file()`，`main.c`）：

```text
開機 → 掛上 FAT → 對每一個層檔：
    檔案存在？ ── 是 ──► 不動它（印「預置已存在」）
        │
        否
        ▼
    這個板有出廠預置？ ── 有 ──► 寫入板級預置
        │
        否
        ▼
    寫入內建最小預設（保證檔案一定存在）
```

三條不變量：

1. **只建立、不覆蓋**：檔案存在就永遠不動（連 `version` 都不改）。使用者刪掉 → 下次開機重建。
2. **建立後行為與「冇檔」一致**：內建最小預設的內容 = 「全部用程式預設」，
   所以「第一次開機」和「刪掉檔案」跑出來的行為一樣，不會出現「重開機才生效」。
3. **核心不讀 config 也能開機**：`nc4` / `net` / `core` / NCM 引擎都是 `NC_TASK_SYSTEM`，
   照 C 宣告跑；`tasks.json` 只影響租戶。所以 ③ 全冇、② 也冇，板子仍然收得到指令。

**2026-11-13 更新：出廠形狀由晶片能力決定。**
`nc_cfg_defaults_system()` 不再是一份固定常數，而是由 `nc_cfg_chip.c`（IDF `soc_caps.h` 的事實表）
砌出來：有 USB-Serial-JTAG 的板子（S3／P4／C3…）預設 `usb_jtag`、只有 OTG 的 S2 才預設 `CDC`、
`usb.msc` 預設 0（CDC 先；**2026-11-17 起交碟開關搬到 per-link** `links[].msc`，global 已停用）、USB parser 4,128 B。`app_main` 第一行就把
`CONFIG_IDF_TARGET` 交給預設，所以「第一次開機」與「config 壞掉」跑出來的形狀都對得上晶片。
詳見 [usb-config.md](usb-config.md) §0.5。

**「冇登記就唔佔」是第 ③ 層的規則**：`links[]` 冇這個名 → 唔宣告緩衝、唔建 bus；
CDC／MSC／log 三樣都冇人要 → 連 TinyUSB 都唔裝。以前缺名會當成啟用，
把一條線從 `links[]` 刪掉記憶體照樣佔住 —— 2026-11-13 已改（`main.c` 的 `link_enabled()`）。

> ✅ **2026-11-16 已補第 ① 層**：`main.c` 有 `k_min_system`／`k_min_network`／`k_min_tasks`
> 三個最小骨架字串；`seed_presets()` 在「冇板級預置」時用它們，所以 `config/` 下四個檔
> 一定會存在。`hardware` 由 `ncportgen` 的 `preset_hardware.json` 保證；`driver.json`
> 仍然是 build-time 資料，runtime 未讀。

---

## 0.6 名稱對齊：Interface／Hardware（2026-11-17 提案）

你講的「Network 改叫 Interface、所有硬件線路註冊進去」方向是對的，但要先分清楚
**哪個 Network**、以及**哪些東西根本不能在 runtime 改**。

| 現在 | 建議名字 | 內容 | 能不能 runtime 改 |
|---|---|---|---|
| `hardware.json` | **Interface／Hardware** | 實體接口：GPIO／PWM／UART／SPI／I2C／ETH／SDIO／USB | 部分（見下表） |
| `system.json` 的 `nc4.links` | **Channels**（或保留 links） | 邏輯通道：NC4 over USB／UART／TCP／WS… | ✅ |
| `system.json` 的 `nc4.routes` | **Routes** | 幀去 decodeN／轉發 | ✅ |
| `network.json` | **Connectivity**（不是 Interface） | WiFi STA/AP、hostname、credentials | ✅ |
| `tasks.json` | Tasks | 任務 level／affinity／prio（1–6，可選） | ✅ |

> **`system.json` 的 `links[].rx` 正常唔需要寫**：固件預設已是量測最優
> （[net-results §6](net-results-2026-11-15.md)）。只有換線路參數／RAM 壓力／
> 未量過的新 transport 才覆寫；改之前先用 `STATUS keys=net` 量。
| `wiring.json` | **F.wiring**（build） | 板級腳位／匯流排預設 | ❌ 要重 build |
| `board.json`＋`sdkconfig.defaults` | **F.spec／F.build**（build） | flash／PSRAM／CPU／console／OTG／HS／分割表 | ❌ 要重 build |

**不要把 `network.json` 改名做 Interface**：它是 WiFi 連線憑證與 policy，不是硬件接口。
「Interface」留給 `hardware.json`（實體接口）比較準。

### 不是所有「硬件線路」現在都可 runtime 改

| 接口 | 現在狀態 | 寫哪裡 |
|---|---|---|
| PIN（GPIO） | ✅ typed | `hardware.json` 的 `PIN` |
| PWM | ✅ typed | `PWM` |
| UART | ✅ typed pins | `UART`（NC4 使用另外在 `system.json` links） |
| ETH | ✅ typed enable／pins | `ETH` |
| SDIO | ✅ typed | `SDIO` |
| SPI／I2C／I80／I2S／ENC／XL9555 | 🟡 只係 generated seed，未 typed | 改了**不會生效**，等 driver |
| USB | 🟡 link 在 `system.json`；`msc`／`cdc_log` 在 `usb` 段；port 能力在 build | 見 [usb-layers.md](usb-layers.md) |
| flash／PSRAM／CPU／console／分割表 | ❌ build-time | `board.json`／`sdkconfig.defaults` |

所以「把所有硬件線路註冊進 Interface」不是改個名就完成：**每個 driver 要先有 typed struct**
（PIN／PWM／UART／ETH／SDIO 已有；SPI／I2C／I80／I2S 未有），USB 也要先有 port registry。
改名本身只係文件／schema 的整理，唔會令未 typed 的段生效。

### USB 在 Interface 裡應該長什麼樣（目標，未實作）

- `hardware.json` 的 `USB` 段只放**用戶選擇**：`enable`、personality 偏好（`usj`／`otg`）、
  `msc`／`log` 這些 consumer 開關；**不是** `functions` 配對表。
- port 能力（有幾條 PHY、USJ／OTG、FS／HS）由 build／板子註冊，不由用戶填。
- 真正 personality 由 claim 推導：有 MSC claim → OTG；否則有 USJ 用 USJ。
- 現在 interim 仍然係 `system.json` 的 `links[].kind`（＋per-link `msc`）／global `usb.cdc_log`。

### 0.7 Network 只 claim 硬件（2026-11-17 修正）

以前 `network.json` 的 `lan` 段重複寫了一次硬件：

```jsonc
"lan": { "driver": "RMII", "phy_type": "IP101", "phy_addr": 1,
         "gpio": { "mdc": 31, "mdio": 52, "ref_clk": 50 } }   // 已移除
```

這四個其實是**硬件事實**，而且 `wiring.json` → generated `hardware.json` 已經有；
runtime code 讀的是 `hardware.json` 的 `ETH`，`network.json` 的 `lan.gpio` 從來冇人讀。
重複寫一份只會出現「改了 network 但冇效果」的假象。

現在改成：

```jsonc
// hardware.json（硬件註冊；由 wiring 生成，用戶可覆蓋）
"ETH": { "enable": 1, "mdc": 31, "mdio": 52, "ref_clk": 50,
         "phy_type": "IP101", "phy_addr": 1 }

// network.json（只 claim／用，不再寫 GPIO／PHY）
"lan": { "enable": 1, "ip": { "mode": "static", ... } }
```

- **Hardware** 註冊：這塊板有哪個接口、腳位在哪、PHY 型號／位址。
- **Network** claim：要不要用這條有線、IP 怎樣拿；硬件不存在／`ETH.enable=0` 就唔用。
- 同 Router 一樣：Network 應該由 hardware capability **自動識別**，不是自己再抄一份腳位。
- **SPI 接 PHY**（W5500 之類）亦一樣：Hardware 的 `ETH` 段將來的 `driver:"SPI"`＋
  `spi/cs/rst/int` 負責註冊；Network 只 claim。現在 parser 認得這些 key 但未 typed，
  所以 SPI PHY 未可用。
- 未做：`lan.enable` 的 claim 未真正檢查 hardware（現時 `hardware.json` 冇 ETH 段仍會
  用編譯期內建腳位）；目標是 claim 失敗就唔起，不再 fallback。

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
【build-time，開發者；唔會出現在 USB】
ports/
  hardware/<chip>/<board>/
    board.json               ← 不可變事實（chip/flash/PSRAM/console/features）
    wiring.json              ← 板級 pins/buses（ncportgen 編成 nc_port.c，零 runtime 解析）
    driver.json              ← PHY/DMA/TFT/USB 參數（build-time，runtime 未讀）
    sdkconfig.defaults       ← IDF build config（L1 + 硬體能力碎片）
    partitions.csv           ← 這塊 flash 的分割表（可選）
  project/<chip>/<board>/
    project.json             ← 項目選擇（hardware 指回哪塊板 / seed）
    data/config/             ← 只放給用戶的 seed（hardware/system/network/tasks）
    （**冇 sdkconfig.defaults**：project 只放 runtime 設定，唔參與 build）

【runtime，USB 只看見這個】
/data/
├─ config/
│  ├─ hardware.json          完整硬體預設（generated seed；沒寫的欄位回 nc_port.c）
│  ├─ system.json            身份、開機、NC4 通道與解碼路線
│  ├─ network.json           IP / mask / gw / DHCP
│  ├─ tasks.json             任務 core / prio / stack 覆蓋
│  └─ resources.json         外部任務資源表格（誰提供什麼、誰需要什麼）
└─ pixel/                    ★ 效果自己的樹（見 §5.1）
```

| 檔案 | 擁有者 | 讀的時機 | 缺檔 / 壞檔 |
|---|---|---|---|
| `wiring.json`（build-time） | 硬體層 | **build 時**（`ncportgen` → `nc_port.c`） | 用 `k_hw` 內建預設 |
| `hardware.json`（user） | 硬體層 | **硬體 init 之前**（覆蓋 generated 預設） | 用 generated 預設 |
| `driver.json`（build-time） | driver 層 | 未接 | 用 driver 內建預設 |
| `system.json` | 系統層（main） | **硬體起來之後**（分配要看實際硬體） | 只啟動硬體實際支援的通道 |
| `network.json` | 網路層 | ETH/WiFi 起來前 | 用內建 IP 或 DHCP |
| `tasks.json` | 任務管理器 | 任務登記前 | level／affinity；`prio`（1–6，可選）覆寫 loop 宣告值；`level=-1`=停用 |
| `resources.json` | ncm / 資源表 | 掃描之後、載入之前 | 用固件預設（空表） |
| `pixel/`（目錄） | ncm | 掃描 `pixel/effects/` | 不啟用效果 |

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
② hardware.json → 硬體 init（GPIO/SPI/I2C/UART/ETH PHY；缺檔用 generated `nc_port.c`）
③ driver.json   → driver init（ETH DMA、USB、TFT…）
④ 硬體起來了，讀分配：
     system.json  → nc4.links[] / routes[]（引擎由 routes 推導，不寫 engines[]）
     network.json → IP
     tasks.json   → 任務覆蓋
⑤ 宣告（記帳）  看②③的實際結果 + ④的分配
⑥ 資源 carve    nc_plan_commit(當下可用量)
⑦ NC4 通道啟動  建 transport → 建 bus → 掛 decode 引擎
⑧ 任務啟動     nc_task_start_all() → nc_task_boot() 依 layer
⑨ pixel/（掃 `pixel/effects/*.nce`，attach 時才載入）
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
      { "name": "usb0", "kind": "usb", "index": 0, "msc": 1 },
      { "kind": "uart", "index": 0 },
      { "kind": "eth_tcp", "index": 0, "port": 6000 }
    ],
    "routes": [                                 // 誰解、要不要轉出去（router 永遠生效）
      { "in": "nctx",     "out": ["decode0"] },   // 本機 → 自己
      { "in": "outbound", "out": [] },            // 本機 → 外
      { "in": "usb0",     "out": ["decode0"] },
      { "in": "uart0",    "out": ["decode1"] },
      { "in": "eth_tcp0", "out": ["decode0", "uart0"] }
    ]
  }
}
```

**Link schema（canonical，2026-11-16 晚間定案）**：

- `kind` 是 composite：`eth_tcp` / `wifi_tcp` / `eth_udp` / `wifi_udp` / `eth_ws` / `wifi_ws`，
  實體線是 `uart` / `usb`（硬件類別，載入時解析成 `cdc`／`usb_jtag`；舊檔可直接寫 `cdc`／`usb_jtag`）；
  **interface 已包含在 kind，不再寫 `iface`**。
- `index` 是同一 kind 的編號；line identity = `kind + index`。
- `port` 只在 link 頂層寫一次；`rx` 也直接在 link 下面（有量測偏離時才寫，見 net-lines §5）。
- 冇寫 `name` 時，由 `kind + index` 生成（`eth_tcp0`、`wifi_udp1`…）；`name` 可以覆寫。
- `service` 不是 link 欄位；claim 由 kind／port 決定。
- **舊格式仍然接受**：`kind:"tcp", iface:"eth"` 會合成 `eth_tcp`；`cfg:{port,rx}` 亦會讀，
  但 `python3 tools/board.py check` 會提醒。

`routes[].out` 同時接受**通道**（轉發）與 **decode 引擎**（本地執行），跟舊專案的
`out: ["self"]` 同一個語意（新專案的本機來源叫 `nctx`）；`decodeN` 有人指向才開（N = 核心編號），**不需要宣告引擎**。
`ncmN` 由 `.nct` / `.nce` manifest 的 `core` 推導，也不能放進 `out`（它不吃幀）。
完整規則見 [routing.md](routing.md)。

> **現況（已接）**：開機 ④ 真的會讀 `routes[]`，每行**第一個** `decodeN` 決定那條線由第 N 核解；
> `decode0` / `decode1` 任務的存在與否、以及 CDC / UART / TCP 三條 bus 掛哪一核，都由此推導。
> 讀不到有效 `routes[]` 就退回板子預設表並印警告。`0x1105 ROUTE_GET` 可線上查實際生效的表。
> **未接**：`out` 裡的 link（轉發）仍然被忽略；兩核仍然共用同一個 decoder。

#### `links[].enable`：通道存在與否（2026-11-04）

`enable` 缺省 = 1（向後兼容）。開機讀一次之後就決定這條線存不存在：

- **`0`**：不宣告緩衝（plan 裡面 0 B）、不建立 bus、`routes[]` 提到它那行直接跳過。
  所以也不會因為一條死線而把某個 decode 核「撐」起來。
- **中途要開啟**：改 config ＋ 重啟。plan 的宣告窗口在 `commit` 之後關閉，
  runtime 沒有「後補宣告」的路徑 —— 這正是避免碎片與隱形佔用的設計。
- **現況**：`CDC` / `UART` / `TCP` / `UDP` 都跟它。`WS` 暫時不提供關閉，
  因為 `nc4_ws_start()` 同時 host 住 `/config.json` 的 HTTP server；
  要拆開之後才可以獨立關。
- **P4 特例**：本板沒有 USB OTG，所以 `CDC` 寫 `enable: 0`；
  以前照樣宣告 8,205 B 的 parser 緩衝，永遠沒有人讀。

### `hardware.json`（硬體層）

```jsonc
{
  "version": 1,
  "_gpio_convention": "（見下）",

  "ETH":  { "enable": 1, "driver": "rmii",   // rmii（現在）| spi（W5500 類，未實作）
            "phy_type": "IP101", "phy_addr": 1,
            "GPIO": { "mdc": 31, "mdio": 52, "ref_clk": 50,
                      "spi": -1, "cs": -1, "rst": -1, "int": -1 } },
  "USB":  { "enable": 1, "_comment": "S3 USB D-/D+ = GPIO19/20；phy 在 usj-fs / otg-fs 二選一",
              "list": [ { "phy": "otg-fs", "enable": 1,
                          "GPIO": { "dm": 19, "dp": 20 } } ] },
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

  "USB":    { "enable": 1,
              "list": [ { "phy": "usj-fs", "GPIO": { "dm": 19, "dp": 20 }, "enable": 1 } ] },
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

  // GPIO／PHY（driver／phy_type／phy_addr）在 hardware.json 的 ETH 段；
  // Network 只 claim／用，不重複硬件。
  "lan": {
    "enable": 1,
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
- **Wi-Fi** 用 `CONFIG_NETCORE_WIFI`（`firmware/main/Kconfig.projbuild`，預設 **n**）編進 image。
  未編入時 `wifi.enable: 1` 只會在 log 說「未編入」，而**靜態與 flash 都是 0**。
  編入但 `enable: 0` 仍然要付 **48,992 B 靜態 ＋ 548,768 B flash**（S3 實測），
  所以預設關閉 —— 兩道閘要同時開才會真的用 Wi-Fi。
- BT / ESP-NOW 的 build-time 能力（`ports/_profiles/*.defaults`）尚未建立；
  將來照同一個模式（Kconfig ＋ runtime `enable`）做。
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
├─ effects/          效果檔 *.nce（載入器掃這個目錄）
├─ map/              像素映射（實體排列 → 邏輯索引）
└─ modes/            模式 / 預設（開機跑哪個、參數）
```

規則：

- **檔案系統就是註冊表**：`ls pixel/effects/` 就知道有哪些效果，沒有另一張總表。
- 每個 `.nct` / `.nce` 自帶 manifest（kind / core / fps / px / cmds），載入器驗它，不靠外部設定。
- `map/`、`modes/` 的 schema 等效果引擎落地再定；現在只載入 + 驗證。
- **UI 暫時不做**（本來要放 `ui.json`，取消）。

>`pixel/` 的檔案由使用者用隨身碟管理；firmware 只讀，`ASSET_RELOAD` 才重新掃描。

---

## 6. 程式對應

| 東西 | 位置 | 狀態 |
|---|---|---|
| `nc_cfg_open/root/version/close` | `firmware/components/sys/config/` | ❌ 還沒有 |
| 各層自己的 parser | 各 component（`sys/store`、`sys/transport`、`main`） | ❌ 還沒有 |
| 出廠預置（每個檔一份） | `ports/project/<chip>/<board>/data/config/*.json` | ❌ 現在只有一個 `config.json`（321 B，只有 System + Network.lan） |
| 效果樹 | `/data/pixel/`（`effects/`、`map/`、`modes/`） | 🟡 載入器掃 `pixel/effects/`（路徑本輪改） |
| NVS 覆蓋 | `net/rx_core`、`net/rx_split`、`cid` … | ✅ 已有（優先於 config） |

**優先序**：NVS（執行期指令寫的）> `config/*.json`（使用者編的）> 程式預設。

---

## 7. 遷移步驟

| 步 | 做什麼 | 驗收 |
|---|---|---|
| C0 | 建 `sys/config`：開檔 → cJSON → 關檔；缺檔/壞檔回 NULL | host test：三種檔案狀態 |
| C1 | `system.json` 的 `nc4.links` 先接（唯讀）；`routes[]` 的 `in → decodeN` 已生效（決定哪一核解、哪一核存在），`out` 的 link 轉發未做 | 改 `slots` 重開機生效；改 `routes[].in` 的 `decodeN` 會換核 |
| C2 | `hardware.json` / `driver.json` 接掉 `main.c` 的寫死值 | 🟡 ETH 已完成（2026-11-08，含 `enable`）；SDIO 早前已完成；其餘段與 `driver.json` 未做 |
| C3 | `network.json` / `tasks.json` | IP 與 core 可由檔案覆蓋 |
| C4 | 出廠預置改成多檔（`ports/project/<board>/data/config/`） | 新板首次開機長出 `config/` 目錄 |
| C5 | `pixel/` 樹（`effects/`、`map/`、`modes/`） | 拖拉 `.nce` 進 `pixel/effects/` 就生效 |

---

## 8. 一個原則

**每一層只讀自己的檔案、只用自己懂的欄位、缺檔就用預設。**
中央不解析、不轉發、不幫別人驗證 —— 這樣任何一個檔壞掉，都只是那一層退化。
