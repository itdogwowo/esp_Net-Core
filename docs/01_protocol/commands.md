# 指令使用現況與新增規則

> **用途**：回答兩個問題 ——「現在實作了哪些指令」與「要加新指令時該放哪裡」。
> **原則**：**能重用就不新增。** 舊專案的 `slave/schema/*.json` 是權威表，先查再決定。
> 相關：`slave/schema/*.json`（mp_Net-Core）、`transports.md`

---

## 1. 新指令的三條規則

1. **先查舊 schema**：`sys` / `status` / `file` / `ota` / `pixel` / `stream` / `bench` / `hw` / `now` / `heartbeat` / `audio`。
   有現成的就直接用，欄位也照它的順序與型別。
2. **真的沒有才加**，而且加在**語義所屬的域的最後一個空號**，不要另開新域。
3. **測試專用一律放 `0xFxxx`**，不得混進產品域。測試指令可以隨時刪，不影響對外契約。

---

## 2. 目前已實作（全部重用既有指令）

| CMD | 名稱 | 來源 schema | 狀態 |
|---|---|---|---|
| `0x1005` | SYS_TASK_QUERY | sys.json | ✅ |
| `0x1006` | SYS_TASK_RSP | sys.json | ✅ |
| `0x100F` | REBOOT | sys.json | ✅ |
| `0x1016` | SET_MASTER | sys.json | ✅ |
| `0x1101` | STATUS_GET（keys：空 / `?` / sys,mem,task） | status.json | ✅ |
| `0x1102` | STATUS_RSP（`status_json`） | status.json | ✅ |
| `0x1811` | BENCH_READY | bench.json | ✅ |
| `0x1812` | BENCH_DATA | bench.json | ✅ |
| `0x1813` | BENCH_RESULT | bench.json | ✅ |
| `0x1814` | BENCH_REPORT | bench.json | ✅ |
| `0x2201`–`0x2220` | OTA 子集（BEGIN/WRITE/END/ABORT/ACK/VERSION/CAPS/PROGRESS/STATE/ERROR/APPLY） | ota.json | ✅ |

### 測試專用（`0xFxxx`，可隨時刪）

| CMD | 用途 |
|---|---|
| `0xF001` DEV_BENCH | `size(u16) iters(u16) mode(u8)` → `0xF002` JSON。mode 0=decode、1=pack、2=erase 4K、3=write 4K、4=erase 64K |
| `0xF002` DEV_RSP | 上述回覆（`str_u16len` JSON） |
| `0xF003` HUB_BENCH | `iters(u32) [mode(u8)]` → `0xF004` JSON：雙核 hub 交換量測（writer core1 / reader core0）。`mode` bit0=1 用舊版次序（deadline 太晚）重現 read=0，回歸測試用 |
| `0xF004` HUB_RSP | `{iters,read,us,per_sec,torn,w,w_full,r_null,exit:[w,r],late}`；`exit` 1=完成 2=逾時 3=等不到 start |
| `0xF007` TX_BENCH | `frames(u32) size(u16) mode(u8)` → `0xF004` JSON：**上傳**（打包＋送出）量測。mode 0=同核（同一 task 打包後送出）、1=分核（core1 打包進插槽、decode task 送出） |
| `0xF004` 也會被 `0xF007` 使用 | `{mode,sent,packed,size,us,mb_s}` |
| `0xF008` BUS_STATS | `[reset:u8]` → `0xF009` JSON：每條 bus 的 `{name,mode,frame,frames,mb,copied,parse_ms,us_per_frame}` + `resync`。`copied=0` 就是零複製 |
| `0xF009` BUS_RSP | 上述回覆（`str_u16len` JSON） |

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
| `0x1103/0x1104` | STATUS_SET / STATUS_ACK | 要遠端改設定並落盤 |
| `0x2212`–`0x2217` | LAST / PARTITION_STATUS / VERIFY | 要 OTA 歷史與細節驗證 |
