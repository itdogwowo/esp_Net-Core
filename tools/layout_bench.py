#!/usr/bin/env python3
"""layout_bench.py — 排版編譯器（`nc_px_layout`）真機量測：0xF077 LAYOUTBENCH。

需要 **EffectPerf 映像**（`ESP32-S3-Matrix-EffectPerf`）＋ OptionRom 無關；
Zero-Show 不含這條指令。payload `[px:u16][groups:u16][variant:u8][reps:u8]`：
  variant 0 = 每組一段連續 run；1 = 每組兩段（跨區，模擬零件散落幾條燈帶）。

用法：
  python3 tools/layout_bench.py --port /dev/cu.usbmodemXXXX --all
  python3 tools/layout_bench.py --port ... --px 9600 --groups 500 --variant 1
"""
import argparse
import struct
import sys
import time

import serial

sys.path.insert(0, "tools")
import nc4  # noqa: E402


def run(ser, px, groups, variant, reps, timeout):
    payload = struct.pack("<HHBB", px, groups, variant, reps)
    ser.reset_input_buffer()
    ser.write(nc4.pack(0xF077, payload, 0xFFFF))
    r = nc4.wait_for(ser, 0xF078, timeout)
    if r is None:
        return None
    n = struct.unpack_from("<H", r)[0]
    return r[2:2 + n].decode("utf-8", "replace")


def main():
    ap = argparse.ArgumentParser(description="排版編譯器真機量測（0xF077）")
    ap.add_argument("--port", required=True)
    ap.add_argument("--px", type=int, default=9600)
    ap.add_argument("--groups", type=int, default=500)
    ap.add_argument("--variant", type=int, default=1, choices=(0, 1))
    ap.add_argument("--reps", type=int, default=8)
    ap.add_argument("--all", action="store_true", help="掃 100／500／1000 組 × variant 0／1")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--timeout", type=float, default=20.0)
    args = ap.parse_args()

    cases = []
    if args.all:
        for v in (0, 1):
            for g in (100, 500, 1000):
                cases.append((args.px, g, v))
    else:
        cases.append((args.px, args.groups, args.variant))

    s = serial.Serial(args.port, 115200, timeout=0.2)
    time.sleep(0.6)
    try:
        for (px, groups, variant) in cases:
            t0 = time.time()
            out = run(s, px, groups, variant, args.reps, args.timeout)
            if args.json:
                print(out)
                continue
            if out is None:
                print(f"px={px} groups={groups} variant={variant}: 冇回覆")
                continue
            import json
            try:
                d = json.loads(out)
            except ValueError:
                print(out)
                continue
            print(f"px={d.get('px')} groups={d.get('groups')} v={d.get('variant')} "
                  f"→ runs={d.get('runs')}  build={d.get('build_us')}µs  "
                  f"apply8={d.get('apply_us')}µs  apply12={d.get('apply12_us')}µs  "
                  f"（{time.time() - t0:.1f}s）")
    finally:
        s.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
