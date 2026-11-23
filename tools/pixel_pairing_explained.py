#!/usr/bin/env python3
"""pixel_pairing_explained.py — 用圖解釋隨機配對的三種做法與實測。

輸出：docs/assets/pixel-pairing/pairing-explained.png
資料：0xF077（9,600 px、完全隨機 9,598 段、USB 實測）。
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

F_TITLE = ImageFont.truetype(FONT, 40)
F_SUB = ImageFont.truetype(FONT, 20)
F_SEC = ImageFont.truetype(FONT, 24)
F_BODY = ImageFont.truetype(FONT, 19)
F_SMALL = ImageFont.truetype(FONT, 17)
F_TINY = ImageFont.truetype(FONT, 15)
F_BIG = ImageFont.truetype(FONT, 30)


def text(d, xy, s, f, fill=INK, anchor="la"):
    d.text(xy, s, font=f, fill=fill, anchor=anchor)


def cell(d, x, y, w, h, label, color, bg):
    d.rounded_rectangle((x, y, x + w, y + h), radius=6, fill=bg, outline=color, width=2)
    text(d, (x + w // 2, y + h // 2 - 9), label, F_TINY, INK, anchor="ma")


def fig():
    W, H = 1840, 1080
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    text(d, (56, 24), "隨機配對：為什麼 16 B run 慢，平面表快", F_TITLE)
    text(d, (56, 70), "B ＝ byte ＝ 位元組（16 B＝16 個位元組＝128 位元）。"
                      "完全隨機時 9,598 段、每段只有 1 顆。", F_SUB, MUTED)

    # ① 現在的 16 B run
    y = 120
    text(d, (56, y), "① 現在：每段 16 B（一次處理一整段，但隨機時每段只有 1 顆）", F_SEC)
    yy = y + 40
    x = 90
    for label, w, color, bg in (("dst 4B", 120, RED, RED_BG), ("src 4B", 120, RED, RED_BG),
                                ("count 4B", 120, RED, RED_BG), ("job 4B", 120, RED, RED_BG)):
        cell(d, x, yy, w, 46, label, color, bg)
        x += w + 8
    text(d, (x + 12, yy + 10), "→ 每段要：查 job → 查 aprog（兩次間接）、每顆再算 3 次 %",
         F_BODY, MUTED)

    # ② 平面 8 B
    y = 250
    text(d, (56, y), "② 平面 8 B／顆：落點＋三個通道起點，全部預先算好", F_SEC)
    yy = y + 40
    x = 90
    for label, w in (("dst 2B", 120), ("c0 2B", 120), ("c1 2B", 120), ("c2 2B", 120)):
        cell(d, x, yy, w, 46, label, AMBER, AMBER_BG)
        x += w + 8
    x += 20
    for label in ("dst\n2B",):
        pass
    text(d, (x, yy + 4), "→ 讀 8 B、直接寫 4 B；冇 job/aprog、冇 %", F_BODY, MUTED)
    text(d, (x, yy + 30), "實測：12.29 → 6.23 ms", F_BODY, AMBER)

    # ③ 平面 4 B
    y = 380
    text(d, (56, y), "③ 平面 4 B／顆：表細一半，通道起點即時計（3 次 %）", F_SEC)
    yy = y + 40
    x = 90
    for label, w in (("dst 2B", 120), ("src 2B", 120)):
        cell(d, x, yy, w, 46, label, GREEN, GREEN_BG)
        x += w + 8
    text(d, (x + 12, yy + 4), "→ 表 38.4 KB（8 B 版 76.8 KB）", F_BODY, MUTED)
    text(d, (x + 12, yy + 30), "實測：12.29 → 4.53 ms（−63%）", F_BODY, GREEN)

    # 柱狀比較
    y = 520
    text(d, (56, y), "實測（9,600 px、完全隨機、USB 上板）", F_SEC)
    rows = [("現在 16 B run", 12.29, RED),
            ("平面 8 B（預算 c0/c1/c2）", 6.23, AMBER),
            ("平面 4 B（dst+src，即時算）", 4.53, GREEN),
            ("平面 8 B 只讀表（唔寫入）", 3.95, MUTED)]
    x0, x1 = 520, 1560
    yy = y + 44
    for name, ms, col in rows:
        text(d, (x0 - 14, yy + 2), name, F_BODY, INK, anchor="ra")
        d.rounded_rectangle((x0, yy, x1, yy + 28), radius=6, fill=WHITE, outline=LINE, width=1)
        d.rectangle((x0 + 1, yy + 1, x0 + max(2, int((x1 - x0) * ms / 14.0)), yy + 27), fill=col)
        text(d, (x1 + 14, yy + 1), f"{ms:.2f} ms", F_BODY, col)
        yy += 40

    # 解釋
    y2 = 740
    text(d, (56, y2), "點解會快？", F_SEC)
    lines = [
        "• 隨機時 9,598 段全部只有 1 顆 → 現在的做法把「每段固定開銷」付了 9,598 次。",
        "• 平面表把「查 job、查 aprog、算通道起點」全部搬到建立組合時做一次（14.8 ms，可入快取）。",
        "• 每格只剩：讀表 ＋ 寫 4 B。實測 6.23 ms 之中，讀表佔 3.95、寫入佔約 2.3。",
        "• 4 B 版再快，是因為表由 76.8 KB 減到 38.4 KB（PSRAM 讀取減半），3 次 % 比讀 4 B 便宜。",
    ]
    yy = y2 + 42
    for ln in lines:
        text(d, (56, yy), ln, F_BODY, MUTED)
        yy += 30

    text(d, (56, H - 40), "註：4 B 版的效能原型用近似 src 值量成本；正式版會在建表時直接存正確 src。",
         F_SMALL, MUTED)

    os.makedirs(OUT, exist_ok=True)
    img.save(os.path.join(OUT, "pairing-explained.png"))


if __name__ == "__main__":
    fig()
    print("wrote", os.path.abspath(os.path.join(OUT, "pairing-explained.png")))
