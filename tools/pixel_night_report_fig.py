#!/usr/bin/env python3
"""pixel_night_report_fig.py — 夜間執行報告圖（前後對比＋完成／未完成）。

輸出：docs/assets/pixel-pairing/night-report.png
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
GREY = (71, 85, 105)
GREY_BG = (241, 245, 249)
PURPLE = (109, 40, 217)

F_TITLE = ImageFont.truetype(FONT, 42)
F_SUB = ImageFont.truetype(FONT, 20)
F_SEC = ImageFont.truetype(FONT, 26)
F_BODY = ImageFont.truetype(FONT, 20)
F_SMALL = ImageFont.truetype(FONT, 17)
F_TINY = ImageFont.truetype(FONT, 15)
F_BIG = ImageFont.truetype(FONT, 40)


def text(d, xy, s, f, fill=INK, anchor="la"):
    d.text(xy, s, font=f, fill=fill, anchor=anchor)


def card(d, x, y, w, h, title, big, lines, color, bg):
    d.rounded_rectangle((x, y, x + w, y + h), radius=12, fill=bg, outline=color, width=3)
    text(d, (x + 16, y + 10), title, F_BODY, color)
    text(d, (x + 16, y + 36), big, F_BIG, INK)
    yy = y + 84
    for ln in lines:
        text(d, (x + 16, yy), ln, F_TINY, MUTED)
        yy += 22


def bar(d, x0, x1, y, segs, total):
    d.rounded_rectangle((x0, y, x1, y + 40), radius=7, fill=WHITE, outline=LINE, width=2)
    x = x0
    span = x1 - x0
    for ms, col, name in segs:
        w = max(2, int(span * ms / total))
        d.rectangle((x + 1, y + 2, x + w, y + 38), fill=col)
        if w > 110:
            text(d, (x + 8, y + 9), name, F_SMALL, WHITE)
        x += w
    return sum(s[0] for s in segs)


def fig():
    W, H = 1860, 1120
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    text(d, (56, 22), "夜間執行報告：零拷貝前後對比", F_TITLE)
    text(d, (56, 70), "真機實測（9,600 px、mock render、50 fps）；下面係今晚完成與未完成。",
         F_SUB, MUTED)

    # 前後對比
    text(d, (56, 116), "配對層：每幀成本（9,600 px）", F_SEC)
    x0, x1 = 300, 1200
    y = 168
    text(d, (56, y + 6), "之前", F_BODY, MUTED)
    before = [(2.143, RED, "清畫布 2.14"), (3.148, AMBER, "寫入 3.15"),
              (4.572, RED, "複製 4.57")]
    used_b = bar(d, x0, x1, y, before, 10.0)
    text(d, (x1 + 20, y + 4), f"{used_b:.2f} ms", F_SEC, INK)
    y += 60
    text(d, (56, y + 6), "之後", F_BODY, GREEN)
    after = [(3.257, AMBER, "寫入 3.26"), (0.008, GREEN, "commit 0.01")]
    used_a = bar(d, x0, x1, y, after, 10.0)
    text(d, (x1 + 20, y + 4), f"{used_a:.2f} ms", F_SEC, GREEN)
    text(d, (x1 + 20, y + 34), f"−{used_b - used_a:.1f} ms（−{100*(used_b-used_a)/used_b:.0f}%）",
         F_BODY, GREEN)

    # 完成卡
    y2 = 300
    text(d, (56, y2), "今晚完成（全部有測試／真機證據）", F_SEC)
    cards = [
        ("零拷貝", "−67%", ["9,600 px：9.87 → 3.27 ms", "clear=0、commit=0.01 ms",
                          "64 px：49.9 fps、apply 31 µs"], GREEN, GREEN_BG),
        ("開機預建", "269 → 7 ms", ["首次缺 1 個：built=1", "第二次：built=0",
                                "結果經 0xF079 讀回"], BLUE, BLUE_BG),
        ("抖動 4 子幀", "6.03 ms", ["2,400 px、200 Hz 子幀", "視覺 50 fps、CPU 31.5%",
                                "apply4 ＋ apply_dither"], PURPLE, (245, 243, 255)),
        ("aprog 動態", "naprog=1", ["移除固定 32", "先數 distinct 再配置",
                                 "suite 只餘既有失敗"], AMBER, AMBER_BG),
    ]
    cw, ch, gx = 430, 150, 16
    for i, c in enumerate(cards):
        card(d, 56 + i * (cw + gx), y2 + 44, cw, ch, *c)

    # 未完成
    y3 = 540
    text(d, (56, y3), "未完成（已寫入報告，等排期）", F_SEC)
    todo = [
        ("6 B 單表", "已定案、未實作"),
        ("assets 動態", "仍 item[32]（8.4 KB BSS）"),
        ("舊 plan 8 條", "production 冇用、未刪"),
        ("chunked DMA", "可選"),
    ]
    for i, (name, note) in enumerate(todo):
        x = 56 + i * 430
        d.rounded_rectangle((x, y3 + 44, x + 414, y3 + 128), radius=10, fill=GREY_BG,
                            outline=LINE, width=2)
        text(d, (x + 14, y3 + 54), name, F_BODY, INK)
        text(d, (x + 14, y3 + 88), note, F_SMALL, MUTED)

    # 注意
    y4 = 700
    text(d, (56, y4), "要你留意", F_SEC)
    d.rounded_rectangle((56, y4 + 44, 1800, y4 + 150), radius=12, fill=RED_BG, outline=RED, width=3)
    text(d, (76, y4 + 56), "① 裝置 FAT 寫入被鎖：/pixel/* 上傳回 err_busy；要斷電重插，"
                           "之後重新上傳 Zero-Show 的 /pixel。", F_BODY, INK)
    text(d, (76, y4 + 90), "② pxEngine_test／pxProgram_test 失敗係 HEAD 既有（07cbc0c），"
                           "不是今晚改動；run_all.sh 已改為強制重建。", F_BODY, INK)
    text(d, (76, y4 + 124), "③ 測試 harness 之前被舊 binary 遮住 → 才會「假全綠」。", F_BODY, INK)

    # 底部數字
    text(d, (56, H - 96), "真機數字：64 px production 49.9 fps（clear=0）；9,600 px pair 3.27 ms"
                          "（之前 9.87）；2,400 px × 4 子幀：200 Hz、50 視覺 fps、apply 6.03 ms。",
         F_SMALL, MUTED)
    text(d, (56, H - 64), "commit：e13d673、e7c8d27、f266cd8、d8ee422、58e367a（本機，未 push）。",
         F_SMALL, MUTED)
    text(d, (56, H - 32), "完整報告：docs/00_pm/night-report-2026-11-24.md", F_SMALL, INK)

    os.makedirs(OUT, exist_ok=True)
    img.save(os.path.join(OUT, "night-report.png"))


if __name__ == "__main__":
    fig()
    print("wrote", os.path.abspath(os.path.join(OUT, "night-report.png")))
