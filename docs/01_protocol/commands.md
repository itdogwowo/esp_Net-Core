# 指令使用現況與新增規則

> **用途**：回答兩個問題 ——「現在實作了哪些指令」與「要加新指令時該放哪裡」。
> **原則**：**能重用就不新增。** 舊專案的 `slave/schema/*.json` 是權威表，先查再決定。
> 相關：`slave/schema/*.json`（mp_Net-Core）、`transports.md`

---

## 1. 新指令的四條規則

1. **先查舊 schema**：`sys` / `status` / `file` / `ota` / `pixel` / `stream` / `bench` / `hw` / `now` / `heartbeat` / `audio`。
   有現成的就直接用，欄位也照它的順序與型別。
2. **真的沒有才加**，而且加在**語義所屬的域的最後一個空號**，不要另開新域。
3. **測試專用一律放 `0xFxxx`**，不得混進產品域。測試指令可以隨時刪，不影響對外契約。
   **`0xFxxx` 整個區段都是「假指令」**：它們不是產品功能，是量測與回歸用的工具。
   上游 `mp_Net-Core` 的 schema **沒有這個區段** —— 看到 `0xF0xx` 就當它不存在（見 §2.1）。
4. **handler 住在所屬域**：產品域放 `components/action/<域>_action/`；測試／量測
   指令放 `components/action/test_action/`（**名字 `test_` 開頭就係測試**，同產品域
   同層，唔另開 `dev/`）；`main.c` 只做裝配（建 decoder → 呼叫各 action 表），
   唔應該再有新 handler。核心 `sys`／`status` 域的表暫時仍喺 `main.c`
   （歷史遺留，見下表），搬過去係 P2 重構。

   **目錄名 = 上游 schema 的 group 名**：

```text
components/action/
├─ file_action/     0x20xx  file 域（FILE_QUERY / ERROR_RSP）        ✅
├─ ota_action/      0x22xx  ota 域                                   ✅
├─ sys_action/      0x1xxx  sys 域                                   ⬜ 待建（暫在 main.c）
├─ status_action/   0x11xx  status 域（STATUS_GET / SET / ACK）      ⬜ 待建（暫在 main.c）
├─ bench_action/    0x18xx  bench 域                                 ⬜ 待建（暫在 main.c）
└─ test_action/     0xFxxx  測試／量測區（test_* = 測試）             ✅
```

### 1.1 指令 ID 分區（含外掛保留區）

| 區段 | 用途 | 實作位置 |
|---|---|---|
| `0x1000–0x10FF` | sys 域（REBOOT／SET_MASTER／SYS_TASK_*） | main.c（暫）→ `action/sys_action` |
| `0x1100–0x11FF` | status／config／route 域 | main.c（暫）→ `action/status_action` |
| `0x1800–0x18FF` | bench 域 | main.c（暫） |
| `0x2000–0x20FF` | file 域 | `action/file_action` |
| `0x2200–0x22FF` | OTA 域 | `action/ota_action` |
| **`0x3000–0x33FF`** | **核心功能域**（跟舊專案編號）：`0x30` stream／`0x31` pixel／`0x32` audio／**`0x33` motor（新增）** | 舊專案：前三個已實作；`0x33` 見 §1.1.2 |
| **`0x3400–0x3FFF`** | **外掛私有指令（保留）**：模組喺 `build.json` 的 `cmds` 宣告，由 ncm 註冊／路由；核心區唔可以由外掛佔用 | `components/host/ncm`（路由，未實作） |
| `0xF000–0xF0FF` | 測試／量測（唔係產品功能） | `components/action/test_action`（＋main.c 少量 bench） |

### 1.1.1 指令表由 component 自己登記（2026-11-19）

以前 `main.c` 有一張寫死的清單（`tables[] = { &s_core_action_table, … }`）——
「邊條指令入映像」由 `main.c` 決定。現在改成**自己現身**：

- 每個 component 用 `NC_ACTION_TABLE_DEF(...)` 宣告表；巨集會產生一個
  **constructor**，在 `app_main` 之前呼叫 `nc_action_add()` 把表交去管理器。
- `main.c` 只叫 `nc_action_init_all()`，**冇清單、冇 `#ifdef` 列表**。
- 登記表**冇固定上限**：先收在 8 格靜態表，滿了 `malloc` 更大再搬（constructor 在 heap 初始化之後跑）。
- 代價：component 冇任何 symbol 被 main 引用 → 要加 `WHOLE_ARCHIVE`
  （`file_action`／`ota_action`／`test_action` 已加），否則 linker 會整份丟掉。
- 效果：**「呢個 component 有冇 link 進來」就是唯一開關** —— 加／減一個 action
  資料夾就加／減一組指令，`main.c` 唔使改。實測（2026-11-19，S3 測試映像）：
  改前改後 `status cmds` 都是 **85 條（其中 0xF0xx 58 條）**。

> 對照舊專案 `mp_Net-Core/slave/action/registry.py`：那邊是「每個 module 自己
> `register(app)`，`app.py` 只叫 `register_all(app)`」。C 版把 registry 也拿走，
> 由 linker／constructor 做，所以連 registry 清單都唔需要。

### 1.1.2 `0x33xx` 電機域（Motor，新增 2026-11-22）

跟 `0x30` stream／`0x31` pixel／`0x32` audio 同一個風格：**`0x33xx` = 電機**。
呢個係**核心域**，唔係外掛私有區；由我方定義，對方（棕樹叔叔的 UART 裝置）照住實作。

**設計原則：目標 ＋ 時間（set-and-follow）** —— 匯流排上只有「意圖」，波形喺本地產生。
一次移動一幀（`9 + payload + 4`），移動完線上是 0；唔可以逐格／逐角度餵。

| CMD | 名稱 | 方向 | Payload | 說明 |
|---|---|---|---|---|
| `0x3301` | MOTOR_SET | M→S | `axis u8` `target u16` `ms u16` `flags u8` | 目標位置 ＋ 到位時間。`ms=0` = 用預設速度 |
| `0x3302` | MOTOR_STOP | M→S | `axis u8` `mode u8` | `0`=減速停、`1`=急停、`0xFF`=全部 |
| `0x3303` | MOTOR_QUERY | M→S | `axis u8` | 查即時狀態（回 `0x3304`） |
| `0x3304` | MOTOR_STATE | S→M | `axis u8` `pos u16` `target u16` `state u8` `ms_left u16` `flags u8` | `flags.bit0` = `pos` 係開環估算（唔好假裝準） |
| `0x3305` | MOTOR_HOME | M→S | `axis u8` `timeout_ms u16` | 找限位／回零（推桿上電用） |

**位置單位**：`0..4095`（`u16`，12-bit），跟舊專案 `slave/lib/hw/uart_motor.py` 一致 ——
`0` = 全收、`4095` = 全伸、`2048` = 半程。
Servo 由 LV2 映射成脈寬（`500 + pos × 2000/4095` µs、50 Hz）；工具層再顯示角度，**wire 上唔用度數**。

**`flags`（`0x3301`）**

| bit | 意思 |
|---|---|
| 0 | 相對位置（1）／絕對位置（0） |
| 1 | 到位主動回 `0x3304`（事件），唔使 poll |
| 2–7 | 保留，送 0 |

**落地分工**

| 角色 | 要做什麼 |
|---|---|
| 我方 LV2 | 收 `0x3301` → 自己內插（servo）或餵 `uart_motor.move_to()`（推桿）；到位／邊界 → 停 |
| 棕樹叔叔的 UART 裝置 | 最少實作 `0x3301`／`0x3302`；有 `0x3303/04` 更好（面板顯示得到位置） |
| 若他晶片跑不動 NC4 | 由我方 LV2 翻譯成他的私有 frame（`FF addr value FE` 之類），他一行都唔使改 |

**同其他域的邊界**

- `0x3105 MODE_SET`（`mode_type=2` SERVO 組）＝ 播一段**編排**；`0x3301` ＝ **直接定位**。
  關係同 LED 一樣：`0x3105`（模式）對 `0x3003`（逐幀直推）。
- `0x1401 HW_CTL`（PIN／PWM）＝ 手動佔空比，用來測試／逃生，**唔好做正式電機接口**（冇時間、冇限位、冇回授）。

> 現況：新韌體未實作 `0x33xx`（收到只會 `ESP_LOGD 未註冊的 cmd`）；舊專案 `schema/` 亦未有 `motor.json`。
> 兩邊都未落地之前，對外只可以講「號碼已分配、定義已定」，唔可以講「已經通」。

### 1.2 `action` 到底是什麼：表格＋handler，唔係可載入檔案

- **表格**只係索引：`{cmd, handler, owner}`（`nc_action.h`），開機統計→一次配置→
  排序→封存；`nc_action_find()` 二分搜尋。
- **具體功能**係每個域自己寫的 C handler（`on_<動詞>`），住喺該域的 component。
- **唔會引入 `.nca` 副檔名**：`.nct`／`.nce` 係「可載入容器」（資料＋機器碼）；
  指令 handler 係固件程式碼，唔可以亦唔應該由外部載入（安全／實時／ABI）。
  如果將來要俾外掛加指令：指令 ID 寫入 `.nct` 的 manifest／`build.json`，
  由 ncm 受控路由（配額＋timeout），唔係新增檔案格式。

> **已修正（2026-11-04）**：8 條測試指令搬到 `components/action/test_action/`，
> 由 `NC_ENABLE_TESTS` gate。`main.c` 的 `s_core_actions[]` 只剩 5 條產品指令：
> `0x1005 SYS_TASK_QUERY`、`0x100F REBOOT`、`0x1016 SET_MASTER`、`0x1101 STATUS_GET`、
> `0x1105 ROUTE_GET`。
> 所以現在「想找某條指令的實作」要分兩個地方找。搬過去之後，
> **看到 `action/` 就知道那裡有指令**，`main.c` 只剩開機順序。
>
> 之前我寫的 `core_action` 是錯的 —— 沒有這個 group。`0x1xxx` 在上游屬於 **`sys`**。



---

## 2. 目前已實作

| CMD | 名稱 | 來源 schema | 狀態 |
|---|---|---|---|
| `0x1005` | SYS_TASK_QUERY | sys.json | ✅ |
| `0x1006` | SYS_TASK_RSP | sys.json | ✅ |
| `0x100F` | REBOOT | sys.json | ✅ |
| `0x1016` | SET_MASTER | sys.json | ✅ |
| `0x1101` | STATUS_GET（keys：空 / `?` / sys,mem,task,modules,**provides**,storage,ncm[,net]） | status.json | ✅ |
| `0x1102` | STATUS_RSP（`status_json`） | status.json | ✅ |
| `0x1811` | BENCH_READY | bench.json | ✅ |
| `0x1812` | BENCH_DATA | bench.json | ✅ |
| `0x1813` | BENCH_RESULT | bench.json | ✅ |
| `0x1814` | BENCH_REPORT | bench.json | ✅ |
| `0x2201`–`0x2220` | OTA 子集（BEGIN/WRITE/END/ABORT/ACK/VERSION/CAPS/PROGRESS/STATE/ERROR/APPLY） | ota.json | ✅ |
| `0x2001` | FILE_BEGIN（file_id/total/chunk/sha256/path → 開 `<path>.part`；成功回 `0x2004` offset=0） | file.json | ✅ |
| `0x2002` | FILE_CHUNK（file_id/offset/data；成功回 `0x2004`） | file.json | ✅ |
| `0x2003` | FILE_END（驗 sha → 舊檔 rename `.bak`、新檔上位 → 回 `0x2006`） | file.json | ✅ |
| `0x2004` | FILE_ACK（file_id + offset） | file.json | ✅ |
| `0x2005` | FILE_QUERY（path → 存在/sha256/size/free/pending） | file.json | ✅ |
| `0x2006` | FILE_QUERY_RSP | file.json | ✅ |
| `0x2007` | FILE_READ（path/offset/length → 回 `0x2002`，file_id=0） | file.json | ✅ |
| `0x2008` | FILE_CONFIRM（刪 `.bak`，清 pending） | file.json | ✅ |
| `0x2009` | FILE_DELETE（刪檔＋`.bak`＋`.part`） | file.json | ✅ |
| `0x200A` | FILE_UNDO（刪新檔，`.bak` 改回原名） | file.json | ✅ |
| `0x200B` | FILE_SCAN（target u8；0=local／1=SD；walk 後寫 `/manifest.json`，冇回覆） | file.json | ✅ |
| `0x2010` | FILE_ERROR_RSP（7 個 bool；`err_busy=1` = FAT 在 host 手上） | file.json | ✅ |
| `0x1103` | STATUS_SET（`System.file_mode`：0=交還 host、1=裝置接管；`System.fs_format`：1=下次開機抹 data 分區重建 FAT） | status.json | ✅ 子集 |
| `0x1104` | STATUS_ACK（`ok` + `message`） | status.json | ✅ |
| `0x1105` | ROUTE_GET（空 payload → 回開機時定下的路由表） | status.json | ✅ |
| `0x1106` | ROUTE_RSP（`routes_json`） | status.json | ✅ |

### `0x1105 ROUTE_GET`：為什麼要一條查詢指令

路由是**開機時**由 `system.json` 的 `routes[]` 讀進來、算一次就固定的（見
[../03_notes/routing.md](../03_notes/routing.md)）。它不隨時間變，但**改 config 之後
要能確認板子真的吃到了哪一份** —— 沒有這條指令就只可以看 UART 開機 log。

回覆長這樣（`nc4_router_json()` 產生）：

```json
{"set":1,"core0":3,"core1":1,
 "routes":[{"line":"nctx","core":0,"out":["decode0"]},
           {"line":"CDC","core":0,"out":["decode0"]},
           {"line":"UART","core":1,"out":["decode1"]},
           {"line":"TCP","core":0,"out":["decode0","UART"]}]}
```

- `set` = 1 表示表真的設過（0 = 冇人叫過 `nc4_router_set()`，router 等於冇開）。
- `core0` / `core1` = 各有幾條線派去第 0 / 第 1 核；`0` 代表該核的 decode 任務
  **不會被建立**（除非有 `nc4_router_keep()`）。
- `core` = 本地由哪一核解（`out` 裡第一個 `decodeN`）。
- `out` = **完整的目的地清單**（`decodeN` 與 link 名原樣照抄）：`decodeN` = 本地執行、
  link 名 = 轉寄。2026-11-08 起轉寄真的生效（見
  [routing.md §7.1](../03_notes/routing.md)）；只轉不解的行仍然唔會建立通道。

### `0x1103 STATUS_SET`：`System.fs_format` = 下次開機重建 FAT

沿用舊專案「設定狀態 → 下次開機生效」的做法，沒有新增指令號：

```json
0x1103 STATUS_SET  {"System":{"fs_format":1}}   persist=1
0x100F REBOOT
```

（也接受點分路徑 `{"System.fs_format":1}`，以及別名 `System.format`。）

- `fs_format=1` → 寫 NVS `boot/fs_format`，回 `0x1104 STATUS_ACK {ok,message}`；
  **不會在解碼路徑抹 7 MB**。`STATUS_SET` 只改狀態，格式化延後到開機。
- `0x100F REBOOT` → 下次 `storage_init()` 見到旗標：
  `nc_store_format()`（抹 7 MB → WL mount → `f_mkfs` → mount → seed）→ 清旗標。
- `fs_format=0` → 取消請求。
- 開機後 `STATUS_GET keys=storage`：
  - `req=1` = 有待處理的格式化請求（未 reboot）
  - `req=0` = 已消費
  - `fmt=0` = 這次開機真的重建過；`-99` = 沒走重建
- `first` 仍是「第一次初始化」旗標；`fs_format` 是**明確請求**，不受 `first` 限制。
- 風險：會清掉使用者所有檔案；只用在 factory reset 或 FAT 確定壞掉。
- 正常換韌體**不會**走這條：換版只刪 `config/*.json`，再 seed。

### `0x1101 STATUS_GET`：KEY 只是「揀邊幾份報告」，不是「查一個值」

> 常問：「不是輸入 KEY 就回覆嗎？為什麼會有 2.4 KB 那麼大？」

因為 KEY 揀的是**報告**，不是**欄位**。`sys` / `mem` / `tasks` / `provides` / `storage` / `ncm`
每一個都是一整份結構化報告，不是一個數字：

| KEY | 內容 | 大約大小 |
|---|---|---|
| `sys` | 版本、cID、晶片、運行時間、開機原因 | ~200 B |
| `mem` | 每個 class 的 plan 帳（宣告 / carve / 用量 / 峰值） | ~1,500 B |
| `tasks` | 每個任務 × 每個 loop 的 core / layer / prio / period / running / enabled | ~1,500 B |
| `provides` | 誰提供什麼（buffer / JSON / 數值） | ~900 B |
| `storage` | FAT 擁有者、`/ro` vs `/data`、`boot`（`prov`／`fmtq`／`degr`／`fw_new`）、`fmt`、free、pending、`sd{en,mnt,mb,vfs_mb,drv,fr,probe,sv_err,err,drives}` | ~400 B |
| `cfg` | 兩層 config 的載入結果：`miss`（缺檔）/`bad`（壞 JSON）/`ver`/`def`（用預設）/`inv`（不合法）/`unk`（不認識的鍵） | ~200 B |
| `log` | log 管道：`cap`（hook 裝了？）/`run`（consumer 起了？）/`lvl`/`mask`/`buf`/`used`/`peak`/`produced`/`sent`/`dropped`/`busy`/`offline` | ~250 B |
| `rt` | 執行期 FreeRTOS task 表（name / prio / stack HWM），要 `CONFIG_FREERTOS_USE_TRACE_FACILITY` | ~500 B |
| `ncm` | 每個 `.nct` / `.nce` 的 manifest（kind / core / fps / px / mem / 指令） | 1,000–2,600 B |
| `net` | IP / MAC / link 狀態（只在 P4） | ~300 B |

**空 KEY ＝ 全部**，所以加起來 ≈5.8 KB，而回覆幀上限是 `NC4_REPLY_JSON_MAX` = 2,600 B。
所以「空 KEY」的語意是**盡力而為**，不是「保證全部」：

- 砌得落就照砌，砌唔落就**停手**，改回一個**合法**的物件
  `{"truncated":1,"cap":2600,"tips":"keys=sys,storage,cfg,log,mem,provides,tasks,ncm 分批問"}`。
- 順序是**由便宜到貴**（sys → storage → cfg → log → rt → mem → provides → tasks → ncm → wifi → net），
  所以截斷時你**一定**拿到便宜那幾份，貴的（`tasks` / `ncm`）才可能被切走。
- 想要全部就**分批問**：先 `keys=sys,storage`，再 `keys=cfg,log`，再 `keys=tasks`，再 `keys=ncm`。
  每份都遠低於 2,600 B，一定完整。
- `keys=?` 回可用 KEY 清單（唔會回內容），用來探測這版 firmware 支援哪些區塊。

> 為什麼不把上限提到 8 KB？因為回覆幀要放進 `NC4_MAX_FRAME`（8,205 B），
> 而那個 8,192 是**經驗參數**（見 [frame.md](frame.md)），不是可以隨便借的空間。
> 一份 5.8 KB 的 status 會把整條線的 tx 佔住幾十 ms，其他回覆全部排隊 ——
> 分批問反而快。

### FAT 為什麼不用新指令

換 FAT 的擁有者不需要新指令號 —— 那是「一個具名的狀態」，走既有的
`0x1103 STATUS_SET`：`{"System":{"file_mode":1}}`。
「下次開機重建 FAT」也一樣走 `0x1103`：`{"System":{"fs_format":1}}`（見 §2）。

讀取（`0x2005`）**任何模式都答得到**：主機擁有 FAT 時走唯讀視圖（`/ro`），
回覆的 `pending=1` 表示「可能過期或半截」；裝置接管時走 `/data`，`pending=0` 是真值。
只有**寫入**才需要 `file_mode=1`。設計與實測見
[../03_notes/fat-ownership.md](../03_notes/fat-ownership.md)。

### 測試專用（`0xFxxx`，可隨時刪）

> ⚠️ **這一整段是假的。** `0xFxxx` 不是產品契約、不在上游 `slave/schema/*.json` 裡、
> 也不保證跨版本存在。它們只是「量測某個機制有沒有真的做到」的工具：
> 吞吐、零複製、雙核 hub 交換、慢工作延後。**要接產品的東西，一律不要用 `0xFxxx`。**
>
> **`0xF005` DEV_SLOW 與 `0xF006` UART_BENCH 已刪除**（2026-11-04）。
> 它們是為了證明「慢工作佇列」有效而存在的假工作；而指令層本來就是嚴格一問一答，
> 「慢指令擋住後面的指令」這個問題**天然不存在**（見 [execution-rules §7](../03_notes/execution-rules.md)）。
> 慢工作佇列（`slow_worker` + `nc_hub` jobs）亦一併移除。

| CMD | 用途 |
|---|---|
| `0xF001` DEV_BENCH | `size(u16) iters(u16) mode(u8)` → `0xF002` JSON。mode 0=decode、1=pack（純 CPU，不碰 flash／網路）。舊的 2=erase 4K、3=write 4K、4=erase 64K **已移除**：任何通道打一幀就能擦 OTA slot |
| `0xF002` DEV_RSP | 上述回覆（`str_u16len` JSON） |
| `0xF003` HUB_BENCH | `iters(u32) [mode(u8)]` → `0xF004` JSON：雙核 hub 交換量測（writer core1 / reader core0）。`mode` bit0=1 用舊版次序（deadline 太晚）重現 read=0，回歸測試用 |
| `0xF004` HUB_RSP | `{iters,read,us,per_sec,torn,w,w_full,r_null,exit:[w,r],late}`；`exit` 1=完成 2=逾時 3=等不到 start |
| `0xF007` TX_BENCH | `frames(u32) size(u16) mode(u8)` → `0xF004` JSON：**上傳**（打包＋送出）量測。mode 0=同核（同一 task 打包後送出）、1=分核（core1 打包進插槽、decode task 送出） |
| `0xF008` BUS_STATS | `[reset:u8]` → `0xF009` JSON：每條 bus 的 `{name,mode,frame,frames,mb,copied,parse_ms,us_per_frame,region,slots[],peak,reads,max_read,slot_full,tx,tx_max,max_dlen,max_ok}` + `resync`。`copied=0` 就是零複製；`max_read`／`peak`／`slot_full` 是**緩衝貼身化**的量測依據（見 [buffer-tightening.md](../03_notes/buffer-tightening.md)） |
| `0xF009` BUS_RSP | 上述回覆（`str_u16len` JSON） |
| `0xF00A` NET_TUNE | `split(u8) core(u8) [frame_align(u8)]` → `0xF004` JSON：把 RX 收斂模式寫進 NVS（`net/rx_split`、`net/rx_core`、`net/frame_align`），**下次開機生效** |
| `0xF00B` UART_LOOP | `frames(u16) size(u16)` → `0xF004` `{sent,frame}`：由 **handler** 直接灌 UART 迴路。⚠️ handler 揸住 decoder 鎖，寫入期間 decode1 不能消費 → 只適合小量（≤ RX ring），量吞吐請用 `0xF00E` |
| `0xF00C` LOOP_STAT | 空 → `0xF004` `{rx,sent}`：UART 迴路已收到的幀數／串流已寫出的幀數 |
| `0xF00F` UART_INJECT | 原始 bytes → `0xF004` `{wrote:N}`：把 payload **原樣寫入 UART**（IO7→IO8 迴路）。用途是驗證雙核並行：注入一條慢指令（例如 `0xF001`）讓 decode1 卡住，同時量 core0 的 RTT |
| `0xF00E` UART_STREAM | `frames(u16) size(u16)` → `0xF004` `{started,frames,size}`：**由獨立 task** 灌 UART（唔揸 decoder 鎖），配合 `0xF00C` 取進度。`size ≤ 256` |
| `0xF01A` BIG_REPLY | `[n:u16]` → `0xF004`：回一條長度 n（預設 3,000）的字串，用來驗「回覆超限」。**n > 2,600 時預期收到 `{"err":"reply_too_big","need":n,"cap":2600}`**；n ≤ 2,600 時內容是 `x`（不是 JSON，只為驗邊界） |
| `0xF01B` LINK_FWD | `name_len(u16) name(bytes) frame(bytes)` → `0xF004` `{link,len,err,frames,bytes,drops}`：把一段**已打包好的 NC4 幀**交給指定 link（走 `nc4_link_send`，與 router 轉寄同一條路）。未知 link → `err=ESP_ERR_NOT_FOUND` |
| `0xF01C` MEM_BENCH | `[psram_kb:u16][iters:u16]`（預設 256 KB、4 次）→ `0xF004`：`{int:{kb,memset_mbps,read_mbps,copy_mbps}, psram:{...}, msync_us, msync_err, sum}`。量 internal／PSRAM 實際頻寬與 cache flush；用來決定 PSRAM 可否做 framebuffer |
| `0xF01D` TILE_BENCH | `[total_kb:u16][tile_kb:u16][iters:u16]`（預設 40/8/8）→ `0xF004`：`{total_kb,tile_kb,iters,full_us,tile_us,full_mbps,tile_mbps,ratio_x10}`。整幀 streaming vs 分塊（cache-hot）的實測；證明單 pass 是 bus-bound、tiling 只快 1.3× |
| `0xF050` CPU_BURN | `[iters:u32]`（預設 50,000）→ `0xF004` `{iters,us,sum}`：native C 版 LCG 迴圈（同外掛 `bench_task.wat` 同 workload），用嚟量 wasm 直譯 vs C 的實際倍數。2026-11-17 S3 實測 50,000 次：C 1,683 µs、wasm 24,887 µs（≈14.8×），checksum 一致 |
| `0xF060` MATRIX | `op(u8) [args]` → `0xF004` JSON。像素效果**生產者**（`pixel` 任務；`CONFIG_NETCORE_PIXEL`）。op：`0` STATS、`1` effect(u8)、`2` dither(u8)、`3` color(r,g,b)、`4` gain(u8)、`5` fps(u16 LE)、`6` zone(idx,rate,phase,[bits])、`7` zmask(u8)、`8` bits(u8)、`9` speed(u16)、`10` zbits(8)、`11` zeff(idx,eff)、`12` zeffs(8)、`13` demo(on)、`14` wspeed(u16)、`15` zdither(8)、`16` frac(u8)。回覆見 [pixel-renderer.md](../03_notes/pixel-renderer.md) |
| `0xF061` RENDER | `op(u8)` → `0xF004` JSON。渲染**任務**（`render`）的狀態。`op=0` STATS：`{"producer":{…},"render":{…}}`；`op=1` 預留（回 ok）。見 [pixel-renderer.md](../03_notes/pixel-renderer.md) |
| `0xF062` PXBENCH | `[lanes:u8][px:u16 LE][frames:u8]` → `0xF004` `{lanes,px,frames,units,us_enc,us_mix,us_direct,us_packed,us_pack,us_merge,sram,max_fps_x10,pass50}`：像素編碼吞吐量測（唔需要燈板） |
| `0xF063` SINBENCH | `[iters:u16 LE]`（可省，預設 50000，夾 100..500000）→ `0xF004` `{"n","smp","lin":{"ns","err"}}`；`err` = milli-LSB（×100）。量**固件唯一的正弦**（`nc_px_engine_sin_lin`）的實機 ns/call ＋ 對 double 的最大誤差（65536 相位全掃）。舊的 q12／sin 已於 2026-11-23 刪除。見 [sin_bench/README](../../tools/hosttest/sin_bench/README.md) |
| `0xF064` PROGBENCH | `[px:u16][flags:u8]`（flags bit0=per-led、bit1=canvas 放 PSRAM）→ `0xF004` `{px,val_ns:{keep,math_now,square,pulse_wave,pulse,starter},plan_us,frame_ns,sram,per_led,canvas,pair12_ns,conv_ns,dither_ns}`：六工具的**實機**逐值＋plan／scatter＋12-bit 畫布成本 |
| `0xF065` EFBENCH | `[px:u16 LE][frames:u16 LE][vpp:u16 LE]`（frames=0 按 px 自動；vpp=每顆幾個值 1..4，可省＝1）→ **`0xF066`** `{px,fr,rep,nv,eff:{vpp,ev,vals_ns,stream_ns,fast_ns,psram,build_us:[4],per_val_ns:[4],gam_ns,gam_raw_ns},mem:{c8i,c8p,c12i,c12p,ifree,pfree},eval_ns,vals_ns,pair8,pair12,conv,pipe:{i8,p8,i12,p12,mix},chunk:{64,256,1024},jobs:{j2,j4}}`。**只測效果管線**：program 逐值／逐幀、8／12-bit 配對、12→8 捨入／抖動、internal／PSRAM／混合、分塊、多 job；`eff` 就係「每幀真的算 px×vpp 個值」那一段（`vals_ns` 無波表、`stream_ns` 生產路徑有 `% total`、`fast_ns` 無除法、`build_us` 四種波各建 256 值一次、`gam_ns` 每幀 ev 個值走 gamma 表、`gam_raw_ns` 同一批值只 `>>8` 唔做 gamma，兩者相減就係 gamma 的價錢）。`CONFIG_NETCORE_EFFECT_BENCH`（不需要 `NC_ENABLE_TESTS`）；見 [effect-perf.md](../00_pm/effect-perf.md) |
| `0xF067` PSRAMBENCH | `[size_kb:u16 LE][iters:u16 LE]`（預設 256 KB、8 次，夾 16..1024 KB、1..32）→ **`0xF068`** `{size,it,ok,int:{wr,rd,cp},psram:{wr,wr_cold,rd,rd_cold,cp},mix:{i2p,p2i,p2p},chunk:{1k,4k,16k,64k},msync_us:{c2m,m2c},msync_err}`（全部 MiB/s）。大 buffer 留 PSRAM、以 chunk 讀入 internal 的純搬運量測；`CONFIG_NETCORE_EFFECT_BENCH`；見 [effect-perf.md §9](../00_pm/effect-perf.md#L268) |
| `0xF069` PIPEBENCH | `[px:u16 LE][frames:u16 LE][mode:u8]`（mode 0=1 值/px wwww、1=3 值/px rgb、2=1 值/px w）→ **`0xF06A`** `{px,nv,mode,fr,rep,gen_us,pair_us:{range,table,rnd},conv_us,full_us:{range,table,rnd,ps},mem}`（µs）。9,600 個效果值→配對→12→8 的全流程；RGBW→I80 不在內。`CONFIG_NETCORE_EFFECT_BENCH`；見 [effect-perf.md §10](../00_pm/effect-perf.md#L315) |
| `0xF081` FPS | 無參數 → **`0xF082`** `{target_us,n,avg_us,sd_us,min_us,max_us,bad,starved,late,slack_us,catchup,overrun,hist[64]}`：render 逐幀間隔統計；payload[0]=1 先清零。工具：`tools/fps_trace.py` |
| `0xF07B` FLATREAD | 無參數 → **`0xF07C`** `{ok,bytes,us}`：讀 `map/matrix.json`（量 FAT 讀速）；有 4 B size payload 就讀 `/flat_bench.bin`。量測用（pair 指令表） |
| `0xF06B` SHOWFRAME | 無參數 → **`0xF06C`** `{px,seq,mode,white,apply_us,hex}`（0xF06B 最近一幀 4 B/px hex；`nodata`／`big`）。production Show 的 live frame；`show` 指令表。見 [show-pipeline.md](../00_pm/show-pipeline.md) |
| `0xF071` PIPESTATS | 無參數 → **`0xF072`** JSON：`eff{pair}` 分段（avg/max/last）、`apply`／`white`／`commit`／`lat`、`render`（frame/enc/tx/fps）、`full`。三任務計時。 |
| `0xF073` PIPEBURN | `[seconds:u8]`（0=停）→ **`0xF074`** `{ok,burn_s}`；effect 暫時全速跑（pair/render 仍受環／50 fps 限制）。 |
| `0xF075` PIPERESET | 無參數 → **`0xF076`** `{ok,reset}`；清零分段統計。 |
| `0xF06F` RELOAD | 無參數 → **`0xF070`** `{"ok":1,"queued":"reload"}`；叫 content 任務下一幀邊界重新掃 `/pixel` 四層（改檔不重開）。見 [show-pipeline.md](../00_pm/show-pipeline.md) |
| `0xF06D` SHOWMODE | `[mode_id:u8]` → **`0xF06E`** `{ok,queued,mode,white}`；排隊切換 mode，show loop 喺下一幀邊界 rebuild plan（唔會同 decode task 同時改 plan）。見 [show-pipeline.md](../00_pm/show-pipeline.md) |

> ⚠️ 新增測試指令之前先查号：**指令碼重複時只會保留先登記的那一個**（`nc_action_init()`
> 會在開機 `ESP_LOGE` 並丟棄後來者）。登記次序是 core →（測試的 bench／SD）→ test_action
> → ota → file，所以生產指令永遠不會被測試表蓋掉。0xF012／0xF013 已被 SD bench 佔用；
> 0xF005／0xF006 是 2026-11-04 刪除的舊號，不要再復用。

`0xF004` 是共用的回覆號（HUB / SLOW / UART / TX / NET_TUNE / BIG_REPLY / LINK_FWD 都用它），
因為它們都是「量測結果」。

### 名稱一律照上游，不要自己發明

上游 `mp_Net-Core/slave/schema/*.json` 是**權威表**（含 payload 的欄位名、型別、順序）。
我們的 `#define NC4_CMD_*` 只是給 C 用的別名，**數值與名字都要跟上游一致**；
不一致的地方就是 bug（主機端會照 schema 發，對不上就整條指令失效）。

`sys` 域（`slave/schema/sys.json`）現況對照：

| CMD | 上游名字 | 我們 | 備註 |
|---|---|---|---|
| `0x1001` | DISCOVER | ❌ | 從機不需要主動發現 |
| `0x1002` | SLAVE_ANNOUNCE | ❌ | 同上 |
| `0x1004` | SYS_CTRL | ❌ | `wifi_enable` / `core_control`，等 WiFi 落地 |
| `0x1005` | SYS_TASK_QUERY | ✅ | 空 payload → `0x1006` |
| `0x1006` | SYS_TASK_RSP | ✅ | `tasks_json`（`str_u16len`） |
| `0x1007` | SYS_TASK_SET | ❌ | `task_name` + `affinity_c0` + `affinity_c1`；**要做執行期改核就接它**（本地已冇對應 API，見下方註） |
| `0x1008` | WIFI_CTRL | ❌ | 等 WiFi |
| `0x1009` | WEB_CTRL | ❌ | |
| `0x100A`–`0x100C` | TIME_SYNC / TIME_SYNC_RSP / TIME_OFFSET_APPLY | ❌ | 多節點同步時間 |
| `0x100D` / `0x100E` | IDENTIFY_REQ / IDENTIFY_RSP | ❌ | 多節點配對 |
| `0x100F` | REBOOT | ✅ | `delay_ms(u32)`，已照上游 |
| `0x1010` / `0x1011` | WREPL_CTRL / WREPL_RSP | ❌ | 無線替換（舊專案的 hot-swap） |
| `0x1012` / `0x1013` | NET_START / NET_START_RSP | ❌ | |
| `0x1014` / `0x1015` | GET_IP / IP_RSP | ❌ | 目前 IP 走 `0x1101 STATUS_GET` |
| `0x1016` | SET_MASTER | ✅ | `master_cid(u16)` |
| `0x1017` / `0x1018` | WEBUI_CTRL / WEBUI_RSP | ❌ | |
| `0x1019` | （上游沒有） | ⚠️ | 曾用作 `RESET_TO_DOWNLOAD`，**已刪**；這個號留給 `ASSET_RELOAD`（見 §3） |

> **`0x1005` / `0x1006` / `0x1007` 是一組**：查、回、設。我們只做了前兩個。
> `0x1007` 之所以未做，是因為**本地根本沒有換核 API** —— `nc_task_set_core()` 已刪，
> 核心在開機時由宣告的 affinity ＋ router 定死。要做遠端改核，先要有執行期換核的機制，
> 那是比「加一條指令」大得多的事。


---

### OTA 的時間語義（2026-11-17 實測）

- **`0x2201 OTA_BEGIN` 的 ACK 是同步 erase 之後才發**：韌體先 `esp_ota_begin()`，
  它會抹 `ALIGN_UP(image_size, 4 KB)`。S3 實測 **≈237 KB/s**：
  1.2 MB 映像 → **~5.2 s**；327 KB → ~1.4 s。主機的 BEGIN timeout 要按映像大小估
  （`tools/ota.py`：`10 s + size/100 KB`），唔可以硬編 5 s —— 硬編 5 s 會令「大少少」
  的映像回 `no reply`（2026-11-17 就是這樣卡了 test 映像；見 status §7.81）。
- **超時 ≠ 失敗**：ACK 遲到時，隨後用 `0x221A STATE` 會見到 `writing=1`，
  `0x2218 PROGRESS` 的 `image_size` 已入帳、`written=0`；可以當作已開始繼續寫。
- **中斷／超時留下的 session**（`state=writing`）要先送 `0x2205 OTA_ABORT` 清掉
  （`tools/ota.py --abort`），否則下一次 BEGIN 回 `err_begin_partition_conflict`。
- **`0x2203 OTA_END` 亦會驗整份 image**（`esp_ota_end`）：1.2 MB 約 1–2 s，
  主機 timeout 用 15–30 s。

---

## 3. 「不得不加」的指令（目前只有一個）

| 需求 | 為什麼沒有替代 | 建議落點 |
|---|---|---|
| **ASSET_RELOAD** | 舊版改完 assets 只能 `0x100F REBOOT`。C 版的資產在使用者的隨身碟上，拖完檔要能**不重開機**就生效；而且 reload 需要「卸載 → 驗證 → 換上 → 失敗保留舊版」這一串語義，REBOOT 表達不了 | **`0x1019`**（sys 域，`0x1018` 之後的第一個空號） |

`0x1019` 原本被我拿去當 `RESET_TO_DOWNLOAD`，已刪除 —— 現在空著，正好給它。

### 已知但**不**需要新指令的

| 需求 | 用什麼 |
|---|---|
| 韌體更新 | `0x22xx OTA`（已實作） |
| 查記憶體 plan / buffer 統計 | `0x1101 STATUS_GET keys=mem` |
| 查模組（kind / 資源 / 指令 / 綁核） | `0x1101 STATUS_GET keys=tasks` |
| 查「誰提供什麼」（buffer / JSON / 數值） | `0x1101 STATUS_GET keys=provides` |
| 查任務與綁核 | `0x1005 SYS_TASK_QUERY` |
| 改 master 位址 | `0x1016 SET_MASTER` |
| 檔案傳輸 | **CDC FAT**：`0x2001`–`0x2003`＋`0x2007`–`0x200B`（冇 USB driver 都通）；有 USB 嘅板可選 MSC（拖檔） |
| 效能量測 | `0x1811`–`0x1814`（產品域）＋ `0xF001`（開發用） |

> **冇 directory listing 指令**：跟 mp 一樣，`0x200B FILE_SCAN` 會 walk `/data`
> 並寫出 `/manifest.json`（`{path:{"s","h","p"}}`）；UI 用 `0x2007 FILE_READ`
> 讀一次 manifest 就砌到樹。`0x2005` 只答單一 path。
> 詳細語義見 [../03_notes/file-transfer.md](../03_notes/file-transfer.md)。

### 檔案寫入的 .bak / pending 語義

`FILE_END` 不會直接覆蓋舊檔：舊檔先 rename 成 `<path>.bak`，新檔才上位。
`pending = .bak 存在`；UI 可以提示「待確認」：

- `0x2008 FILE_CONFIRM` → 刪 `.bak`（正式生效）
- `0x200A FILE_UNDO` → 刪新檔、`.bak` 改回原名（回滾）

首次寫入（本來冇檔）唔會有 `.bak`，所以 `pending=0`。

---

## 4. 還沒實作但有 schema 的（等需求出現再做）

| CMD | 名稱 | 什麼時候需要 |
|---|---|---|
| `0x1007` | SYS_TASK_SET | 要執行期改 task 的 affinity |
| `0x100D/0x100E` | IDENTIFY_REQ/RSP | 多節點掃描配對 |
| `0x1103/0x1104` | STATUS_SET / STATUS_ACK | ~~要遠端改設定並落盤~~ → 已實作 `System.file_mode` / `System.fs_format` 子集（見 §2） |
| `0x200D` | FILE_MOVE | 要搬檔／改名 |
| `0x200E/0x200F` | FILE_PARTIAL_QUERY/RSP | 要斷點續傳 |
| `0x2011` | FILE_PROMOTE | SD 暫存 → root |
| `0x2212`–`0x2217` | LAST / PARTITION_STATUS / VERIFY | 要 OTA 歷史與細節驗證 |
