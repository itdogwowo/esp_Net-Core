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
    ap.add_argument("--variant", type=int, default=1, choices=(0, 1, 2, 3, 4, 17),
                    help="0=每組一段連續、1=每組兩段、2=完全隨機（每顆一條單顆 run）、"
                         "3=每粒燈自己一個 group、4=每個通道一條 entry（4×px 條）、"
                         "17=I80 波形合成 A/B（wave_pack+merge8 vs 展開＋轉置一次過）")
    ap.add_argument("--reps", type=int, default=8)
    ap.add_argument("--all", action="store_true",
                    help="掃 100／500／1000 組 × variant 0／1 ＋ 完全隨機（variant 2）")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--timeout", type=float, default=20.0)
    args = ap.parse_args()

    cases = []
    if args.all:
        for v in (0, 1):
            for g in (100, 500, 1000):
                cases.append((args.px, g, v))
        cases.append((args.px, 0, 2))     # 上限：每顆一條單顆 run
        cases.append((args.px, 0, 3))     # 上限＋每粒燈自己一個 group
        # 通道級：4×px 條 entry，px 大過 ~4,000 就爆 entry 記憶體，所以固定 2,400 px
        cases.append((2400, 0, 4))
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
            # Task 7：6 B 表 vs 舊 run 路徑（同一次量、同一份 layout）
            flat_n = d.get('flat_n')
            runs_us = d.get('apply_runs_us')
            if flat_n is not None and runs_us is not None:
                note = "" if flat_n else "（nflat=0：今次行 runs，同上一行一樣）"
                ratio = (runs_us / d['apply_us']) if d.get('apply_us') else 0.0
                print(f"    6 B 表 {flat_n} 顆：apply={d.get('apply_us')}µs  vs  "
                      f"runs={runs_us}µs（{ratio:.2f}×）{note}")
            # 一幀的「總成本」＝ 效果層生成值流 ＋ 配對層寫入（單子幀）
            vu = d.get('vals_us')
            if vu is not None and d.get('apply_us') is not None:
                tot = vu + d['apply_us']
                print(f"    一幀總成本：值流 {vu}µs ＋ apply {d['apply_us']}µs "
                      f"＝ {tot}µs（nv={d.get('nv')}）")
            # Task 6：I80 波形合成 A/B（variant 17）
            bo, bn, bok = d.get('bus_old_us'), d.get('bus_new_us'), d.get('bus_ok')
            if bo is not None and bn is not None:
                ratio = (bo / bn) if bn else 0.0
                print(f"    I80 合成：舊路徑 {bo}µs  vs  一次過 {bn}µs（{ratio:.2f}×）"
                      f"  逐 byte 相同={bok}")
            # production 的抖動層：4 子幀 ＝ 4 次掃表
            du, dru = d.get('dither_us'), d.get('dither_runs_us')
            if du is not None and dru is not None:
                r2 = (dru / du) if du else 0.0
                print(f"    抖動 4 子幀：{du}µs  vs  {dru}µs（{r2:.2f}×）")
    finally:
        s.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
