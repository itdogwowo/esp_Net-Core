#!/usr/bin/env python3
"""pixel_extreme_cards.py — 16×600 極端情況：每個部件一張卡（唔加總）。

輸出：docs/assets/pixel-pairing/extreme-cards.png
資料：status §7.123、§7.175–§7.194（真機）。
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
GREEN = (5, 150, 105)
GREEN_BG = (236, 253, 245)
PURPLE = (109, 40, 217)
PURPLE_BG = (245, 243, 255)
AMBER = (180, 83, 9)
AMBER_BG = (255, 251, 235)
RED = (185, 28, 28)
RED_BG = (254, 242, 242)
BLUE = (29, 78, 216)
BLUE_BG = (239, 246, 255)
GREY = (71, 85, 105)
TEAL = (13, 148, 136)
TEAL_BG = (240, 253, 250)
GREY_BG = (241, 245, 249)

F_TITLE = ImageFont.truetype(FONT, 42)
F_SUB = ImageFont.truetype(FONT, 21)
F_SEC = ImageFont.truetype(FONT, 27)
F_CARD = ImageFont.truetype(FONT, 24)
F_BIG = ImageFont.truetype(FONT, 34)
F_SMALL = ImageFont.truetype(FONT, 18)
F_TINY = ImageFont.truetype(FONT, 16)


def text(d, xy, s, f, fill=INK, anchor="la"):
    d.text(xy, s, font=f, fill=fill, anchor=anchor)


def card(d, x, y, w, h, title, big, lines, color, bg):
    d.rounded_rectangle((x, y, x + w, y + h), radius=14, fill=bg, outline=color, width=3)
    text(d, (x + 16, y + 10), title, F_CARD, color)
    text(d, (x + 16, y + 44), big, F_BIG, INK)
    yy = y + 88
    for ln in lines:
        text(d, (x + 16, yy), ln, F_TINY, MUTED)
        yy += 22


def fig():
    W, H = 1860, 800
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    text(d, (56, 24), "16×600 極端情況：每個部件一張卡（唔加總）", F_TITLE)
    text(d, (56, 74), "每張卡自己一個數字；要組合就自己揀。數字為真機實測（mock 管線 min of 3×5；"
                      "I80 見 §7.123）。", F_SUB, MUTED)

    x0, y0 = 56, 122
    cw, ch, gx, gy = 430, 168, 16, 16

    row1 = [
        ("效果層", "5.03 ms", ["38,400 波（9,600 顆 × 4 通道）", "max 5.99 ms；RGB 28,800 ≈ 3.8 ms"],
         GREEN, GREEN_BG),
        ("抖動層 12→8", "3.43 ms", ["單次轉換（9,600 值）", "4 子幀只在高刷模式：2,400 px→4.63 ms"],
         PURPLE, PURPLE_BG),
        ("配對層", "12.29 ms", ["9,600 px、完全隨機（9,598 段）", "500 組 4.06；2,400 隨機 2.69"],
         AMBER, AMBER_BG),
        ("I80 波形合成", "19.44 ms", ["16×600：pack 8.2 ＋ merge 11.3", "2,400 px 未量（線性推算 ≈4.9）"],
         BLUE, BLUE_BG),
    ]
    for i, c in enumerate(row1):
        card(d, x0 + i * (cw + gx), y0, cw, ch, *c)

    row2 = [
        ("清畫布（現況）", "2.0 ms", ["9,600 px memset 38.4 KB", "2,400 px → 0.1 ms；將刪"],
         RED, RED_BG),
        ("複製去環（現況）", "4.5 ms", ["畫布 → 環（38.4 KB）", "2,400 px → 0.33 ms；將刪"],
         RED, RED_BG),
        ("推送：CPU 提交", "1.16 ms", ["86.8 KB msync（16×600）", "只佔 CPU 1.16 ms"],
         TEAL, TEAL_BG),
        ("WS2812 線路等待", "18 ms", ["600 顆 × 24 bit ÷ 800 kHz（唔佔 CPU）", "16×150×4 子幀亦 18 ms"],
         TEAL, TEAL_BG),
    ]
    for i, c in enumerate(row2):
        card(d, x0 + i * (cw + gx), y0 + ch + gy, cw, ch, *c)

    row3 = [
        ("組合建立（500 組）", "40 ms", ["9,600 px、1,000 段", "完全隨機（9,598 段）137 ms"],
         GREY, GREY_BG),
        ("讀快取（500 組）", "10 ms", ["12.5 KB 檔", "單組（0.6 KB）3.6–4.1 ms"],
         GREY, GREY_BG),
        ("寫快取", "265 ms", ["每個組合（FAT 建立檔案）", "同檔案大小無關"],
         GREY, GREY_BG),
        ("開機補建", "≈300 ms", ["每個缺少的組合（build 38＋寫 265）", "已有就跳過"],
         GREY, GREY_BG),
    ]
    for i, c in enumerate(row3):
        card(d, x0 + i * (cw + gx), y0 + 2 * (ch + gy), cw, ch, *c)

    text(d, (56, H - 50), "用法：橫向三行分別係「每格 CPU」「硬件層」「一次性」；"
                          "每格總數自己按需要加（例如 效果 5.03 ＋ 抖動 3.43 ＋ 配對 12.29）。",
         F_SMALL, MUTED)

    os.makedirs(OUT, exist_ok=True)
    img.save(os.path.join(OUT, "extreme-cards.png"))


if __name__ == "__main__":
    fig()
    print("wrote", os.path.abspath(os.path.join(OUT, "extreme-cards.png")))
