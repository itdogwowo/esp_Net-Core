#!/usr/bin/env python3
"""pixel_apply_exp_fig.py — apply 優化實驗圖（真機 9,600 px）。"""
import os
from PIL import Image, ImageDraw, ImageFont

F = "/System/Library/Fonts/STHeiti Medium.ttc"
OUT = os.path.join(os.path.dirname(__file__), "..", "docs", "assets", "pixel-pairing")
INK = (17, 24, 39); MUTED = (100, 116, 139); LINE = (203, 213, 225)
BG = (248, 250, 252); BLUE = (29, 78, 216); GREEN = (5, 150, 105)
AMBER = (180, 83, 9); RED = (185, 28, 28); GREY = (148, 163, 184)

T = ImageFont.truetype(F, 40); S = ImageFont.truetype(F, 20)
B = ImageFont.truetype(F, 20); V = ImageFont.truetype(F, 18)
N = ImageFont.truetype(F, 22)

rows = [
    ("基準：PSRAM flat（direct）", 4.99, GREY),
    ("＋uniform RGB 專用內圈", 5.01, GREY),
    ("＋pipe core0／8 KB", 5.42, AMBER),
    ("＋pipe core1／16 KB", 5.42, AMBER),
    ("＋值流 pool 放 internal", 5.39, AMBER),
]
bench = 1.60  # lab: 4,800 顆、唔同 workload

W, H = 1500, 760
img = Image.new("RGB", (W, H), BG); d = ImageDraw.Draw(img)
d.text((56, 24), "apply 優化實驗：三項都無增益", font=T, fill=INK)
d.text((56, 74), "真機 9600、mode full256（9,600 px、1 run、RGB、6 B 表 PSRAM）；"
                 "每項獨立量測。", font=S, fill=MUTED)

x0, x1 = 520, 1300
y = 150
for name, ms, col in rows:
    d.text((56, y + 8), name, font=B, fill=INK)
    w = int((x1 - x0) * ms / 6.0)
    d.rounded_rectangle((x0, y, x1, y + 34), radius=6, fill=(255, 255, 255), outline=LINE, width=2)
    d.rectangle((x0 + 1, y + 2, x0 + w, y + 32), fill=col)
    d.text((x0 + w + 12, y + 6), f"{ms:.2f} ms", font=N, fill=INK)
    y += 54

# bench 參考
y += 10
d.text((56, y + 8), "lab：pipe（bench 4,800 顆）", font=B, fill=GREEN)
w = int((x1 - x0) * bench / 6.0)
d.rounded_rectangle((x0, y, x1, y + 34), radius=6, fill=(255, 255, 255), outline=LINE, width=2)
d.rectangle((x0 + 1, y + 2, x0 + w, y + 32), fill=GREEN)
d.text((x0 + w + 12, y + 6), f"{bench:.2f} ms（唔可以直接類比）", font=V, fill=GREEN)

d.text((56, y + 90),
       "解讀：production apply ≈ 0.56 µs/px，成本係「flat 57.6 KB 讀 + 值流 57.6 KB 讀 + 環 38.4 KB 寫」合成；\n"
       "switch／chunk 預取／值流位置都唔係瓶頸。5.4 ms = 20 ms 預算的 27%，可接受。\n"
       "下一步建議：uniform RGB 用 4 B 表（dst16＋src16）→ 讀取 57.6 → 38.4 KB，先量再決定。",
       font=S, fill=INK)
os.makedirs(OUT, exist_ok=True)
img.save(os.path.join(OUT, "apply-experiments.png"))
print("wrote", os.path.join(OUT, "apply-experiments.png"))
