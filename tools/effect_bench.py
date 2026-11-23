#!/usr/bin/env python3
"""effect_bench.py — 讀 ESP32-S3-Matrix-EffectPerf 的 0xF065 EFBENCH 結果。

這個工具只做一件事：叫板上「純效果」bench 跑一組 px，印成人看的表。
它**不**開啟序列埠以外的東西；如果 WebMaster 已經佔住 CDC，用 `--via`
走它的 REST `/api/cmd`（同一個 server、同一條連線，兩邊可以同時看）。

用法：
    # WebMaster 已開（建議；UI 同 AI 共用同一條 CDC）
    python3 tools/effect_bench.py --via http://127.0.0.1:8011 --px 64,960,9600

    # 直接開 CDC（WebMaster 未開時）
    python3 tools/effect_bench.py --port /dev/cu.usbmodemXXXX --px 64

    # 只看原始 JSON
    python3 tools/effect_bench.py --via http://127.0.0.1:8011 --json

板上要跑 `ESP32-S3-Matrix-EffectPerf`（CONFIG_NETCORE_EFFECT_BENCH=y）。
回覆是 0xF066，payload = 2 B 長度前綴 + JSON。
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

CMD = 0xF065
RSP = 0xF066


def ask_direct(port: str, px: int, frames: int, vpp: int, timeout: float) -> dict:
    import serial

    with serial.Serial(port, 115200, timeout=0.2) as s:
        time.sleep(0.6)
        payload = struct.pack("<HHH", px, frames, vpp)
        s.reset_input_buffer()
        s.write(nc4_demux.pack(CMD, payload))
        rd = nc4_demux.Reader(s)
        f = rd.wait_for({RSP}, timeout)
        if f is None:
            raise SystemExit("沒有回覆：板上是 EffectPerf 映像嗎？（0xF065／0xF066）")
        n = struct.unpack_from("<H", f[1])[0]
        return json.loads(f[1][2:2 + n].decode("utf-8", "replace"))


def ask_via(base: str, px: int, frames: int, vpp: int, timeout: float) -> dict:
    payload = struct.pack("<HHH", px, frames, vpp).hex()
    req = urllib.request.Request(
        base.rstrip("/") + "/api/cmd",
        data=json.dumps({
            "cmd": f"0x{CMD:04X}",
            "payload_hex": payload,
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


def fmt_ns(v) -> str:
    if not v:
        return "—"
    if v >= 1_000_000:
        return f"{v / 1_000_000:.3f} ms"
    if v >= 1_000:
        return f"{v / 1_000:.2f} µs"
    return f"{v} ns"


def placement(r: dict) -> str:
    m = r.get("mem", {})
    parts = []
    if m.get("c8i"):
        parts.append("8i")
    if m.get("c8p"):
        parts.append("8p")
    if m.get("c12i"):
        parts.append("12i")
    if m.get("c12p"):
        parts.append("12p")
    return "+".join(parts) or "none"


def show(r: dict) -> None:
    px = r.get("px", 0)
    print(f"\npx={px}  frames={r.get('fr')}  reps={r.get('rep')}  nv={r.get('nv')}"
          f"  canvas={placement(r)}")
    eff = r.get("eff") or {}
    if eff:
        vpp = eff.get("vpp", 1)
        ev = eff.get("ev", 0) or 1
        print(f"  ── 效果層：每顆 {vpp} 個值 → 每幀 {eff.get('ev')} 個值"
              f"（{px} px × {vpp}）──")
        print(f"  建立值流        interpreter {fmt_ns(eff.get('vals_ns'))}"
              f"   波表(%%) {fmt_ns(eff.get('stream_ns'))}"
              f"   波表(快·無除法) {fmt_ns(eff.get('fast_ns'))}")
        fast = eff.get("fast_ns") or 0
        if fast:
            print(f"  波表快路徑     {fmt_ns(fast)} → {fast / 20_000_000 * 100:.1f}% of 20 ms"
                  f"   （{fast / max(1, ev):.1f} ns/值）")
        print(f"  波表(%% 取模)   {fmt_ns(eff.get('stream_ns'))} → "
              f"{(eff.get('stream_ns') or 0) / 20_000_000 * 100:.1f}% of 20 ms"
              f"   （{(eff.get('stream_ns') or 0) / max(1, ev):.1f} ns/值）")
        build = eff.get("build_us") or []
        per = eff.get("per_val_ns") or []
        names = ["正弦 math_now", "方波 square", "脈衝 pulse_wave", "保持 keep"]
        if len(build) == 4 and len(per) == 4:
            print("  一次性（建整條 256 個值的波）：")
            for i, nm in enumerate(names):
                print(f"    {nm:18s} {build[i]:7d} µs   每值 {per[i]:4d} ns"
                      f"   → 每幀 {ev} 值 ≈ {per[i] * ev / 1_000_000:.3f} ms")
    print(f"  eval            {fmt_ns(r.get('eval_ns'))} / 值"
          f"   （舊節每幀 {r.get('nv')} 個值：{fmt_ns(r.get('vals_ns'))}）")
    p8, p12 = r.get("pair8", {}), r.get("pair12", {})
    print(f"  pair8           int {fmt_ns(p8.get('i'))}   psram {fmt_ns(p8.get('p'))}")
    print(f"  pair12          int {fmt_ns(p12.get('i'))}   psram {fmt_ns(p12.get('p'))}")
    cv = r.get("conv", {})
    print(f"  12→8 捨入       int {fmt_ns(cv.get('i'))}   psram {fmt_ns(cv.get('p'))}")
    print(f"  12→8 抖動       int {fmt_ns(cv.get('di'))}   psram {fmt_ns(cv.get('dp'))}")
    pipe = r.get("pipe", {})
    print(f"  整條效果        i8 {fmt_ns(pipe.get('i8'))}   p8 {fmt_ns(pipe.get('p8'))}")
    print(f"                  i12 {fmt_ns(pipe.get('i12'))}   p12 {fmt_ns(pipe.get('p12'))}"
          f"   mix {fmt_ns(pipe.get('mix'))}")
    ch = r.get("chunk", {})
    print(f"  分塊 64/256/1024  {fmt_ns(ch.get('64'))} / {fmt_ns(ch.get('256'))}"
          f" / {fmt_ns(ch.get('1024'))}")
    jb = r.get("jobs", {})
    print(f"  多 job 2/4      {fmt_ns(jb.get('j2'))} / {fmt_ns(jb.get('j4'))}")

    best = pipe.get("i12") or pipe.get("mix") or pipe.get("i8") or 0
    if best and px:
        print(f"  → 12-bit 全鏈每顆 {best / px:.1f} ns/px"
              f"；50 FPS（20 ms）預算的 {best / 20_000_000 * 100:.1f}%")


def main() -> int:
    ap = argparse.ArgumentParser()
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--port", help="直接開 CDC，例如 /dev/cu.usbmodem…")
    src.add_argument("--via", metavar="URL", help="經 WebMaster REST，例如 http://127.0.0.1:8011")
    ap.add_argument("--px", default="64", help="逗號分隔的燈鏈長度（預設 64＝8×8）")
    ap.add_argument("--vpp", type=int, default=1,
                    help="每顆幾個值：1=單色、3=rgb、4=rgbw（預設 1）；"
                         "效果層每幀就算 px×vpp 個值")
    ap.add_argument("--frames", type=int, default=0, help="每組幀數（0 = 板上按 px 自動）")
    ap.add_argument("--timeout", type=float, default=90.0, help="單次指令等待秒數（預設 90）")
    ap.add_argument("--json", action="store_true", help="只印原始 JSON")
    args = ap.parse_args()

    vpp = max(1, min(4, args.vpp))
    px_list = [max(8, min(16384, int(x))) for x in args.px.split(",") if x.strip()]
    out = []
    for px in px_list:
        if args.port:
            r = ask_direct(args.port, px, args.frames, vpp, args.timeout)
        else:
            r = ask_via(args.via, px, args.frames, vpp, args.timeout)
        out.append(r)
        if args.json:
            print(json.dumps(r, ensure_ascii=False))
        else:
            show(r)

    if not args.json and len(out) > 1:
        print("\n=== 匯總（效果層：每幀 ev 個值；int＝internal、ps＝PSRAM、mix＝12p→8i）===")
        print(f"{'px':>6} {'vpp':>4} {'ev':>7} {'值流(表)':>11} {'值流(逐值)':>12}"
              f" {'i12':>10} {'p12':>10} {'mix':>10}")
        for r in out:
            p = r.get("pipe", {})
            eff = r.get("eff") or {}
            print(f"{r.get('px'):>6} {eff.get('vpp', 1):>4} {eff.get('ev', 0):>7}"
                  f" {fmt_ns(eff.get('stream_ns')):>11} {fmt_ns(eff.get('vals_ns')):>12}"
                  f" {fmt_ns(p.get('i12')):>10} {fmt_ns(p.get('p12')):>10}"
                  f" {fmt_ns(p.get('mix')):>10}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
