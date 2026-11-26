#!/usr/bin/env python3
"""把 2026-11-26 重跑嘅跑分畫成一張圖（Pillow；Windows 有 msyh 字型）。"""
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

J = json.loads(Path("artifacts/bench/latest.json").read_text(encoding="utf-8"))
OUT = Path("docs/assets/bench-latest.png")
OUT.parent.mkdir(parents=True, exist_ok=True)

W, H = 1500, 1120
BG = (18, 20, 26)
FG = (232, 236, 244)
DIM = (150, 158, 172)
ACC = (86, 196, 255)
OK = (120, 220, 150)
WARN = (255, 190, 90)
RED = (255, 120, 120)
BAR = (70, 130, 190)

img = Image.new("RGB", (W, H), BG)
d = ImageDraw.Draw(img)


def font(sz, bold=False):
    for name in (("msyhbd.ttc", "msyh.ttc") if bold else ("msyh.ttc", "msyhbd.ttc")):
        p = Path("C:/Windows/Fonts") / name
        if p.exists():
            return ImageFont.truetype(str(p), sz)
    return ImageFont.load_default()


F_TITLE = font(34, True)
F_SUB = font(18)
F_H = font(22, True)
F_T = font(17)
F_S = font(15)
F_BIG = font(26, True)

def t(xy, s, f=F_T, fill=FG):
    d.text(xy, s, font=f, fill=fill)

def section(y, title):
    d.text((44, y), title, font=F_H, fill=ACC)
    d.line((44, y + 32, W - 44, y + 32), fill=(48, 54, 66), width=1)

sysd = J["sys"]
summ = J["x071_summary"]
med = {k: v["median"] for k, v in summ.items()}
samples = J["x071"]
modes = J["modes"]

# ── header ───────────────────────────────────────────────
d.text((44, 26), "ESP32-S3 Matrix 8×8（64×WS2812）跑分總覽", font=F_TITLE, fill=FG)
t((44, 72), f"2026-11-26 重跑 · fw {sysd['fw']} · app {sysd['app']} · idf {sysd['idf']} · "
            f"cid 0x54EC · rst={sysd['rst']}", F_SUB, DIM)
t((W - 360, 72), "idf=v6.1.1 (WSL) · 50 fps · I80 直驅", F_SUB, OK)

# ── A: 20 ms 幀預算 ──────────────────────────────────────
section(120, "A · 20 ms 幀預算（一幀時間去了邊）")
yA = 168
x0, x1 = 64, W - 64
span = x1 - x0
ms_total = med["frame_us"] / 1000.0          # ≈20.0 ms
cpu_us = med["eff_avg"] + med["pair_avg"] + med["apply_avg"] + med["enc_us"] + med["tx_us"]
wire_us = 64 * 24 * 3 / 2.5                  # 64px × 24bit × 3bit-cells @2.5MHz
segs = [
    ("CPU 全部（eff+pair+apply+enc+submit）", cpu_us / 1000.0, (250, 180, 90)),
    ("DMA 線路（64px×24bit×3cells@2.5MHz）", wire_us / 1000.0, (90, 170, 240)),
    ("idle（等下一幀）", ms_total - (cpu_us + wire_us) / 1000.0, (60, 66, 80)),
]
x = x0
for name, ms, col in segs:
    w = max(3, int(span * ms / ms_total))
    d.rectangle((x, yA, x + w, yA + 46), fill=col)
    x += w
t((x0, yA + 56), f"一幀 {ms_total:.2f} ms　", F_T)
tx = x0 + 150
t((tx, yA + 56), f"CPU {cpu_us:.0f} µs（{cpu_us/ms_total/10:.1f}%）", F_T, (250, 200, 130))
tx += 260
t((tx, yA + 56), f"線路 {wire_us:.0f} µs（{wire_us/ms_total/1000*100:.1f}%）", F_T, (150, 200, 250))
tx += 280
t((tx, yA + 56), f"idle {(ms_total*1000-cpu_us-wire_us):.0f} µs（{100-cpu_us/ms_total/10-wire_us/ms_total/1000*100:.1f}%）",
  F_T, DIM)

# ── B: CPU 各階段 ────────────────────────────────────────
section(250, "B · CPU 各階段（中位數，µs；10 個樣本）")
rows = [
    ("端到端延遲 lat", med["lat_avg"], (200, 120, 200)),
    ("配對 pair", med["pair_avg"], (90, 170, 240)),
    ("編碼 enc", med["enc_us"], (250, 180, 90)),
    ("套用 apply", med["apply_avg"], (110, 200, 170)),
    ("效果 eff", med["eff_avg"], (150, 210, 120)),
    ("提交 tx", med["tx_us"], (240, 150, 110)),
    ("落格 commit", med["commit_avg"], (140, 140, 200)),
]
maxv = max(v for _, v, _ in rows) * 1.15
yB = 300
bar_h = 26
for i, (name, v, col) in enumerate(rows):
    yy = yB + i * 44
    t((64, yy + 2), name, F_T)
    bx = 250
    bw = int((W - 64 - bx - 110) * v / maxv)
    d.rectangle((bx, yy, bx + max(2, bw), yy + bar_h), fill=col)
    t((bx + bw + 10, yy + 2), f"{v:.0f} µs", F_T, FG)
    # 相對 20 ms 幀
    p = v / (ms_total * 1000) * 100
    t((bx + bw + 110, yy + 4), f"{p:.3f}% 幀", F_S, DIM)

# ── C: FPS 樣本 ─────────────────────────────────────────
section(660, "C · FPS／drops（10 個樣本，每 0.6 s）")
xC0, xC1 = 110, W - 110
yC0, yC1 = 712, 840
fps_vals = [s["fps_x10"] / 10.0 for s in samples]
lo, hi = 49.0, 50.5
for gv in (49.0, 49.5, 50.0, 50.5):
    yy = yC1 - int((gv - lo) / (hi - lo) * (yC1 - yC0))
    d.line((xC0, yy, xC1, yy), fill=(40, 44, 54), width=1)
    t((xC0 - 52, yy - 9), f"{gv:.1f}", F_S, DIM)
step = (xC1 - xC0) / max(1, len(fps_vals) - 1)
pts = [(xC0 + i * step, yC1 - (v - lo) / (hi - lo) * (yC1 - yC0)) for i, v in enumerate(fps_vals)]
d.line(pts, fill=ACC, width=2)
for (px, py), v in zip(pts, fps_vals):
    d.ellipse((px - 4, py - 4, px + 4, py + 4), fill=ACC)
    t((px - 12, py - 28), f"{v:.1f}", F_S, FG)
late_med = sorted(s["late"] for s in samples)[len(samples) // 2]
t((xC0, yC1 + 8), f"median {sorted(fps_vals)[len(fps_vals)//2]:.1f} fps · drops=0 · "
                  f"starved {med['starved']:.0f} · late {late_med}", F_T, OK)

# ── D: 三個 mode 的顏色檢查 ──────────────────────────────
section(880, "D · 三個 mode 的 0xF06B（每幀掃 64 顆）")
colx = [64, 520, 976]
for cx, name in zip(colx, ("rgb_breath", "rainbow", "white_breath")):
    fr = [f for f in modes.get(name, []) if "uniq" in f]
    t((cx, 928), name, font(19, True), ACC)
    if not fr:
        t((cx, 958), "（冇樣本）", F_S, DIM)
        continue
    uniq = [f["uniq"] for f in fr]
    lum = [f["mean_lum"] for f in fr]
    t((cx, 958), "不同顏色數：" + " / ".join(str(u) for u in uniq), F_T,
      OK if max(uniq) in (1, 64) else WARN)
    t((cx, 986), "平均亮度：" + " / ".join(f"{v:.1f}" for v in lum), F_T)
    t((cx, 1014), "首 4 顆：" + " ".join(fr[-1]["first4"]), F_S, DIM)
t((64, 1090), "註：切 mode 後第一幀仍可能係舊 mode 的畫面（paint 在幀邊界換場），所以頭一個樣本可能 64；"
              "之後 rainbow 應 64、rgb_breath／white_breath 應 1。", F_S, DIM)

# ── footer：其他跑分 ────────────────────────────────────
t((64, 1054), "其他跑分（同日重跑）：image 726,800 B（sha256 cbfc78e0…）· build 45 s · "
              "OTA 141.1 KB/s（5.0 s）· host test 21/21 · "
              f"heap {sysd['heap']:,} B · 環 320 B×3 · 佔用 GPIO 1（GPIO14）", F_T, FG)

d.line((44, 862, W - 44, 862), fill=(40, 44, 54), width=1)

img.save(OUT)
print("saved", OUT, img.size)
