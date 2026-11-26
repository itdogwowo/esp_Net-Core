# 狀態儲存格式：緊湊二進位（唔直接存 JSON）

> 2026-11-26。用家定案方向：**設定系統一律「出廠 → FAT 微調 → 壞了回退」**；
> 而且值會登記入 **state manager**（`nc_state`），所以要有**緊湊、可版本化、可校驗**
> 的二進位格式，**唔好直接存 JSON**。效果（pixel assets）同樣：JSON 只做輸入，
> 讀完即刻轉成 runtime 緊湊形態，開機唔再 parse JSON。

---

## 1. 統一四層套路（所有 config 域都一樣）

| 層 | 內容 | 儲存位置 | 出錯時 |
|---|---|---|---|
| **L0 出廠** | build-time 生成（`nc_port.c`／`preset_*.json`） | flash（embedded） | 冇得壞（常量） |
| **L1 FAT 微調** | 人手可改的 JSON（唯一人讀格式） | `/data/config/*.json` | JSON 壞 → **忽略 L1**，用 L0 |
| **L2 派生二進位** | L1 parse 後的緊湊 cache | `/data/config/*.ncb`（或 NVS blob） | cache 壞／版本唔夾 → 由 L1／L0 重生 |
| **L3 回退** | 上一份「確認可用」的快照（last-known-good） | `*.ncb.bak` | 全部壞 → 用 L0，並大聲 warn |

**規則**
1. JSON 只係**輸入**；runtime 同 state manager 只存**二進位**。
2. 二進位檔一律有 `magic + version + crc32`；版本唔夾＝當壞檔（唔會亂讀）。
3. 壞檔**明示**（log／status），唔好靜默降級（鐵律 9）。
4. 派生 cache 可以隨時刪：刪完只係慢一次（重生），唔會失資料。

---

## 2. `NCB2`：state manager 的通用狀態 blob

用嚟存「一堆細狀態」（log 級別、boot 旗標、校準值、last-known-good config…）。
NVS 本身係 blob，NCB2 係 blob 內部的**自描述格式**（唔係 raw struct —
raw struct 一改 firmware 就讀錯，係現時 `nc_state` v1 的弱點：size 一變就 drop）。

```text
header (20 B)
  "NCB2" (4) | ver u16 | flags u16 | strtab_len u32 | payload_len u32 | crc32 u32
strtab: n u16, n × (len u8 + utf8)            ← key 只存一次（字典）
payload: 值樹，tag u8
  0 null | 1 false | 2 true
  3 int      → zigzag varint
  4 float    → 8 B double
  5 str      → varint len + utf8
  6 arr      → varint count + 值×count
  7 obj      → varint count + (8 keyref + varint dict_idx + 值)×count
crc32 = crc32(strtab + payload)
```

- **varint**：0..127 = 1 B；負數用 zigzag → 小幅負數（例如 GPIO `-1`）只 1 B。
- **字典**：`"enable"`、`"_comment"` 等重複 key 全檔只出現一次。
- **向前兼容**：未知 tag → 跳過（靠 len 前綴）；未知 key → 保留原文唔用。
- 上限：blob ≤ 4 KB（NVS 單值上限；大 config 用 L2 檔案）。

**實測（lab 原型，generic JSON→NCB2）**

| 檔案 | JSON | NCB2 | 省 |
|---|---:|---:|---:|
| `hardware.json` | 5,992 | 3,462 | 42.2% |
| `driver.json` | 2,830 | 1,618 | 42.8% |
| `system.json` | 2,274 | 1,378 | 39.4% |
| `tasks.json` | 1,494 | 1,134 | 24.1% |
| `resources.json` | 519 | 483 | 6.9% |
| 合計 7 個檔 | 13,971 | 8,788 | **37.1%** |

> 呢個係**通用** encoder（連 key 名）。若按固定 schema 編（field id，唔存 key），
> 預計 hardware／driver 可再細 50%+（目標：hardware ~700 B、driver ~350 B）。
> 原型：`modules/lab/state_format_lab/ncb2_preview.py`。

---

## 3. `NCC2`：config 派生 cache（FAT）

每個 config 層（system／hardware／driver／tasks／resources）一個 cache：

```text
"NCC2" (4) | ver u16 | layer u8 | rsv u8 | cfg_hash u32 | payload_len u32 | crc32 u32
cfg_hash = crc32(L1 JSON bytes)        ← 同 L1 唔同就自動失效
payload  = 固定 schema binary（field id + varint；冇 key 名）
```

**開機流程**

```text
讀 L1 JSON bytes → 算 hash
  ├─ L2 *.ncb 存在、ver 夾、crc 過、cfg_hash 一樣 → 直接反序列化（快）
  └─ 否則 → parse JSON（失敗就 L0）→ 序列化 L2 + .bak（last-known-good）
```

**好處**：正常開機**唔需要 cJSON**；JSON 壞咗都仲有上一次可用嘅 binary。

---

## 4. `PXA2`：pixel assets 編譯 cache

效果／map／mode／registry 已經係「JSON → runtime 緊湊形態」（aprog 指令流、
runs、6 B flat 表）；**未有跨開機 cache**（每次開機重新 parse＋compile）。
補一個編譯 cache：

```text
"PXA2" (4) | ver u16 | kind u8（mode/effect/map/registry） | rsv u8
assets_hash u32 | payload_len u32 | crc32 u32 | payload…
```

- `payload`：direct mode 用「aprog 指令流 + 參數表 + 每節點邊界」；
  mode 用「節點樹＋參數」（同現時 JSON 等價但完全數值化）。
- layout 已經有 `.nclt`（binary、6 B/px flat）——**唔重做**，PXA2 只補 program/mode。
- 開機：hash 夾 → 直接載入；唔夾／壞 → 由 JSON 重建（現有流程）→ 寫回 cache。
- 刪 cache＝只係慢一次；JSON 仍然係唯一人改來源。

---

## 5. 落地次序（估時）

| 階段 | 內容 | 估時 |
|---|---|---|
| **P1** | `NCB2` codec（C，~250 行）＋host test（round-trip／varint／壞 CRC）＋`nc_state` 改用它（header 化、last-known-good） | 1 日 |
| **P2** | `NCC2`：hardware／driver 兩層（最常改、最大）派生 cache＋開機流程 | 1 日 |
| **P3** | `PXA2`：mode／registry 編譯 cache（program 次之） | 1–2 日 |
| P4 | `status cfg`／boot log 顯示 cache 命中、版本、fallback 原因 | 0.5 日 |

**風險**：二進位格式一改就要版本升級＋拒絕舊版（唔可以「估」）；要 host test
覆蓋 round-trip 同壞檔；schema 變更要同時 bump `ver`。

---

## 6. 同現有機制的關係

| 現有 | 之後 |
|---|---|
| `nc_state` 用 raw struct 落 NVS（size 一變就 drop） | 改用 `NCB2`（自描述、版本化、可加減欄位） |
| config 每次開機 parse JSON | `NCC2` cache；JSON 只做人改輸入 |
| pixel 每次開機 parse＋compile | `PXA2` cache；`.nclt` 已存在唔動 |
| 壞檔只靜默回預設 | 一律 warn＋status 顯示（L3 last-known-good） |

> **一句話**：JSON 係「人機介面」，二進位係「機機介面」；兩者都由同一套
> 「出廠 → FAT → 回退」規則管，格式由 magic／ver／hash／crc 保證唔會亂讀。

---

## 8. 哈希規則（所有中間產物一律照呢條）

**用家定案（2026-11-26）**：中間產物要寫入來源 JSON 哈希，開機對哈希：
一樣＝直接載入；唔一樣＝重新生成。

### 8.1 每個中間物嘅 header（固定佈局）

```text
magic 4 B | ver u16 | flags u16 | src_hash u32 | payload_len u32 | blob_crc u32
                                 ^^^^^^^^^^        ^^^^^^^^^        ^^^^^^^^^
                                 來源 JSON 哈希    長度             自身 CRC
```

- **`src_hash`**：來源 JSON 嘅 CRC32（IEEE，zlib），按**固定次序**累加：
  `hash = crc32(hash, "<檔名>\0" + raw_bytes)`；檔名用相對路徑，例如
  `wiring.json`、`driver.json`、`project/sdkconfig.defaults`。
  用 **raw bytes**（FAT 上實際內容），唔做 normalization。
- **`blob_crc`**：header 之後 payload 嘅 CRC32（防讀取／寫入半截）。
- **`ver`**：格式版本（schema 一改就 bump；舊 blob 直接拒讀）。

### 8.2 開機流程（統一）

```text
讀 FAT JSON bytes → 算 src_hash
  ├─ 中間物存在 且 ver 夾 且 src_hash 相同 且 blob_crc 過 → 直接載入（hit）
  └─ 否則 → 由 JSON 重新生成 → 寫回（atomic：.tmp → rename）→ log miss（old→new）
```

- **hit**：開機唔使 parse JSON（最快）；log `ncs hit src=0x…`
- **miss**：parse JSON；若 JSON 壞 → 回 L0 出廠；log `ncs miss old=0x… new=0x…`
- **CRC 撞車**（人為）唔喺威脅模型內；意外改動 8 KB 內容撞 CRC 機率 ~1/2³²，
  而且撞車都只會「用舊值一次」，下次改動即 miss。

### 8.3 各中間物嘅 `src_hash` 範圍

| 中間物 | src_hash 來源 |
|---|---|
| **NCS1**（`state.ncs`／NVS） | 所有 config layer 嘅 JSON（固定次序：hardware、driver、system、tasks、resources） |
| **NCC2**（config 派生 cache） | 該 layer 嘅 JSON（單一檔） |
| **PXA2**（pixel assets 編譯 cache） | effects.json＋mode／map／registry 所在嗰批 JSON |
| **NCLT**（layout cache） | `key` 已經係 mode/map/assets 內容 hash 嘅 FNV；等同 src_hash（見 §8.4） |
| **preset_activation.json**（生成期激活表） | `wiring.json`＋`driver.json`＋project `sdkconfig.defaults`（ncportgen 已輸出 `src_hash`＋逐檔 crc） |

### 8.4 NCLT 補充

`.nclt` header 本身已有 `key`（FNV-1a over mode_hash/map_hash/px/assets_hash/ver）＋
`crc`；**key 就係來源哈希**，所以開機載入前比較 `key` 已等同「JSON hash 一樣嗎」。
唔另加 `src_hash` 欄位（避免重複），但 header 註解會寫明呢個等價關係。
