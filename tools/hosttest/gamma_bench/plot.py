#!/usr/bin/env python3
"""plot.py — 把 gamma_bench 的 CSV 畫成一張圖（有 gamma vs 冇 gamma）。

    python tools/hosttest/gamma_bench/plot.py artifacts/tmp/gamma.csv docs/00_pm/gamma-compare.png

圖有三部分：
  1. 冇 gamma：輸入 level 直接當輸出（灰階漸變）
  2. 有 gamma：同一批輸入，輸出 = gamma(level×257)>>8（暗好多）
  3. 兩條曲線疊在同一張 grid 上；128 的水平線標出「看起來一半亮」的位置
"""
from __future__ import annotations

import csv
import sys

from PIL import Image, ImageDraw, ImageFont

BG = (255, 255, 255)
INK = (32, 32, 32)
MUTED = (140, 140, 140)
GAMMA = (214, 92, 22)
ACCENT = (60, 110, 200)


def font(size: int):
    for path in (
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    ):
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    return ImageFont.load_default()


def main() -> int:
    src = sys.argv[1] if len(sys.argv) > 1 else "artifacts/tmp/gamma.csv"
    dst = sys.argv[2] if len(sys.argv) > 2 else "artifacts/tmp/gamma_compare.png"

    rows = []
    with open(src, newline="", encoding="utf-8") as fh:
        for r in csv.DictReader(fh):
            rows.append((int(r["level"]), int(r["linear8"]), int(r["gamma8"])))
    if not rows:
        raise SystemExit(f"{src} 冇資料")

    W, H = 1100, 710
    X0, X1 = 30, 1070          # 內容左右
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    f_title, f_body, f_small = font(26), font(17), font(14)

    d.text((X0, 16), "gamma table: same input, different brightness", font=f_title, fill=INK)
    d.text((X0, 50),
           "linear in = level x 257 (16-bit)  |  output = 8-bit display value",
           font=f_small, fill=MUTED)

    span = X1 - X0

    def strip(y: int, h: int, pick, title: str, note: str) -> None:
        d.text((X0, y - 24), title, font=f_body, fill=INK)
        d.text((X0 + span - 470, y - 22), note, font=f_small, fill=MUTED)
        for i in range(span):
            lv = round(i * 255 / (span - 1))
            v = pick(lv)
            d.line([(X0 + i, y), (X0 + i, y + h - 1)], fill=(v, v, v))
        d.rectangle([X0 - 1, y - 1, X1, y + h], outline=MUTED)

    by_level = {lv: (lin, gam) for lv, lin, gam in rows}
    strip(92, 74, lambda lv: by_level[lv][0], "no gamma (linear)",
          "middle of the fade = 128 (looks too bright)")
    strip(200, 74, lambda lv: by_level[lv][1], "with gamma table (x^2.2)",
          "middle of the fade = 55 (looks right)")

    # ── 曲線 ──
    PY0, PY1 = 340, 596
    d.text((X0, PY0 - 30), "output vs input", font=f_body, fill=INK)
    d.text((X0 + span - 300, PY0 - 28), "x: input level   y: output", font=f_small, fill=MUTED)

    def px(lv: int) -> int:
        return X0 + round(lv * span / 255)

    def py(v: int) -> int:
        return PY1 - round(v * (PY1 - PY0) / 255)

    for v in range(0, 256, 32):
        y = py(v)
        d.line([(X0, y), (X1, y)], fill=(232, 232, 232))
        d.text((X0 - 2, y - 16), str(v), font=f_small, fill=MUTED)
    for lv in range(0, 256, 64):
        d.text((px(lv) - 10, PY1 + 6), str(lv), font=f_small, fill=MUTED)
    d.rectangle([X0, PY0, X1, PY1], outline=MUTED)

    d.line([(px(0), py(0)), (px(255), py(255))], fill=MUTED, width=2)      # 對照（identity）
    pts = [(px(lv), py(by_level[lv][1])) for lv, _, _ in rows]
    d.line(pts, fill=GAMMA, width=3)

    half_y = py(128)
    for x in range(X0, X1, 14):
        d.line([(x, half_y), (x + 7, half_y)], fill=ACCENT, width=2)
    d.text((X1 - 200, half_y - 22), "50% output (128)", font=f_small, fill=ACCENT)

    # 50% 輸出時，輸入要去到幾多
    cross = next((lv for lv, _, gam in rows if gam >= 128), 255)
    d.line([(px(cross), half_y), (px(cross), PY1)], fill=ACCENT, width=1)
    d.text((px(cross) - 8, PY1 + 30), f"{round(cross * 100 / 255)}% of the fade",
           font=f_small, fill=ACCENT)
    d.text((X1 - 30, PY1 + 8), "255", font=f_small, fill=MUTED)

    d.text((X0, H - 26),
           "grey = no gamma (identity)     orange = with gamma table     "
           "board cost: 151 ns/value (effect-perf.md 3.2)",
           font=f_small, fill=MUTED)

    img.save(dst)
    print(f"{dst}  {img.size[0]}x{img.size[1]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
