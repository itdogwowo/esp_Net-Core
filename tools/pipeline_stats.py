#!/usr/bin/env python3
"""pipeline_stats.py — 讀三任務（effect→pair→render）分段計時。

    python3 tools/pipeline_stats.py --via http://127.0.0.1:8011
    python3 tools/pipeline_stats.py --via ... --reset
    python3 tools/pipeline_stats.py --via ... --burn 3
    python3 tools/pipeline_stats.py --via ... --burn 3 --json

韌體：0xF071 PIPESTATS（0xF072）、0xF073 PIPEBURN（0xF074）、
0xF075 PIPERESET（0xF076）。
"""
from __future__ import annotations

import argparse
import json
import time
import urllib.error
import urllib.request


def call(base: str, cmd: str, payload: bytes = b"") -> dict:
    body = json.dumps({
        "cmd": cmd, "payload_hex": payload.hex(),
        "expect": {0xF071: ["0xF072"], 0xF073: ["0xF074"],
                   0xF075: ["0xF076"]}.get(int(cmd, 16), []),
        "timeout": 10,
    }).encode()
    req = urllib.request.Request(base.rstrip("/") + "/api/cmd", data=body,
                                 headers={"Content-Type": "application/json",
                                          "X-Actor": "ai"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return json.loads(r.read().decode())
    except urllib.error.HTTPError as e:
        raise SystemExit(f"HTTP {e.code}: {e.read().decode(errors='replace')}")
    except urllib.error.URLError as e:
        raise SystemExit(f"連不上 {base}: {e.reason}")


def stats(base: str) -> dict:
    reply = call(base, "0xF071")
    data = reply.get("json")
    if not isinstance(data, dict):
        raise SystemExit(f"0xF071 回覆不是 JSON：{reply}")
    return data


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--via", default="http://127.0.0.1:8011")
    ap.add_argument("--reset", action="store_true")
    ap.add_argument("--burn", type=int, default=0,
                    help="先跑 N 秒 effect 極限 burn（0=不跑）")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    if args.reset:
        print(call(args.via, "0xF075").get("json"))
    if args.burn > 0:
        print(f"burn {args.burn}s …", call(args.via, "0xF073", bytes([args.burn])).get("json"))
        time.sleep(args.burn + 1)
        # 停 burn（已到期；明確送 0）
        call(args.via, "0xF073", b"\x00")
    d = stats(args.via)
    if args.json:
        print(json.dumps(d, ensure_ascii=False, indent=1))
        return 0
    e, p, r = d.get("eff", {}), d.get("pair", {}), d.get("render", {})
    a, w, c, l = d.get("apply", {}), d.get("white", {}), d.get("commit", {}), d.get("lat", {})
    print(f"effect : n={e.get('n')}  avg={e.get('avg_us')}us  max={e.get('max_us')}us  last={e.get('last_us')}us")
    print(f"pair   : n={p.get('n')}  avg={p.get('avg_us')}us  max={p.get('max_us')}us")
    print(f"  apply={a.get('avg_us')}/{a.get('max_us')}us  "
          f"white={w.get('avg_us')}/{w.get('max_us')}us  "
          f"commit={c.get('avg_us')}/{c.get('max_us')}us  "
          f"lat={l.get('avg_us')}/{l.get('max_us')}us")
    print(f"render : frames={r.get('frames')}  fps={r.get('fps_x10', 0)/10:.1f}  "
          f"frame={r.get('frame_us')}us  enc={r.get('enc_us')}us  tx={r.get('tx_us')}us")
    print(f"full   : {d.get('full')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
