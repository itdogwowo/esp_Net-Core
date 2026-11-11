#!/usr/bin/env python3
"""8×8 WS2812 矩陣（0xF060 MATRIX）的小工具。

    tools/matrix.py --port /dev/cu.usbmodem* stats
    tools/matrix.py --port /dev/cu.usbmodem* effect breath
    tools/matrix.py --port /dev/cu.usbmodem* dither off
    tools/matrix.py --port /dev/cu.usbmodem* ramp        # 效果 5（暗階 A/B 用）
    tools/matrix.py --port /dev/cu.usbmodem* zone 1 2 1  # 第 1 區 → rate 2、phase 1
    tools/matrix.py --port /dev/cu.usbmodem* zmask 0x03  # 只開第 0、1 區
    tools/matrix.py --port /dev/cu.usbmodem* fps 300
    tools/matrix.py --port /dev/cu.usbmodem* gain 64
    tools/matrix.py --port /dev/cu.usbmodem* color 24 8 0

回覆是 0xF004 + u16 長度 + JSON（同其他測試指令）。
指令契約：docs/01_protocol/commands.md §2（0xF060）；原理：docs/03_notes/rgb-matrix-dither.md
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nc4_demux import ADDR_BROADCAST, Reader, pack  # noqa: E402

CMD_MATRIX = 0xF060
CMD_REPLY = 0xF004

EFF_ALIAS = {
    "off": 0, "zone1": 1, "breath": 2, "wave": 3, "solid": 4, "ramp": 5,
}

EFFECTS = {
    "off": 0,
    "zone1": 1,
    "breath": 2,
    "wave": 3,
    "solid": 4,
    "ramp": 5,
}


def payload_for(args) -> bytes:
    if args.what == "stats":
        return bytes([0])
    if args.what == "effect":
        return bytes([1, EFFECTS[args.value]])
    if args.what == "dither":
        m = {"off": 0, "temporal": 1, "spatial": 2, "on": 1, "1": 1, "0": 0, "yes": 1}
        return bytes([2, m[args.value]])
    if args.what == "color":
        r, g, b = args.value
        return bytes([3, r, g, b])
    if args.what == "gain":
        return bytes([4, args.value])
    if args.what == "fps":
        return bytes([5]) + struct.pack("<H", args.value)
    if args.what == "zone":
        idx, rate, phase = args.value[:3]
        out = bytes([6, idx, rate, phase])
        if len(args.value) >= 4:
            out += bytes([args.value[3]])
        return out
    if args.what == "bits":
        return bytes([8, args.value])
    if args.what == "zeff":
        idx, eff = args.value
        return bytes([11, idx, EFF_ALIAS.get(eff, eff)]) if isinstance(eff, str) else bytes([11, idx, eff])
    if args.what == "zeffs":
        vals = [EFF_ALIAS.get(x, x) for x in args.value.split(",")]
        vals = [int(v, 0) if isinstance(v, str) else v for v in vals]
        if len(vals) != 8:
            raise SystemExit("要 8 個效果，例如 2,3,4,1,2,3,4,5")
        return bytes([12] + vals)
    if args.what == "zdither":
        vals = [int(x, 0) for x in args.value.split(",")]
        if len(vals) != 8:
            raise SystemExit("要 8 個模式，例如 0,1,1,1,1,1,1,2")
        return bytes([15] + vals)
    if args.what == "frac":
        return bytes([16, args.value])
    if args.what == "wspeed":
        return bytes([14]) + struct.pack("<H", args.value)
    if args.what == "demo":
        return bytes([13, 1 if args.value in ("on", "1", "yes") else 0])
    if args.what == "zdemo":
        return bytes([12] + [2, 3, 4, 5, 2, 3, 4, 5])
    if args.what == "zones":
        vals = [int(x, 0) for x in args.value.split(",")]
        if len(vals) != 8:
            raise SystemExit("要 8 個位數，例如 8,9,10,11,12,13,14,15")
        return bytes([10] + vals)
    if args.what == "speed":
        return bytes([9]) + struct.pack("<H", args.value)
    if args.what == "zmask":
        return bytes([7, args.value])
    raise SystemExit(f"未知子指令：{args.what}")


def main() -> int:
    ap = argparse.ArgumentParser(description="8×8 WS2812 矩陣控制（0xF060）")
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--addr", type=lambda v: int(v, 0), default=ADDR_BROADCAST)
    ap.add_argument("--raw", action="store_true", help="直接印回覆 JSON 原文")
    sub = ap.add_subparsers(dest="what", required=True)
    sub.add_parser("stats", help="幀率／late／tx_us／每區 rate+phase")
    p = sub.add_parser("effect", help="0 off 1 zone1 2 breath 3 wave 4 solid 5 ramp")
    p.add_argument("value", choices=sorted(EFFECTS))
    p = sub.add_parser("dither", help="off / temporal（時間）/ spatial（空間 4×4）")
    p.add_argument("value", choices=["off", "temporal", "spatial", "on", "1", "0", "yes"])
    p = sub.add_parser("color", help="SOLID 的 r g b（0..255）")
    p.add_argument("value", nargs=3, type=int)
    p = sub.add_parser("gain", help="整體亮度 0..255")
    p.add_argument("value", type=int)
    p = sub.add_parser("fps", help="目標幀率（上限 460）")
    p.add_argument("value", type=int)
    p = sub.add_parser("zone", help="第 idx 區（0..7）的 rate（1..8）、phase、[bits]")
    p.add_argument("value", nargs="+", type=int)
    p = sub.add_parser("bits", help="全域模擬位數 8..16（12 = 每級 1/16，8 = 原生）")
    p.add_argument("value", type=int)
    p = sub.add_parser("zdither", help="8 區抖動模式（0 off / 1 temporal / 2 spatial）")
    p.add_argument("value", help="逗號分隔 8 個，例如 0,1,1,1,1,1,1,2")
    p = sub.add_parser("frac", help="SOLID 再加 1/256 級（0..255）：color 3 + frac 128 = 顯示 3.5")
    p.add_argument("value", type=int)
    p = sub.add_parser("wspeed", help="波浪掃一個週期幾多 ms（預設 3000，越大越慢）")
    p.add_argument("value", type=int)
    p = sub.add_parser("demo", help="自動 demo on／off（10 段循環；開機預設開）")
    p.add_argument("value", choices=["on", "off", "1", "0", "yes"])
    p = sub.add_parser("zeff", help="單區效果：idx，eff（off/zone1/breath/wave/solid/ramp）")
    p.add_argument("value", nargs=2, type=str)
    p = sub.add_parser("zeffs", help="8 區效果一次過（逗號分隔，可用名）")
    p.add_argument("value")
    sub.add_parser("zdemo", help="8 區各唔同效果（呼吸/波浪/單色/色階 輪住）")
    p = sub.add_parser("zones", help="一次過設 8 區位數（8,9,...,15），並排比較用")
    p.add_argument("value", help="逗號分隔 8 個 8..16")
    p = sub.add_parser("speed", help="呼吸每段毫秒（預設 4000 → R/G/B 共 12 s）")
    p.add_argument("value", type=int)
    p = sub.add_parser("zmask", help="bit k = 第 k 區輸出（例如 0x01）")
    p.add_argument("value", type=lambda v: int(v, 0))
    sub.add_parser("sweep", help="8 區 8..15 位並排（配合 solid 暗階睇邊個最順）")
    args = ap.parse_args()

    if args.what == "sweep":
        payload = bytes([10] + [8 + i for i in range(8)])
    else:
        payload = payload_for(args)

    import serial  # pyserial

    with serial.Serial(args.port, args.baud, timeout=0.2) as s:
        time.sleep(0.3)
        s.write(pack(CMD_MATRIX, payload, args.addr))
        resp = Reader(s).wait_for({CMD_REPLY}, 2.0)
        if resp is None:
            print("沒有回覆（裝置沒開？firmware 冇 CONFIG_NETCORE_RGB_MATRIX？）",
                  file=sys.stderr)
            return 1
        _cmd, data = resp
        if len(data) >= 2 and struct.unpack_from("<H", data, 0)[0] == len(data) - 2:
            data = data[2:]
        text = data.decode("utf-8", "replace")
        if args.raw:
            print(text)
            return 0
        if args.what == "sweep":
            print("8 區已設為 8,9,10,11,12,13,14,15 位（第 0 行 = 8 位 … 第 7 行 = 15 位）")
            print("跟住：effect solid + color 8 8 8 + gain 170，睇邊一行最順又唔閃")
        try:
            print(json.dumps(json.loads(text), indent=2, ensure_ascii=False))
        except json.JSONDecodeError:
            print(text)
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
