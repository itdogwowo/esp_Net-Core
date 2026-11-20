#!/usr/bin/env python3
"""dev_probe.py — 在真機（ESP32-S3／P4）上量效果引擎的 sine 成本同精度。

兩種模式：

  ① 逐次 sine（推薦；需要含 0xF063 SINBENCH 的 build）
     python tools/hosttest/sin_bench/dev_probe.py --port COM24 --sin
     直接量固件那三個函數（q12／lin／sin）的 ns/call ＋ 對 double 的最大誤差；
     唔使切效果、唔使回歸、唔使還原，一次 flash 就有齊 status §7.134「未做」的數字。

  ② 合成斜率（舊模式；只有 0xF060 的 build 都可以）
     python tools/hosttest/sin_bench/dev_probe.py --port COM24 --samples 40
     切效果／分區，讀引擎自己報的 `render_us`，用「有幾多 sine」做線性回歸。
     只能得到「sine ＋ wave 其餘數學」的合成值，分唔開逐次成本。

前提：
    · 板上跑的是有 test_action 的 build，NC4 走 USB CDC
    · 模式① 要 0xF063；模式② 要 0xF060（op：1=EFFECT 2=DITHER 4=GAIN 7=ZMASK 8=BITS 12=ZEFFS 0=STATS）
"""
from __future__ import annotations

import argparse
import json
import statistics
import struct
import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import nc4  # noqa: E402  （同 repo 的 NC4 工具共用 framing）

EFF = {0: "OFF", 1: "ZONE1", 2: "BREATH", 3: "WAVE", 4: "SOLID", 5: "RAMP"}
SINE_PER_FRAME = {0: 0, 1: 0, 2: 8, 3: 72, 4: 0, 5: 0}   # 64 px 的近似正弦呼叫數

# 0xF063 回覆的三個算法（名稱 → 表大小；同 host sin_bench 對得上）
SIN_ALGOS = (("q12", "多項式（MP）", "0 B"),
             ("lin", "256 錨點混合", "514 B"),
             ("sin", "1024 點 Q15 表", "2 KB"))


def send(s, cmd: int, payload: bytes, wait: float = 0.3):
    s.reset_input_buffer()
    s.write(nc4.pack(cmd, payload, nc4.ADDR_BROADCAST))
    dm = nc4.Demux()
    frames = []
    end = time.time() + wait
    while time.time() < end:
        data = s.read(4096)
        if not data:
            continue
        for kind, v in dm.feed(data):
            if kind == "frame":
                frames.append(v)
    return frames


def hub_json(s, cmd: int, payload: bytes = b"", wait: float = 0.3):
    """送一條指令，回傳第一個 0xF004（hub 回覆）裡的 JSON；冇就 None。"""
    for c, p in send(s, cmd, payload, wait):
        if c == 0xF004 and len(p) > 3:
            n = struct.unpack_from("<H", p, 0)[0]
            try:
                return json.loads(p[2:2 + n].decode("utf-8", "replace"))
            except json.JSONDecodeError:
                pass
    return None


def stats(s):
    return hub_json(s, 0xF060, b"\x00", 0.2)


def sample_render_us(s, n: int):
    vals = []
    for _ in range(n):
        st = stats(s)
        if st is not None:
            vals.append(st["render_us"])
    return vals


def summarize(vals):
    vals = sorted(vals)
    if not vals:
        return None
    return vals[0], statistics.median(vals), vals[int(len(vals) * 0.9)], len(vals)


def run_sin(s, iters: int, as_json: bool) -> int:
    """模式①：0xF063 SINBENCH → 逐次 ns ＋ 精度。"""
    r = hub_json(s, 0xF063, struct.pack("<H", iters), 2.0)
    if r is None:
        print("冇回覆：板上 build 要有 0xF063（test_action ＋ CONFIG_NETCORE_PIXEL）")
        return 1
    if as_json:
        print(json.dumps(r, ensure_ascii=False))
        return 0
    print(f"iters={r.get('n')}  取樣點={r.get('smp')}（誤差 = milli-LSB，即 1/100 LSB）")
    print(f"{'algo':<6}{'ns/call':>9}{'err LSB':>10}   {'表':<6}{'說明'}")
    for key, desc, table in SIN_ALGOS:
        a = r.get(key, {})
        ns = a.get("ns")
        err = a.get("err")
        print(f"{key:<6}{ns if ns is not None else '?':>9}"
              f"{(err / 100.0) if err is not None else float('nan'):>10.2f}   {table:<6}{desc}")
    print("\n判讀：ns 係實機數字（240 MHz S3／360 MHz P4），同 host 的相對次序應該一致；"
          "\n      誤差同 host sin_bench 對得上就代表兩邊真係同一份算法。")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="例如 COM24 或 /dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--sin", action="store_true",
                    help="模式①：0xF063 SINBENCH（逐次 sine 成本＋精度），推薦")
    ap.add_argument("--iters", type=int, default=50000, help="模式① 每個算法的迭代數（100..500000）")
    ap.add_argument("--samples", type=int, default=40, help="模式② 每個效果取幾多次 render_us")
    ap.add_argument("--json", action="store_true", help="模式① 直接印原始 JSON")
    args = ap.parse_args()

    with serial.Serial(args.port, args.baud, timeout=0.2) as s:
        time.sleep(0.6)
        if args.sin:
            iters = max(100, min(500000, args.iters))
            return run_sin(s, iters, args.json)

        send(s, 0xF060, bytes([8, 8]))      # BITS = 8（8-bit 原生）
        send(s, 0xF060, bytes([2, 0]))      # DITHER off
        send(s, 0xF060, bytes([7, 0xFF]))   # 8 區全開
        send(s, 0xF060, bytes([4, 60]))     # GAIN 60（預設）

        print(f"{'effect':<8}{'sine/frame':>11}{'min':>7}{'median':>8}{'p90':>7}{'n':>4}")
        rows = []
        for e, name in EFF.items():
            send(s, 0xF060, bytes([1, e]))
            time.sleep(0.35)
            r = summarize(sample_render_us(s, args.samples))
            if r is None:
                print(f"{name:<8}  no data")
                continue
            mn, med, p90, n = r
            rows.append((name, SINE_PER_FRAME[e], mn, med, p90))
            print(f"{name:<8}{SINE_PER_FRAME[e]:>11}{mn:>7.0f}{med:>8.0f}{p90:>7.0f}{n:>4}")

        # 線性回歸：median render_us 對 sine 數 → 每粒 sine（連同 wave 其他數學）幾 ns
        pts = [(sine, med) for (_n, sine, _mn, med, _p) in rows]
        if len(pts) >= 2:
            base = next((m for (s, m) in pts if s == 0), None)
            top = max(pts, key=lambda p: p[0])
            if base is not None and top[0] > 0:
                per = (top[1] - base) / top[0] * 1000.0
                print(f"\n斜率：({top[1]:.0f} − {base:.0f}) µs ÷ {top[0]} sine "
                      f"≈ {per:.0f} ns /（sine ＋ wave 其餘數學）")
                print("逐次成本請用 --sin（0xF063）；這一組係合成值。")

        # 還原
        send(s, 0xF060, bytes([12]) + bytes([255] * 8))
        send(s, 0xF060, bytes([1, 2]))
        print("已還原：分區效果跟全域、effect = BREATH")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
