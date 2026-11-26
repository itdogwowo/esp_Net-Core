#!/usr/bin/env python3
"""硬件矩陣＋儲存上限原型。

用家設計：
  1. JSON 列出**所有支援硬件**，預設 "enable": 0（模板＝可選清單）；
  2. 每個硬件有固定「通道數上限」與「欄位」，用戶只填 GPIO＋少量數值；
  3. schematic 矩陣 → key 數有限 → **儲存上限可算**（唔使估）；
  4. GPIO 用 48-bit bitmap（0..48），enable 時佔 bit → 衝突 O(1) 檢查。

呢個 lab 計：
  - 矩陣 key 總數、NCS1 worst-case 大小、sparse 實際大小
  - 生成模板 JSON（all enable=0）量大小
"""
from __future__ import annotations

import json
from pathlib import Path

# (unit, max_channels, fields_per_channel, notes)
# 欄位數按現有 schema／舊 config.json 的最壞情況（唔會再加新欄位就先計死）
MATRIX = [
    ("sys.system",   1, 12),
    ("hw.pin",      48,  3),   # 48 個 GPIO 位置（GPIO 矩陣）
    ("hw.pwm",       8,  2),
    ("hw.uart",      3,  6),
    ("hw.spi",       2,  7),
    ("hw.i2c",       2,  3),
    ("hw.i2s",       1,  5),
    ("hw.sdio",      1,  8),
    ("hw.i80",       2, 22),   # count+16 data+wr/dc/cs/freq+...
    ("hw.eth",       1,  8),
    ("hw.enc",       1,  2),
    ("dev.usb",      2,  4),
    ("dev.led",      2, 10),
    ("dev.apa102",   2,  5),
    ("dev.pca9685",  2,  4),
    ("dev.tft",      1,  8),
    ("dev.xl9555",   1,  4),
    ("dev.gt1151q",  1,  3),
    ("dev.husb238",  1,  3),
    ("dev.pcm5102",  1,  2),
    ("dev.uart_motor", 2, 3),
    ("dev.sd",       1,  5),
    ("dev.audio",    1,  2),
]

# NCS1：id varint ≤2 B；value zigzag varint ≤5 B（u32 最大 0xFFFFFFFF → 5 B）
ID_MAX_BYTES = 2
VAL_MAX_BYTES = 5
HDR = 12


def main() -> int:
    total_keys = sum(ch * f for _, ch, f in MATRIX)
    worst = HDR + total_keys * (ID_MAX_BYTES + VAL_MAX_BYTES)
    # 實際 sparse：用戶通常只 enable 兩三個硬件 → 只寫佢哋嘅 key
    typical_enabled = [("hw.i80", 1, 22), ("dev.led", 1, 10)]
    typ_keys = sum(ch * f for _, ch, f in typical_enabled)
    typ_bytes = HDR + typ_keys * (ID_MAX_BYTES + VAL_MAX_BYTES)

    print(f"{'unit':16} {'ch':>4} {'fields':>7} {'keys':>6}")
    for name, ch, f in MATRIX:
        print(f"{name:16} {ch:4} {f:7} {ch*f:6}")
    print("-" * 40)
    print(f"{'合計':16} {'':4} {'':7} {total_keys:6} keys")
    print()
    print(f"NCS1 header            {HDR} B")
    print(f"worst-case（全部寫）    {worst} B = {HDR} + {total_keys}×(≤{ID_MAX_BYTES}+≤{VAL_MAX_BYTES})")
    print(f"typical（只開 i80+led） {typ_bytes} B（{typ_keys} keys）")
    print(f"NVS 單 blob 上限 4 KB → {'OK' if worst <= 4096 else '超出！要拆 blob'}")

    # 生成模板 JSON（所有硬件 enable=0）
    tpl = {"version": 1,
           "_comment": ["全部支援硬件，預設 enable=0；用戶只改要用的（GPIO＋少量數值）。",
                        "數值上限／型別由 schema 管；GPIO 用 48-bit bitmap 檢查衝突。"]}
    for name, ch, f in MATRIX:
        dom, unit = name.split(".")
        tpl.setdefault(dom, {})[unit] = {
            "enable": 0,
            "max": ch,
            "fields": f,
            "list": [{f"ch{i}": {"gpio": -1}} for i in range(min(ch, 2))],
        }
    raw = json.dumps(tpl, ensure_ascii=False, indent=2).encode("utf-8")
    out = Path("artifacts/bench/hw_template_preview.json")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(raw)
    print()
    print(f"模板 JSON（所有硬件 enable=0）：{len(raw)} B → {out}")

    # GPIO bitmap 示例
    print()
    print("GPIO bitmap 示例：uint64_t 一支開關 48 個位置（bit0..47 有效）")
    bits = (1 << 14) | (1 << 1) | (1 << 2)   # Matrix: I80 D0=14, wr=1, dc=2
    print(f"  Matrix 佔用 = 0x{bits:012x}（{bin(bits).count('1')} 支）")
    print(f"  再要 GPIO14 → 衝突？ {(bits >> 14) & 1 == 1}")
    print(f"  再要 GPIO10 → 衝突？ {(bits >> 10) & 1 == 1}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
