#!/usr/bin/env python3
"""show_verify.py — 讀 Show firmware 的 live frame（0xF06B），驗 white floor／亮度上限。

用法：
    python3 tools/show_verify.py --via http://127.0.0.1:8011
    python3 tools/show_verify.py --via http://127.0.0.1:8011 --white 0 --max-led 40
    python3 tools/show_verify.py --via http://127.0.0.1:8011 --white 80 --json

`--white` 省略 = 用 frame 自己回報的 white；`--max-led` 可驗「唔太光」。
"""
from __future__ import annotations

import argparse
import json
import urllib.error
import urllib.request


def api(base: str, path: str, body=None, actor="ai", timeout=10):
    req = urllib.request.Request(
        base.rstrip("/") + path,
        data=None if body is None else json.dumps(body).encode(),
        headers={"Content-Type": "application/json", "X-Actor": actor},
        method="POST" if body is not None else "GET",
    )
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        raise SystemExit(f"HTTP {e.code} {path}: {e.read().decode(errors='replace')}")
    except urllib.error.URLError as e:
        raise SystemExit(f"連不上 {base}: {e.reason}")


def frame(base: str) -> dict:
    body = api(base, "/api/cmd", {
        "cmd": "0xF06B", "payload_hex": "", "expect": ["0xF06C"], "timeout": 8,
    })
    j = body.get("json")
    if not isinstance(j, dict):
        raise SystemExit(f"0xF06B 回覆不是 JSON：{body}")
    return j


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--via", default="http://127.0.0.1:8011")
    ap.add_argument("--white", type=int, default=None,
                    help="預期白混合百分比；省略 = 用 frame 回報的 white")
    ap.add_argument("--max-led", type=int, default=None,
                    help="可選：所有 R/G/B/W 必須 <= 這個值（防太光）")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    j = frame(args.via)
    if j.get("err"):
        raise SystemExit(f"裝置未出 frame：{j}")
    px = int(j.get("px", 0))
    hexs = j.get("hex", "")
    data = bytes.fromhex(hexs)
    if px <= 0 or len(data) != px * 4:
        raise SystemExit(f"frame 長度錯：px={px} bytes={len(data)} hex={len(hexs)}")

    expected_white = (args.white if args.white is not None
                      else int(j.get("white", 0) or 0))
    floor = round(255 * expected_white / 100)
    rows = []
    mins = [255, 255, 255, 255]
    maxs = [0, 0, 0, 0]
    for i in range(px):
        r, g, b, w = data[i * 4:i * 4 + 4]
        rows.append({"i": i, "r": r, "g": g, "b": b, "w": w})
        for c, v in enumerate((r, g, b, w)):
            mins[c] = min(mins[c], v)
            maxs[c] = max(maxs[c], v)
    out = {
        "ok": True, "px": px, "seq": j.get("seq"), "white": expected_white,
        "floor": floor, "maxLed": args.max_led, "min": mins, "max": maxs, "sample": rows[:8],
    }
    if args.json:
        print(json.dumps(out, ensure_ascii=False))
    else:
        print(f"0xF06B OK：px={px} seq={j.get('seq')} mode={j.get('mode')} "
              f"white={expected_white}% floor={floor} "
              f"apply_us={j.get('apply_us')}")
        print(f"  min RGBA={mins}  max RGBA={maxs}")
        for row in rows[:8]:
            print(f"  px{row['i']:02d}  R={row['r']:3d} G={row['g']:3d} "
                  f"B={row['b']:3d} W={row['w']:3d}")

    # white floor：R/G/B 至少 floor（效果只會加 0..(255-floor)）；W 應約 floor。
    bad = [r for r in rows if r["w"] < floor - 1 or min(r["r"], r["g"], r["b"]) < floor - 1]
    if bad:
        print(f"  ✘ {len(bad)} 顆低於 {floor}：{bad[:5]}")
        return 1
    if args.max_led is not None and max(maxs) > args.max_led:
        print(f"  ✘ 最亮 {max(maxs)} 超過上限 {args.max_led}")
        return 1
    print(f"  ✔ 全部像素符合 white {expected_white}% floor"
          + (f"，最亮 {max(maxs)} ≤ {args.max_led}" if args.max_led is not None else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
