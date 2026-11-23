#!/usr/bin/env python3
"""pixel_effect_fig.py — 效果層基準：9,600 顆 × 4 通道（38,400 個波）。

輸出：docs/assets/pixel-pairing/effect-38400.png
資料：2026-11-24 真機 0xF071（ESP32-S3-Zero-Show9600、mode full38400）
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
AMBER = (180, 83, 9)
AMBER_BG = (255, 251, 235)
RED = (185, 28, 28)
BLUE = (29, 78, 216)
GREY = (148, 163, 184)

F_TITLE = ImageFont.truetype(FONT, 44)
F_SUB = ImageFont.truetype(FONT, 22)
F_SEC = ImageFont.truetype(FONT, 28)
F_BODY = ImageFont.truetype(FONT, 22)
F_SMALL = ImageFont.truetype(FONT, 19)
F_NUM = ImageFont.truetype(FONT, 28)


def text(d, xy, s, f, fill=INK, anchor="la"):
    d.text(xy, s, font=f, fill=fill, anchor=anchor)


def rounded(d, box, r, fill, outline=None, width=2):
    d.rounded_rectangle(box, radius=r, fill=fill, outline=outline, width=width)


def fig():
    W, H = 1760, 980
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)

    text(d, (60, 28), "效果層基準：9,600 顆 × 4 通道（38,400 個波）", F_TITLE)
    text(d, (60, 82), "每個 LED 的每個通道都分配一個波；50 fps ＝ 每格 20 ms。"
                      "真機量測（0xF071、avg of 336 格）。", F_SUB, MUTED)

    # ── 左：三個規模的效果層時間 ──
    text(d, (60, 150), "效果層時間", F_SEC)
    rows = [("256 個波（一個效果）", 0.039, GREEN),
            ("9,600 個波（每顆 1 值）", 1.146, GREEN),
            ("38,400 個波（每顆 × 4 通道）", 5.025, AMBER)]
    x0, x1 = 420, 1100
    scale = (x1 - x0) / 20.0
    y = 200
    for name, ms, col in rows:
        text(d, (60, y + 8), name, F_BODY, INK)
        d.rounded_rectangle((x0, y, x1, y + 44), radius=8, fill=WHITE, outline=LINE, width=2)
        d.rectangle((x0 + 2, y + 2, x0 + max(3, int(ms * scale)), y + 42), fill=col)
        text(d, (x1 + 14, y + 6), f"{ms:.2f} ms", F_BODY, col)
        y += 72
    # 20 ms 界線
    gx = x0 + int(20 * scale)
    d.line((gx, 190, gx, y - 10), fill=RED, width=3)
    text(d, (gx - 10, 168), "20 ms（50 fps）", F_SMALL, RED, anchor="ra")

    # ── 右：每格預算（38,400 個波的情況） ──
    bx, by, bw = 1230, 200, 470
    text(d, (bx, 150), "每格預算（38,400 波）", F_SEC)
    rounded(d, (bx, by, bx + bw, by + 340), 14, WHITE, LINE, 2)
    text(d, (bx + 18, by + 16), "20 ms 預算", F_BODY, MUTED)
    # 堆疊條
    total = 20.0
    bar_h = 52
    yy = by + 60
    d.rounded_rectangle((bx + 18, yy, bx + bw - 18, yy + bar_h), radius=8,
                        fill=(241, 245, 249), outline=LINE, width=2)
    segs = [(5.03, AMBER, "效果 5.0"), (12.4, GREY, "配對 12.4")]
    xx = bx + 20
    span = (bw - 40)
    for ms, col, name in segs:
        w = int(span * ms / total)
        d.rectangle((xx, yy + 2, xx + w, yy + bar_h - 2), fill=col)
        text(d, (xx + 8, yy + 12), name, F_SMALL, WHITE)
        xx += w
    text(d, (bx + 18, yy + bar_h + 18), "已用 17.4 ms（87%）；剩 2.6 ms", F_BODY, INK)
    text(d, (bx + 18, yy + bar_h + 54), "送燈 18 ms（物理、與 CPU 重疊）", F_SMALL, MUTED)
    text(d, (bx + 18, yy + bar_h + 92), "效果 5.0 ms ＝ 20 ms 的 25%", F_BODY, AMBER)
    text(d, (bx + 18, yy + bar_h + 128), "每顆 LED（4 通道）0.52 µs", F_SMALL, MUTED)
    text(d, (bx + 18, yy + bar_h + 158), "每個波 131 ns", F_SMALL, MUTED)
    text(d, (bx + 18, yy + bar_h + 194), "註：配對 12.4 ms 含清畫布＋複製（將刪）", F_SMALL, MUTED)

    # ── 底部：換算 ──
    y3 = 620
    text(d, (60, y3), "換算（50 fps 基準）", F_SEC)
    items = [
        ("38,400 個波", "5.03 ms", "20 ms 的 25%"),
        ("9,600 顆 LED", "0.52 µs／顆", "4 通道合計"),
        ("每個波", "131 ns", "波表查表＋取模"),
        ("RGB 三通道（28,800 波）", "≈3.8 ms", "線性推算"),
    ]
    xx = 60
    for name, val, note in items:
        rounded(d, (xx, y3 + 46, xx + 400, y3 + 150), 12, WHITE, LINE, 2)
        text(d, (xx + 16, y3 + 58), name, F_BODY, MUTED)
        text(d, (xx + 16, y3 + 92), val, F_NUM, INK)
        text(d, (xx + 16, y3 + 126), note, F_SMALL, MUTED)
        xx += 420

    rounded(d, (60, 800, 1700, 920), 14, GREEN_BG, GREEN, 3)
    text(d, (84, 818), "結論", F_BODY, GREEN)
    text(d, (84, 852), "效果層 38,400 個波 = 5.03 ms（20 ms 的 25%），仍然有充足餘量；"
                       "真正吃預算的是配對層的舊路徑（清畫布＋複製 6.5 ms）與 I80 波形合成（19.4 ms）。",
         F_SMALL, INK)
    text(d, (84, 884), "如果每個 LED 都要 4 個獨立波（RGBW），效果層每格成本就是 5 ms 級，"
                       "不是 1 ms 級——這個要計入 50 fps 的預算。", F_SMALL, MUTED)

    os.makedirs(OUT, exist_ok=True)
    img.save(os.path.join(OUT, "effect-38400.png"))


if __name__ == "__main__":
    fig()
    print("wrote", os.path.abspath(os.path.join(OUT, "effect-38400.png")))
