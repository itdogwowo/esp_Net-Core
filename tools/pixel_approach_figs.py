#!/usr/bin/env python3
"""pixel_approach_figs.py — 現在 vs 打算：配對（隨機表）與波形合成。

輸出：docs/assets/pixel-pairing/approach-now-vs-plan.png
資料：0xF077 原型（2026-11-24，9,600 px、完全隨機 9,598 段）。
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
GREEN = (5, 150, 105)
GREEN_BG = (236, 253, 245)
BLUE = (29, 78, 216)
BLUE_BG = (239, 246, 255)
AMBER = (180, 83, 9)
AMBER_BG = (255, 251, 235)

F_TITLE = ImageFont.truetype(FONT, 40)
F_SUB = ImageFont.truetype(FONT, 20)
F_SEC = ImageFont.truetype(FONT, 25)
F_BODY = ImageFont.truetype(FONT, 20)
F_SMALL = ImageFont.truetype(FONT, 17)
F_TINY = ImageFont.truetype(FONT, 15)
F_BIG = ImageFont.truetype(FONT, 34)


def text(d, xy, s, f, fill=INK, anchor="la"):
    d.text(xy, s, font=f, fill=fill, anchor=anchor)


def panel(d, x, y, w, h, title, color, bg, rows):
    d.rounded_rectangle((x, y, x + w, y + h), radius=14, fill=bg, outline=color, width=3)
    text(d, (x + 18, y + 12), title, F_SEC, color)
    yy = y + 52
    for row in rows:
        kind = row[0]
        if kind == "time":
            text(d, (x + 18, yy), row[1], F_SMALL, MUTED)
            text(d, (x + 18, yy + 20), row[2], F_BIG, row[3])
            yy += 62
        elif kind == "bar":
            label, ms, color2 = row[1], row[2], row[3]
            bx0 = x + 250
            bx1 = x + w - 110
            text(d, (x + 18, yy + 2), label, F_BODY, INK)
            d.rounded_rectangle((bx0, yy, bx1, yy + 22), radius=5, fill=WHITE,
                                outline=LINE, width=1)
            d.rectangle((bx0 + 1, yy + 1, bx0 + max(2, int((bx1 - bx0) * ms / 20.0)), yy + 21),
                        fill=color2)
            text(d, (bx1 + 8, yy - 1), f"{ms:.2f} ms", F_BODY, color2)
            yy += 34
        else:
            text(d, (x + 18, yy), row[1], F_TINY, MUTED)
            yy += 20


def fig():
    W, H = 1880, 980
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    text(d, (56, 24), "現在 vs 打算：隨機配對與 I80 波形合成", F_TITLE)
    text(d, (56, 72), "左：完全隨機配對（9,598 段、每段 1 顆）——已用原型實測。"
                      "右：波形合成——現況實測＋打算做法（待原型驗證）。", F_SUB, MUTED)

    # 左：配對
    panel(d, 56, 116, 870, 470, "配對層（完全隨機）", AMBER, AMBER_BG, [
        ("time", "現在：16 B run × 9,598 段，每段查 job→aprog、3 次 %", "12.28 ms", RED),
        ("bar", "現在（apply）", 12.28, RED),
        ("text", "  run = {dst, src, count, job}；每段落點再算 c0/c1/c2"),
        ("time", "打算：平面 8 B（dst, c0, c1, c2）＋最小迴圈", "6.22 ms", GREEN),
        ("bar", "打算（原型實測）", 6.22, GREEN),
        ("text", "  直接讀 8 B、寫 4 B；冇 job/aprog 間接、冇 %"),
        ("text", "  平面表建立 14.8 ms（一次性，跟組合建立/快取一齊做）"),
        ("text", "  省 49%；如果 count>1 的段保留，混合做法也一樣快"),
    ])

    # 右：波形合成
    panel(d, 954, 116, 870, 470, "I80 波形合成（16×600）", BLUE, BLUE_BG, [
        ("time", "現在：逐 lane wave_pack → merge8 逐 byte 移位/OR", "19.44 ms", RED),
        ("bar", "現在（pack＋merge）", 19.44, RED),
        ("text", "  86.4 KB 輸出 ÷ 19.44 ms ≈ 4.5 MB/s（CPU-bound，遠低於記憶體上限）"),
        ("time", "SWAR 原型（已實測）", "18.51 ms（1.22×）", GREEN),
        ("bar", "SWAR 原型（實測）", 18.51, GREEN),
        ("text", "  8×8 轉置正確（match=1），但只快 1.22×——瓶頸是 gather 不是算術"),
        ("time", "打算 C：單次展開＋轉置（FastLED 式）", "待原型", AMBER),
        ("time", "打算 B：chunked DMA（與送燈重疊）", "可選", AMBER),
        ("text", "  async 已存在：pack 與 18 ms 送燈並行 → wall time ≈ max(encode, 18)"),
        ("text", "  chunked 只降低單次 encode 延遲，不改變總量"),
    ])

    text(d, (56, 700), "為什麼可以快：", F_SEC)
    text(d, (56, 742), "① 配對：隨機的 9,598 段全部是 1 顆，成本幾乎全是「每段固定開銷」；"
                       "壓成平面表就只剩純寫入。", F_BODY, MUTED)
    text(d, (56, 776), "② 波形合成：現在逐 byte 做移位／OR（4.5 MB/s），"
                       "改用 64-bit SWAR 一次轉 8×8（記憶體上限 ~40 MB/s 以上）→ 有 5–8 倍空間。",
         F_BODY, MUTED)
    text(d, (56, 810), "③ RMT 唔可行：ESP32-S3 RMT 冇 DMA，16 條線 × 600 顆一定要用 LCD_CAM（I80）。",
         F_BODY, MUTED)

    text(d, (56, H - 46), "資料：0xF077 原型（9,600 px、固定種子、min of 3×5）；"
                          "波形合成數字見 §7.122／§7.123，SWAR 為估算、待原型。", F_SMALL, MUTED)

    os.makedirs(OUT, exist_ok=True)
    img.save(os.path.join(OUT, "approach-now-vs-plan.png"))


if __name__ == "__main__":
    fig()
    print("wrote", os.path.abspath(os.path.join(OUT, "approach-now-vs-plan.png")))
