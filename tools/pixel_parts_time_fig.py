#!/usr/bin/env python3
"""pixel_parts_time_fig.py — 目前 pipeline 零件同時間（真機量測）。"""
import os
from PIL import Image, ImageDraw, ImageFont

F = "/System/Library/Fonts/STHeiti Medium.ttc"
OUT = os.path.join(os.path.dirname(__file__), "..", "docs", "assets", "pixel-pairing")
INK = (17, 24, 39); MUTED = (100, 116, 139); LINE = (203, 213, 225)
BG = (248, 250, 252); BLUE = (29, 78, 216); GREEN = (5, 150, 105)
AMBER = (180, 83, 9); RED = (185, 28, 28); PURPLE = (109, 40, 217); GREY = (100, 116, 139)

T = ImageFont.truetype(F, 38); S = ImageFont.truetype(F, 19); B = ImageFont.truetype(F, 20)
M = ImageFont.truetype(F, 17); N = ImageFont.truetype(F, 22)

W, H = 1800, 1180
img = Image.new("RGB", (W, H), BG); d = ImageDraw.Draw(img)
d.text((56, 24), "目前 pipeline：零件同時間（真機，9,600 px）", font=T, fill=INK)
d.text((56, 72), "50 fps ＝ 20 ms／視覺幀；16×600 的線路 18 ms 係硬下限，CPU 要喺陰影內完成。",
       font=S, fill=MUTED)

# ── A. 每幀預算（16×600、50 fps）──
y = 130
d.text((56, y), "A. 每幀 20 ms 預算（16×600、50 fps）", font=B, fill=INK)
x0, x1 = 380, 1560
y += 40
d.text((56, y + 6), "CPU 工作", font=M, fill=INK)
segs = [
    (0.3, GREEN, "effect 0.3"),
    (5.4, BLUE, "pair apply 5.4"),
    (0.1, GREY, "white 0.1"),
    (0.5, AMBER, "commit 0.5"),
]
x = x0
for ms, col, name in segs:
    w = int((x1 - x0) * ms / 20.0)
    d.rectangle((x, y, x + w, y + 36), fill=col)
    if w > 90:
        d.text((x + 6, y + 8), name, font=M, fill=(255, 255, 255))
    x += w
d.rectangle((x0, y, x1, y + 36), outline=LINE, width=2)
d.text((x1 + 12, y + 6), "≈6.3 ms（31%）", font=N, fill=INK)

y += 58
d.text((56, y + 6), "剩餘預算", font=M, fill=MUTED)
d.rectangle((x, y, x1, y + 36), fill=(226, 232, 240))
d.text((x + 10, y + 8), "≈13.7 ms 緩衝", font=M, fill=MUTED)

# ── B. 16×600 線路 vs encode ──
y += 90
d.text((56, y), "B. 16×600：線路 18 ms vs encode 6.4 ms（Task 6 後）", font=B, fill=INK)
y += 40
d.text((56, y + 6), "WS2812 線路", font=M, fill=INK)
w = int((x1 - x0) * 18.0 / 20.0)
d.rectangle((x0, y, x0 + w, y + 34), fill=RED)
d.text((x0 + 8, y + 6), "18.0 ms（硬下限）", font=M, fill=(255, 255, 255))
y += 50
d.text((56, y + 6), "I80 encode", font=M, fill=INK)
w = int((x1 - x0) * 6.4 / 20.0)
d.rectangle((x0, y, x0 + w, y + 34), fill=BLUE)
d.text((x0 + 8, y + 6), "6.4 ms（舊 24.0，3.77×）", font=M, fill=(255, 255, 255))
d.text((x1 + 12, y + 4), "陰影內：OK", font=N, fill=GREEN)

# ── C. 一次性成本 ──
y += 90
d.text((56, y), "C. 一次性成本（切 mode／開機）", font=B, fill=INK)
y += 40
rows = [
    ("layout 快取讀取", 10, "ms", GREY, "500 組"),
    ("layout 重建（500 組）", 40, "ms", AMBER, ""),
    ("layout 重建（隨機下限）", 152, "ms", RED, ""),
    ("flatify（真實 map）", 0.55, "ms", GREEN, ""),
    ("flatify（隨機下限）", 14.5, "ms", RED, ""),
    ("playlist 預建下一張", 12, "ms", BLUE, "1 run"),
    ("切換 swap", 0.246, "ms", GREEN, "246 µs"),
]
maxv = 160.0
for name, ms, unit, col, note in rows:
    d.text((56, y + 6), name, font=M, fill=INK)
    w = max(3, int((x1 - x0) * ms / maxv))
    d.rectangle((x0, y, x0 + w, y + 26), fill=col)
    label = f"{ms:g} {unit}" + (f"（{note}）" if note else "")
    d.text((x0 + w + 10, y + 3), label, font=M, fill=INK)
    y += 36

# ── D. 記憶體 ──
y += 20
d.text((56, y), "D. 記憶體（9,600 px）", font=B, fill=INK)
y += 36
d.text((56, y + 4), "PSRAM：環 115.2 KB ／ flat 57.6 KB ×2（current＋next）／值流 19.2 KB ×2 ／"
                    "layout 12.5 KB ×2；internal：flat4（未用）／chunk 24 KB（預設關）／堆疊。",
       font=M, fill=INK)

d.text((56, H - 40), "圖：tools/pixel_parts_time_fig.py；數字來源 docs/00_pm/status.md §7.220–7.232。",
       font=M, fill=MUTED)
os.makedirs(OUT, exist_ok=True)
img.save(os.path.join(OUT, "parts-time.png"))
print("wrote", os.path.join(OUT, "parts-time.png"))
