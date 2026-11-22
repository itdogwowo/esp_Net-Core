#!/usr/bin/env python3
"""pixel_amortize_fig.py — 一次建立、長時間播放：把建立成本攤到每格。

輸出：docs/assets/pixel-pairing/build-once.png
"""
import os

from PIL import Image, ImageDraw, ImageFont

FONT = "/System/Library/Fonts/STHeiti Medium.ttc"
OUT = os.path.join(os.path.dirname(__file__), "..", "docs", "assets", "pixel-pairing")

INK = (31, 41, 51)
MUTED = (82, 96, 109)
LINE = (203, 213, 225)
BG = (248, 250, 252)
RED = (220, 38, 38)
GREEN = (16, 185, 129)
GREEN_BG = (236, 253, 245)
AMBER = (217, 119, 6)
BLUE = (37, 99, 235)
WHITE = (255, 255, 255)

F_TITLE = ImageFont.truetype(FONT, 44)
F_SUB = ImageFont.truetype(FONT, 24)
F_SEC = ImageFont.truetype(FONT, 30)
F_BODY = ImageFont.truetype(FONT, 24)
F_SMALL = ImageFont.truetype(FONT, 20)
F_NUM = ImageFont.truetype(FONT, 28)


def text(d, xy, s, f, fill=INK, anchor="la"):
    d.text(xy, s, font=f, fill=fill, anchor=anchor)


def rounded(d, box, r, fill, outline=None, width=2):
    d.rounded_rectangle(box, radius=r, fill=fill, outline=outline, width=width)


def fig():
    W, H = 1800, 980
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    text(d, (60, 34), "同一個「隨機組合」播 5–10 分鐘：建立只做一次", F_TITLE)
    text(d, (60, 92), "「換場一次」＝播放清單換到另一個組合（或切 mode／重新載入）時做一次；"
                      "同一組合期間每格唔會再建立。", F_SUB, MUTED)

    # 時間軸
    y = 160
    x0, x1 = 90, 1710
    d.line((x0, y + 80, x1, y + 80), fill=LINE, width=4)
    # 建立（紅色小塊）
    d.rectangle((x0, y + 40, x0 + 34, y + 120), fill=RED)
    text(d, (x0 + 44, y + 34), "建立組合：137 ms（只此一次）", F_BODY, RED)
    # 播放（綠色長條）
    d.rectangle((x0 + 160, y + 52, x1 - 20, y + 108), fill=GREEN_BG, outline=GREEN, width=3)
    text(d, (x0 + 180, y + 64), "每格：清 2.0 ＋ 寫 12.3 ＋ 交 4.5 ＝ 18.8 ms（完全隨機、8 位）",
         F_BODY, GREEN)
    text(d, (x0 + 160, y + 130), "5 分鐘 = 15,000 格", F_SMALL, MUTED)
    text(d, (x1 - 260, y + 130), "10 分鐘 = 30,000 格", F_SMALL, MUTED)

    # 攤平表
    y2 = 380
    text(d, (60, y2), "把 137 ms 攤到每格（完全隨機，最壞情況）", F_SEC)
    rows = [
        ("5 分鐘（15,000 格）", "137 ms ÷ 15,000 = 0.009 ms／格", "18.8 + 0.009 ≈ 18.8 ms", "94%"),
        ("10 分鐘（30,000 格）", "137 ms ÷ 30,000 = 0.005 ms／格", "18.8 + 0.005 ≈ 18.8 ms", "94%"),
        ("500 組（真實組合）", "40 ms ÷ 15,000 = 0.003 ms／格", "10.2 + 0.003 ≈ 10.2 ms", "51%"),
    ]
    yy = y2 + 54
    text(d, (90, yy), "情況", F_BODY, MUTED)
    text(d, (620, yy), "建立攤到每格", F_BODY, MUTED)
    text(d, (1080, yy), "每格總計（20 ms 預算）", F_BODY, MUTED)
    text(d, (1600, yy), "佔用", F_BODY, MUTED)
    yy += 40
    d.line((80, yy, 1720, yy), fill=LINE, width=2)
    yy += 16
    for (a, b, c, pct) in rows:
        text(d, (90, yy), a, F_NUM, INK)
        text(d, (620, yy), b, F_BODY, MUTED)
        text(d, (1080, yy), c, F_BODY, GREEN if pct == "51%" else AMBER)
        text(d, (1600, yy), pct, F_NUM, GREEN if pct == "51%" else AMBER)
        yy += 52

    # 備註
    yy += 10
    rounded(d, (60, yy, 1740, yy + 150), 14, (239, 246, 255), BLUE, 3)
    text(d, (90, yy + 16), "什麼情況會再建立一次？", F_BODY, BLUE)
    text(d, (90, yy + 56), "① 切換 mode（0xF06D）  ② 重新載入 /pixel（0xF06F）  "
                           "③ 自動播放換下一個組合（registry 的 maxF 到）  ④ 重開機", F_SMALL, MUTED)
    text(d, (90, yy + 96), "只要同一個組合一直播，就只付一次 137 ms；之後每格只剩清＋寫＋交。",
         F_SMALL, MUTED)

    os.makedirs(OUT, exist_ok=True)
    img.save(os.path.join(OUT, "build-once.png"))


if __name__ == "__main__":
    fig()
    print("wrote", os.path.abspath(os.path.join(OUT, "build-once.png")))
