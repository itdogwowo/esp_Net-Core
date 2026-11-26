#!/usr/bin/env python3
"""2026-11-26 跑分重跑：經 WebMaster REST 讀 Matrix 板上所有現行指標，寫入 JSON。

用法：python artifacts/_tmp/bench_rerun.py
輸出：artifacts/bench/latest.json
"""
import json
import statistics
import time
import urllib.request

BASE = "http://127.0.0.1:8011"
OUT = "artifacts/bench/latest.json"


def api(path, body=None, method=None):
    data = None
    headers = {"X-Actor": "ai"}
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(BASE + path, data=data, headers=headers,
                                 method=method or ("POST" if body is not None else "GET"))
    with urllib.request.urlopen(req, timeout=25) as r:
        return json.loads(r.read().decode())


def cmd(code, expect, payload=b""):
    return api("/api/cmd", {"cmd": code, "payload_hex": payload.hex(),
                            "expect": [expect], "timeout": 6})["json"]


def pct(vals, q):
    if not vals:
        return 0
    s = sorted(vals)
    i = min(len(s) - 1, max(0, round((len(s) - 1) * q)))
    return s[i]


def main() -> int:
    res = {"when": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
           "note": "經 WebMaster REST 由 COM24 讀；idf=v6.1.1 (WSL) 建置"}

    sysd = api("/api/status?keys=sys")["data"]["sys"]
    res["sys"] = sysd
    tasks = api("/api/status?keys=tasks")["data"]["tasks"]
    res["tasks"] = [{"name": t["name"], "level": t["level"], "started": t["started"],
                     "loops": [{"n": l["name"], "run": l["running"]} for l in t["loops"]]}
                    for t in tasks]
    mem = api("/api/status?keys=mem")["data"]["mem"]
    res["buf"] = mem.get("buf")

    # 0xF071 x10 樣本
    samples = []
    for _ in range(10):
        j = cmd("0xF071", "0xF072")
        r = j["render"]
        samples.append({
            "fps_x10": r["fps_x10"], "frames": r["frames"], "fresh": r["fresh"],
            "starved": r["starved"], "late": r["late"],
            "frame_us": r["frame_us"], "frame_max_us": r["frame_max_us"],
            "enc_us": r["enc_us"], "tx_us": r["tx_us"],
            "eff_avg": j["eff"]["avg_us"], "pair_avg": j["pair"]["avg_us"],
            "apply_avg": j["apply"]["avg_us"], "commit_avg": j["commit"]["avg_us"],
            "lat_avg": j["lat"]["avg_us"], "lat_max": j["lat"]["max_us"],
            "drops": j["drops"], "puts": j["puts"], "fill": j["fill"],
        })
        time.sleep(0.6)
    res["x071"] = samples
    keys = ["fps_x10", "frame_us", "frame_max_us", "enc_us", "tx_us", "eff_avg",
            "pair_avg", "apply_avg", "commit_avg", "lat_avg", "lat_max",
            "starved", "drops", "fill"]
    res["x071_summary"] = {}
    for k in keys:
        vals = [s[k] for s in samples]
        res["x071_summary"][k] = {
            "min": min(vals), "max": max(vals),
            "median": statistics.median(vals),
            "mean": round(statistics.mean(vals), 2),
        }

    # 三種 mode 的 0xF06B
    modes = {}
    for mid, name in ((1, "rgb_breath"), (2, "rainbow"), (3, "white_breath")):
        cmd("0xF06D", "0xF06E", bytes([mid]))
        time.sleep(1.2)
        frames = []
        for _ in range(3):
            j = cmd("0xF06B", "0xF06C")
            if "hex" not in j:
                frames.append({"err": j})
                time.sleep(0.5)
                continue
            hx = j["hex"]
            px = [hx[k * 8:(k + 1) * 8] for k in range(len(hx) // 8)]
            r = [int(p[0:2], 16) for p in px]
            g = [int(p[2:4], 16) for p in px]
            b = [int(p[4:6], 16) for p in px]
            frames.append({
                "mode": j["mode"], "seq": j["seq"], "px": j["px"],
                "uniq": len(set(px)),
                "r_min": min(r), "r_max": max(r),
                "g_min": min(g), "g_max": max(g),
                "b_min": min(b), "b_max": max(b),
                "w_max": max(int(p[6:8], 16) for p in px),
                "first4": px[:4], "mean_lum": round(sum(r + g + b) / (3 * len(px)), 2),
            })
            time.sleep(0.5)
        modes[name] = frames
    res["modes"] = modes

    with open(OUT, "w", encoding="utf-8") as f:
        json.dump(res, f, ensure_ascii=False, indent=2)
    print(f"saved {OUT}")
    print("sys:", {k: res["sys"][k] for k in ("fw", "app", "idf", "rst", "heap", "h_int", "h_dma", "h_ps")})
    print("x071 median:", {k: res["x071_summary"][k]["median"] for k in
                            ("fps_x10", "frame_us", "enc_us", "tx_us", "apply_avg",
                             "pair_avg", "lat_avg", "drops")})
    for name, fr in modes.items():
        ok = [f for f in fr if "uniq" in f]
        if ok:
            print(f"{name}: uniq={[f['uniq'] for f in ok]} lum={[f['mean_lum'] for f in ok]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
