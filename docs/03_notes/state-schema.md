# 狀態 schema：路徑字典 → (id, 值)（唔存 key 字串）

> 2026-11-26。用家提出：舊專案 `mp_Net-Core/slave/schema/` 已經有「schema 做資料」嘅做法；
> 硬件有上限（SPI≤2、I2C≤2、UART≤3、I80≤2、GPIO≤48…），所以狀態 key 係**有限集合**。
> 與其存 JSON／存 key 字串，不如 **schema 定義「路徑 → u16 id ＋ 型別 ＋ 範圍 ＋ 預設」**，
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

## 2. Key id 佈局（u16）

```text
bit 15..12  domain   (sys/hw/driver/pixel/task…)
bit 11..8   unit      (spi=1, i2c=2, uart=3, i80=4, pin=5, eth=6, sdio=7…)
bit  7..4   index     (0..15：通道／lane／項次)
bit  3..0   field     (每 unit 最多 16 個欄位)

例：spi[0].gpio.sck  = 0x1_1_0_2
system.frame_interval_ms = 0x0_0_0_2
driver.led.px        = 0x2_0_0_5   （driver domain=2、unit=led=0）
```

- 上限寫入 schema（`max_units`／`max_index`）；超出直接 parse error，唔會跌出 id 空間。
- 一個 domain 16 unit × 16 index × 16 field = 4096 個 key，總共 16 domain → 65,536。
  ESP 實際用唔到 1%（Matrix 全 config 約 60–100 個 key）。

---

## 3. 儲存格式 `NCS1`（schema 版，最省）

```text
header 16 B: "NCS1" | ver u16 | count u16 | src_hash u32 | blob_crc u32
body:  count × ( id varint , value zigzag-varint )   ← 按 id 升序

src_hash = 來源 JSON（config layers）嘅 CRC32；開機：
  file 存在 && ver 夾 && src_hash == 現時 JSON hash && blob_crc 過 → 直接載入
  否則 → parse JSON → 重新生成（atomic 替換）→ log miss（old→new）
```

- **唔存 key 字串、唔存型別、唔存 default**：型別喺 schema 表；default 喺 L0 出廠。
- **sparse**：只存「同 default 唔同」嘅值；大部分 config 只改幾項 → blob 好細。
- 讀：`id → schema[index] → 型別/範圍`；未知 id 跳過（向前兼容）。
- 寫：`nc_state_set(id, value)` 累積 dirty → `nc_state_commit()` 一次編碼寫入。

### 實測（`modules/lab/state_schema_lab/schema_preview.py`）

| 檔案 | JSON | 通用 NCB2（有 key 字典） | **NCS1（schema id）** |
|---|---:|---:|---:|
| hardware.json | 5,992 B | 3,462 B | — |
| driver.json | 2,830 B | 1,618 B | — |
| 兩者合計 | **8,822 B** | 5,080 B | **85 B（省 99.0%）** |

> 85 B 係「Matrix 實際有改嘅 21 個 key」；schema 條目 25 個。全 config（含 PIN/PWM/UART/
> ETH/SDIO/tasks）估計 **150–400 B**，仍然比 JSON 省 95%+。
> 注意：NCS1 唔適合 pixel assets（陣列大、數量動態）——那些繼續用 NCB2／PXA2。

---

## 4. schema 點寫（草案）

```json
{
  "version": 1,
  "domains": {
    "sys":    { "id": 0, "units": [
        { "name": "system", "id": 0, "fields": [
            { "name": "frame_interval_ms", "id": 2, "type": "u16", "min": 1, "max": 1000, "default": 20 },
            { "name": "num_pixels", "id": 3, "type": "u16", "min": 0, "max": 65535, "default": 0 }
        ]}
    ]},
    "hw":     { "id": 1, "units": [
        { "name": "spi", "id": 1, "max_index": 2, "fields": [
            { "name": "sck",  "id": 2, "type": "i8", "min": -1, "max": 48, "default": -1 },
            { "name": "mosi", "id": 3, "type": "i8", "min": -1, "max": 48, "default": -1 }
        ]}
    ]},
    "driver": { "id": 2, "units": [
        { "name": "led", "id": 0, "fields": [
            { "name": "enable", "id": 1, "type": "bool", "default": 0 },
            { "name": "px",     "id": 5, "type": "u16", "min": 1, "max": 65535, "default": 0 }
        ]}
    ]}
  }
}
```

- `type`：`bool`／`u8`／`u16`／`u32`／`i8`／`i16`／`i32`／`enum`（附 `values[]`）／`str`（固定長）。
- `min/max`：parse 時驗；超出＝invalid（明示，唔截斷）。
- `default`：同 L0 出廠對齊；**儲存只寫「同 default 唔同」嘅**。

---

## 5. 同現有 JSON 層嘅關係

```text
人改：  /data/config/hardware.json（路徑名，例如 "I80": {"list":[...]}}）
          │  parser 用 schema 將路徑 → key id
          ▼
state：  nc_state（RAM：id → value；schema 表查型別/範圍）
          │  commit：只 encode dirty → NCS1 blob
          ▼
落盤：  NVS blob（或 /data/config/state.ncs；header+id/值，sparse）
          │  開機：blob 優先；壞／版本唔夾 → 由 JSON（再壞 → L0 出廠）
```

- 三層套路不變：**L0 出廠 → L1 FAT JSON → L2 NCS1 狀態 blob → 壞了回退**。
- JSON 係唯一人讀格式；NCS1 係機讀格式；兩者由**同一份 schema** 對應。
- 冇 JSON 改動唔使重寫 NCS1（hash 對得上就跳過）。

---

## 6. 落地（估時）

| 階段 | 工作 | 估時 |
|---|---|---|
| **P1** | `state-schema.json`（skeleton：sys/hw/driver 三 domain，~60 key）＋ncportgen 編 C 表＋`NCS1` encode/decode＋host test（round-trip、範圍、未知 id） | 1 日 |
| **P2** | `nc_state` v2：`nc_state_set_id()`、dirty→commit、NVS blob「NCS1」＋版本／CRC；開機 JSON→NCS1 遷移，壞檔回退 | 1 日 |
| P3 | JSON parser 改為「路徑 → id」寫入 state（hardware 先行，driver／system 跟） | 1 日 |
| P4 | `status cfg` 顯示 blob size／key 數／命中率；schema 版本唔夾即拒讀 | 0.5 日 |

**風險**：schema id 一改就要 bump schema ver（NCS1 header 有 ver，舊 blob 自動拒讀）；
array 索引固定（通道上限寫死）——但硬件本身就有限，正好利用。

---

## 7. magic 登記表（防止再撞名）

| magic | 用途 | 位置 |
|---|---|---|
| `NCB2` | 通用／動態狀態 blob（自描述，字典＋TLV）——⏸ 暫緩 | NVS／FAT |
| `NCS1` | **schema 化狀態 blob**（id＋varint，最省）——✅ **唯一要做** | NVS／FAT |
| `NCC2` | config 派生 cache（cfg_hash 綁 JSON） | FAT |
| `PXA2` | pixel assets 編譯 cache——⏸ 暫緩 | FAT |
| `NCLT` | layout 編譯 cache（原 PXLT，已改名）——✅ 現有 | FAT |
| `NCB1`… | 舊版／保留 | — |

新格式一律 **`NC` 字頭**，並先在此表登記。
