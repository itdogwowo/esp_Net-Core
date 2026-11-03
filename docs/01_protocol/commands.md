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
4. **handler 一律住在 `components/action/`**。`action/` 是**唯一**有指令處理程式的地方；
   `main.c` 只做裝配（建立 decoder → 呼叫各 action 的 `*_register()`），一條 handler 都不該有。

   **目錄名 = 上游 schema 的 group 名**（上游 `slave/action/<group>_actions.py` 就是這個規則）：

```text
components/action/
├─ sys_action/      0x1xxx  sys 域（REBOOT / SET_MASTER / SYS_TASK_* / NET_TUNE …）  ⬜ 待建
├─ status_action/   0x11xx  status 域（STATUS_GET / SET / ACK）                      ⬜ 待建
├─ file_action/     0x20xx  file 域（FILE_QUERY / ERROR_RSP）                        ✅ 已有
├─ ota_action/      0x22xx  ota 域                                                   ✅ 已有
├─ bench_action/    0x18xx  bench 域（BENCH_READY / DATA / RESULT / REPORT）          ⬜ 待建
└─ test_action/     0xFxxx  測試區（上游冇這個 group，所以只能自訂名）                ⬜ 待建
```

> **現況違規**：`main.c` 的 `s_core_actions[]` 有 15 條，但其中 **10 條是 `0xFxxx` 測試指令**
> （DEV_BENCH / HUB_BENCH / TX_BENCH / BUS_STATS / DEV_SLOW / UART_BENCH / NET_TUNE /
> BENCH_READY / BENCH_DATA / BENCH_RESULT），真正的產品指令只有
> `0x1005 SYS_TASK_QUERY`、`0x100F REBOOT`、`0x1016 SET_MASTER`、`0x1101 STATUS_GET`、
> `0x1105 ROUTE_GET` 五條。
> 所以現在「想找某條指令的實作」要分兩個地方找。搬過去之後，
> **看到 `action/` 就知道那裡有指令**，`main.c` 只剩開機順序。
>
> 之前我寫的 `core_action` 是錯的 —— 沒有這個 group。`0x1xxx` 在上游屬於 **`sys`**。



---

## 2. 目前已實作（全部重用既有指令）

| CMD | 名稱 | 來源 schema | 狀態 |
|---|---|---|---|
| `0x1005` | SYS_TASK_QUERY | sys.json | ✅ |
| `0x1006` | SYS_TASK_RSP | sys.json | ✅ |
| `0x100F` | REBOOT | sys.json | ✅ |
| `0x1016` | SET_MASTER | sys.json | ✅ |
| `0x1101` | STATUS_GET（keys：空 / `?` / sys,mem,task,modules,**provides**,storage,fx[,net]） | status.json | ✅ |
| `0x1102` | STATUS_RSP（`status_json`） | status.json | ✅ |
| `0x1811` | BENCH_READY | bench.json | ✅ |
| `0x1812` | BENCH_DATA | bench.json | ✅ |
| `0x1813` | BENCH_RESULT | bench.json | ✅ |
| `0x1814` | BENCH_REPORT | bench.json | ✅ |
| `0x2201`–`0x2220` | OTA 子集（BEGIN/WRITE/END/ABORT/ACK/VERSION/CAPS/PROGRESS/STATE/ERROR/APPLY） | ota.json | ✅ |
| `0x2005` | FILE_QUERY（path → 存在/sha256/size/free/pending） | file.json | ✅ 唯讀 |
| `0x2006` | FILE_QUERY_RSP | file.json | ✅ |
| `0x2010` | FILE_ERROR_RSP（`err_busy=1` = FAT 目前在 host 手上） | file.json | ✅ |
| `0x1103` | STATUS_SET（目前只認 `System.file_mode`：0=交還 host、1=裝置接管） | status.json | ✅ 子集 |
| `0x1104` | STATUS_ACK（`ok` + `message`） | status.json | ✅ |
| `0x1105` | ROUTE_GET（空 payload → 回開機時定下的路由表） | status.json | ✅ |
| `0x1106` | ROUTE_RSP（`routes_json`） | status.json | ✅ |

### `0x1105 ROUTE_GET`：為什麼要一條查詢指令

路由是**開機時**由 `system.json` 的 `routes[]` 讀進來、算一次就固定的（見
[../03_notes/routing.md](../03_notes/routing.md)）。它不隨時間變，但**改 config 之後
要能確認板子真的吃到了哪一份** —— 沒有這條指令就只可以看 UART 開機 log。

回覆長這樣（`nc4_router_json()` 產生，每條線約 25 B）：

```json
{"set":1,"core0":3,"core1":1,
 "routes":[{"line":"nctx","core":0},{"line":"CDC","core":0},
           {"line":"UART","core":1},{"line":"TCP","core":0}]}
```

- `set` = 1 表示表真的設過（0 = 冇人叫過 `nc4_router_set()`，router 等於冇開）。
- `core0` / `core1` = 各有幾條線派去第 0 / 第 1 核；`0` 代表該核的 decode 任務
  **不會被建立**（除非有 `nc4_router_keep()`）。
- 表裡只有 `line → core`，**沒有方向**。`out` 裡的 link（轉發）目前**未實作**：
  讀 config 時只取每行第一個 `decodeN` 當核心，其餘 `out` 成員被忽略。

### `0x1101 STATUS_GET`：KEY 只是「揀邊幾份報告」，不是「查一個值」

> 常問：「不是輸入 KEY 就回覆嗎？為什麼會有 2.4 KB 那麼大？」

因為 KEY 揀的是**報告**，不是**欄位**。`sys` / `mem` / `tasks` / `provides` / `storage` / `fx`
每一個都是一整份結構化報告，不是一個數字：

| KEY | 內容 | 大約大小 |
|---|---|---|
| `sys` | 版本、cID、晶片、運行時間、開機原因 | ~200 B |
| `mem` | 每個 class 的 plan 帳（宣告 / carve / 用量 / 峰值） | ~1,500 B |
| `tasks` | 每個任務 × 每個 loop 的 core / level / count / avg / max / over / wrap | ~2,000 B |
| `provides` | 誰提供什麼（buffer / JSON / 數值） | ~900 B |
| `storage` | FAT 擁有者、`/ro` vs `/data`、free、pending | ~100 B |
| `fx` | 每個 `.fxe` 的 manifest（kind / core / fps / px / mem / 指令） | 1,000–2,600 B |
| `net` | IP / MAC / link 狀態（只在 P4） | ~300 B |

**空 KEY ＝ 全部**，所以加起來 ≈5.8 KB，而回覆幀上限是 `NC4_REPLY_JSON_MAX` = 2,600 B。
所以「空 KEY」的語意是**盡力而為**，不是「保證全部」：

- 砌得落就照砌，砌唔落就**停手**，改回一個**合法**的物件
  `{"truncated":1,"cap":2600,"tips":"keys=sys,storage,mem,provides,tasks,fx 分批問"}`。
- 順序是**由便宜到貴**（sys → storage → mem → provides → tasks → fx → net），
  所以截斷時你**一定**拿到便宜那幾份，貴的（`tasks` / `fx`）才可能被切走。
- 想要全部就**分批問**：先 `keys=sys,mem`，再 `keys=tasks`，再 `keys=fx`。
  每份都遠低於 2,600 B，一定完整。

> 為什麼不把上限提到 8 KB？因為回覆幀要放進 `NC4_MAX_FRAME`（8,205 B），
> 而那個 8,192 是**經驗參數**（見 [frame.md](frame.md)），不是可以隨便借的空間。
> 一份 5.8 KB 的 status 會把整條線的 tx 佔住幾十 ms，其他回覆全部排隊 ——
> 分批問反而快。

### FAT 為什麼不用新指令

換 FAT 的擁有者不需要新指令號 —— 那是「一個具名的狀態」，走既有的
`0x1103 STATUS_SET`：`{"System":{"file_mode":1}}`。

讀取（`0x2005`）**任何模式都答得到**：主機擁有 FAT 時走唯讀視圖（`/ro`），
回覆的 `pending=1` 表示「可能過期或半截」；裝置接管時走 `/data`，`pending=0` 是真值。
只有**寫入**才需要 `file_mode=1`。設計與實測見
[../03_notes/fat-ownership.md](../03_notes/fat-ownership.md)。

### 測試專用（`0xFxxx`，可隨時刪）

> ⚠️ **這一整段是假的。** `0xFxxx` 不是產品契約、不在上游 `slave/schema/*.json` 裡、
> 也不保證跨版本存在。它們只是「量測某個機制有沒有真的做到」的工具：
> 吞吐、零複製、雙核 hub 交換、慢工作延後。**要接產品的東西，一律不要用 `0xFxxx`。**
>
> 其中 `0xF005` / `0xF006` 連「工作」都是假工作（`vTaskDelay` 模擬寫 flash、UART 自環 bench）——
> 它們存在的唯一目的是**證明「慢工作被搬出解碼路徑」這件事真的有效**，
> 不是提供任何實際功能。真正會用到慢工作佇列的，是之後的 OTA apply、config 落盤、FAT 同步。

| CMD | 用途 |
|---|---|
| `0xF001` DEV_BENCH | `size(u16) iters(u16) mode(u8)` → `0xF002` JSON。mode 0=decode、1=pack、2=erase 4K、3=write 4K、4=erase 64K |
| `0xF002` DEV_RSP | 上述回覆（`str_u16len` JSON） |
| `0xF003` HUB_BENCH | `iters(u32) [mode(u8)]` → `0xF004` JSON：雙核 hub 交換量測（writer core1 / reader core0）。`mode` bit0=1 用舊版次序（deadline 太晚）重現 read=0，回歸測試用 |
| `0xF004` HUB_RSP | `{iters,read,us,per_sec,torn,w,w_full,r_null,exit:[w,r],late}`；`exit` 1=完成 2=逾時 3=等不到 start |
| `0xF005` DEV_SLOW | `ms(u16)` → 排進慢工作佇列，worker 延遲 ms 後回 `0xF004` JSON（`{"slow_ms":N,"deferred":1}`）。**假工作**：只是 `vTaskDelay`，用來證明 decode task 沒有被卡住 |
| `0xF006` UART_BENCH | `baud(u32) frames(u16) size(u16)` → 交給 worker 做 UART 自環吞吐量測，回 `0xF004` JSON。**假工作**：測的是 UART 本身，不是產品功能 |
| `0xF007` TX_BENCH | `frames(u32) size(u16) mode(u8)` → `0xF004` JSON：**上傳**（打包＋送出）量測。mode 0=同核（同一 task 打包後送出）、1=分核（core1 打包進插槽、decode task 送出） |
| `0xF008` BUS_STATS | `[reset:u8]` → `0xF009` JSON：每條 bus 的 `{name,mode,frame,frames,mb,copied,parse_ms,us_per_frame}` + `resync`。`copied=0` 就是零複製 |
| `0xF009` BUS_RSP | 上述回覆（`str_u16len` JSON） |
| `0xF00A` NET_TUNE | `split(u8) core(u8) [frame_align(u8)]` → `0xF004` JSON：把 RX 收斂模式寫進 NVS（`net/rx_split`、`net/rx_core`、`net/frame_align`），**下次開機生效** |

`0xF004` 是共用的回覆號（HUB / SLOW / UART / TX / NET_TUNE 都用它），因為它們都是「量測結果」。

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
| 檔案上傳 | MSC（拖檔），不需要 `0x20xx` |
| 效能量測 | `0x1811`–`0x1814`（產品域）＋ `0xF001`（開發用） |

---

## 4. 還沒實作但有 schema 的（等需求出現再做）

| CMD | 名稱 | 什麼時候需要 |
|---|---|---|
| `0x1007` | SYS_TASK_SET | 要執行期改 task 的 affinity |
| `0x100D/0x100E` | IDENTIFY_REQ/RSP | 多節點掃描配對 |
| `0x1103/0x1104` | STATUS_SET / STATUS_ACK | ~~要遠端改設定並落盤~~ → 已實作 `System.file_mode` 子集（見 §2） |
| `0x2212`–`0x2217` | LAST / PARTITION_STATUS / VERIFY | 要 OTA 歷史與細節驗證 |
