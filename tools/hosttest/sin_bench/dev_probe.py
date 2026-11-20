#!/usr/bin/env python3
"""dev_probe.py — 在真機（ESP32-S3）上量效果引擎的 per-frame 成本。

為什麼要這支：正弦算法（查表 vs 多項式）在 host 上的比值，唔一定-transfer 到板上。
板上唯一唔使 flash 新固件的方法，是用 0xF060 切效果／分區，再讀 `render_us`
（引擎自己量的「effect_render + 抖動 + emit」時間），用「有幾多 sine」做線性回歸。

用法：
    python tools/hosttest/sin_bench/dev_probe.py --port COM24
    python tools/hosttest/sin_bench/dev_probe.py --port COM24 --samples 60

前提：
    · 板上跑的是有 test_action（0xF060）的 build，NC4 走 USB CDC
    · 0xF060 op：1=EFFECT 2=DITHER 4=GAIN 7=ZMASK 8=BITS 12=ZEFFS 0=STATS
    · 量完會還原：分區效果 → 跟全域、effect → BREATH
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


def stats(s):
    for cmd, payload in send(s, 0xF060, b"\x00", 0.2):
        if cmd == 0xF004 and len(payload) > 3:
            n = struct.unpack_from("<H", payload, 0)[0]
            try:
                return json.loads(payload[2:2 + n].decode("utf-8", "replace"))
            except json.JSONDecodeError:
                pass
    return None


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


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="例如 COM24 或 /dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--samples", type=int, default=40)
    args = ap.parse_args()

    with serial.Serial(args.port, args.baud, timeout=0.2) as s:
        time.sleep(0.6)
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

        # 還原
        send(s, 0xF060, bytes([12]) + bytes([255] * 8))
        send(s, 0xF060, bytes([1, 2]))
        print("已還原：分區效果跟全域、effect = BREATH")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
