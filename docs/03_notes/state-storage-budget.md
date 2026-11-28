# 狀態儲存預算：NCSTATE 實際會佔幾多（`state.ncst`／RAM store）

> **這份係生成物**：`python3 tools/state_budget.py --write`。
> `run_all.sh` 用 `--check` 驗它冇過期；改 schema／value key 之後要重跑。
> 背景：[state-storage-format.md](state-storage-format.md)｜A/B 實測見 §3.5｜[state-system.md](state-system.md)｜[state-schema.md](state-schema.md)

## 0. 一句話

NCSTATE **只存 `class=value` 的 key**；`struct`（links／routes／wifi／未實作硬件）只係字典，**0 B store／0 B blob**。所以「所有硬件配置齊」並唔等於 286 筆全寫。

## 1. 現時規模

- schema v2：**286 key／16 unit**
- 其中 `class=value`：**26 個**（sys 4／i80 5／spi 7／led 10）；`struct`：260 個（只登記）

## 1.5 遮罩機制前後（class mask＋activation mask＋entry 瘦身）

| 設計 | store 需要 | 實際使用 | blob |
|---|---:|---:|---:|
| 舊計劃：全部 565 key 都入 store（32 B／筆） | **18,080 B** | — | — |
| 冇 class 遮罩：286 key 全當 value（12 B／筆） | **3,432 B**（286 格，遠超 96 格） | — | 約 **1,981 B** |
| **現在**：class 遮罩只留 26 個 value key；store 固定 96 格 | **8 B＋cap×12** | 預設 **228 B**（19 筆）／全開 312 B（26 筆） | 預設 **140 B**／全開 **184 B** |

- **省到嘅**：store 容量由 3,432 B → 1,156 B（**−2,276 B**）；實際載入由 3,432 B → 228/312 B（**−3,204／−3,120 B**）；blob 由 ~1,981 B → 140/184 B（**−1,841／−1,797 B**）。
- **mask 之前（舊計劃 565 key×32 B）**：18,080 B；現在 1,156 B（**−16,924 B**）。
- **activation mask 單獨**嘅作用：未啟動 unit（例：SPI 7 個 key）唔造 record／唔入 blob（約 −42 B blob）；但 **s_store 係固定 96 格，唔會因 inactive 而縮細**。
- **歷史實測**（status §7.293／§7.294）：entry 32→12 B（s_store 2,052→772，−1,280 B）＋s_recs 768 B 搬 PSRAM（−764 B）＋store 64→96 格（+384 B）＝ state 佔 SRAM **2,820→1,160 B（−1,660 B）**；之後 S3 回寫再加 dirty 384＋CRC 表 1,024，所以同「完全冇 state」比最終係 +2,640 B（見 §3.5）。

## 2. `state.ncst` blob（FAT）— 所有 value key 都寫

| 場景 | record 數 | blob bytes | 每個 record 平均 |
|---|---:|---:|---:|
| 全部值 0（最細） | 26 | **174 B** | 6.7 B |
| 典型值（示範） | 26 | **187 B** | 7.2 B |
| 最大值（varint 最長） | 26 | **209 B** | 8.0 B |

- 對照實測：SuperMini **5 筆＝50 B**（§7.291）、Matrix 舊量 **18 筆＝124 B**——同公式一致（含 26 B header）。
- 實機典型只會更細：activation mask 會濾走未啟動 unit（例：LED 未宣告 → 少 10 筆）。

### 實機量測（2026-11-27，ESP32-S3-Zero）

| 情境 | record 數 | blob bytes | 對照 |
|---|---:|---:|---|
| 預設 project（I80＋LED） | 19 | **140 B** | 計算值一致（sys 4＋i80 5＋led 10） |
| 所有支援硬件開齊（＋SPI） | 26 | **184 B** | inactive=0；計算值一致 |

- 量測方法：HEAD firmware（`fw=1d75fea-dirty`、`idf=v6.1.1`、`app=263862e7`）OTA 上板；`status cfg` 的 `state{n,bytes,store,apply,inactive,hit}`。
- 兩次開機第二次 `hit=1`、bytes 不變；`schema.bad=0`。

### 每個 unit 的 record bytes（唔含 header）

| unit | value key | 全部 0 | 典型 | 最大 |
|---|---:|---:|---:|---:|
| `i80` | 5 | 30 B | 33 B | 38 B |
| `led` | 10 | 60 B | 64 B | 71 B |
| `spi` | 7 | 42 B | 45 B | 51 B |
| `sys` | 4 | 16 B | 19 B | 23 B |

## 3. RAM／PSRAM／flash（固定，同寫幾多筆無關）

| 項目 | 大小 | 位置 | 說明 |
|---|---:|---|---|
| `s_store`（靜態部分） | **8 B** | internal SRAM | 2026-11-28 起只係 pointer＋len＋cap |
| `s_store`（開機配置） | 12 B × cap | internal heap | **cap = 掃到筆數＋8**；預設 19 筆 → cap 27 → **324 B**；全開 26 筆 → cap 34 → 408 B |
| `s_dirty_keys` | 384 B | internal SRAM | 回寫用 dirty 表（S3；96×4） |
| `s_crc_tbl` | 1,024 B | internal SRAM | cfg 值集 hash 用的表驅動 CRC 表 |
| `s_recs` | 768 B | **PSRAM** | 開機一次暫存（32×24 B），SRAM 只剩 4 B pointer |
| 字典表 | **29,735 B** | **flash（.rodata）** | 0 SRAM；keys 陣列 10,296 B＋單位／enum pointer＋name／path 字串 18,071 B。**舊文寫 12,404 B 係漏咗字串** |
| cfg structs | ~3,552 B | internal SRAM | 現時仍然存在（state 未完全取代） |
| cJSON 暫態 | ~20–35 KB | 開機 heap | B02 parse 時短暫；之後釋放 |

→ **state 相關常駐 SRAM 約 1,740 B**（8 靜態＋324 開機 heap＋dirty 384＋CRC 1,024；未計舊 cfg structs）；PSRAM 768 B；blob 寫入 FAT 最多幾百 B。

## 3.5 舊 JSON 模式 vs 現在（A/B build 實測）

同一塊 Zero、同一 IDF 6.1.1、同一個 project，兩個 commit 各建一次：
舊＝`bd70185（NCSTATE 之前，config JSON → cfg structs）`；新＝`1d75fea-dirty（HEAD）`。

| 項目 | 舊（JSON → cfg） | 現在（＋NCSTATE） | 差 |
|---|---:|---:|---:|
| app image（flash） | 735,264 B | 777,904 B | **+42,640 B** |
| `.dram0.bss`（SRAM） | 101,568 B | 104,208 B | **+2,640 B** |
| `.dram0.data`（SRAM） | 39,223 B | 39,223 B | +0 B |
| 靜態 SRAM 合計 | 140,791 B | 143,431 B | **+2,640 B** |
| flash `.text` | 475,480 B | 485,708 B | +10,228 B |
| flash `.rodata` | 161,052 B | 193,468 B | **+32,416 B** |

> 註：呢個 A/B 係 **2026-11-28 動態 store 之前** 嘅量測；之後 s_store 由固定 1,156 B 改成「8 B 靜態＋開機按掃描筆數配置」，static bss 再 −1,152 B（heap +324 B，淨 −828 B）。

**+2,640 B SRAM 的來源**（`nm` 逐符號）：`s_store` 1,156 B、`s_dirty_keys` 384 B、
`s_crc_tbl`（表驅動 config hash）1,024 B、`s_result` 40 B＋幾個 pointer／counter ≈ 80 B。
**+42,640 B flash**：state 層 code ≈+10 KB、schema 字典／字串 ≈+32 KB。

其他層：FAT 上 JSON 11,872 B **仍然存在**（權威），另加 `state.ncst` 140–184 B；
開機 cJSON 暫態兩邊一樣（hit 仍然要 parse JSON），新加 `s_recs` 768 B（PSRAM）。
所以現階段 NCSTATE **未省 RAM／flash**：佢係在 cfg 之上加一層 cache／API；
真正省位要等「hit 跳過 JSON parse＋cfg structs 由 store 取代」（未做）：
潛在 −3,552 B SRAM＋−20–35 KB 開機暫態；flash 字典唔會消失。

## 4. 如果全部 286 個 key 都當 value（假設性最壞情況）

- blob：**1,981 B**；store 需要 **3,436 B**（286 格 × 12 B）
- 現時 store 上限 96 格（動態配置；上限 1,152 B）→ **放唔落**（會截斷）。
- 呢個就係 `class=struct` 同 activation mask 存在的理由：只把「真的會變、真係 driver 讀」
  的值入 store；其餘留在 cfg／字典，0 B。

## 5. 點重算

```bash
python3 tools/state_budget.py            # 印呢個表
python3 tools/state_budget.py --write    # 更新呢份文件
python3 tools/state_budget.py --check    # run_all 用（驗冇過期）
```

公式：`blob = 26 + Σ(1 + varint(key) + varint(len) + value)`；sint 用 zigzag、bool 固定 1 B。Python 計法同真 C codec 對過數（26 個 value key 典型值：C codec = 187 B）。
