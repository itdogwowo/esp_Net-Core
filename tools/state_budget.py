#!/usr/bin/env python3
"""狀態儲存預算：schema → NCSTATE blob／RAM store 實際佔用（唔需要板）。

    python3 tools/state_budget.py              # 印表格
    python3 tools/state_budget.py --write      # 寫 docs/03_notes/state-storage-budget.md
    python3 tools/state_budget.py --check      # 驗文件冇過期（run_all 用）
    python3 tools/state_budget.py --json       # 機器讀

點解要有這支：用家問「所有硬件配置齊、有遮罩（只存實際值）時會用幾多緩衝？」
NCSTATE 的大小唔係靠感覺——header 固定、record 用 varint，key 由 schema 決定；
呢支按 `schema/state-keys.json` 逐個 key 計，並且同真 C codec（`nc_state_blob_size`）
對過數（2026-11-27：26 個 value key 典型值 = **187 B**，兩邊一致）。

公式：
    blob = 26 B header + Σ record
    record = 1 (type) + varint_len(key id) + varint_len(value_len) + value_len
      · uint／enum：value = varint(value)
      · sint（I80 wr／dc／cs 等）：value = varint(zigzag(v))
      · bool：value_len = 1
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCHEMA = ROOT / "schema/state-keys.json"
DOC = ROOT / "docs/03_notes/state-storage-budget.md"

HDR = 26                     # NCSTATE header
STORE_SLOTS = 96             # 動態 store 容量上限（NC_STATE_STORE_MAX）
STORE_ENTRY = 12             # sizeof(nc_state_entry_t)
DYN_RESERVE = 8              # 熱插拔 key headroom（NC_STATE_DYN_RESERVE）
STORE_STATIC = 8             # 2026-11-28：s_store = pointer+len+cap（8 B），唔再係固定 96 格
DIRTY_BYTES = STORE_SLOTS * 4                   # s_dirty_keys（S3 回寫）
RECS_PSRAM = 32 * 24                            # 開機暫存 s_recs（搬到 PSRAM）
DICT_FLASH = 29735                              # schema_data.c.obj 總 text／rodata（keys 10,296＋units 192＋enum pointer＋18,071 字串）
DICT_KEYS = 10296                               # nc_state_schema_keys 陣列（286×36）
CFG_STRUCTS = 3552                              # 現時 cfg struct 常駐（state 未完全取代）

# 實機量測（2026-11-27，ESP32-S3-Zero；fw 1d75fea-dirty／idf v6.1.1／app 263862e7）
# 舊 JSON 模式 vs 現在 A/B（同一塊 Zero、同一 IDF 6.1.1、同一 project；commit bd70185 vs HEAD）
# 遮罩機制（class=value／activation mask／entry 瘦身）前後
MASK_BEFORE_KEYS = 565          # 未加 9 類大表前的 worst-case 計劃（status §7.292）
MASK_BEFORE_ENTRY = 32          # 舊 entry：key+type+bytes[16]+u/i
MASK_ALL_KEYS_STORE = 3432      # 286 key ×12 B（現 entry，但唔用 class mask 全入）
MASK_ACTUAL_USED = 19 * 12      # 現在實際 19 筆
MASK_ACTUAL_ALL_OPEN = 26 * 12  # 全部支援硬件開齊 26 筆

AB = {
    "old_commit": "bd70185（NCSTATE 之前，config JSON → cfg structs）",
    "new_commit": "1d75fea-dirty（HEAD）",
    "old_image": 735264, "new_image": 777904,        # app .bin bytes
    "old_bss": 101568, "new_bss": 104208,
    "old_data": 39223, "new_data": 39223,
    "old_text": 475480, "new_text": 485708,
    "old_rodata": 161052, "new_rodata": 193468,
}

MEASURED = {
    "baseline": {"desc": "I80＋LED（sys 4＋i80 5＋led 10）", "records": 19, "bytes": 140},
    "all": {"desc": "再加 SPI（sys 4＋i80 5＋spi 7＋led 10）", "records": 26, "bytes": 184},
    "static": {"bss": 104208, "data": 39223, "total": 143431},
    "plan": {"items": 3, "int": 5088, "psram": 2615},
}

# 代表性值（典型場景；用嚟計 record 的 varint 長度）
TYPICAL = {
    "NC_KEY_SYS_CID": 0xF5F9,
    "NC_KEY_SYS_FRAME_MS": 20,
    "NC_KEY_SYS_NUM_PIXELS": 256,
    "NC_KEY_SYS_LOG_DEBUG": 0,
    "NC_KEY_I80_COUNT": 8,
    "NC_KEY_I80_WR": 2,
    "NC_KEY_I80_DC": 4,
    "NC_KEY_I80_CS": -1,
    "NC_KEY_I80_FREQ": 80_000_000,
    "NC_KEY_SPI_ENABLE": 1,
    "NC_KEY_SPI_BAUD": 4_000_000,
    "NC_KEY_SPI_PHASE": 0,
    "NC_KEY_SPI_POL": 0,
    "NC_KEY_SPI_SCK": 14,
    "NC_KEY_SPI_MOSI": 15,
    "NC_KEY_SPI_MISO": 16,
    "NC_KEY_LED_ENABLE": 1,
    "NC_KEY_LED_BACKEND": 1,
    "NC_KEY_LED_BUS": 0,
    "NC_KEY_LED_LANES": 1,
    "NC_KEY_LED_PX": 256,
    "NC_KEY_LED_FMT": 0,
    "NC_KEY_LED_CLK": 2_500_000,
    "NC_KEY_LED_BIT_CELLS": 3,
    "NC_KEY_LED_T0H": 1,
    "NC_KEY_LED_T1H": 2,
}

TYPE_MAX = {"u8": 255, "u16": 65535, "u32": 0xFFFFFFFF,
            "i8": -128, "i32": -2147483648, "bool": 1, "enum": 0, "str": 0}


def varint_len(v: int) -> int:
    v &= 0xFFFFFFFF
    n = 1
    while v > 0x7F:
        v >>= 7
        n += 1
    return n


def zigzag(v: int) -> int:
    return ((v << 1) ^ (v >> 31)) & 0xFFFFFFFF


def rec_bytes(key: dict, val: int) -> int:
    typ = key["type"]
    if typ == "bool":
        vlen = 1
    elif typ in ("i8", "i32"):
        vlen = varint_len(zigzag(int(val)))
    else:
        vlen = varint_len(int(val))
    return 1 + varint_len(int(key["id"], 16)) + varint_len(vlen) + vlen


def value_for(key: dict, mode: str) -> int:
    if mode == "zero":
        return 0
    if mode == "typical":
        return TYPICAL.get(key["name"], 0)
    typ = key["type"]
    if typ == "enum":
        return max(len(key.get("values", [])) - 1, 0)
    if typ == "str":
        return 0
    return TYPE_MAX.get(typ, 0)


def scenario(keys: list[dict], mode: str) -> tuple[int, dict[str, int], dict[str, int]]:
    total = HDR
    by_unit: dict[str, int] = {}
    by_type: dict[str, int] = {}
    for k in keys:
        n = rec_bytes(k, value_for(k, mode))
        total += n
        by_unit[k["unit"]] = by_unit.get(k["unit"], 0) + n
        by_type[k["type"]] = by_type.get(k["type"], 0) + 1
    return total, by_unit, by_type


def hypothetical_all(keys: list[dict]) -> tuple[int, int]:
    """假設全部 key 都係 value（最壞情況）：blob bytes＋store 需要幾多 bytes。"""
    total = HDR
    for k in keys:
        typ = k["type"]
        if typ == "bool":
            vlen = 1
        elif typ == "i8":
            vlen = varint_len(zigzag(-128))
        elif typ == "i32":
            vlen = 5
        elif typ in ("u8",):
            vlen = 1
        elif typ == "u16":
            vlen = 2
        elif typ == "u32":
            vlen = 5
        elif typ == "enum":
            vlen = 2
        else:
            vlen = 8
        total += 1 + varint_len(int(k["id"], 16)) + varint_len(vlen) + vlen
    return total, len(keys) * STORE_ENTRY + 4


def unit_breakdown(keys: list[dict]) -> list[dict]:
    rows = []
    units = sorted({k["unit"] for k in keys})
    for unit in units:
        sel = [k for k in keys if k["unit"] == unit]
        row = {"unit": unit, "keys": len(sel)}
        for mode in ("zero", "typical", "max"):
            total, _, _ = scenario(sel, mode)
            row[mode] = total - HDR          # 該 unit 的 record bytes（唔含 header）
        rows.append(row)
    return rows


def build_doc(schema: dict, value_keys: list[dict], all_keys: list[dict]) -> str:
    z, _u, _t = scenario(value_keys, "zero")
    t, tu, tt = scenario(value_keys, "typical")
    m, mu, mt = scenario(value_keys, "max")
    all_blob, all_store = hypothetical_all(all_keys)
    rows = unit_breakdown(value_keys)

    L: list[str] = []
    L.append("# 狀態儲存預算：NCSTATE 實際會佔幾多（`state.ncst`／RAM store）")
    L.append("")
    L.append("> **這份係生成物**：`python3 tools/state_budget.py --write`。")
    L.append("> `run_all.sh` 用 `--check` 驗它冇過期；改 schema／value key 之後要重跑。")
    L.append("> 背景：[state-storage-format.md](state-storage-format.md)｜A/B 實測見 §3.5｜"
             "[state-system.md](state-system.md)｜[state-schema.md](state-schema.md)")
    L.append("")
    L.append("## 0. 一句話")
    L.append("")
    L.append("NCSTATE **只存 `class=value` 的 key**；`struct`（links／routes／wifi／未實作硬件）"
             "只係字典，**0 B store／0 B blob**。所以「所有硬件配置齊」並唔等於 286 筆全寫。")
    L.append("")
    L.append("## 1. 現時規模")
    L.append("")
    L.append(f"- schema v{schema.get('version')}：**{len(all_keys)} key／{len(schema.get('units', []))} unit**")
    L.append(f"- 其中 `class=value`：**{len(value_keys)} 個**（sys 4／i80 5／spi 7／led 10）"
             f"；`struct`：{len(all_keys) - len(value_keys)} 個（只登記）")
    L.append("")
    L.append("## 1.5 遮罩機制前後（class mask＋activation mask＋entry 瘦身）")
    L.append("")
    L.append("| 設計 | store 需要 | 實際使用 | blob |")
    L.append("|---|---:|---:|---:|")
    L.append(f"| 舊計劃：全部 {MASK_BEFORE_KEYS} key 都入 store（32 B／筆） | "
             f"**{MASK_BEFORE_KEYS * MASK_BEFORE_ENTRY:,} B** | — | — |")
    L.append(f"| 冇 class 遮罩：286 key 全當 value（12 B／筆） | **{MASK_ALL_KEYS_STORE:,} B**（"
             f"286 格，遠超 96 格） | — | 約 **{hypothetical_all(all_keys)[0]:,} B** |")
    L.append(f"| **現在**：class 遮罩只留 26 個 value key；store 固定 96 格 | "
             f"**{STORE_STATIC} B＋cap×12** | 預設 **{MASK_ACTUAL_USED} B**（19 筆）／全開 "
             f"{MASK_ACTUAL_ALL_OPEN} B（26 筆） | 預設 **140 B**／全開 **184 B** |")
    L.append("")
    L.append("- **省到嘅**：store 容量由 3,432 B → 1,156 B（**−2,276 B**）；"
             "實際載入由 3,432 B → 228/312 B（**−3,204／−3,120 B**）；"
             "blob 由 ~1,981 B → 140/184 B（**−1,841／−1,797 B**）。")
    L.append("- **mask 之前（舊計劃 565 key×32 B）**：18,080 B；現在 1,156 B（**−16,924 B**）。")
    L.append("- **activation mask 單獨**嘅作用：未啟動 unit（例：SPI 7 個 key）唔造 record／唔入 blob"
             "（約 −42 B blob）；但 **s_store 係固定 96 格，唔會因 inactive 而縮細**。")
    L.append("- **歷史實測**（status §7.293／§7.294）：entry 32→12 B（s_store 2,052→772，−1,280 B）"
             "＋s_recs 768 B 搬 PSRAM（−764 B）＋store 64→96 格（+384 B）＝ "
             "state 佔 SRAM **2,820→1,160 B（−1,660 B）**；之後 S3 回寫再加 dirty 384＋CRC 表 1,024，"
             "所以同「完全冇 state」比最終係 +2,640 B（見 §3.5）。")
    L.append("")
    L.append("## 2. `state.ncst` blob（FAT）— 所有 value key 都寫")
    L.append("")
    L.append("| 場景 | record 數 | blob bytes | 每個 record 平均 |")
    L.append("|---|---:|---:|---:|")
    L.append(f"| 全部值 0（最細） | {len(value_keys)} | **{z} B** | {z / len(value_keys):.1f} B |")
    L.append(f"| 典型值（示範） | {len(value_keys)} | **{t} B** | {t / len(value_keys):.1f} B |")
    L.append(f"| 最大值（varint 最長） | {len(value_keys)} | **{m} B** | {m / len(value_keys):.1f} B |")
    L.append("")
    L.append("- 對照實測：SuperMini **5 筆＝50 B**（§7.291）、Matrix 舊量 **18 筆＝124 B**"
             "——同公式一致（含 26 B header）。")
    L.append(f"- 實機典型只會更細：activation mask 會濾走未啟動 unit（例：LED 未宣告 → 少 10 筆）。")
    L.append("")
    L.append("### 實機量測（2026-11-27，ESP32-S3-Zero）")
    L.append("")
    L.append("| 情境 | record 數 | blob bytes | 對照 |")
    L.append("|---|---:|---:|---|")
    L.append(f"| 預設 project（I80＋LED） | {MEASURED['baseline']['records']} | "
             f"**{MEASURED['baseline']['bytes']} B** | 計算值一致（sys 4＋i80 5＋led 10） |")
    L.append(f"| 所有支援硬件開齊（＋SPI） | {MEASURED['all']['records']} | "
             f"**{MEASURED['all']['bytes']} B** | inactive=0；計算值一致 |")
    L.append("")
    L.append("- 量測方法：HEAD firmware（`fw=1d75fea-dirty`、`idf=v6.1.1`、`app=263862e7`）"
             "OTA 上板；`status cfg` 的 `state{n,bytes,store,apply,inactive,hit}`。")
    L.append("- 兩次開機第二次 `hit=1`、bytes 不變；`schema.bad=0`。")
    L.append("")
    L.append("### 每個 unit 的 record bytes（唔含 header）")
    L.append("")
    L.append("| unit | value key | 全部 0 | 典型 | 最大 |")
    L.append("|---|---:|---:|---:|---:|")
    for r in rows:
        L.append(f"| `{r['unit']}` | {r['keys']} | {r['zero']} B | {r['typical']} B | {r['max']} B |")
    L.append("")
    L.append("## 3. RAM／PSRAM／flash（固定，同寫幾多筆無關）")
    L.append("")
    L.append("| 項目 | 大小 | 位置 | 說明 |")
    L.append("|---|---:|---|---|")
    L.append(f"| `s_store`（靜態部分） | **{STORE_STATIC} B** | internal SRAM | 2026-11-28 起只係 pointer＋len＋cap |")
    L.append(f"| `s_store`（開機配置） | {STORE_ENTRY} B × cap | internal heap | **cap = 掃到筆數＋{DYN_RESERVE}**；"
             f"預設 19 筆 → cap 27 → **{STORE_ENTRY * 27} B**；全開 26 筆 → cap 34 → {STORE_ENTRY * 34} B |")
    L.append(f"| `s_dirty_keys` | {DIRTY_BYTES} B | internal SRAM | 回寫用 dirty 表（S3；96×4） |")
    L.append("| `s_crc_tbl` | 1,024 B | internal SRAM | cfg 值集 hash 用的表驅動 CRC 表 |")
    L.append(f"| `s_recs` | {RECS_PSRAM} B | **PSRAM** | 開機一次暫存（32×24 B），SRAM 只剩 4 B pointer |")
    L.append(f"| 字典表 | **{DICT_FLASH:,} B** | **flash（.rodata）** | 0 SRAM；keys 陣列 {DICT_KEYS:,} B＋單位／enum pointer＋"
             "name／path 字串 18,071 B。**舊文寫 12,404 B 係漏咗字串** |")
    L.append(f"| cfg structs | ~{CFG_STRUCTS:,} B | internal SRAM | 現時仍然存在（state 未完全取代） |")
    L.append("| cJSON 暫態 | ~20–35 KB | 開機 heap | B02 parse 時短暫；之後釋放 |")
    L.append("")
    L.append(f"→ **state 相關常駐 SRAM 約 {STORE_STATIC + STORE_ENTRY * 27 + DIRTY_BYTES + 1024:,} B**"
             f"（8 靜態＋324 開機 heap＋dirty 384＋CRC 1,024；未計舊 cfg structs）；"
             "PSRAM 768 B；blob 寫入 FAT 最多幾百 B。")
    L.append("")
    L.append("## 3.5 舊 JSON 模式 vs 現在（A/B build 實測）")
    L.append("")
    L.append(f"同一塊 Zero、同一 IDF 6.1.1、同一個 project，兩個 commit 各建一次：")
    L.append(f"舊＝`{AB['old_commit']}`；新＝`{AB['new_commit']}`。")
    L.append("")
    L.append("| 項目 | 舊（JSON → cfg） | 現在（＋NCSTATE） | 差 |")
    L.append("|---|---:|---:|---:|")
    L.append(f"| app image（flash） | {AB['old_image']:,} B | {AB['new_image']:,} B | "
             f"**+{AB['new_image'] - AB['old_image']:,} B** |")
    L.append(f"| `.dram0.bss`（SRAM） | {AB['old_bss']:,} B | {AB['new_bss']:,} B | "
             f"**+{AB['new_bss'] - AB['old_bss']:,} B** |")
    L.append(f"| `.dram0.data`（SRAM） | {AB['old_data']:,} B | {AB['new_data']:,} B | "
             f"{AB['new_data'] - AB['old_data']:+,} B |")
    L.append(f"| 靜態 SRAM 合計 | {AB['old_bss'] + AB['old_data']:,} B | "
             f"{AB['new_bss'] + AB['new_data']:,} B | "
             f"**+{AB['new_bss'] + AB['new_data'] - AB['old_bss'] - AB['old_data']:,} B** |")
    L.append(f"| flash `.text` | {AB['old_text']:,} B | {AB['new_text']:,} B | "
             f"{AB['new_text'] - AB['old_text']:+,} B |")
    L.append(f"| flash `.rodata` | {AB['old_rodata']:,} B | {AB['new_rodata']:,} B | "
             f"**{AB['new_rodata'] - AB['old_rodata']:+,} B** |")
    L.append("")
    L.append("> 註：呢個 A/B 係 **2026-11-28 動態 store 之前** 嘅量測；之後 s_store 由固定 1,156 B "
             "改成「8 B 靜態＋開機按掃描筆數配置」，static bss 再 −1,152 B（heap +324 B，淨 −828 B）。")
    L.append("")
    L.append("**+2,640 B SRAM 的來源**（`nm` 逐符號）：`s_store` 1,156 B、`s_dirty_keys` 384 B、")
    L.append("`s_crc_tbl`（表驅動 config hash）1,024 B、`s_result` 40 B＋幾個 pointer／counter ≈ 80 B。")
    L.append("**+42,640 B flash**：state 層 code ≈+10 KB、schema 字典／字串 ≈+32 KB。")
    L.append("")
    L.append("其他層：FAT 上 JSON 11,872 B **仍然存在**（權威），另加 `state.ncst` 140–184 B；")
    L.append("開機 cJSON 暫態兩邊一樣（hit 仍然要 parse JSON），新加 `s_recs` 768 B（PSRAM）。")
    L.append("所以現階段 NCSTATE **未省 RAM／flash**：佢係在 cfg 之上加一層 cache／API；")
    L.append("真正省位要等「hit 跳過 JSON parse＋cfg structs 由 store 取代」（未做）：")
    L.append("潛在 −3,552 B SRAM＋−20–35 KB 開機暫態；flash 字典唔會消失。")
    L.append("")
    L.append("## 4. 如果全部 286 個 key 都當 value（假設性最壞情況）")
    L.append("")
    L.append(f"- blob：**{all_blob:,} B**；store 需要 **{all_store:,} B**（{len(all_keys)} 格 × {STORE_ENTRY} B）")
    L.append(f"- 現時 store 上限 {STORE_SLOTS} 格（動態配置；上限 {STORE_SLOTS * STORE_ENTRY:,} B）→ **放唔落**（會截斷）。")
    L.append("- 呢個就係 `class=struct` 同 activation mask 存在的理由：只把「真的會變、真係 driver 讀」")
    L.append("  的值入 store；其餘留在 cfg／字典，0 B。")
    L.append("")
    L.append("## 5. 點重算")
    L.append("")
    L.append("```bash")
    L.append("python3 tools/state_budget.py            # 印呢個表")
    L.append("python3 tools/state_budget.py --write    # 更新呢份文件")
    L.append("python3 tools/state_budget.py --check    # run_all 用（驗冇過期）")
    L.append("```")
    L.append("")
    L.append("公式：`blob = 26 + Σ(1 + varint(key) + varint(len) + value)`；"
             "sint 用 zigzag、bool 固定 1 B。Python 計法同真 C codec 對過數"
             f"（26 個 value key 典型值：C codec = {t} B）。")
    L.append("")
    return "\n".join(L)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true", help="寫 docs/03_notes/state-storage-budget.md")
    ap.add_argument("--check", action="store_true", help="驗文件冇過期（run_all 用）")
    ap.add_argument("--json", action="store_true", help="機器讀")
    args = ap.parse_args()

    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    all_keys = schema["keys"]
    value_keys = [k for k in all_keys if k.get("class") == "value"]
    doc = build_doc(schema, value_keys, all_keys)

    if args.json:
        z, _u, _t = scenario(value_keys, "zero")
        t, _u2, _t2 = scenario(value_keys, "typical")
        m, _u3, _t3 = scenario(value_keys, "max")
        all_blob, all_store = hypothetical_all(all_keys)
        print(json.dumps({
            "value_keys": len(value_keys), "all_keys": len(all_keys),
            "blob_zero": z, "blob_typical": t, "blob_max": m,
            "store_static": STORE_STATIC,
            "store_default_cap": 19 + DYN_RESERVE,
            "store_default_heap": (19 + DYN_RESERVE) * STORE_ENTRY, "dirty": DIRTY_BYTES,
            "recs_psram": RECS_PSRAM, "dict_flash": DICT_FLASH,
            "hypothetical_all_value_blob": all_blob,
            "hypothetical_all_value_store": all_store,
            "mask_before_old_plan_store": MASK_BEFORE_KEYS * MASK_BEFORE_ENTRY,
            "mask_before_all_keys_store": MASK_ALL_KEYS_STORE,
            "store_static_after_dynamic": STORE_STATIC,
            "store_used_default": MASK_ACTUAL_USED,
            "store_used_all_open": MASK_ACTUAL_ALL_OPEN,
        }, ensure_ascii=False, indent=2))
        return 0

    if args.write:
        DOC.write_text(doc, encoding="utf-8")
        print(f"✔ 寫入 {DOC.relative_to(ROOT)}（{len(doc.encode())} B）")
    if args.check:
        have = DOC.read_text(encoding="utf-8") if DOC.is_file() else None
        if have is None:
            print(f"✘ 缺 {DOC.relative_to(ROOT)}；跑 python3 tools/state_budget.py --write",
                  file=sys.stderr)
            return 1
        if have != doc:
            print(f"✘ {DOC.relative_to(ROOT)} 過期；跑 python3 tools/state_budget.py --write",
                  file=sys.stderr)
            return 1
    if not args.write and not args.check:
        print(doc)
    return 0


if __name__ == "__main__":
    sys.exit(main())
