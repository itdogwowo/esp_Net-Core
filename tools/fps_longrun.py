#!/usr/bin/env python3
"""fps_longrun.py — 小時級 FPS 穩定度／漂移長跑（0xF081）。

- 自動等板（--port 可給 /dev/cu.usbmodem* 或 auto）
- 固定間隔取樣，寫 CSV；板重啟／拔插會記錄 gap 並自動重連
- 完結（或 SIGTERM）印摘要：ppm 平均/標準差/最壞、overrun 總數、
  建議兩塊 ESP 的強制同步間隔

用法（背景）：
  python3 tools/fps_longrun.py --port auto --hours 4 --interval 60 \
      --csv /tmp/fps_longrun.csv --wait-port 7200
"""
import argparse
import csv
import glob
import json
import os
import signal
import statistics
import struct
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import nc4

CMD = 0xF081
RSP = 0xF082
_stop = False


def _sig(_s, _f):
    global _stop
    _stop = True


signal.signal(signal.SIGTERM, _sig)
signal.signal(signal.SIGINT, _sig)


def find_port(spec):
    if spec and spec != "auto" and os.path.exists(spec):
        return spec
    for p in sorted(glob.glob("/dev/cu.usbmodem*")):
        return p
    return None


def ask(port, reset=False, timeout=4.0):
    import serial
    try:
        s = serial.Serial(port, 115200, timeout=0.3)
    except Exception:
        return None
    try:
        s.write(nc4.pack(CMD, b"\x01" if reset else b"", 0xFFFF))
        r = nc4.wait_for(s, RSP, timeout)
    finally:
        s.close()
    if r is None or len(r) < 2:
        return None
    n = struct.unpack_from("<H", r)[0]
    try:
        return json.loads(r[2:2 + n].decode("utf-8", "replace"))
    except Exception:
        return None


def summary(rows, hours):
    if not rows:
        print("沒有樣本")
        return
    ppms = [r["ppm"] for r in rows if r.get("target_us")]
    overs = sum(r.get("overrun", 0) for r in rows)
    bad = sum(r.get("bad", 0) for r in rows)
    gaps = sum(1 for r in rows if r.get("gap"))
    frames = rows[-1].get("frames", 0)
    print("\n—— 長跑摘要 ——")
    print(f"duration {hours:.2f} h，樣本 {len(rows)}，gap {gaps}，frames {frames}")
    if ppms:
        print(f"ppm mean {statistics.mean(ppms):+.2f}  sd "
              f"{statistics.pstdev(ppms) if len(ppms) > 1 else 0:.2f}  "
              f"worst |{max(abs(p) for p in ppms):.2f}|")
    print(f"bad {bad}，overrun {overs}")
    if ppms and max(abs(p) for p in ppms) > 0:
        rel = 2 * max(abs(p) for p in ppms)
        print(f"（保守）兩塊 ESP 相對漂移 {rel:.2f} ppm")
        for tol in (1.0, 5.0, 20.0):
            sec = (tol / 1000.0) / (rel * 1e-6)
            print(f"  tolerance {tol:g} ms → resync 每 {sec:.0f} s")


def main():
    ap = argparse.ArgumentParser(description="小時級 FPS 穩定度／漂移長跑（0xF081）")
    ap.add_argument("--port", default="auto", help="/dev/cu.usbmodemXXXX 或 auto")
    ap.add_argument("--hours", type=float, default=4.0)
    ap.add_argument("--interval", type=float, default=60.0, help="取樣秒數")
    ap.add_argument("--wait-port", type=float, default=0.0, help="等板的總秒數（0=即刻失敗）")
    ap.add_argument("--csv", default="/tmp/fps_longrun.csv")
    args = ap.parse_args()

    t_wait = time.time() + args.wait_port
    port = find_port(args.port)
    while port is None and time.time() < t_wait and not _stop:
        print(f"[{time.strftime('%H:%M:%S')}] 等板中…（{int(t_wait - time.time())} s）")
        time.sleep(10)
        port = find_port(args.port)
    if port is None:
        print("沒有可用 port（板未連上）")
        return 2
    print(f"用 port {port}；清零後開始 {args.hours:g} h（每 {args.interval:g} s 取樣）")
    if ask(port, reset=True) is None:
        print("清零失敗（板冇回覆）")
        return 2

    t0 = time.time()
    t_end = t0 + args.hours * 3600
    rows = []
    with open(args.csv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t", "elapsed_s", "target_us", "avg_us", "ppm", "sd_us", "max_us",
                    "bad", "catchup", "overrun", "slack_us", "frames", "gap"])
        last_frames = 0
        while time.time() < t_end and not _stop:
            d = ask(port)
            gap = 0
            if d is None:
                # 板重啟／拔插：重連，記 gap
                gap = 1
                port2 = None
                for _ in range(30):
                    time.sleep(2)
                    port2 = find_port(args.port)
                    if port2 is not None and ask(port2) is not None:
                        port = port2
                        break
                if port2 is None:
                    time.sleep(args.interval)
                    continue
            else:
                target = d.get("target_us", 0)
                avg = d.get("avg_us", 0)
                ppm = ((avg - target) / target * 1e6) if target else 0.0
                frames = d.get("n", 0)
                if frames < last_frames:
                    gap = 1          # 計數倒退＝重啟
                last_frames = frames
                row = {
                    "t": round(time.time(), 1),
                    "elapsed_s": round(time.time() - t0, 1),
                    "target_us": target, "avg_us": avg, "ppm": ppm,
                    "sd_us": d.get("sd_us", 0), "max_us": d.get("max_us", 0),
                    "bad": d.get("bad", 0), "catchup": d.get("catchup", 0),
                    "overrun": d.get("overrun", 0),
                    "slack_us": d.get("slack_us", 0), "frames": frames, "gap": gap,
                }
                rows.append(row)
                w.writerow([row[k] for k in row])
                f.flush()
                if len(rows) % 10 == 1:
                    print(f"[{time.strftime('%H:%M:%S')}] +{row['elapsed_s']/60:.1f} min "
                          f"ppm {ppm:+.2f} sd {row['sd_us']} max {row['max_us']} "
                          f"bad {row['bad']} overrun {row['overrun']} slack {row['slack_us']}")
            time.sleep(args.interval)

    summary(rows, (time.time() - t0) / 3600)
    print(f"CSV {args.csv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
