#!/usr/bin/env python3
"""psram_bench.py — 讀 EffectPerf 的 0xF067 PSRAMBENCH（大緩衝／混合讀取）。

板上要有 `ESP32-S3-Matrix-EffectPerf`（CONFIG_NETCORE_EFFECT_BENCH=y）。
量的是純資料搬運，不是效果：
  · internal / PSRAM 的 memset、sequential read、memcpy（MiB/s）
  · PSRAM cold read（每次 M2C invalidate）與 writeback（C2M）
  · internal ↔ PSRAM 混合 copy
  · 大 buffer 留在 PSRAM、以 1/4/16/64 KB chunk 讀入 internal scratch

用法：
    python3 tools/psram_bench.py --via http://127.0.0.1:8011 --size-kb 64,256,512
    python3 tools/psram_bench.py --port /dev/cu.usbmodemXXXX --size-kb 256
    python3 tools/psram_bench.py --via … --json
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

CMD = 0xF067
RSP = 0xF068


def ask_direct(port: str, size_kb: int, iters: int, timeout: float) -> dict:
    import serial

    with serial.Serial(port, 115200, timeout=0.2) as s:
        time.sleep(0.6)
        s.reset_input_buffer()
        s.write(nc4_demux.pack(CMD, struct.pack("<HH", size_kb, iters)))
        f = nc4_demux.Reader(s).wait_for({RSP}, timeout)
        if f is None:
            raise SystemExit("沒有回覆：板上有 0xF067 嗎？（EffectPerf 映像）")
        n = struct.unpack_from("<H", f[1])[0]
        return json.loads(f[1][2:2 + n].decode("utf-8", "replace"))


def ask_via(base: str, size_kb: int, iters: int, timeout: float) -> dict:
    req = urllib.request.Request(
        base.rstrip("/") + "/api/cmd",
        data=json.dumps({
            "cmd": f"0x{CMD:04X}",
            "payload_hex": struct.pack("<HH", size_kb, iters).hex(),
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


def show(r: dict) -> None:
    size = r.get("size", 0)
    print(f"\nsize={size // 1024} KB  iters={r.get('it')}  ok={r.get('ok')}")
    it = r.get("int", {})
    ps = r.get("psram", {})
    mix = r.get("mix", {})
    ch = r.get("chunk", {})
    print(f"  internal   wr {it.get('wr', 0):>7}  rd {it.get('rd', 0):>7}"
          f"  cp {it.get('cp', 0):>7}  MiB/s")
    print(f"  PSRAM      wr {ps.get('wr', 0):>7}  wr_cold {ps.get('wr_cold', 0):>7}"
          f"  rd {ps.get('rd', 0):>7}  rd_cold {ps.get('rd_cold', 0):>7}"
          f"  cp {ps.get('cp', 0):>7}")
    print(f"  mixed      i2p {mix.get('i2p', 0):>7}  p2i {mix.get('p2i', 0):>7}"
          f"  p2p {mix.get('p2p', 0):>7}")
    print(f"  chunk read 1K {ch.get('1k', 0):>7}  4K {ch.get('4k', 0):>7}"
          f"  16K {ch.get('16k', 0):>7}  64K {ch.get('64k', 0):>7}  MiB/s")
    ms = r.get("msync_us", {})
    print(f"  msync      C2M {ms.get('c2m', 0)} µs  M2C {ms.get('m2c', 0)} µs"
          f"  err {r.get('msync_err', 0)}")


def main() -> int:
    ap = argparse.ArgumentParser()
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--port", help="直接開 CDC")
    src.add_argument("--via", metavar="URL", help="經 WebMaster REST")
    ap.add_argument("--size-kb", default="256", help="逗號分隔（16..1024）")
    ap.add_argument("--iters", type=int, default=8, help="每組次數（1..32）")
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    sizes = [max(16, min(1024, int(x))) for x in args.size_kb.split(",") if x.strip()]
    iters = max(1, min(32, args.iters))
    out = []
    for kb in sizes:
        r = (ask_direct(args.port, kb, iters, args.timeout) if args.port
             else ask_via(args.via, kb, iters, args.timeout))
        out.append(r)
        if args.json:
            print(json.dumps(r, ensure_ascii=False))
        else:
            show(r)

    if not args.json and len(out) > 1:
        print("\n=== 匯總（MiB/s；PSRAM rd/cold、混合 p2i、chunk 64K）===")
        print(f"{'KB':>6} {'p_rd':>8} {'p_cold':>8} {'p2i':>8} {'chunk64k':>9}")
        for r in out:
            ps, mix, ch = r.get("psram", {}), r.get("mix", {}), r.get("chunk", {})
            print(f"{r.get('size', 0) // 1024:>6} {ps.get('rd', 0):>8}"
                  f" {ps.get('rd_cold', 0):>8} {mix.get('p2i', 0):>8}"
                  f" {ch.get('64k', 0):>9}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
