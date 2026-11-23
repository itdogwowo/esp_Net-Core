#!/usr/bin/env python3
"""pixel_extreme_bars.py — 16×600 極端情況：棒形圖（長度 = 時間）。

三把尺：
  A. 每格 CPU（0–20 ms）
  B. 每格線路等待（WS2812 協議硬性、不佔 CPU；0–20 ms）
  C. 一次性（0–320 ms）

輸出：docs/assets/pixel-pairing/extreme-bars.png
資料：status §7.123、§7.175–§7.196（真機）。
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
PURPLE = (109, 40, 217)
AMBER = (180, 83, 9)
RED = (185, 28, 28)
BLUE = (29, 78, 216)
TEAL = (13, 148, 136)
GREY = (71, 85, 105)

F_TITLE = ImageFont.truetype(FONT, 42)
F_SUB = ImageFont.truetype(FONT, 21)
F_SEC = ImageFont.truetype(FONT, 27)
F_BODY = ImageFont.truetype(FONT, 21)
F_SMALL = ImageFont.truetype(FONT, 18)
F_VAL = ImageFont.truetype(FONT, 21)


def text(d, xy, s, f, fill=INK, anchor="la"):
    d.text(xy, s, font=f, fill=fill, anchor=anchor)


def bars(d, y, rows, total, x0=520, x1=1560, bar_h=30, gap=14, budget=None):
    scale = (x1 - x0) / total
    for name, ms, col in rows:
        d.rounded_rectangle((x0, y, x1, y + bar_h), radius=6, fill=WHITE, outline=LINE, width=1)
        w = max(2, int(ms * scale))
        d.rectangle((x0 + 1, y + 1, x0 + w, y + bar_h - 1), fill=col)
        text(d, (x0 - 14, y + 2), name, F_BODY, INK, anchor="ra")
        text(d, (x1 + 16, y + 2), (f"{ms:.2f} ms" if ms < 100 else f"{ms:.0f} ms"), F_VAL, col)
        y += bar_h + gap
    if budget is not None:
        gx = x0 + int(budget * scale)
        d.line((gx, y - (bar_h + gap) * len(rows) - 4, gx, y - gap + 2), fill=RED, width=3)
        text(d, (gx + 6, y - gap + 4), f"{budget:.0f} ms", F_SMALL, RED)
    return y


def fig():
    W, H = 1860, 1240
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    text(d, (56, 20), "16×600 極端情況（棒長 = 時間）", F_TITLE)
    text(d, (56, 66), "三把尺：A 每格 CPU、B 每格線路等待（WS2812 硬性、唔佔 CPU）、C 一次性。真機實測。",
         F_SUB, MUTED)

    text(d, (56, 104), "A. 每格 CPU（20 ms ＝ 50 fps）", F_SEC)
    cpu_rows = [
        ("I80 波形合成（16×600）", 19.44, BLUE),
        ("配對層（9,600 完全隨機）", 12.29, AMBER),
        ("效果層（38,400 波）", 5.03, GREEN),
        ("抖動 4 子幀（2,400 px）", 4.63, PURPLE),
        ("複製去環（現況）", 4.50, RED),
        ("配對層（500 組）", 4.06, AMBER),
        ("抖動 12→8（9,600 值）", 3.43, PURPLE),
        ("配對層（2,400 隨機）", 2.69, AMBER),
        ("清畫布（現況）", 2.00, RED),
        ("推送：CPU 提交（16×600）", 1.16, TEAL),
    ]
    y = bars(d, 148, cpu_rows, 22.0, budget=20.0)

    text(d, (56, y + 8), "B. 每格線路等待（WS2812 協議硬性；CPU 同期可以做其他事）", F_SEC)
    wire_rows = [
        ("WS2812 送燈：16×600、1 幀", 18.0, TEAL),
        ("WS2812 送燈：16×150、4 子幀", 18.0, TEAL),
    ]
    y = bars(d, y + 48, wire_rows, 22.0, budget=20.0)

    text(d, (56, y + 10), "C. 一次性（0–320 ms，另一把尺）", F_SEC)
    once_rows = [
        ("開機補建（每個缺的組合）", 300, GREY),
        ("寫快取（每個組合）", 265, GREY),
        ("組合建立（完全隨機）", 137, GREY),
        ("組合建立（500 組）", 40, GREY),
        ("讀快取（500 組）", 10, GREY),
    ]
    bars(d, y + 50, once_rows, 320.0)

    text(d, (56, H - 92), "WS2812 送燈 = 600 顆 × 24 bit ÷ 800 kHz ＝ 18 ms（線路時間，唔係 CPU）；"
                           "CPU 只花 1.16 ms 提交 86.8 KB。16×150 × 4 子幀同樣是 18 ms。",
         F_SMALL, MUTED)
    text(d, (56, H - 62), "4 子幀只在子幀率 ≥200 Hz 的模式出現 → 每條線 px ÷4（2,400）；"
                           "「9,600 px × 4 子幀」唔係可配置實況（上界 20.04 ms）。", F_SMALL, MUTED)
    text(d, (56, H - 32), "顏色：綠＝效果、紫＝抖動、橙＝配對、藍＝硬件 CPU、青＝送燈（CPU／線路）、"
                           "紅＝現況待刪、灰＝一次性。", F_SMALL, MUTED)

    os.makedirs(OUT, exist_ok=True)
    img.save(os.path.join(OUT, "extreme-bars.png"))


if __name__ == "__main__":
    fig()
    print("wrote", os.path.abspath(os.path.join(OUT, "extreme-bars.png")))
