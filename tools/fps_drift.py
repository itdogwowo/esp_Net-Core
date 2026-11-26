#!/usr/bin/env python3
"""fps_drift.py — 重複量測 ESP 內部時間漂移（ppm），估算兩塊 ESP 幾久要強制同步。

每輪：清零 0xF081 → 等 window 秒 → 讀 avg（逐幀間隔）→ ppm = (avg-target)/target。
重複 N 輪，輸出每輪 ppm、平均／標準差／最大，並換算：
  兩塊 ESP 相對漂移 ≈ 2 × |ppm|（保守）
  tolerance（1/5/20 ms）→ 建議 resync 間隔 = tolerance / (相對漂移 × 1e-6)
"""
import argparse
import json
import statistics
import struct
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import nc4

CMD = 0xF081
RSP = 0xF082


def ask(port, reset=False, timeout=5.0):
    import serial
    payload = b"\x01" if reset else b""
    s = serial.Serial(port, 115200, timeout=0.3)
    try:
        s.write(nc4.pack(CMD, payload, 0xFFFF))
        r = nc4.wait_for(s, RSP, timeout)
    finally:
        s.close()
    if r is None:
        return None
    n = struct.unpack_from("<H", r)[0]
    try:
        return json.loads(r[2:2 + n].decode("utf-8", "replace"))
    except Exception:
        return None


def sum_avg(base, d):
    """精確平均間隔：用 (sum_us, n) 差分，避開 avg_us 只有整數 µs 的 ±50 ppm 量化。"""
    dn = int(d.get("n", 0)) - int(base.get("n", 0))
    ds = int(d.get("sum_us", 0)) - int(base.get("sum_us", 0))
    if dn <= 0 or ds <= 0:
        return None
    return ds / dn


def main():
    ap = argparse.ArgumentParser(description="重複量測 esp_timer/render 漂移（ppm）")
    ap.add_argument("--port", required=True)
    ap.add_argument("--windows", type=int, default=6, help="重複幾輪（預設 6）")
    ap.add_argument("--window", type=float, default=30.0, help="每輪秒數（預設 30 s）")
    ap.add_argument("--precise", action="store_true",
                    help="用 sum_us/n 差分計 ppm（sub-ppm；唔 reset，適合兩 IC 對齊）")
    ap.add_argument("--csv")
    args = ap.parse_args()

    rows = []
    for i in range(args.windows):
        base = ask(args.port) if args.precise else None
        if args.precise and base is None:
            print("！ 基準讀取失敗")
            return 1
        if not args.precise:
            if ask(args.port, reset=True) is None:
                print("！ reset 失敗")
                return 1
        t0 = time.monotonic()
        time.sleep(args.window)
        d = ask(args.port)
        if d is None:
            print("！ 讀取失敗")
            return 1
        target = d.get("target_us", 0)
        if args.precise:
            avg = sum_avg(base, d)
            if avg is None:
                print("！ 樣本不足（sum_us/n 差分為 0）")
                return 1
            elapsed = time.monotonic() - t0
        else:
            avg = d.get("avg_us", 0)
            elapsed = args.window
        ppm = ((avg - target) / target * 1e6) if target else 0.0
        row = {
            "round": i + 1,
            "target_us": target,
            "avg_us": avg,
            "elapsed_s": elapsed,
            "ppm": ppm,
            "sd_us": d.get("sd_us", 0),
            "max_us": d.get("max_us", 0),
            "bad": d.get("bad", 0),
            "catchup": d.get("catchup", 0),
            "overrun": d.get("overrun", 0),
            "frames": d.get("n", 0),
        }
        rows.append(row)
        print(f"round {i+1:2d}: target {target} avg {avg:.4f} ppm {ppm:+.3f} "
              f"sd {row['sd_us']} max {row['max_us']} bad {row['bad']} "
              f"catchup {row['catchup']} overrun {row['overrun']} frames {row['frames']}")

    ppms = [r["ppm"] for r in rows]
    mean = statistics.mean(ppms)
    sd = statistics.pstdev(ppms) if len(ppms) > 1 else 0.0
    worst = max(abs(p) for p in ppms)
    print("\n—— 摘要 ——")
    print(f"rounds {len(rows)} × {args.window:g}s")
    print(f"ppm    mean {mean:+.2f}  sd {sd:.2f}  worst |{worst:.2f}|")

    # 相對漂移（兩塊同型號，保守用 2×worst）
    for label, rel in (("同向最壞（2×worst）", 2 * worst), ("差異（sd）", sd)):
        if rel <= 0:
            continue
        print(f"\n{label}：{rel:.2f} ppm")
        for tol in (1.0, 5.0, 20.0):
            sec = (tol / 1000.0) / (rel * 1e-6)
            human = f"{sec/3600:.1f} h" if sec >= 3600 else f"{sec/60:.1f} min"
            print(f"  tolerance {tol:g} ms → resync 每 {human}（{sec:.0f} s）")

    if args.csv:
        import csv
        with open(args.csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)
        print(f"\nCSV {args.csv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
