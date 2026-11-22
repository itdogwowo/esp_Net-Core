#!/usr/bin/env python3
"""pixel_layers_figs.py — 用「效果層／配對層／硬件層」講 16×600 每格成本。

輸出：docs/assets/pixel-pairing/layers-16x600.png
"""
import os

from PIL import Image, ImageDraw, ImageFont

FONT = "/System/Library/Fonts/STHeiti Medium.ttc"
OUT = os.path.join(os.path.dirname(__file__), "..", "docs", "assets", "pixel-pairing")

INK = (31, 41, 51)
MUTED = (82, 96, 109)
LINE = (203, 213, 225)
BG = (248, 250, 252)
GREEN = (16, 185, 129)
GREEN_BG = (236, 253, 245)
AMBER = (217, 119, 6)
AMBER_BG = (255, 251, 235)
BLUE = (37, 99, 235)
BLUE_BG = (239, 246, 255)
PURPLE = (124, 58, 237)
PURPLE_BG = (245, 243, 255)
WHITE = (255, 255, 255)

F_TITLE = ImageFont.truetype(FONT, 44)
F_SUB = ImageFont.truetype(FONT, 24)
F_SEC = ImageFont.truetype(FONT, 30)
F_BODY = ImageFont.truetype(FONT, 24)
F_SMALL = ImageFont.truetype(FONT, 20)
F_NUM = ImageFont.truetype(FONT, 30)


def text(d, xy, s, f, fill=INK, anchor="la"):
    d.text(xy, s, font=f, fill=fill, anchor=anchor)


def rounded(d, box, r, fill, outline=None, width=2):
    d.rounded_rectangle(box, radius=r, fill=fill, outline=outline, width=width)


def layer(d, y, h, title, color, bg, lines, right):
    rounded(d, (60, y, 1740, y + h), 16, bg, color, 3)
    text(d, (90, y + 16), title, F_SEC, color)
    yy = y + 62
    for ln in lines:
        text(d, (90, yy), ln, F_BODY, MUTED)
        yy += 32
    # 右邊：數字
    text(d, (1330, y + 24), right[0], F_NUM, color)
    text(d, (1330, y + 66), right[1], F_SMALL, MUTED)


def fig_layers():
    W, H = 1800, 1080
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    text(d, (60, 34), "16×600：效果層與配對層，每格（50 FPS＝20 ms）的成本", F_TITLE)
    text(d, (60, 92), "9,600 顆 = 16 條線 × 每條 600 顆。硬件層已經驗證（每條線 600 顆 ≈ 18 ms／格）。"
                      "這裡只量效果層、配對層。", F_SUB, MUTED)

    layer(d, 150, 190, "效果層", GREEN, GREEN_BG, [
        "決定「總共幾多像素」（16×600 = 9,600 顆），輸出每顆的 12-bit 亮度值",
        "典型：一個效果 256 個值；極限：每顆燈一個值（9,600 個值）",
    ], ("39 µs ／ 1.14 ms", "典型 ／ 每顆一個值"))

    layer(d, 360, 250, "配對層", AMBER, AMBER_BG, [
        "把效果層的值放到 9,600 個「邊顆燈」的位置（500 個組合也可以）",
        "① 清畫布（9,600 × 4 位元組）  ② 寫入  ③ 交去硬件層",
    ], ("9.7 ms", "清 2.0 ＋ 寫 3.2 ＋ 交 4.5"))

    layer(d, 630, 190, "硬件層（已驗證）", BLUE, BLUE_BG, [
        "16 條線、每條 600 顆、各自 800 kHz；600 × 24 bit ÷ 800 kHz ≈ 18 ms／格",
        "16 條線同步 → 整幅 9,600 顆約 18 ms／格（≈55 FPS，50 FPS 有餘量）",
    ], ("18 ms", "每條線，不是 CPU"))

    # 預算條
    y = 850
    text(d, (60, y), "每格 20 ms 預算（50 FPS）", F_SEC)
    bx, bw, bh = 60, 1680, 46
    rounded(d, (bx, y + 46, bx + bw, y + 46 + bh), 8, WHITE, LINE, 2)
    used = int(bw * 10.8 / 20.0)
    d.rounded_rectangle((bx, y + 46, bx + used, y + 46 + bh), 8, fill=AMBER)
    text(d, (bx + 12, y + 54), "效果層＋配對層 ≈ 10.8 ms（54%）", F_BODY, WHITE)
    text(d, (bx + used + 12, y + 54), "剩 ≈ 9.2 ms（可以加抖動／其他工作）", F_BODY, MUTED)
    text(d, (bx, y + 108), "硬件層 18 ms 是「送燈」的物理時間，同 CPU 的 10.8 ms 重疊（不同工作同時做），"
                           "所以 50 FPS 仍然成立。", F_SMALL, MUTED)

    os.makedirs(OUT, exist_ok=True)
    img.save(os.path.join(OUT, "layers-16x600.png"))


if __name__ == "__main__":
    fig_layers()
    print("wrote", os.path.abspath(os.path.join(OUT, "layers-16x600.png")))
