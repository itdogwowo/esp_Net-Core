#!/usr/bin/env python3
"""Task 6 I80 波形合成 A/B —— 一次性 lab 工具（唔係主體工具）。

用途：對 EffectPerf 映像送 0xF077 LAYOUTBENCH 的 variant 17／18，量
「舊 wave_pack＋merge8」vs「新 nc_px_codec_pack_bus」的 encode 時間，
並核對逐 byte 相同（bus_ok）。

為什麼在 modules/lab：
- 這是一次性量測（Task 6），唔應該加進 tools/layout_bench.py 的通用介面；
- firmware 側的 variant 17／18 只在 test-only 的
  firmware/components/bench/effect_bench/nc_layout_bench.c，唔入 production；
- 量完／16-lane 驗證完可以整包移除。

用法：
  IDF_PATH=$HOME/esp/esp-idf python3 make.py build ESP32-S3-Matrix \\
      --project ESP32-S3-Matrix-EffectPerf
  python3 tools/ota.py --port /dev/cu.usbmodemXXXX \\
      artifacts/ESP32-S3-Matrix__ESP32-S3-Matrix-EffectPerf.bin
  python3 modules/lab/i80_pack_lab/run_ab.py --port /dev/cu.usbmodemXXXX \\
      --px 9600 --variant 17 --reps 8

variant 17 = 8-bit／無抖動（production 廣播路徑）
variant 18 = 12-bit dither（完整 8×8 轉置）
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import time
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
import nc4  # noqa: E402
from nc4_demux import Reader  # noqa: E402

CMD_BENCH = 0xF077
CMD_RSP = 0xF078


def main() -> int:
    ap = argparse.ArgumentParser(description="Task 6 I80 pack A/B（lab）")
    ap.add_argument("--port", required=True)
    ap.add_argument("--px", type=int, default=9600)
    ap.add_argument("--groups", type=int, default=500)
    ap.add_argument("--variant", type=int, default=17, choices=(17, 18),
                    help="17=8-bit 廣播、18=12-bit dither 完整轉置")
    ap.add_argument("--reps", type=int, default=8)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    payload = struct.pack("<HHBB", args.px, args.groups, args.variant, args.reps)
    with serial.Serial(args.port, 115200, timeout=0.2) as s:
        time.sleep(0.6)
        s.reset_input_buffer()
        s.write(nc4.pack(CMD_BENCH, payload, 0xFFFF))
        s.flush()
        r = Reader(s).wait_for({CMD_RSP}, args.timeout)

    if r is None:
        print("冇收到 0xF078（EffectPerf 映像？0xF077 有冇 compile？）", file=sys.stderr)
        return 1

    p = r[1]
    n = struct.unpack_from("<H", p)[0]
    text = p[2:2 + n].decode("utf-8", "replace")
    if args.json:
        print(text)
        return 0
    d = json.loads(text)
    old = d.get("bus_old_us", 0)
    new = d.get("bus_new_us", 0)
    ratio = (old / new) if new else 0.0
    print(f"px={d.get('px')} groups={d.get('groups')} variant={d.get('variant')} "
          f"reps={args.reps}")
    print(f"  舊路徑 {old} µs  →  新路徑 {new} µs（{ratio:.2f}×）")
    print(f"  逐 byte 相同 bus_ok={d.get('bus_ok')}  tick={d.get('px')} px")
    if d.get("bus_ok") != 1:
        print("  ⚠ bus_ok != 1：新舊輸出不同，唔可以當通過", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
