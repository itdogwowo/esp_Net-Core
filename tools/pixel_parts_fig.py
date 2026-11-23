#!/usr/bin/env python3
"""pixel_parts_fig.py — 16×600（9,600 顆）各部件時間的抽象圖。

輸出：docs/assets/pixel-pairing/parts-16x600.png
資料：status §7.123（真 I80）、§7.175–§7.190（mock 管線）。
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

F_TITLE = ImageFont.truetype(FONT, 44)
F_SUB = ImageFont.truetype(FONT, 22)
F_SEC = ImageFont.truetype(FONT, 28)
F_BODY = ImageFont.truetype(FONT, 22)
F_SMALL = ImageFont.truetype(FONT, 19)
F_TINY = ImageFont.truetype(FONT, 17)
F_NUM = ImageFont.truetype(FONT, 25)


def text(d, xy, s, f, fill=INK, anchor="la"):
    d.text(xy, s, font=f, fill=fill, anchor=anchor)


def rounded(d, box, r, fill, outline=None, width=2):
    d.rounded_rectangle(box, radius=r, fill=fill, outline=outline, width=width)


def stage(d, x, y, w, h, title, sub, time, color, bg):
    rounded(d, (x, y, x + w, y + h), 14, bg, color, 3)
    text(d, (x + 16, y + 12), title, F_SEC, color)
    text(d, (x + 16, y + 50), sub, F_TINY, MUTED)
    text(d, (x + w - 16, y + 12), time, F_NUM, color, anchor="ra")


def arrow(d, x, y):
    d.line((x, y, x + 24, y), fill=MUTED, width=4)
    d.polygon([(x + 24, y), (x + 12, y - 7), (x + 12, y + 7)], fill=MUTED)


def bar(d, y, label, segs, total_ms):
    x0, x1 = 470, 1620
    text(d, (60, y + 8), label, F_BODY, INK)
    d.rounded_rectangle((x0, y, x1, y + 46), radius=8, fill=WHITE, outline=LINE, width=2)
    x = x0
    for ms, col, name in segs:
        w = max(2, int((x1 - x0) * ms / total_ms))
        x_end = min(x + w, x1 - 2)
        d.rectangle((x, y + 2, x_end, y + 44), fill=col)
        if x_end - x >= 72:
            text(d, (x + 8, y + 12), name, F_TINY, WHITE)
        x = x_end
    used = sum(s[0] for s in segs)
    text(d, (x1 + 18, y + 4), f"{used:.1f} ms", F_NUM, RED if used > total_ms else INK)
    if used > total_ms:
        text(d, (x1 + 18, y + 30), "超 20 ms", F_SMALL, RED)


def fig():
    W, H = 1840, 1240
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    text(d, (60, 28), "16×600（9,600 顆）各部件時間", F_TITLE)
    text(d, (60, 82), "50 fps ＝ 每格 20 ms。上：完整資料流；中：每格成本；下：一次性成本。"
                      "數字為真機實測（mock 管線 min of 3×5；真 I80 見 §7.123）。", F_SUB, MUTED)

    # ── 五段資料流 ──
    y, bh, bw, gap = 136, 104, 318, 34
    x = 60
    stage(d, x, y, bw, bh, "效果層", "12-bit 值（256 值／每顆一值）", "0.04／1.14 ms", GREEN, GREEN_BG)
    arrow(d, x + bw + 4, y + bh // 2); x += bw + gap
    stage(d, x, y, bw, bh, "抖動層", "12→8（1 或 4 子幀）", "3.4 ms", PURPLE, PURPLE_BG)
    arrow(d, x + bw + 4, y + bh // 2); x += bw + gap
    stage(d, x, y, bw, bh, "配對層", "8-bit 值 → 9,600 個位置", "4.1／12.3 ms", AMBER, AMBER_BG)
    arrow(d, x + bw + 4, y + bh // 2); x += bw + gap
    stage(d, x, y, bw, bh, "波形合成", "I80 packed（pack＋merge）", "19.4 ms", BLUE, BLUE_BG)
    arrow(d, x + bw + 4, y + bh // 2); x += bw + gap
    stage(d, x, y, W - 60 - x, bh, "推送", "CPU 提交／物理送燈", "1.2／18 ms", RED, RED_BG)

    text(d, (60, y + bh + 16), "波形合成 19.4 ms ＝ pack 8.2 ＋ merge 11.3（0xF062、16×600、PSRAM）；"
                               "推送 1.2 ms 係 86.8 KB msync，物理送燈 18 ms 同 CPU 重疊。",
         F_SMALL, MUTED)

    # ── 每格成本 ──
    y2 = 370
    text(d, (60, y2), "每格成本（同一條 20 ms 預算）", F_SEC)
    rows = [
        ("現況（mock、500 組）", [(0.04, GREEN, "效果"), (2.0, RED, "清畫布"),
                                 (3.7, AMBER, "寫入"), (4.5, RED, "複製去環")]),
        ("優化後（mock、500 組）", [(0.04, GREEN, "效果"), (4.1, AMBER, "寫入（含抖動）")]),
        ("完全隨機（優化後）", [(0.04, GREEN, "效果"), (12.3, AMBER, "寫入（9,598 段）")]),
        ("10-bit 抖動（2,400 px）", [(0.04, GREEN, "效果"), (4.6, PURPLE, "4 子幀一次掃表")]),
        ("硬件層（真 I80、16×600）", [(19.4, BLUE, "波形合成"), (1.2, RED, "推送")]),
    ]
    yy = y2 + 42
    for label, segs in rows:
        bar(d, yy, label, segs, 20.0)
        yy += 64

    # ── 一次性 ──
    y3 = yy + 22
    text(d, (60, y3), "一次性（唔係每格）", F_SEC)
    items = [
        ("組合建立（500 組）", "40 ms"),
        ("組合建立（完全隨機）", "137 ms"),
        ("讀快取（500 組）", "10 ms"),
        ("開機補建（每個缺的組合）", "≈300 ms"),
    ]
    xx = 60
    for name, val in items:
        rounded(d, (xx, y3 + 42, xx + 400, y3 + 124), 12, WHITE, LINE, 2)
        text(d, (xx + 16, y3 + 54), name, F_BODY, MUTED)
        text(d, (xx + 16, y3 + 86), val, F_NUM, INK)
        xx += 425

    text(d, (60, H - 92), "註 1：mock render 只量 effect／pair（真 I80 起唔到，因為 DMA 記憶體）；"
                           "所以「現況／優化」兩行係 CPU 成本，唔含波形合成。", F_SMALL, MUTED)
    text(d, (60, H - 60), "註 2：真 I80 16×600 的硬件層每格已 20.6 ms（合成 19.4 ＋ 推送 1.2），"
                           "實測 frame 37.9–39.6 ms → 33–34 fps，並會觸發 Task WDT。", F_SMALL, MUTED)
    text(d, (60, H - 28), "結論：效果／配對／抖動不是牆；16×600 的牆是 I80 波形合成（19.4 ms）。",
         F_SMALL, INK)

    os.makedirs(OUT, exist_ok=True)
    img.save(os.path.join(OUT, "parts-16x600.png"))


if __name__ == "__main__":
    fig()
    print("wrote", os.path.abspath(os.path.join(OUT, "parts-16x600.png")))
