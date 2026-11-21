#!/usr/bin/env python3
"""pipe_bench.py — 9,600 個效果值 → 配對 → 12→8 的整條生產線（0xF069）。

板上要有 `ESP32-S3-Matrix-EffectPerf`。量：
  gen        效果生成器（nc_px_job_vals）產生 nv 個 12-bit 值
  pair_range 現行 range 配對（nc_px_pair_apply12）
  pair_table 連續 offs 查表配對
  pair_rnd   打亂 offs 查表配對（12-bit canvas，模擬非連續 mapping）
  pair_rnd8  同上但 8-bit canvas（production pair 寫法）
  pair_inv8  destination 順序寫、source 隨機讀
  pair_blk8  16 px 一段的 block-random（真實多組別近似）
  conv       12-bit → 8-bit（捨入）
  full_range gen + pair_range + conv
  full_table gen + pair_table + conv
  full_rnd   gen + pair_rnd + conv
  full_ps    gen + pair_range + conv，12-bit canvas 在 PSRAM

RGBW→I80 編碼／送燈唔在呢支量（render backend 已有）。

用法：
    python3 tools/pipe_bench.py --via http://127.0.0.1:8011 --px 9600 --mode wwww
    python3 tools/pipe_bench.py --via http://127.0.0.1:8011 --px 9600 --mode rgb
    python3 tools/pipe_bench.py --via http://127.0.0.1:8011 --px 9600 --mode w
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nc4_demux  # noqa: E402

CMD = 0xF069
RSP = 0xF06A
MODES = {"wwww": 0, "rgb": 1, "w": 2}


def ask_direct(port: str, px: int, frames: int, mode: int, timeout: float) -> dict:
    import serial

    with serial.Serial(port, 115200, timeout=0.2) as s:
        time.sleep(0.6)
        s.reset_input_buffer()
        s.write(nc4_demux.pack(CMD, struct.pack("<HHB", px, frames, mode)))
        f = nc4_demux.Reader(s).wait_for({RSP}, timeout)
        if f is None:
            raise SystemExit("沒有回覆：板上有 0xF069 嗎？")
        n = struct.unpack_from("<H", f[1])[0]
        return json.loads(f[1][2:2 + n].decode("utf-8", "replace"))


def ask_via(base: str, px: int, frames: int, mode: int, timeout: float,
            c8_psram: bool = False) -> dict:
    req = urllib.request.Request(
        base.rstrip("/") + "/api/cmd",
        data=json.dumps({
            "cmd": f"0x{CMD:04X}",
            "payload_hex": (struct.pack("<HHBB", px, frames, mode,
                                        1 if c8_psram else 0).hex()),
            "expect": [f"0x{RSP:04X}"],
            "timeout": timeout,
        }).encode(),
        headers={"Content-Type": "application/json", "X-Actor": "ai"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout + 5) as r:
            body = json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        raise SystemExit(f"WebMaster 回 {e.code}：{e.read().decode(errors='replace')}")
    except urllib.error.URLError as e:
        raise SystemExit(f"連不上 WebMaster（{base}）：{e.reason}")
    if not body.get("ok") or body.get("json") is None:
        raise SystemExit(f"回覆不是 JSON：{body}")
    return body["json"]


def fmt_us(v) -> str:
    if not v:
        return "—"
    if v >= 1000:
        return f"{v / 1000:.3f} ms"
    return f"{v} µs"


def show(r: dict) -> None:
    mode = {0: "wwww（1 值/px）", 1: "rgb（3 值/px）", 2: "w（1 值/px）"}.get(r.get("mode"), "?")
    print(f"\npx={r.get('px')}  nv={r.get('nv')}  mode={mode}  frames={r.get('fr')}")
    nv = max(1, r.get('nv', 1))
    px = max(1, r.get('px', 1))
    gen_ns = r.get('gen_us', 0) * 1000.0 / nv
    print(f"  gen        {fmt_us(r.get('gen_us'))}   （{gen_ns:.0f} ns/值）"
          f"   cached {fmt_us(r.get('gen_cached_us'))}")
    p = r.get("pair_us", {})
    print(f"  pair       range {fmt_us(p.get('range'))}   table {fmt_us(p.get('table'))}"
          f"   random12 {fmt_us(p.get('rnd'))}   packed12 {fmt_us(p.get('packed12'))}"
          f"   （range {p.get('range', 0) * 1000.0 / px:.0f} ns/px）")
    print(f"             random8 {fmt_us(p.get('rnd8'))}   inverse8 {fmt_us(p.get('inv8'))}"
          f"   block8 {fmt_us(p.get('blk8'))}")
    print(f"  conv       {fmt_us(r.get('conv_us'))}")
    f = r.get("full_us", {})
    print(f"  full       range {fmt_us(f.get('range'))}   table {fmt_us(f.get('table'))}"
          f"   random {fmt_us(f.get('rnd'))}   random8 {fmt_us(f.get('rnd8'))}"
          f"   cached8 {fmt_us(f.get('cached8'))}   inv8 {fmt_us(f.get('inv8'))}"
          f"   blk8 {fmt_us(f.get('blk8'))}   psram12 {fmt_us(f.get('ps'))}")
    mem = r.get("mem", {})
    print(f"  mem        c12_int={mem.get('c12_int')} c12_ps={mem.get('c12_ps')}"
          f"  c8_int={mem.get('c8_int')} c8_ps={mem.get('c8_ps')}")
    best = f.get("range") or 0
    if best:
        print(f"  → 50 FPS（20 ms）預算：full range {best / 20000 * 100:.1f}%"
              f"；剩 {(20000 - best) / 1000:.2f} ms")
        print(f"  → 60 FPS（16.67 ms）：{best / 16667 * 100:.1f}%"
              f"；剩 {(16667 - best) / 1000:.2f} ms")


def main() -> int:
    ap = argparse.ArgumentParser()
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--port", help="直接開 CDC")
    src.add_argument("--via", metavar="URL", help="經 WebMaster REST")
    ap.add_argument("--px", type=int, default=9600)
    ap.add_argument("--frames", type=int, default=5, help="每組幀數（1..8）")
    ap.add_argument("--mode", choices=sorted(MODES), default="wwww")
    ap.add_argument("--c8-psram", action="store_true",
                    help="強制 8-bit canvas 放 PSRAM（固定 policy 測試）")
    ap.add_argument("--timeout", type=float, default=120.0)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    px = max(8, min(16384, args.px))
    frames = max(1, min(8, args.frames))
    mode = MODES[args.mode]
    r = (ask_direct(args.port, px, frames, mode, args.timeout) if args.port
         else ask_via(args.via, px, frames, mode, args.timeout,
                      args.c8_psram))
    if args.json:
        print(json.dumps(r, ensure_ascii=False))
    else:
        show(r)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
