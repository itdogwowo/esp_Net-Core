# 統一狀態格式：`NCSTATE`（state manager 唯一格式）

> 2026-11-26。用家定案：
> 1. **全部統一一個格式＋內部自描述**；
> 2. **pixel 系列例外**——佢可以保留自己一個格式（layout 大表）；
> 3. **state manager 只有一個格式**；
> 4. magic **唔加版本數字**（`"NCSTATE1"` 個 `1` 係我加錯；版本放 header `ver`）。

---

## 1. 定案（兩個格式，冇第三個）

| 用途 | 檔案 | magic（8 byte ASCII） | 內容 |
|---|---|---|---|
| **狀態／設定值**（state manager 唯一） | `/data/config/state.bin` | `"NCSTATE"`＋`\0` | 所有 unit／裝置嘅值（sparse、self-describing） |
| **pixel layout 快取**（例外） | `/pixel/cache/<key>.layout` | `"NCLAYOUT"` | 編譯後嘅大 binary 表（aprog／job／runs） |

- **冇第三個格式**。之前提過嘅 `NCB2`／`NCC2`／`PXA2`／`NCS1`／`NCLT` **全部作廢**，
  只喺文件保留一行「作廢」防撞名。
- **為什麼 magic 唔加 `1`**：版本係 header 嘅 `ver u16`（=1）負責；名字要保持穩定，
  咁 hexdump 見到 `"NCSTATE"` 就知係咩，唔會隨版本改名。

---

## 2. `NCSTATE` 佈局（內部自描述）

### Header（24 B，固定）

```text
offset  size  field
0       8     magic = "NCSTATE\0"
8       2     ver        （=1；格式版本，改 schema 就 bump）
10      2     flags      （保留；bit0=有 src_hash）
12      2     count      （record 數）
14      4     src_hash   （來源 JSON 嘅 CRC32；版本確認用）
18      4     payload_len
22      4     blob_crc   （payload CRC32）
```

### Record（自描述，逐筆）

```text
type   u8      1=無號整數  2=有號整數  3=bytes/字串  4=bool
key    varint  schema 嘅 key id
len    varint  value 佔幾多 byte
value  ...     （varint／zigzag／raw bytes）
```

**自描述嘅意思**：
- reader 淨係靠 `type + len` 就知點解讀同**跳過唔識嘅欄位**（向前兼容）；
- schema 表（id→名／型別／範圍／預設）只係**加速同驗證**，唔係唯一解讀途徑；
  schema 版本舊咗都唔會讀錯，只會「唔識就跳過」；
- 唔存長路徑名（`"hardware.i80.0.wr"`），只存 `u16 id`，慳位。

### 例：Matrix 的 9 筆 record（hexdump）

```text
4e 43 53 54 41 54 45 00    "NCSTATE\0"
01 00 00 00 09 00           ver=1 flags=0 count=9
... src_hash / payload_len / blob_crc ...
01 81 0e 01 01             type=uint key=0x901 len=1 val=1   （i80.data count）
02 82 0e 01 01             type=sint key=0x902 len=1 val=-1  （wr）
...
03 86 18 07 06 "ws2812"    type=bytes key=0xc06 len=7       （format 字串）
```

---

## 3. 大小（實測）

| 項目 | 大小 |
|---|---:|
| Matrix hardware.json＋driver.json | 8,822 B |
| **NCSTATE blob（統一格式）** | **80 B**（24 header＋56 records／9 筆） |
| 每筆平均 | 6.2 B（type 1＋key 1–2＋len 1＋value 1–5） |
| 舊構想（id＋值，無 type/len） | ~52 B |
| **統一格式代價** | +28 B（換嚟自描述＋可跳過未知欄位） |
| worst-case（380 key 全寫） | **≈ 24 + 380×8 ≈ 3.1 KB**（NVS 4 KB blob OK） |
| 典型（只開 i80＋led） | ~100–150 B |

> 80 B vs 8,822 B = **省 99.1%**；而且因為 sparse（只存同預設唔同嘅值），
> 用戶改幾項就只寫幾項。

---

## 4. 開機流程（版本確認）

```text
讀 FAT JSON（hardware/driver/system/tasks/resources，固定次序）→ 算 src_hash
├─ state.bin 存在 && ver 夾 && src_hash 同 && blob_crc 過 → 【hit】直接載入
└─ 否則 → parse JSON → 重新生成 state.bin（atomic：.tmp → rename）→ log【miss old→new】
             └─ JSON 壞 → 回 L0 出廠預設（大聲 warn）
```

- **hit**：開機唔使 parse JSON，直接將 record 填入 state（快）。
- **miss**：重建一次；下次 hit。
- 舊版本／壞檔：**明示**（log＋`status`），唔靜默。
- 檔案可以手動刪：刪完只係 miss 一次，唔會失資料（JSON 仍係源頭）。

---

## 5. Pixel 例外（`.layout`）

layout 係「編譯後大表」（aprog／job／runs，幾百 B 到 32 KB），逐筆 record 開銷太大，
所以**保持自己一個格式**：

```text
" layout" header：magic "NCLAYOUT" | ver u32 | key u32 | px u32 |
                  naprog/njobs/nruns/nvals | payload_bytes | crc32
```

- 檔名 `<key>.layout`；`key` 已經係 mode／map／assets 內容 hash（等同 src_hash）。
- 刪咗自動重建（純快取）；舊 `.pxlt`／`.nclt` 自動當壞檔。

---

## 6. magic 登記表（現行）

| magic | 用途 | 狀態 |
|---|---|---|
| `"NCSTATE"` | 狀態／設定值（state manager 唯一格式） | ✅ 實作中（P1） |
| `"NCLAYOUT"` | pixel layout 快取 | ✅ 已上板（`.layout`） |
| `"NCS1"`／`"NCLT"`／`"NCB2"`／`"NCC2"`／`"PXA2"`／`"PXLT"` | 舊代號 | ❌ 作廢，唔再用 |

**命名規矩**：
1. magic 用 ASCII 全名、**唔加版本數字**（版本喺 header `ver`）。
2. 檔案名講用途（`state.bin`、`<key>.layout`）。
3. 新格式先登記呢張表。
