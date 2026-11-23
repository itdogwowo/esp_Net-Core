#!/usr/bin/env python3
"""pixel_bytes_explained.py — 「16 B／8 B／4 B」到底指什麼。

輸出：docs/assets/pixel-pairing/bytes-explained.png
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
BLUE = (29, 78, 216)
BLUE_BG = (239, 246, 255)

F_TITLE = ImageFont.truetype(FONT, 40)
F_SUB = ImageFont.truetype(FONT, 20)
F_SEC = ImageFont.truetype(FONT, 23)
F_BODY = ImageFont.truetype(FONT, 19)
F_SMALL = ImageFont.truetype(FONT, 17)
F_TINY = ImageFont.truetype(FONT, 14)


def text(d, xy, s, f, fill=INK, anchor="la"):
    d.text(xy, s, font=f, fill=fill, anchor=anchor)


def blk(d, x, y, w, h, label, color, bg):
    d.rounded_rectangle((x, y, x + w, y + h), radius=5, fill=bg, outline=color, width=2)
    text(d, (x + w // 2, y + h // 2 - 8), label, F_TINY, INK, anchor="ma")


def fig():
    W, H = 1800, 1060
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    text(d, (56, 24), "「16 B／8 B／4 B」＝ 每一筆表格資料佔幾多記憶體", F_TITLE)
    text(d, (56, 70), "B ＝ byte（位元組）。1 B ＝ 8 bit。所以 16 B ＝ 16 個位元組 ＝ 128 bit。"
                      "呢個數字同「通道 ID」「位置範圍」無關。", F_SUB, MUTED)

    # 1 byte = 8 bits
    y = 118
    text(d, (56, y), "先睇 1 個位元組（1 B）", F_SEC)
    x = 56
    for i in range(8):
        blk(d, x, y + 34, 52, 38, f"bit{i}", BLUE, BLUE_BG)
        x += 56
    text(d, (x + 8, y + 44), "＝ 1 B ＝ 8 bit（可以存 0–255）", F_BODY, MUTED)

    # 16 B
    y = 220
    text(d, (56, y), "現在：每段 16 B（四個 4 B 欄位）", F_SEC)
    y += 36
    x = 56
    for label, note in (("dst 4B", "落點（32-bit）"), ("src 4B", "值索引（32-bit）"),
                        ("count 4B", "這段幾顆（32-bit）"), ("job 4B", "用哪個效果（32-bit）")):
        blk(d, x, y, 150, 44, label, RED, RED_BG)
        text(d, (x, y + 50), note, F_TINY, MUTED)
        x += 158
    text(d, (x + 6, y + 6), "＝ 16 B（128 bit）", F_BODY, RED)
    text(d, (x + 6, y + 30), "每段一筆；隨機時 9,598 段 → 153.6 KB", F_TINY, MUTED)

    # 8 B
    y = 350
    text(d, (56, y), "平面 8 B：每顆 8 B（四個 2 B 欄位）", F_SEC)
    y += 36
    x = 56
    for label, note in (("dst 2B", "落點（≤65,535）"), ("c0 2B", "R 索引"),
                        ("c1 2B", "G 索引"), ("c2 2B", "B 索引")):
        blk(d, x, y, 150, 44, label, AMBER, AMBER_BG)
        text(d, (x, y + 50), note, F_TINY, MUTED)
        x += 158
    text(d, (x + 6, y + 6), "＝ 8 B（64 bit）", F_BODY, AMBER)
    text(d, (x + 6, y + 30), "每顆一筆；9,600 顆 → 76.8 KB", F_TINY, MUTED)

    # 4 B
    y = 480
    text(d, (56, y), "平面 4 B：每顆 4 B（兩個 2 B 欄位）", F_SEC)
    y += 36
    x = 56
    for label, note in (("dst 2B", "落點（16-bit）"), ("src 2B", "值索引（16-bit）")):
        blk(d, x, y, 150, 44, label, GREEN, GREEN_BG)
        text(d, (x, y + 50), note, F_TINY, MUTED)
        x += 158
    text(d, (x + 6, y + 6), "＝ 4 B（32 bit）", F_BODY, GREEN)
    text(d, (x + 6, y + 30), "每顆一筆；9,600 顆 → 38.4 KB", F_TINY, MUTED)

    # 釐清
    y = 620
    text(d, (56, y), "你提出的三個理解，逐一釐清", F_SEC)
    rows = [
        ("「16 位是四個位描述四個通道？」",
         "唔係。通道就係 R／G／B／W 四個，冇「通道 ID」。c0／c1／c2 是「值流裡第幾個值」的索引（2 B）。"),
        ("「是指每個區塊的頭部表格把範圍限制了？」",
         "一半對：欄位闊度決定範圍（2 B 最多 65,535）。9,600 顆用 2 B 夠；超過 65,535 就要用 4 B 欄位。"),
        ("「4 位／8 位／16 位？」",
         "全部都係 B（位元組），唔係 bit：4 B／8 B／16 B。數字越大＝每筆佔用記憶體越多、讀得越慢。"),
    ]
    yy = y + 40
    for q, a in rows:
        text(d, (56, yy), q, F_BODY, INK)
        text(d, (76, yy + 26), a, F_SMALL, MUTED)
        yy += 66

    text(d, (56, H - 40), "實測（9,600 px、完全隨機）：16 B → 12.29 ms；8 B → 6.23 ms；4 B → 4.53 ms。",
         F_SMALL, MUTED)

    os.makedirs(OUT, exist_ok=True)
    img.save(os.path.join(OUT, "bytes-explained.png"))


if __name__ == "__main__":
    fig()
    print("wrote", os.path.abspath(os.path.join(OUT, "bytes-explained.png")))
