# 統一狀態系統（NCSTATE）現況總結

> 2026-11-26。用家要求「先停低，將目前成果寫成總結／文件」。
> 呢份係**現況總結**：設計、實作、量測、驗證、未做；細節規格見
> [`state-storage-format.md`](state-storage-format.md)、
> [`state-schema.md`](state-schema.md)、[`state-activation.md`](state-activation.md)。
> 歷史演進見 git log。

---

## 1. 一句話

**設定同狀態只有一個持久格式：`NCSTATE`**（檔案 `state.ncst`，magic `"NCSTATE\0"`），
內部**自描述**、**sparse**（只存同出廠預設唔同嘅值）、有 `src_hash` 版本核對、
有 `.bak` last-known-good；pixel 的 layout 快取係唯一例外（`<key>.layout`）。
**激活（邊個 unit 會用）在生成期決定**，由 ncportgen 編成 `nc_port.c` 的 const 表。

---

## 2. 資料流（開機）

```text
FAT JSON（L1，人改；硬件／driver／system／tasks／resources）
   │  nc_cfg_boot()：讀檔 + cJSON + 型別化（順便計 src_hash，表驅動 CRC）
   ▼
cfg structs（RAM）──┐
                    ├─→ nc_state_boot_sync()：
                    │     1. peek /data/config/state.ncst
                    │     2. load 舊 blob → RAM store，同 cfg records 對拍
                    │     3. 判斷：
                    │        hash 對 + 值全中      → hit（唔寫）
                    │        hash 唔對 + 值一樣    → 唔寫（避 762 ms WL 寫盤）
                    │        值有唔同／壞檔        → 重建（atomic：.tmp→.bak→rename）
                    │     4. RAM store 成為 driver 的讀取來源
                    ▼
driver（例：render_task LED）── nc_state_read_*() ──→ 值
```

- **L0 出廠預設**：firmware const（`nc_port.c`／內建表），永遠存在。
- **L1 FAT JSON**：完整、非 sparse、人手改；最高權威。
- **L2 `state.ncst`**：sparse 派生快取（只存改動值），有 CRC＋src_hash。
- **L2b `state.ncst.bak`**：每次寫新檔前保留上一份 → last-known-good。
- 任何一層壞 → 唔會靜靜變零：壞 blob 由 JSON 重生；壞 JSON 回 L0（大聲 warn）。

---

## 2.5 開機語義（2026-11-27 定案，S3）

| 情況 | 邊個贏 | 行為 |
|---|---|---|
| blob `src_hash` ＝ cfg 值集 hash（config 冇改） | **state（override）** | 保留 blob 值；只 prune 停用 unit 的舊記錄 |
| `src_hash` 唔同（config 改過值） | **JSON** | 由 cfg 重建（舊檔 → `.bak`），回寫值清掉 |
| 冇檔／壞檔 | JSON | 重建 |

- 為什麼唔可以用「值同 cfg 唔同」做重建條件：回寫值本身就係「同 cfg 唔同」，
  咁樣一 reboot 就會被清走（2026-11-27 實測 frame_ms 25 → 20，就係呢個 bug）。
- `src_hash` 仍然係「config 值集」的 CRC；回寫唔改佢。改一句 `_comment` 唔會令
  override 失效；改任何設定值就會（可預期）。
- 落盤 API：`nc_state_persist()`（唔可以叫 `nc_state_commit()`——`nc_state.c` 有個
  NVS 世代的同名函式，實測 linker 揀錯，寫入回 OK 但 blob 冇變）。

## 3. 檔案格式（NCSTATE）

```text
Header 26 B:
  0   magic[8]      = "NCSTATE\0"（ASCII；唔加版本數字）
  8   ver u16       = 1（版本喺 header，唔喺名）
  10  flags u16
  12  count u16     record 數
  14  src_hash u32  來源 JSON 的 CRC32（版本核對）
  18  payload_len u32
  22  blob_crc u32  payload CRC32

Record（自描述，逐筆）：
  type u8      1=uint 2=sint 3=bytes/str 4=bool
  key  varint  schema key id
  len  varint  value 長度（令 reader 可以跳過唔識嘅欄位）
  value        varint / zigzag / raw bytes
```

- **sparse**：同預設一樣嘅值唔寫；Matrix 現時 **18 筆＝124 B**。
- **未知 type**：靠 `len` 跳過（向前兼容），不會讀錯。
- **schema**：`schema/state-keys.json` 係 key 唯一 source of truth（現 25 個：
  sys 3／I80 5／SPI 7／LED 10）；`tools/gen_state_keys.py` 生成
  `nc_state_keys.h`；`run_all.sh` 開頭 `--check`，唔同步就測試失敗。

---

## 4. 已實作（C）

| 模組 | 檔案 | 做咩 |
|---|---|---|
| **codec** | `sys/state/nc_state_blob.c/h` | 統一格式 encode／decode；CRC／magic／ver／截斷檢查；未知 type 跳過；`nc_state_blob_rec_crc()`＝值集 CRC（cache 失效判斷） |
| **檔案層** | `sys/state/nc_state_file.c/h` | FAT 讀寫；atomic（`.tmp`→舊檔 `.bak`→rename）；hash helpers |
| **開機同步** | `sys/state/nc_state_boot.c/h` | 由 cfg 造 records → 對拍 → hit／重建；結果俾 status |
| **RAM store** | `sys/state/nc_state_store.c/h` | 固定陣列（**96 筆 × 12 B**）＋對拍 cmp；`s_store` 實測 1,156 B；開機暫存 `s_recs` 768 B 已搬 PSRAM（只剩 4 B pointer） |
| **讀寫 API** | `sys/state/nc_state_api.c/h` | `get_u32/i32/bool`＋`set_*`（set 只改 RAM＋dirty）；`bind_u32/i32()` 回傳 stable 位址給熱路徑。**只存數值**——字串不入 state（名字在 schema 字典） |
| **schema registry** | `sys/state/nc_state_schema.c/h`＋`nc_state_schema_data.[ch]` | 查表（id↔name↔unit）、驗型別／範圍／enum、boot drift 檢查 |
| **key schema** | `schema/state-keys.json`＋`tools/gen_state_keys.py` | **唯一 source of truth**：id／unit／型別／範圍／enum → 生成三份 C 檔＋同步檢查 |
| **cfg hash** | `sys/config/nc_cfg.c/h` | boot 讀 JSON 時順便累計原始 hash（表驅動 CRC）；state／FAT 比對共用 |
| **driver 讀 state** | `tasks/render_task/nc_render_task.c` | LED 10 個 key 由 state store 覆寫（冇就用 cfg，安全 fallback） |
| **activation** | `tools/ncportgen.py` → `nc_port.c/h` | 生成期表 `s_act[]`（unit／level／enabled）＋src_hash＋config hash |
| **status** | `main.c` `status_put_cfg` | `"t"`（cfg_us／state_us）、`"state"`（hit／cmp／mism／n／bytes／store）、`"act"`（src／n／en／units／fat／match）、`"schema"`（ver／crc／keys／units／bad／dirty） |

### 現時 key（18 筆＝Matrix 實際有值）

sys 3（frame_interval_ms／num_pixels／log_debug_mode）
I80 5（data count／wr／dc／cs／freq）
LED 10（enable／backend／bus／lanes／px／fmt／clk／bitcells／t0h／t1h）
SPI 7：schema 有，但 Matrix SPI `enable=0` → 唔寫（sparse 正確）

---

## 5. 激活（生成期決定）

- `wiring.json`＋`driver.json`（＋project sdkconfig）→ ncportgen：
  * `preset_activation.json`（人睇的鏡像）；
  * **`nc_port.c` 的 `s_act[]` const 表**（firmware 真係讀它）＋
    `nc_port_activation()`／`nc_port_activation_src_hash()`／`nc_port_config_hash()`。
- `status cfg.act` 顯示：`src`（activation 來源 hash）、`n`、`en`、`units`、
  `fat`（FAT 上 hardware＋driver 的 hash）、`match`（同 build 預置一唔一致）。
- 語義：`enabled=0` → 零 GPIO／零 buffer；activation 係 build-time，
  **改 enable 要 rebuild**；其他值改完 reboot 生效（FAT 仍然最高權威）。
- Matrix 現況（2026-11-26 21:30）：`{"src":4015395272,"n":22,"en":4,
  "units":"usb,i2c,i80,led","fat":2960301093,"match":1}`。
  （`src` 係 ncportgen 輸入的 hash：wiring／driver／project sdkconfig.defaults；
  改預設就會變，屬正常。）

---

## 6. 量測（Matrix、idf=v6.1.1 WSL、3 次開機穩定）

| 階段 | 優化前 | 現在 | 備註 |
|---|---:|---:|---|
| `cfg_us`（5 JSON parse＋型別化＋hash） | 29.1 ms | **30.1 ms** | 加 raw hash（表驅動 CRC）＋~1 ms |
| `state_us`（hit） | 24.4 ms | **8.2 ms** | 唔再重讀 13 KB JSON |
| `state_us`（值有變、要寫檔） | — | **762 ms** | WL FAT 寫 124 B＋commit；改值時一次性 |
| `state_us`（hash 變但值一樣） | 762 ms | **8.3 ms** | 保留舊檔、唔寫盤（sha256 前後相同實測） |

- 量測方法：B02 用 `esp_timer_get_time()` 包 `nc_cfg_boot()`／`nc_state_boot_sync()`，
  `status cfg.t = {cfg_us, state_us}`。
- **skip JSON parse** 最多省 ~30 ms，但要 schema 覆蓋 links／routes／tasks／resources＋
  setter，工作量大、風險高；暫時**唔做**（已量測、已記錄理由）。

---

## 7. 記憶體（PSRAM＝0）

| 項目 | 大小 | 位置 |
|---|---:|---|
| `state.ncst` | **124 B**（18 筆） | FAT（flash；+4 KB cluster，`.bak` 另 4 KB） |
| RAM store | **~430 B**（18 筆；上限 64 筆 ≈1.5 KB） | internal RAM |
| decode 暫態 | ~124 B | internal（一次） |
| **PSRAM** | **0 B** | — |

- 舊路徑（JSON→struct）常駐 3,552 B＋開機 cJSON 暫態 20–35 KB；
  state 系統現時同 cfg 並存（尚未取代），所以未見完整節省；
  driver 已開始改讀 state（render LED）。

---

## 8. 保護層（失敗退還）

| 層 | 位置 | 失效時 |
|---|---|---|
| L0 出廠預設 | firmware const | 永遠存在 |
| L1 FAT JSON | `/data/config/*.json` | 壞／失 → 回 L0（同以前一樣） |
| L2 `state.ncst` | FAT | 壞／版本唔夾 → 由 L1 重生 |
| L2b `.bak` | FAT | last-known-good，可手動還原 |
| 對拍 | boot 時 store vs cfg | 唔一致 → 當 miss 重寫（self-heal，防舊 cache 靜默生效） |

---

## 9. 操作／驗證方法（上板）

```bash
# 狀態一覽（state／act／t 都在 cfg 內）
python3 tools/nc4.py --port COM24 status cfg

# 想強制 rebuild state.ncst：刪檔 → reboot
python3 tools/nc4file.py …（或 WebMaster → IDE 刪 /config/state.ncst）→ reboot
# 開機見 NCSTATE miss → 重建；舊檔會變 .bak

# 想回復上次狀態：把 state.ncst.bak 改名覆蓋 state.ncst → reboot（hash 對就 hit）
```

- IDE（`http://127.0.0.1:8011/#ide`）可以見到 `/config/state.ncst` 同 `.bak`。
- WebMaster scan 偶爾留 stale manifest（已知問題）：refresh／重新 scan 即可。

---

## 10. 測試

- Host test：**25/25**（`stateBlob_test`、`stateFile_test`、`stateSchema_test`、`stateStore_test`；
  run_all 開頭另跑 schema 三份生成檔 `--check`）。
- 上板驗證（本輪）：
  * hit：`state:{"hit":1,"cmp_ok":1,"n":18,"bytes":124,"store":18}`；
  * self-heal：改 `0x0C01` 值＋合法 CRC → miss（mism=1、first=0x0C01）→ 重生；
  * 值一樣唔寫：`system.json` 加 newline → `rebuilt=0`、state.ncst sha256 不變；
  * activation：`act.match=1`（FAT 同 build 預置一致）；
  * 12 次以上 reboot 穩定，4 條 pixel 任務 `level=4 started=1`。

---

## 11. 未做／已知限制

| 項目 | 狀態 | 備註 |
|---|---|---|
| hit 時跳過 JSON parse | ⏸ 未做 | 已量到最多省 ~30 ms；要補齊 schema＋setter，優先度低 |
| schema registry（查表／驗證／讀寫 API） | ✅ 完成 2026-11-26 | 25 key／4 unit；`status cfg.schema`；host test 25/25 |
| schema 覆蓋寫入落盤（set → JSON） | ⏸ 未做 | 現時 set 只改 RAM；永久改仍然改 JSON（避免半權威路徑） |
| state 真正取代 cfg struct | ⏸ 部分 | 目前 driver 開始讀 state（render LED）；system／tasks 未接 |
| schema 覆蓋 links／routes／tasks／resources | ⏸ 未做 | 做齊才可 skip parse |
| 真正改值時的 762 ms 寫檔 | ⚠ 已知 | 值一樣已避免；可再考慮延後到 boot 後寫 |
| `status` 命中率／歷史統計 | ⏸ 未做 | 現時只有本次開機結果 |
| SPI records 實機 | ✅ 程式有 | Matrix SPI enable=0 → 唔寫（sparse） |
| pixel layout 快取 | ✅ 已有 | `.layout`／`NCLAYOUT`；state 例外 |
| 曾計劃但取消 | ❌ | `NCB2`／`NCC2`／`PXA2`（文件保留防撞名） |

---

## 12. 主要 commits（未 push）

`65fde01` key schema 生成器＋SPI＋run_all 檢查
`e29ebbf` 激活表編入 nc_port.c＋status act
`5110f1a` FAT config hash 比對（act.fat/match）
`1e62b5f` parse 順便計 hash（state hit −16.2 ms）＋開機量測
`4efe9f8` 量測文件（cfg 30.1／state hit 8.2／miss 762 ms）
`44c3a9a` 值一樣唔寫盤（避 762 ms）
（之前：`bd70185` 統一格式定案、`a144855` P2 第一步、`0827045` RAM store＋self-heal、
`708c6cb` driver 讀 state、`87b13a0` 檔名 `state.ncst`…）

---

## 13. 名詞對照（避免混亂）

| 名 | 意思 | 狀態 |
|---|---|---|
| **NCSTATE**（`state.ncst`） | 狀態／設定值機器版 | ✅ 實作中 |
| **NCLAYOUT**（`<key>.layout`） | pixel layout 快取 | ✅ 已有 |
| JSON | 人手改的源頭 | ✅ 一直用 |
| NCB2／NCC2／PXA2 | 曾計劃的其他格式 | ❌ 取消／暫緩 |
