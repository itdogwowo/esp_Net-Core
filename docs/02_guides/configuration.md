# 配置實務：一個設定放哪、怎樣生效、怎樣驗證

> **用途**：你要改一個東西（開 WiFi、改燈數、換腳位、調幀率）時，這一頁回答四件事：
> **放邊個檔 → 要 rebuild 定 reboot → 開機邊一步讀它 → 用咩驗證**。
> 設計理由見 [config-layers.md](../03_notes/config-layers.md)；極限與代價見
> [pixel-renderer.md §13](../03_notes/pixel-renderer.md)。
> **可以設定什麼、範圍幾多、三種設定路徑** → [config-schema.md](config-schema.md)；
> 開新 project 的起點 → [ports/presets/](../../ports/presets/README.md)。

---

## 0. 一句話分層

| 你要改的東西 | 放哪 | 生效方式 |
|---|---|---|
| flash／PSRAM／CPU／console／分割表／target | `ports/hardware/<chip>/<board>/{board.json,sdkconfig.defaults}` | **rebuild** |
| 板上有冇 ETH／WiFi 協處理器 | `board.json` 的 `features` / `profiles` | **rebuild** |
| 某個 project 要不要把 WiFi 編進去 | `ports/project/<chip>/<board>/sdkconfig.defaults` | **rebuild（`--fresh`）** |
| 燈的 lane 數／顆數／腳位／時序 | `firmware/main/Kconfig.projbuild` 的 `NETCORE_PIXEL_*`（通常寫在 project 的 `sdkconfig.defaults`） | **rebuild（`--fresh`）** |
| 幀率、任務開關／核、通道、IP、SSID | `/data/config/*.json`（repo 的 `ports/project/.../data/config/` 是出廠版） | **reboot** |
| 效果／亮度／顏色／位深 | `0xF060` 指令（執行期） | **即時** |

**判準：換了它要不要重燒 firmware？** 要 → build 層；不要 → runtime 層。

---

## 1. 開機到底讀了什麼（B01–B11）

| 階段 | 名字 | 做什麼（與配置有關） |
|---|---|---|
| B01 | `banner+nvs` | NVS 起來（cID、OTA 旗標） |
| B02 | `storage+cfg` | 掛 FAT；**缺的 config 檔由出廠預置／最小骨架補**；讀 `hardware/system/network/tasks/resources.json` |
| B03 | `capability` | 晶片能力（核數、USB 形態） |
| B04 | `hwcheck` | 檢查 `hardware.json` 的資源與 claim |
| B05 | `hardware` | 依 `hardware.json` 起 GPIO／PWM／ETH…（腳位是 init 的輸入） |
| B06 | `declare` | 依 `system.json` 宣告通道緩衝；每個任務宣告自己要多少（render 宣告環與各 lane 的尺寸） |
| B07 | `commit` | 一次 carve（記憶體帳） |
| B08 | `start` | 任務依 `tasks.json` 的 level／affinity 開 loop；**`on_start` 才真正建環與後端**（claim I80 腳位、配 3 個 DMA 槽）、`render` 讀 `frame_interval_ms` 起節拍 |
| B09 | `usb` | USB（MSC／CDC／USJ）依 `system.json` 的 links |
| B10 | `channels` | 建 TCP／UDP／WS／HTTP bus（`iface` 對不上就跳過） |
| B11 | `account` | 印剩餘記憶體 |

---

## 2. WiFi：兩道閘，兩個地方

### 2.1 build 閘（要不要把 esp_wifi 編進映像）

- **預設唔開**：Kconfig `NETCORE_WIFI` 預設 `n`。
- 要開：在 **project 的** `ports/project/<chip>/<board>/sdkconfig.defaults` 加

  ```ini
  CONFIG_NETCORE_WIFI=y
  ```

  然後 `python make.py build <board> --project <p> --fresh`（`--fresh` 必須：
  sdkconfig 已存在時 IDF 不會重新套用 defaults）。
- P4 沒有 radio：唔用這道閘，用 `board.json` 的 `profiles: ["wifi-hosted-c5"]`（或 `-c6`）。
- 代價（S3 實測）：**靜態 SRAM −54,180 B**（bss 19,248＋data 7,792＋IRAM 27,140）、
  **flash ~492 KB**；開機後 internal heap 由 **128,475 → 4,975 B**。

### 2.2 runtime 閘（開機要不要真的連）

`/data/config/network.json`：

```jsonc
"wifi": { "enable": 0, "mode": "sta", "ssid": "", "timeout_s": 60,
          "ip": { "mode": "dhcp" } }
```

- `enable: 0` → 唔呼叫 `esp_wifi_init`（但驅動仍佔靜態記憶體）。
- 憑證（`credentials[]` 的 ssid／pw）在 `nc_wifi.c` 會搬去 NVS；JSON 只留開關。

### 2.3 開機執行路徑

```
B02 讀 network.json
  └─ enable=1 且 CONFIG_NETCORE_WIFI=y？
       ├─ 是 → nc_wifi_init() → esp_netif_init() → 建 STA/AP → 連線
       └─ 否 → 印「Wi-Fi：未編入」或「未啟用」，不佔執行期記憶體
B06/B10 建 TCP/UDP/WS/HTTP 線
  └─ 每個 link 的 iface=wifi：冇編入 → **跳過並警告**（唔會 bind socket）
```

> ⚠️ **踩過的坑**：只關 build 閘、但 FAT 裡的 `network.json` 仍宣告 `iface=wifi`
> 的線 → 以前會照樣 bind → `assert: tcpip_send_msg_wait_sem (Invalid mbox)` →
> **每 0.6 秒重啟**。已修（`nc_net_lines.c` 在 `CONFIG_NETCORE_WIFI=n` 時跳過），
> 但換 build 時仍要記得：**config 裡的線要和 build 能力對得上**。

### 2.4 驗證

```bash
python tools/nc4.py --port COM20 status wifi   # 狀態、IP、RSSI
python tools/nc4.py --port COM20 status net    # 每條線的 frames／吞吐
python tools/nc4_log.py --port COM20           # log 看「未編入／未啟用／連接中」
```

---

## 3. LED（pixel）：build 決定形狀，runtime 只調節奏

### 3.1 build 層（`NETCORE_PIXEL_*`，Kconfig）

| 設定 | 意思 | 改完 |
|---|---|---|
| `NETCORE_PIXEL` | 要不要 pixel 子系統 | rebuild |
| `NETCORE_PIXEL_I80` / `_SPI` | 用哪個後端（clockless／時脈式） | rebuild |
| `_I80_BUS_WIDTH` | 8 或 16 bit（16 要 18 個腳） | rebuild |
| `_I80_LANES` | 一次送幾條（≤ bus_width，最大 16） | rebuild |
| `_I80_PINS` | 腳位字串（D0..D7/D15、WR、DC） | rebuild |
| `_LEDS` | 每條 lane 幾顆 | rebuild |
| `_PCLK_HZ` / `_BIT_CELLS` / `_T0H` / `_T1H` | 波形時序（預設 2.5 MHz×3） | rebuild |
| `_FORMAT` / `_ZONE_PX` / `_DITHER*` | 線材格式、抖動 | rebuild |
| `_CORE` | 渲染任務綁哪核（-1 = 不綁） | rebuild |

這些值**全部在編譯期**寫進 codec／driver；改了一定要 `--fresh` 重建。
沒有硬上限，但超過的代價見 [pixel-renderer.md §13](../03_notes/pixel-renderer.md)。

### 3.2 runtime 層（`/data/config/`）

| 檔 | 欄位 | 作用 |
|---|---|---|
| `system.json` | `frame_interval_ms`（預設 20，1–1000） | **render 任務的節拍**＝fps（20 ms → 50 fps） |
| `tasks.json` | `render` / `pixel` 的 `level`、`affinity` | `level=-1` 停用；`affinity:[0,0]` 唔跑 |
| `0xF060`（執行期） | op 1 效果、3 顏色、4 亮度、8 位深、13 demo… | 即時，唔使重啟 |

### 3.3 開機執行路徑

```
B06 declare：各任務宣告記憶體需求（render 宣告環 "i80.prd"、每 lane 的尺寸）
B07 commit ：一次 carve（環、槽、pack 緩衝；≤48 KB 走 internal，否則 PSRAM）
B08 start  ：render 的 on_start 建環＋後端（claim 腳位、配 3 個 DMA 槽）
              → 起 esp_timer（frame_interval_ms）→ 每拍：取環 → 編碼 → submit
             pixel 的 on_start 按名字找回環 → loop 自由填（唔管 fps）
```

所以：**render 控 FPS，pixel 唔控**；環滿時 pixel 每 1 ms 看一次，平均生產率自動等於渲染率。

### 3.4 驗證

```bash
python tools/nc4.py --port COM20 raw 0xF061 00   # render：fps/frames/enc_us/tx_us/frame_us/fill/fail
python tools/nc4.py --port COM20 raw 0xF060 00   # producer：eff/gain/fps_x10/gen/px
python tools/nc4.py --port COM20 status tasks    # render/prd 的 enabled／running
```

> ⚠️ **`tasks.json` 是 `enabled:0` 就冇燈**。換韌體時裝置會自動重建 FAT（presets
> 有開）；手動改過 config 的板就可能沿用舊檔 → 燈唔動，`0xF061` 全部 0。

---

## 4. 「我想改 X」對照表

| 想改 | 改邊 | 生效 | 驗證 |
|---|---|---|---|
| 開／關 WiFi | project `sdkconfig.defaults` ＋ `network.json` | rebuild ＋ reboot | `status wifi` |
| 換 WiFi SSID／密碼 | `network.json`（或 NVS） | reboot | `status wifi` |
| 燈由 6 條變 4 條 | `_I80_LANES`（project 或 `NC_EXTRA_DEFAULTS`） | rebuild --fresh | `0xF061` 的 `lanes` |
| 每條由 20 顆變 500 顆 | `_LEDS` | rebuild --fresh | `0xF061` 的 `px` |
| 幀率 50 → 40 fps | `system.json` `frame_interval_ms: 25` | reboot | `0xF061` 的 `fps`／`frame_us` |
| 渲染改跑 core 0 | `CONFIG_NETCORE_PIXEL_CORE=0` 或 `tasks.json` affinity | rebuild／reboot | `status tasks` |
| 效果／顏色／亮度 | `0xF060` op | 即時 | `0xF060` 回讀 |
| 換腳位 | `_I80_PINS` | rebuild --fresh | log 的 pin claim、`0xF061` |

**臨時覆寫（不動 `ports/`）**：`-DNC_EXTRA_DEFAULTS=<檔案.defaults>`，會追加在
hardware／project 之後（後者勝）。例：`artifacts/16x600.defaults`。

---

## 5. 改完之後的三步檢查

1. `python tools/board.py check` —— 0 error（硬體事實有沒有放錯層、project 有沒有寫 flash／PSRAM）。
2. 看開機 log 的 B01–B11 是不是全 `ok`，以及 `boot 完成：已具備 0x03ff`。
3. `status sys` 的 `h_int`／`h_ps` 與改動前比較（記憶體有沒有意外變化）。

---

## 6. 讀取語意：能力 → 項目預設 → 用戶微調；唔開唔佔

這是整個 config 系統的四條規則，實作位置一併列出（改之前先看這裡）。

### 6.1 順序

```
① 能力（build）      這塊晶片／這顆 build 有沒有這個東西？
      沒有 → config 寫 enable=1 也沒用（log 講清楚，唔會偷偷生效）
② 項目預設（embedded） ports/project/<chip>/<board>/data/config/*.json
      只在裝置上「沒有這個檔」時寫入
③ 用戶微調（FAT）     /data/config/*.json —— 存在就永遠不被覆蓋
```

| 規則 | 在哪實作 |
|---|---|
| 能力閘（build） | `CONFIG_NETCORE_WIFI`、`CONFIG_NETCORE_PIXEL*`、`SOC_*` 能力；未編入的線在 `nc_net_lines.c` 跳過 |
| 能力閘（runtime） | `iface=eth` 要 `hardware.json` 的 `ETH.enable=1`；`iface=wifi` 要 build 有編 —— 唔成立就**跳過並警告**，唔會照開 socket（冇 lwIP 會 assert） |
| 項目預設只建立不覆蓋 | `main.c` 的 `seed_file()`（印「預置已存在，不動它」） |
| 用戶檔最高優先 | 同上；firmware 只讀，只有「補值」例外 |
| 完全冇檔也有骨架 | `k_min_system` / `k_min_network` / `k_min_tasks` |

> **「編進來」vs「開啓」—— 兩筆完全不同的數**：
> - **編進來**（`CONFIG_NETCORE_WIFI=y`，build 閘）：app **+526 KB**、靜態 SRAM
>   **+54 KB**。**與 runtime 開關無關** —— 冇開也照付。
> - **開啓**（`network.json` 的 `enable=1` 且有名單）：才走去 `esp_netif_init()`／
>   `esp_wifi_init()`；`enable=0` 在 `nc_wifi.c:615-619` 就 return，**0 B runtime**。
> - 所以「省」有兩種省法：唔要這個能力 → 唔好編（省 526 KB flash ＋ 54 KB SRAM）；
>   要能力但今次唔用 → `enable:0`（省 runtime heap／stack，flash 同靜態照付）。

### 6.1b 加一個新的設定值（schema registry，2026-11-26）

設定值的 key 名單／型別／範圍**只有一份**：`schema/state-keys.json`。加／減一個值：

```bash
# 1) 改 schema json（加一行 key：id／name／unit／type／min／max／path）
# 2) 生成 C 表（三份檔，唔准手改）
python3 tools/gen_state_keys.py
# 3) 如果要在開機寫入，nc_state_boot.c 的 build_records() 加一行由 cfg 取值
# 4) rebuild（改過 sdkconfig.defaults 要 --fresh）
```

- 查／讀／寫一律經 `nc_state_schema.*`（`nc_state_key*`、驗範圍）同 `nc_state_api.*`
  （`nc_state_get_*`／`nc_state_set_*`），driver 唔再自己記 key 的型別。
- `status cfg` 的 `schema` 見到 `ver`／`crc`／`keys`／`units`／`bad`（drift）／`dirty`。
- 改 schema 之後舊 `state.ncst` 的未知 key 會自動跳過（向前兼容）；
  三份生成檔同 schema 不同步 → `run_all.sh` 直接失敗。
- 詳細契約：[`state-schema.md`](../03_notes/state-schema.md) §8。

### 6.2 「唔啟用就唔佔資源」— 四種開關

| 開關 | 關掉之後 | 驗證方式 |
|---|---|---|
| `links[].enable: 0` | 唔宣告 parser 緩衝（plan 0 B）、唔建 bus | `status net` 冇這條線 |
| `tasks.json` `level: -1` | 唔 declare、唔 on_start、唔開 loop | `status tasks` 冇這任務 |
| `tasks.json` `affinity: [0,0]` | **同上**（2026-11-19 起一致） | `status mem` 少掉它的資源 |
| `wifi.enable: 0` | 唔 `esp_wifi_init`（但 build 有編就仍有靜態 BSS） | `status wifi`、開機 log |

**實測（2026-11-19，SuperMini 6×20）**：把 `render` 改成 `[0,0]` 後

| | `[0,1]` | `[0,0]` |
|---|---:|---:|
| plan internal | **5,280 B** | **4,128 B**（`i80.prd`／`mock.1` 各 576 B 消失） |
| `status tasks` 的 render | `render@1 run=1` | **完全冇 loop** |
| 燈 | 50 fps | 唔動（也沒有環可寫） |

> 以前 `[0,0]` 只係「loop 唔開」，但仍然 declare ＋ `on_start`（環、後端照建、
> 記憶體照佔）。現在 `slot_will_run()`（`nc_task.c`）把 `level<0` 與 `(0,0)` 統一
> 當「唔跑」：**唔宣告、唔 gather、唔 on_start**。

### 6.3 一個常見誤會

改 `ports/project/.../data/config/*.json` 之後直接 OTA **不會**生效 ——
裝置上的檔案已存在，`seed_file` 只建立不覆蓋。要令新預設生效，二選一：

```bash
# A. 強制重建 FAT（會蓋掉裝置上的用戶設定）
python tools/nc4_file.py --port COM20 set '{"System":{"fs_format":1}}' 1
python tools/nc4.py --port COM20 reboot

# B. 只改一個檔（MSC 模式）—— 刪掉 /data/config/tasks.json 再重開機
```

（`fs_format` 的正確 payload 是 `u16 len + JSON + persist`；用 `set` 的 CLI 參數
時要注意 shell 的引號。）
