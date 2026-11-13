# 命名慣例（C 版）

> **用途**：看到符號或檔名，馬上知道它屬於哪一層、哪個子系統、是任務還是動作。
> **狀態**：2026-11-02 統一（舊的 `nc_tm_*` / `nc_res_*` / `ota_register` / `store_*` / `ws_nc4_*` 已全部改掉）。

---

## 0. 一句話

**`nc_<子系統>_<角色>`；同一個字同時有任務與動作時，用 `<域>_task` / `<域>_action` 後綴區分。**

---

## 1. 為什麼是後綴：`pixel_task` vs `pixel_action`

同一個「域」可以同時有兩種東西，而且未來一定會遇到（`pixel` 就是）：

| 東西 | 是什麼 | 名字 |
|---|---|---|
| **任務** | 一條 loop：收發、渲染、節拍；佔核心、佔 stack | `pixel_task` |
| **動作** | 一組指令：cmd → handler；跑在解碼核上 | `pixel_action` |

```text
components/tasks/pixel_task/
    nc_pixel_task.c            const nc_task_t nc_pixel_task;      ← 給 nc_task_register()
components/action/pixel_action/
    nc_pixel_action.c          nc_pixel_action_register(dec);      ← 開機註冊指令
```

**後綴而不是前綴**：`pixel` 是「做什麼」，`task` / `action` 是「它在系統裡的角色」；
先讀域、再讀角色，跟口語一致（「pixel 的 task」「pixel 的 action」）。
前綴會變成 `nc_task_pixel` / `nc_action_pixel`，兩個不同層的東西擠在同一個字首後面，
掃檔案時反而難分。

> 目前的 `nc4` / `fx` / `store` 還沒有這個問題（沒有同名域），所以維持
> `nc_task`、`nc_fx`、`nc_store`；一旦出現同名域，就照上面的後綴規則拆。

---

## 2. 前綴表（子系統）

| 層 | 子系統 | 前綴 | 檔案 |
|---|---|---|---|
| 協議 | NC4 | `nc4_` | `nc4.c` / `nc4_bus.c` |
| 記憶體 | plan / hub / 原語 | `nc_plan_` / `nc_hub_` / `nc_mem_` | 同名 `.c` |
| 任務 | 任務管理器 | `nc_task_` | `sys/task/nc_task_loop.c`（執行層）、`sys/task_manager/nc_task.c`（管理器）；型別在 `sys/task/include/nc_task.h` |
| FAT | store | `nc_store_` | `nc_store.c` |
| 通道 | WS / UDP / HTTP | `nc4_ws_` / `nc4_udp_` / `nc4_http_` | `nc4_ws.c` … |
| 動作 | 域 | `nc_<域>_action_` | `nc_ota_action.c` / `nc_file_action.c` |
| 效果 | fx | `nc_fx_` | `nc_fx.c` / `nc_fx_manifest.c` |

## 3. 四條規則

1. **不要兩套前綴。** 以前任務層同時有 `nc_tm_*` 與 `nc_task_*`，現在全部 `nc_task_*`。
2. **檔名 = 前綴 + 角色。** `nc_task.c` 是政策、`nc_task_loop.c` 是執行層、
   `nc_ota_action.c` 是 OTA 的動作；看到檔名就知道它負責哪一段。
3. **型別 = `nc_<子系統>_t`；列舉值 = `NC_<子系統>_...`。**
   例：`nc_task_kind_t` / `NC_TASK_SYSTEM`；`nc_store_mode_t` / `NC_STORE_MODE_APP`。
4. **動作只有一個入口。** 一個域一個元件，對外只有
   `void nc_<域>_action_register(nc4_decoder_t *decoder)`；handler 命名 `on_<動詞>`。

---

## 4. 新舊對照（查舊 commit / 舊文件用）

| 舊 | 新 |
|---|---|
| `nc_tm_reset` / `nc_tm_json` / `nc_tm_boot` … | `nc_task_reset` / `nc_task_json` / `nc_task_boot` … |
| `nc_res_req_t` / `nc_res_view_t` | `nc_task_res_t` / `nc_task_res_view_t` |
| `nc_loop_rt_*`（內部） | `nc_task_rt_*` |
| `ota_register()` / `ota_cmds.c` | `nc_ota_action_register()` / `nc_ota_action.c` |
| `file_cmds_register()` / `file_cmds.c` | `nc_file_action_register()` / `nc_file_action.c` |
| `nc_action_ota_*`（上一版） | `nc_ota_action_*`（後綴版） |
| `store_init` / `store_mode` … | `nc_store_init` / `nc_store_mode` … |
| `ws_nc4_start` / `udp_nc4_start` / `http_cfg_register` | `nc4_ws_start` / `nc4_udp_start` / `nc4_http_register` |
| `nc_tm.c`（管理器） | `nc_task.c` |
| `nc_task.c`（舊執行層） | `nc_task_loop.c` |

---

## 5. 術語表：`fx` 這個字已經太混亂，統一講法

`fx` 一個字同時指容器、元件、任務、任務種類 —— 四樣東西，所以一律改用下面的講法。
**完整定義看 [fxe-engine.md](fxe-engine.md)** —— 這裡只列名字。

| 講法 | 指什麼 | 在哪 | 屬於 |
|---|---|---|---|
| **FXE 引擎** | 延後綁定：從檔案載入模組、給記憶體、登記成任務、每 tick 回呼、超預算就停用 | `components/tasks/test_fx_tasks/`（`nc_fx.c`） | **房東**（`NC_TASK_SYSTEM`，插件機制本身） |
| **FXE 模組** | 一個 `.fxe` 載入後的實例（name / payload / decl / 自己的記憶體） | arena 裡 | **租戶**（attach 時 `kind` 由 manifest 決定） |
| **FXE 容器** | `.fxe` 檔格式：48 B 檔頭 + manifest + payload | `tools/`、`nc_fx_manifest.c` | wire format，**唔改名** |
| **FXE arena** | 引擎向 plan 宣告的那塊記憶體（**大小是宣告值，不是上限**） | `nc_fx_init()` | 房東 |
| **房東 / 租戶** | `NC_TASK_SYSTEM` / `NC_TASK_NORMAL` + `NC_TASK_EFFECT` | `nc_task.h` | — |
| **`level` / `layer`** | 任務的次序 / loop 的次序 | `nc_task_t` / `nc_task_loop_t` | 見 [task-lifecycle.md](task-lifecycle.md) §4 |
| **`DATA`** | 幀裡 `LEN` 之後、`CRC32` 之前的那段位元組 | 協議欄位 | 見 [frame.md](../01_protocol/frame.md) §1 |
| **回覆緩衝** | 固件組回覆 JSON 用的暫存（**不是協議的一部分**） | `main.c` | 實作細節，每個執行者一份 |

> 協議的欄位一律照上游寫：`SOF` / `VER` / `ADDR` / `CMD` / `LEN` / `DATA` / `CRC32`。
> **不寫 `plen`**（上游是 `len`），**不把回覆緩衝叫 payload**。完整表見
> [frame.md](../01_protocol/frame.md) §5。

四條規則：

1. **寫文件時不要只寫「fx」**：寫「FXE 引擎」或「FXE 模組」，讓讀者知道是哪一層。
2. **`.fxe` 是檔名，不改**（改了工具鏈、manifest、既有檔案全斷）。
3. **FXE 引擎不綁核**：它自己沒有 loop；每個模組跑哪一核由 manifest / `tasks.json` 決定。
   「FXE 引擎跑 core1」是錯的說法 —— 跑 core1 的是某個模組的 loop。
4. **描述引擎只用中性動詞**：載入 / 卸載 / 啟用 / 停用 / tick。引擎不知道模組在裡面做什麼。

> ⚠️ **現況 vs 目標**：現在 `fx` 這一個任務同時做兩件事 —— 引擎（房東）
> **和** 每 tick 回呼的 loop（`NC_FX_TASK_CORE 1`、prio 3、layer 3）。
> 上面那張表是**目標切法**：引擎留房東、tick loop 降成租戶 ——
> 因為「每 tick 要做事」是功能，不是開機必需。在那之前，講「FXE 引擎」時指的是**引擎那部分**。

---

## 6. 核心任務名 vs 上游（`mp_Net-Core/slave/lib/sys/`）

上游的模組名就是「這件事叫什麼」的權威。**能同名就同名**，不要自己發明。

| 我們（C） | 上游（MicroPython） | 職責 | 建議 |
|---|---|---|---|
| `nc4` | `proto.py`（framing + CRC）+ `dispatch.py`（指令表）+ `tasks/bus_decode.py`（解碼迴圈） | 解碼 → 分派 → 兩核 decode loop | **保留 `nc4`**（協議名，上游也叫 NC4） |
| `net` | `net_bus.py`（`class NetBus`：「純傳輸層 TCP/WS/UDP」） | 傳輸收發 + 每條通道的 RX 緩衝 | **改名 `net_bus`**（同上游；`net` 太窄又太寬） |
| `core` | （冇對應） | 慢工作佇列 + 一個 worker | 上游冇這個東西 → 自創名，建議 `defer`（講職責） |
| `fx` | （冇單一對應） | 引擎 + 每 tick 的 loop | **拆**：`fxe`（引擎，房東）／tick loop（租戶） |
| `probe` | `tasks/hw_sample_task.py`（性質接近） | 開機自測 | 保留（自測用，不入 config） |

兩點要注意：

1. **`net_bus` 是 snake_case，不是 `netBus`。** 上游的 class 叫 `NetBus`，但 module 叫
   `net_bus.py`；C 這邊的符號一律 snake_case（`nc4_bus_add`），所以任務名寫 `net_bus`。
2. **上游的 `NetBus` 明確是「純傳輸層」**：它不驗 CRC（CRC 在 `proto.py` 的 `StreamParser`，
   而 `StreamParser` 是**每條 bus 一個實例**）。我們現在 `nc4.c` 的 `nc4_parser_*`
   對應 `proto.StreamParser`、`nc4_bus.c` 對應 `NetBus` + `AtomicStreamHub` —— **已經同構**，
   不需要把 CRC 搬去 bus。

---

## 7. 講嘢用的 ID（B／T／R／L／C／M）

> 前面幾節講的是「**寫程式時**符號叫乜」。這一節講「**講嘢時**點樣指住一樣東西」——
> 開會、issue、commit message、log、圖，全部用同一套 token。

### 7.1 問題：一個字有兩三個意思

「`storage+cfg`」係**一個開機階段**，定係 storage 同 cfg **兩個功能**？同樣：

| 講法 | 可能係 | 結果 |
|---|---|---|
| `storage+cfg` | 開機階段 B02／或者 store 同 config 兩個模塊 | 講咗五分鐘之後才發現講緊唔同東西 |
| `channel` | bus 通道／NC4 的線（link）／HTTP 的連接 | 同上 |
| `decode0` | 任務 T／decoder 物件／核心編號 | 同上 |
| `pool` | B06 階段／nc4.pool（已停用）／heap 池 | 同上 |

C 名字（`nc_boot_*`、`nc4_bus_*`）解決了「程式裡叫乜」，但**講嘢**仍然會撞。
所以每個「可以指住講嘅東西」有一個 ID，而且**一定要帶前綴**。

### 7.2 六個命名空間

| 前綴 | 指乜 | 權威來源（唯一真相） | 例子 |
|---|---|---|---|
| `B01`–`B12` | **開機階段**（整輩子一次，順序） | `main.c` 的 `stages[]`、`boot.h` 的契約 | `B02 storage+cfg` |
| `T.` | **任務**（有棧、有核、有 loop） | `nc_task_def.name`／`xTaskCreate` 的名 | `T.decode0`、`T.cdc.rx` |
| `R.` | **plan 資源／緩衝**（宣告→carve） | `nc_task_res_t.name` | `R.nc4.rx`、`R.nc4.reply1` |
| `L.` | **通道／線**（link 名） | `nc4_link_register()` 的名字 | `L.CDC`、`L.UART` |
| `C.` | **協議指令** | `0x11xx` 指令表 | `C.0x1101` |
| `M.` | **模塊／功能**（component 或一組行為） | component 目錄名 | `M.store`、`M.config` |

`T.`／`R.`／`L.`／`C.`／`M.` 只是**講嘢用的前綴**，C 符號本身不改名 ——
`decode0`、`nc4.rx`、`CDC` 這些字串本來就是穩定的，加了前綴反而多一層要對。

### 7.3 B 對照表（log 直接查）

| ID | name | 實測 | 一句話 |
|---|---|---|---|
| `B01` | banner+nvs | 9.66 ms | 橫幅、`nvs_flash_init`、讀開機計數 |
| `B02` | storage+cfg | 126.92 ms | 掛 FAT／WL、寫 seed、config 三層解析 |
| `B03` | capability | n/a | 探核數；決定 USB 模式／link 名（只寫 `nc_boot_ctx` 五個欄位） |
| `B04` | hwcheck | n/a | `hardware.json` 的 claim 對帳 |
| `B05` | hardware | 0.33 ms | ETH／SD／UART 腳位；收下路由表 |
| `B06` | pool | n/a | 舊 `nc4.pool` 停用的位置（唔配記憶體） |
| `B07` | declare | 10.45 ms | 向 plan 宣告要幾多（未 malloc） |
| `B08` | commit | 0.76 ms | plan carve（緩衝真正落地）；`nc_wifi_boot()` |
| `B09` | start | 5.39 ms | 建 decode0／1 任務（entry 停在閘前） |
| `B10` | usb | 52.01 ms | TinyUSB 列舉；`nc_log_start()` |
| `B11` | channels | 1.81 ms | 建 bus／producer；`nc_task_boot()` 放行 |
| `B12` | account | 0.19 ms | 印「開機剩餘」，之後只剩心跳 |

`n/a` ＝ 該階段是量測之後才拆出來的（原本混在別人的時間裡），**不是 0.0 ms**。
來源見 [status.md](../00_pm/status.md) §7.23。

### 7.4 三句唔同意思（同一件事的三層）

| 講法 | 意思 | 去邊度查 |
|---|---|---|
| 「`B02` 慢」 | 開機第二階段慢（掛 FAT ＋讀 config ＝ 126.9 ms）→ 開機次序問題 | log `boot[B02] … 126.92 ms` |
| 「`M.store` 慢」 | store 模塊本身慢（同開機次序無關，可能係 FAT 同步/flush） | `components/sys/store/` |
| 「`R.nc4.rx` 唔夠」 | plan 派的 4,192 B parser 環太小 → 是**容量**問題，唔關階段事 | `nc_plan_json`／STATUS |

呢三句以前都會講成「storage 慢」，然後三個人去改三個唔同嘅地方。

### 7.5 規矩

1. **講嘢帶前綴。**「`B11` 之後 core1 才有 loop」唔會有人誤會；「channels 之後」會。
2 . **ID 是契約。** 寫落文件／log／圖之後就唔改。要改＝**換一個新 ID**，並且一齊改
   引用它的地方（`tools/id_check.py` 掃）。
3. **數字只代表先後，唔代表重要。** `B06` 唔係因為緊要所以排第六。
4. **`NC_CAP_*` 是能力位，唔係階段。** 以前叫 `NC_STAGE_*`，同「階段」撞名 ——
   2026-11-13 改名（`needs`／`gives` 用嘅位元）。

### 7.6 已經落地

| 位置 | 改咗乜 |
|---|---|
| `firmware/main/boot.h` | `nc_boot_stage_t.id`、`nc_boot_result_t.id`；命名空間寫在 header |
| `firmware/main/boot.c` | `boot[B02] storage+cfg 126.92 ms ok`；摘要 `B01 9.66 \| B02 126.92 \| …` |
| `firmware/main/main.c` | 12 行表加 `"B01"…"B12"`；`NC_STAGE_*` → `NC_CAP_*` |
| `docs/00_pm/boot.svg`、`core-timeline.svg` | 兩張圖都用 B-id，同 log 對得上 |
| `tools/id_check.py` | 掃 docs／SVG 的 ID token，對 code 是否存在（唔存在就 fail） |

### 7.7 未做

- `T.`／`R.`／`L.`／`C.` 目前只是約定，code 未有前綴、亦未強制檢查（`id_check.py`
  只做「存在性」，唔知語義）。
- STATUS 未印 B-id（`nc_boot_results()` 已經帶 `id`，接上去就有一行開機帳）。
- 上游（MicroPython）那邊未有對應的 ID 表；跨 repo 講嘢暫時仍然用名字。



