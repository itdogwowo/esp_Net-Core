#!/usr/bin/env python3
"""fps_trace.py — 追蹤 render 每幀間隔的穩定性（0xF081）。

用法：
  python3 tools/fps_trace.py --port /dev/cu.usbmodemXXXX --seconds 10
  python3 tools/fps_trace.py --port ... --reset --csv fps.csv --plot fps.png

輸出：終端摘要（target/avg/sd/min/max/bad、1 ms histogram）、可選 CSV／PNG。
"""
import argparse
import json
import struct
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import nc4

CMD_FPS = 0xF081
RSP_FPS = 0xF082


def query(port, reset=False, timeout=5.0):
    payload = b"\x01" if reset else b""
    s = serial_open(port)
    try:
        s.write(nc4.pack(CMD_FPS, payload, 0xFFFF))
        r = nc4.wait_for(s, RSP_FPS, timeout)
    finally:
        s.close()
    if r is None:
        return None
    n = struct.unpack_from("<H", r)[0]
    try:
        return json.loads(r[2:2 + n].decode("utf-8", "replace"))
    except Exception:
        return None


def serial_open(port):
    import serial
    return serial.Serial(port, 115200, timeout=0.3)


def pct(x, base):
    return (100.0 * x / base) if base else 0.0


def main():
    ap = argparse.ArgumentParser(description="render 每幀間隔穩定性（0xF081）")
    ap.add_argument("--port", required=True)
    ap.add_argument("--seconds", type=float, default=10.0, help="追蹤秒數（預設 10）")
    ap.add_argument("--interval", type=float, default=0.5, help="取樣間隔秒（預設 0.5）")
    ap.add_argument("--reset", action="store_true", help="開始前清零統計")
    ap.add_argument("--csv", help="把每次取樣寫成 CSV")
    ap.add_argument("--plot", help="輸出 PNG（histogram＋時間線）")
    ap.add_argument("--json", action="store_true", help="最後印完整 JSON")
    args = ap.parse_args()

    if args.reset:
        query(args.port, reset=True)

    samples = []
    t_end = time.time() + args.seconds
    while time.time() < t_end:
        d = query(args.port)
        if d is None:
            print("！ 取樣失敗（超時）", file=sys.stderr)
        else:
            d["_t"] = round(time.time(), 3)
            samples.append(d)
        time.sleep(args.interval)

    if not samples:
        print("沒有樣本（板冇回覆）")
        return 1

    last = samples[-1]
    target = last.get("target_us", 0)
    n = last.get("n", 0)
    avg = last.get("avg_us", 0)
    sd = last.get("sd_us", 0)
    mn = last.get("min_us", 0)
    mx = last.get("max_us", 0)
    bad = last.get("bad", 0)
    hist = last.get("hist", [])
    avg_fps = (1e6 / avg) if avg else 0.0
    target_fps = (1e6 / target) if target else 0.0

    print(f"target      {target} µs（{target_fps:.2f} fps）")
    print(f"frames      {n}")
    print(f"avg         {avg} µs（{avg_fps:.2f} fps）")
    print(f"sd          {sd} µs（{pct(sd, target):.2f}% of target）")
    print(f"min / max   {mn} / {mx} µs（max deviation {max(mx - target, target - mn)} µs）")
    print(f"bad         {bad}（|dt-target| > 20%）")
    print(f"starved     {last.get('starved', 0)}（環冇新格）；late {last.get('late', 0)}；"
          f"frames {last.get('frames', 0)}")

    if hist:
        top = sorted(range(len(hist)), key=lambda i: hist[i], reverse=True)[:5]
        print("hist top    " + "，".join(f"{i} ms×{hist[i]}" for i in top if hist[i]))

    verdict = "STABLE"
    if not target:
        verdict = "NO TARGET"
    elif pct(sd, target) > 2.0 or max(mx - target, target - mn) > target * 0.25:
        verdict = "UNSTABLE"
    elif pct(sd, target) > 1.0:
        verdict = "BORDERLINE"
    print(f"verdict     {verdict}")

    if args.json:
        print(json.dumps(last, ensure_ascii=False))

    if args.csv:
        import csv
        with open(args.csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["t", "n", "avg_us", "sd_us", "min_us", "max_us", "bad", "target_us"])
            for d in samples:
                w.writerow([d.get("_t"), d.get("n"), d.get("avg_us"), d.get("sd_us"),
                            d.get("min_us"), d.get("max_us"), d.get("bad"),
                            d.get("target_us")])
        print(f"CSV         {args.csv}")

    if args.plot:
        try:
            from PIL import Image, ImageDraw, ImageFont
        except Exception:
            print("（冇 PIL，略過 plot）")
            return 0
        f = "/System/Library/Fonts/STHeiti Medium.ttc"
        try:
            T = ImageFont.truetype(f, 28)
            S = ImageFont.truetype(f, 16)
        except Exception:
            T = ImageFont.load_default()
            S = T
        W, H = 1200, 640
        img = Image.new("RGB", (W, H), (248, 250, 252))
        d = ImageDraw.Draw(img)
        d.text((40, 20), f"FPS 穩定度：{verdict}", font=T, fill=(17, 24, 39))
        d.text((40, 60), f"target {target} µs／avg {avg} µs／sd {sd} µs／"
                         f"max {mx} µs／bad {bad}", font=S, fill=(100, 116, 139))
        # histogram
        x0, y0, w, h = 60, 110, 1080, 220
        d.rectangle((x0, y0, x0 + w, y0 + h), outline=(203, 213, 225), width=2)
        hmax = max(hist) if hist else 1
        bw = w / max(1, len(hist))
        for i, c in enumerate(hist):
            bh = int(h * c / hmax)
            d.rectangle((x0 + i * bw, y0 + h - bh, x0 + (i + 1) * bw - 1, y0 + h),
                        fill=(29, 78, 216))
        tgt = int(target / 1000)
        if 0 <= tgt < len(hist):
            d.line((x0 + tgt * bw, y0, x0 + tgt * bw, y0 + h), fill=(185, 28, 28), width=2)
        d.text((x0, y0 + h + 6), "每 1 ms 一格（紅線 = target）", font=S, fill=(100, 116, 139))
        # timeline (fps per sample)
        y1 = 430
        d.text((x0, y1), "fps（每次取樣）", font=S, fill=(100, 116, 139))
        vals = [(1e6 / d_["avg_us"]) if d_.get("avg_us") else 0 for d_ in samples]
        if vals:
            lo, hi = min(vals), max(vals)
            span = (hi - lo) or 1
            pts = []
            for i, v in enumerate(vals):
                x = x0 + (w * i / max(1, len(vals) - 1))
                y = y1 + 40 + int(120 * (1 - (v - lo) / span))
                pts.append((x, y))
            d.line(pts, fill=(5, 150, 105), width=2)
            d.text((x0, y1 + 170), f"{lo:.1f} – {hi:.1f} fps", font=S, fill=(17, 24, 39))
        img.save(args.plot)
        print(f"PNG         {args.plot}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
