# 狀態 schema：路徑字典 → (id, 值)（唔存 key 字串）

> 2026-11-26。用家提出：舊專案 `mp_Net-Core/slave/schema/` 已經有「schema 做資料」嘅做法；
> 硬件有上限（SPI≤2、I2C≤2、UART≤3、I80≤2、GPIO≤48…），所以狀態 key 係**有限集合**。
> 與其存 JSON／存 key 字串，不如 **schema 定義「路徑 → u32 id ＋ 型別 ＋ 範圍 ＋ 預設」**，
> 儲存只寫 `(id, 值)`。ESP 硬件就咁多，id 空間綽綽有餘。

---

## 1. 舊專案 vs 新設計

| 舊 `slave/schema/*.json`（mp_Net-Core） | 新 `state-schema` |
|---|---|
| **command schema**：`cmd` id ＋ payload 欄位型別（u8/u16/str…） | **state schema**：路徑 ＋ key id ＋ 型別／範圍／預設 |
| 由 `schema_codec.py` 編成 dispatch／field 表，validator 用 viper 解碼 | 由 ncportgen 編成 C sorted table；JSON parser 同 state 儲存共用 |
| 目的：**收發快**（唔使每次 parse JSON） | 目的：**存得省＋讀得快**（唔存 key 字串、sparse 只存改過嘅） |

兩者同一哲學：**schema 係資料，編譯期變表；runtime 只做數值查找**。

---

## 2. Key id 佈局（u32，8/8/8/8）

```text
bit 31..24  domain   (sys／hw／driver／pixel／task…)
bit 23..16  unit      (0=sys、7=i80、8=spi、12=led…)
bit 15..8   index     (0..255：通道／lane／pin 編號)
bit  7..0   field     (每 unit 最多 256 個欄位)

例：0x00070005 = domain 0／unit 7（i80）／index 0／field 5（freq）
    0x00000002 = system.frame_interval_ms
    0x000C0005 = driver.led.px
    PIN 47     = 0x0002_2F_01（unit=pin、index=47、field=1）← 4-bit index 放唔落
```

- **2026-11-27 由 u16（4/4/4/4）改成 u32（8/8/8/8）**：原因是 PIN 有 48 支，4-bit
  index（0..15）根本放唔落；另外 P4／C3 的 I2S／I80／PARLIO 數量都唔同，唔想再綁死。
- 成本（實測）：blob 每筆 key 由 2 B varint → 3 B（id < 2^21），18 筆 = **+18 B**；
  store entry 由 28 B → 32 B，64 筆 = 2 KB（`h_int` 194 KB，冇壓力）。
- 一個 domain 256 unit × 256 index × 256 field ≈ 16.7M 個 key，實際用 < 0.01%。
- 生成器驗兩件事：`id != 0`（0 = 未設定／未啟動）＋ `(id >> 16) & 0xFF == unit id`。

---

## 3. 儲存格式 `NCSTATE`（schema 版，最省）

```text
統一格式：NCSTATE（state manager 唯一格式，內部自描述）——詳見 state-storage-format.md

header 26 B: "NCSTATE\0" | ver u16 | flags u16 | count u16 |
             src_hash u32 | payload_len u32 | blob_crc u32
record:      type u8 | key varint | len varint | value
             （type 1=uint 2=sint 3=bytes 4=bool；len 令 reader 可以跳過唔識嘅）

src_hash = **值集**（將寫入的 records）嘅 CRC32；開機：
  file 存在 && ver 夾 && src_hash == 現時 JSON hash && blob_crc 過 → 直接載入
  否則 → parse JSON → 重新生成（atomic 替換）→ log miss（old→new）
```

- **唔存 key 字串、唔存型別、唔存 default**：型別喺 schema 表；default 喺 L0 出廠。
- **sparse**：只存「同 default 唔同」嘅值；大部分 config 只改幾項 → blob 好細。
- 讀：`id → schema[index] → 型別/範圍`；未知 id 跳過（向前兼容）。
- 寫：`nc_state_set_*()` 先驗 schema（型別／範圍）→ 改 RAM store → 計 dirty；
  持久化仍然係「JSON → NCSTATE（boot 時重算）」，冇第二條半權威路徑。

### 實測（`modules/lab/state_schema_lab/schema_preview.py`）

| 檔案 | JSON | 通用 NCB2（有 key 字典） | **NCSTATE（schema id）** |
|---|---:|---:|---:|
| hardware.json | 5,992 B | 3,462 B | — |
| driver.json | 2,830 B | 1,618 B | — |
| 兩者合計 | **8,822 B** | 5,080 B | **85 B（省 99.0%）** |

> 85 B 係「Matrix 實際有改嘅 21 個 key」；schema 條目 25 個。全 config（含 PIN/PWM/UART/
> ETH/SDIO/tasks）估計 **150–400 B**，仍然比 JSON 省 95%+。
> 注意：NCSTATE 唔適合 pixel assets（陣列大、數量動態）——那些繼續用 NCB2／PXA2。

---

## 4. schema 實際長什麼樣（v2）

單一檔案 `schema/state-keys.json`：`units[]`（能力群組）＋ `keys[]`（每個 key 一行）。

```json
{
  "version": 2,
  "units": [
    { "id": 12, "name": "led", "desc": "燈鏈（driver.json 的 LED 段）" }
  ],
  "keys": [
    { "id": "0x0C05", "name": "NC_KEY_LED_PX", "unit": "led", "type": "u16",
      "min": 1, "max": 65535, "path": "driver.json:LED.px", "desc": "每條 lane 幾顆" }
  ]
}
```

- `unit` 一定要等於 key id 的 unit byte（生成器驗），「key 屬邊個能力」唔會同 id 打架。
- `type`：`u8`／`u16`／`u32`／`i8`／`i32`／`bool`／`enum`／`str`；`str` 要寫 `max_len`（唔含 NUL）。
- `enum` 要寫 `values[]`（由 0 連續排列），例如 LED backend：`none/i80/spi/rmt/parlio/direct/mock`。
- `min/max`：`nc_state_set_*()` 同 `nc_state_schema_check()` 共用同一條規則；超出＝明示失敗。
- **唔寫 `default`**：出廠值仍然只有一份（C 的 `nc_cfg_defaults_*`／Kconfig），避免第二個真相。

---

## 5. 同現有 JSON 層嘅關係

```text
人改：  /data/config/hardware.json（路徑名，例如 "I80": {"list":[...]}}）
          │  parser 用 schema 將路徑 → key id
          ▼
state：  nc_state（RAM：id → value；schema 表查型別/範圍）
          │  commit：只 encode dirty → NCSTATE blob
          ▼
落盤：  NVS blob（或 /data/config/state.ncst；header+id/值，sparse）
          │  開機：blob 優先；壞／版本唔夾 → 由 JSON（再壞 → L0 出廠）
```

- 三層套路不變：**L0 出廠 → L1 FAT JSON → L2 NCSTATE 狀態 blob → 壞了回退**。
- JSON 係唯一人讀格式；NCSTATE 係機讀格式；兩者由**同一份 schema** 對應。
- 冇 JSON 改動唔使重寫 NCSTATE（hash 對得上就跳過）。

---

## 6. 落地狀態（2026-11-26）

| 階段 | 狀態 |
|---|---|
| **P1** schema json＋生成器＋`NCSTATE` codec／host test | ✅ 完成 |
| **P2** 開機 JSON→`state.ncst`、hit／miss／self-heal、`.bak` | ✅ 完成 |
| **P3** driver 讀 state（render LED 第一條）＋**schema registry API** | ✅ 進行中（system／tasks／links 未接） |
| **P4** `status cfg` 顯示 `state`／`schema`／`act` | ✅ 完成 |

**風險**：schema 加／減 key 唔使改 code，但「由 cfg 造 record」仍然要 `nc_state_boot.c`
的一行（值從哪個 struct 來，schema 表達唔到）；漏咗唔會爆，只會係 sparse 冇寫。

---

## 7. magic 登記表（防止再撞名）

| magic | 用途 | 位置 |
|---|---|---|
| `NCB2` | 通用／動態狀態 blob（自描述，字典＋TLV）——⏸ 暫緩 | NVS／FAT |
| `NCSTATE` | **schema 化狀態 blob**（id＋varint，最省）——✅ **唯一要做** | NVS／FAT |
| `NCC2` | config 派生 cache（cfg_hash 綁 JSON） | FAT |
| `PXA2` | pixel assets 編譯 cache——⏸ 暫緩 | FAT |
| `NCLAYOUT` | layout 編譯 cache（原 PXLT，已改名）——✅ 現有 | FAT |
| `NCB1`… | 舊版／保留 | — |

新格式一律 **`NC` 字頭**，並先在此表登記。

---

## 8. 唯一入口：schema registry（2026-11-26）

**加／減能力只改一個地方**：`schema/state-keys.json`。
（圖：[schema-registry.svg](../00_pm/schema-registry.svg)，由 `python3 tools/config_diagram.py` 生成；
逐 key 一覽：[config-schema.md §1](../02_guides/config-schema.md)。）

```text
schema/state-keys.json
   │  python3 tools/gen_state_keys.py
   ├─→ include/nc_state_keys.h          key id＋NC_UNIT_*（C 名）
   ├─→ include/nc_state_schema_data.h   runtime 表的型別／counts／CRC
   └─→ nc_state_schema_data.c           表：id／unit／type／min／max／enum／path
        │
        ├─ nc_state_schema.c   查表／驗型別／驗範圍（boot 同 set 共用）
        └─ nc_state_api.c      讀寫唯一入口（nc_state_get_*／nc_state_set_*）
```

| 想做的事 | 只需要改 |
|---|---|
| 加一個設定值 | schema json 加一行 → 跑生成器 → rebuild；（要 boot 寫入的話）`nc_state_boot.c` 的 `build_records()` 加一行由 cfg 取值 |
| 加一個能力群組（unit） | schema json 的 `units[]` 加一項＋key 引用它 |
| 減一個能力 | schema json 刪 key（或整個 unit）→ 生成器 → rebuild；舊 blob 的未知 key 自動跳過（向前兼容） |
| 查某個 key | `nc_state_key(id)`／`nc_state_key_name("NC_KEY_LED_PX")` |
| 讀值 | `nc_state_get_u32/i32/bool/str()`（型別由 schema 決定，用錯函式回 false） |
| 寫值 | `nc_state_set_u32/i32/bool/str()`（schema 驗型別＋範圍；只改 RAM，見下） |

- `run_all.sh` 開頭跑 `gen_state_keys.py --check`：三份生成檔任何一份同 schema 唔同步 →
  測試直接失敗（防止 id 漂移）。
- `status cfg` 的 `schema`：`{"ver":2,"crc":4062868727,"keys":25,"units":4,"bad":0,"dirty":0}`；
  `bad` = boot 由 cfg 造出的 record 同 schema 唔一致（code／schema drift，會 log 出嚟）。
- **寫入語義**：`nc_state_set_*()` 只改 RAM store（`dirty` 計唔同 key 數）；
  reboot 仍然以 `/data/config/*.json` 為權威，永久改要改 JSON。
- 現時覆蓋：sys（3）／I80（5）／SPI（7）／LED（10）＝ **25 個 key、4 個 unit**；
  Matrix 的 `state.ncst` 18 筆／124 B。
