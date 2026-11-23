#!/usr/bin/env python3
"""pixel_three_schemes.py — 16 B（run） / 8 B / 4 B 三種表的能力與速度對比。

輸出：docs/assets/pixel-pairing/three-schemes.png
資料：0xF077 USB 實測（9,600 px）。
"""
import os

from PIL import Image, ImageDraw, ImageFont

FONT = "/System/Library/Fonts/STHeiti Medium.ttc"
OUT = os.path.join(os.path.dirname(__file__), "..", "docs", "assets", "pixel-pairing")

INK = (17, 24, 39)
MUTED = (100, 116, 139)
LINE = (203, 213, 225)
BG = (248, 250, 252)
WHITE = (255, 255, 255)
RED = (185, 28, 28)
RED_BG = (254, 242, 242)
AMBER = (180, 83, 9)
AMBER_BG = (255, 251, 235)
GREEN = (5, 150, 105)
GREEN_BG = (236, 253, 245)

F_TITLE = ImageFont.truetype(FONT, 40)
F_SUB = ImageFont.truetype(FONT, 20)
F_SEC = ImageFont.truetype(FONT, 23)
F_BODY = ImageFont.truetype(FONT, 19)
F_SMALL = ImageFont.truetype(FONT, 17)
F_TINY = ImageFont.truetype(FONT, 15)


def text(d, xy, s, f, fill=INK, anchor="la"):
    d.text(xy, s, font=f, fill=fill, anchor=anchor)


def fig():
    W, H = 1840, 1020
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    text(d, (56, 24), "三種表：16 B（run）／8 B／4 B", F_TITLE)
    text(d, (56, 70), "上：能力對比；下：實測速度。8 B 同 4 B 都係「每顆一筆」（每顆都有自己的表項）。", F_SUB, MUTED)

    # 能力表
    x0, y0 = 56, 116
    cols = [("能力", 420), ("16 B run", 330), ("8 B／顆", 330), ("4 B／顆", 330)]
    x = x0
    text(d, (x, y0), cols[0][0], F_SEC)
    text(d, (x + cols[0][1], y0), cols[1][0], F_SEC, RED)
    text(d, (x + cols[0][1] + cols[1][1], y0), cols[2][0], F_SEC, AMBER)
    text(d, (x + cols[0][1] + cols[1][1] + cols[2][1], y0), cols[3][0], F_SEC, GREEN)
    y = y0 + 36
    d.line((x0, y, 1800, y), fill=LINE, width=2)
    y += 10

    rows = [
        ("每段可有不同寫入型別", "得（job→write）", "要分表（每表一型別）", "要分表（每表一型別）"),
        ("一個 mode 多個效果", "得（job→aprog）", "要分表（每 effect 一張）", "要分表（每 effect 一張）"),
        ("覆蓋範圍／反轉／stride／跨型別 slice", "得（建表時展開）", "得（建表時展開）", "得（建表時展開）"),
        ("效果值 12-bit", "得（只存索引）", "得（只存索引）", "得（只存索引）"),
        ("像素上限", "u32", "u16（65,535）", "u16（65,535）"),
        ("值索引上限", "u32", "u16（65,535）", "u16（65,535）"),
        ("表大小（9,600 px）", "16 B × 段數（1 段＝16 B）", "76.8 KB", "38.4 KB"),
        ("快取檔／讀取", "12.5 KB／10 ms", "76.8 KB／約 50–80 ms*", "38.4 KB／約 25–40 ms*"),
        ("建表（一次性）", "40–137 ms（已有）", "+14.8 ms（8 B）", "+14.8 ms（4 B）"),
    ]
    for name, a, b, c in rows:
        text(d, (x0, y), name, F_BODY, INK)
        text(d, (x0 + cols[0][1], y), a, F_SMALL, RED)
        text(d, (x0 + cols[0][1] + cols[1][1], y), b, F_SMALL, AMBER)
        text(d, (x0 + cols[0][1] + cols[1][1] + cols[2][1], y), c, F_SMALL, GREEN)
        y += 34

    # 速度
    y2 = y + 30
    text(d, (56, y2), "實測速度（apply，9,600 px，USB）", F_SEC)
    hdr = [("排列", 420), ("16 B run", 300), ("8 B／顆", 300), ("4 B／顆", 300)]
    x = 56
    for name, w in hdr:
        text(d, (x, y2 + 40), name, F_BODY, MUTED)
        x += w
    xr = 56
    x1 = 476
    x2 = 776
    x3 = 1076
    yy = y2 + 76
    data = [
        ("1 段（9,600 px）", "2.98 ms", "3.50 ms", "2.28 ms"),
        ("500 組（500 段）", "3.73 ms", "3.29 ms", "2.38 ms"),
        ("500 組（1,000 段）", "4.10 ms", "—", "—"),
        ("完全隨機（9,598 段）", "12.29 ms", "6.23 ms", "4.50 ms"),
    ]
    for name, a, b, c in data:
        text(d, (xr, yy), name, F_BODY, INK)
        text(d, (x1, yy), a, F_BODY, RED)
        text(d, (x2, yy), b, F_BODY, AMBER)
        text(d, (x3, yy), c, F_BODY, GREEN)
        yy += 32

    text(d, (56, H - 92), "重點：寫入型別同多效果都可以保留——只要「每個 entry 一張表」"
                          "（表頭帶 write／aprog，值流各自）。8 B 版比 4 B 大、又冇多能力，所以被 4 B 支配。",
         F_SMALL, MUTED)
    text(d, (56, H - 60), "4 B 版的 src 係「值流索引」；每個 entry 一張表，表頭帶 write／aprog，"
                          "值流各自——所以索引唔會爆，亦唔會混到第二個效果。",
         F_SMALL, MUTED)
    text(d, (56, H - 28), "建議：run 表保留為正本；切換時每個 entry 展開一張 4 B 表（表頭帶 write／aprog）。"
                          "  * 快取讀取時間為推算（12.5 KB → 10 ms 線性）。",
         F_SMALL, INK)

    os.makedirs(OUT, exist_ok=True)
    img.save(os.path.join(OUT, "three-schemes.png"))


if __name__ == "__main__":
    fig()
    print("wrote", os.path.abspath(os.path.join(OUT, "three-schemes.png")))
