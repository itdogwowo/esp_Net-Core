#!/usr/bin/env python3
"""pixel_pairing_figs.py — 排版配對的說明圖（Pillow 畫，唔需要 matplotlib）。

輸出（docs/assets/pixel-pairing/）：
  buffer-policy.png   固定緩衝政策：HOT internal／BULK PSRAM／DMA
  test-rig.png        之前的測試是怎樣做的（EffectPerf 0xF069 probe）
  pairing-shapes.png  「完全隨機」是什麼、四種訪問形狀、實測用時

用法：
  /path/to/python3 tools/pixel_pairing_figs.py
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
PURPLE = (124, 58, 237)
PURPLE_BG = (245, 243, 255)
BLUE = (37, 99, 235)
RED = (220, 38, 38)
WHITE = (255, 255, 255)


def font(size):
    return ImageFont.truetype(FONT, size)


F_TITLE = font(46)
F_SUB = font(24)
F_SECTION = font(30)
F_CARD = font(25)
F_CARD_S = font(21)
F_NOTE = font(20)
F_SMALL = font(18)
F_BIG = font(34)


def new_canvas(w, h):
    img = Image.new("RGB", (w, h), BG)
    return img, ImageDraw.Draw(img)


def text(d, xy, s, f, fill=INK, anchor="la"):
    d.text(xy, s, font=f, fill=fill, anchor=anchor)


def rounded(d, box, r, fill, outline=None, width=2):
    d.rounded_rectangle(box, radius=r, fill=fill, outline=outline, width=width)


def card(d, x, y, w, h, title, sub, accent, sub2=None):
    rounded(d, (x, y, x + w, y + h), 14, WHITE, accent, 2)
    d.rectangle((x, y, x + 6, y + h), fill=accent)
    text(d, (x + 22, y + 14), title, F_CARD, INK)
    yy = y + 48
    for line in sub:
        text(d, (x + 22, yy), line, F_CARD_S, MUTED)
        yy += 27
    if sub2:
        text(d, (x + 22, y + h - 30), sub2, F_SMALL, accent)


# ═══════════════════════ 1. 緩衝政策 ═══════════════════════
def fig_buffer_policy():
    W, H = 1800, 1180
    img, d = new_canvas(W, H)
    text(d, (60, 42), "實際緩衝配置：內部靜態 < 1 KB，其他全部 PSRAM", F_TITLE)
    text(d, (60, 104), "2026-11-22 修正：之前把 3 份 aprog（各 17 KB）放 internal BSS = 51.5 KB；"
                       "已移到 PSRAM。以下係修正後的真實數字。", F_SUB, MUTED)

    y = 175
    rounded(d, (60, y, W - 60, y + 300), 18, GREEN_BG, GREEN, 3)
    text(d, (90, y + 18), "HOT  ─  internal SRAM（靜態 ~800 B ＋ 小 heap）", F_SECTION, GREEN)
    cw, ch, gap = 520, 190, 26
    x = 90
    card(d, x, y + 70, cw, ch, "wave table（每條值流）",
         ["256 × 2 B = 512 B（最多 4,096 值）", "gen 15.6 ms → 2.0 ms 的來源", "plan 變更才重建"],
         GREEN, "internal heap；OOM 退 interpreter")
    x += cw + gap
    card(d, x, y + 70, cw, ch, "指標／小狀態",
         ["layout 指標 4 B、值流 pool 指標 16 B", "effect frame 272 B", "pair 狀態 220 B"],
         GREEN, "合計 ~800 B（原本 62 KB）")
    x += cw + gap
    card(d, x, y + 70, cw, ch, "I80 DMA 緩衝",
         ["DMA 真正讀的一定係 internal", "由 render ／ drv_i80 擁有", "唔關 pair 事"],
         GREEN, "PSRAM 版只係編碼暫存")

    y = 505
    rounded(d, (60, y, W - 60, y + 330), 18, AMBER_BG, AMBER, 3)
    text(d, (90, y + 18), "BULK  ─  PSRAM（所有大表；固定政策）", F_SECTION, AMBER)
    x = 90
    card(d, x, y + 70, cw, 230, "layout（runs／jobs／aprog）",
         ["500 組：jobs 4 KB ＋ runs 8 KB", "aprog 532 B／效果", "最壞 9,600 runs = 153 KB"],
         AMBER, "pair 每幀順序讀 runs")
    x += cw + gap
    card(d, x, y + 70, cw, 230, "canvas ＋ 值流 pool ×2",
         ["canvas 9,600 px = 38.4 KB", "Σ nv × 2 B ×2（按 asset 去重）", "pixel_n=9,600 rgb → 115 KB"],
         AMBER, "effect 寫／pair 讀")
    x += cw + gap
    card(d, x, y + 70, cw, 230, "map／mode pool ＋ 雜項",
         ["map 500 組 ≈ 32 KB", "mode 500 條目 ≈ 32 KB", "asset 表 8.4 KB、0xF06B 快照 2 KB"],
         AMBER, "全部由檔案大小決定")

    y = 865
    rounded(d, (60, y, W - 60, y + 130), 18, PURPLE_BG, PURPLE, 3)
    text(d, (90, y + 18), "編譯暫存（換場一次，之後 free）", F_SECTION, PURPLE)
    text(d, (90, y + 70), "9,600 px／500 組：pool 38.4 KB ＋ tmp 38.4 ＋ counts 38.4 ＋ cursor 38.4 ＋ "
                          "entries 48 ＋ runs 153.6 ≈ 355 KB（全部 PSRAM，用完即放）。", F_CARD_S, MUTED)

    y = 1015
    text(d, (60, y), "冇 PSRAM 的板（Matrix）：值流 pool 退 internal（板子能力），其餘照樣。"
                     "production 64 px 的 h_int 實測約 102 KB——修正後應該多返 ~61 KB，仍要上板確認。",
         F_NOTE, MUTED)
    os.makedirs(OUT, exist_ok=True)
    img.save(os.path.join(OUT, "buffer-policy.png"))


# ═══════════════════════ 2. 測試方法 ═══════════════════════
def fig_test_rig():
    W, H = 1800, 1020
    img, d = new_canvas(W, H)
    text(d, (60, 42), "之前的測試是怎樣做的：EffectPerf 0xF069「純 CPU probe」", F_TITLE)
    text(d, (60, 104), "同一份 C 實作、同一顆 ESP32-S3 240 MHz；但唔初始化 I80、唔送燈，"
                       "所以只量 effect→pair→12→8 的 CPU／記憶體，唔包 render。", F_SUB, MUTED)

    boxes = [
        (60, 200, 470, 190, "① PC 端", BLUE,
         ["tools/pipe_bench.py", "--px 9600 --frames 2 --mode rgb", "CDC 直連 或 WebMaster REST"]),
        (600, 200, 470, 190, "② EffectPerf 映像", AMBER,
         ["CONFIG_NETCORE_EFFECT_BENCH", "只有 effect_bench component", "冇 network／ncm／0xF060–64"]),
        (1140, 200, 600, 190, "③ 0xF069 handler", PURPLE,
         ["allocate：vals 57.6 KB / c8 38.4 KB", "c12 76.8 KB / tables", "跑三階段並用 esp_timer 計時"]),
    ]
    for (x, y, w, h, t, c, lines) in boxes:
        rounded(d, (x, y, x + w, y + h), 16, WHITE, c, 3)
        text(d, (x + 24, y + 16), t, F_CARD, c)
        yy = y + 56
        for ln in lines:
            text(d, (x + 24, yy), ln, F_CARD_S, MUTED)
            yy += 30
    # arrows
    for (x1, x2, y) in [(530, 600, 295), (1070, 1140, 295)]:
        d.line((x1, y, x2 - 12, y), fill=LINE, width=4)
        d.polygon([(x2, y), (x2 - 14, y - 9), (x2 - 14, y + 9)], fill=LINE)

    # stages
    y = 470
    text(d, (60, y), "三個階段（同 production 同一份 function）", F_SECTION)
    st = [
        (60, "gen", "nc_px_aprog_vals()", ["interpreter：15.6 ms", "wave table：2.0 ms"], GREEN),
        (620, "pair", "nc_px_layout_apply / pair_apply12", ["range／table／random／inverse／block", "8-bit／12-bit、internal／PSRAM"], AMBER),
        (1180, "conv", "nc_px_canvas12_to_8()", ["12→8 捨入 3.4 ms", "（抖動更貴：4.7 ms）"], PURPLE),
    ]
    for (x, name, fn, lines, c) in st:
        rounded(d, (x, y + 44, x + 520, y + 230), 16, WHITE, c, 3)
        text(d, (x + 24, y + 60), name, F_BIG, c)
        text(d, (x + 24, y + 108), fn, F_CARD_S, INK)
        yy = y + 144
        for ln in lines:
            text(d, (x + 24, yy), ln, F_CARD_S, MUTED)
            yy += 28

    # caveats
    y = 760
    rounded(d, (60, y, W - 60, y + 200), 16, (255, 247, 237), RED, 3)
    text(d, (90, y + 18), "測試數字的邊界（唔好當成出片 fps）", F_CARD, RED)
    text(d, (90, y + 62), "• 唔包 render：9,600 顆單 lane 物理 288 ms/幀；16 lane × 600 px 才是 18 ms/幀（≈55 fps）。", F_CARD_S, MUTED)
    text(d, (90, y + 98), "• 數字是 3 次 run 取穩定值；px=9600、nv=28800（rgb 3 值/LED）、"
                          "table_rnd[k] = (k × 7919) % 9600（近似隨機 permutation）。", F_CARD_S, MUTED)
    text(d, (90, y + 134), "• 「內部 / PSRAM」用 esp_ptr_internal()／esp_ptr_external_ram() 證實，"
                           "唔係靠推測。", F_CARD_S, MUTED)
    os.makedirs(OUT, exist_ok=True)
    img.save(os.path.join(OUT, "test-rig.png"))


# ═══════════════════════ 3. 完全隨機＋用時 ═══════════════════════
def fig_pairing_shapes():
    W, H = 1800, 1160
    img, d = new_canvas(W, H)
    text(d, (60, 42), "「完全隨機」是什麼？四種訪問形狀＋實測用時（9,600 px RGB）", F_TITLE)
    text(d, (60, 104), "同樣寫 9,600 顆，差別只在「寫入次序」與「來源讀取次序」——"
                       "PSRAM 對次序極敏感。", F_SUB, MUTED)

    def grid(x, y, w, top_label, bot_label, top_vals, bot_vals, bot_color=AMBER):
        n = len(top_vals)
        label_w = 64
        cw = (w - label_w) // n
        text(d, (x, y + 8), top_label, F_SMALL, MUTED)
        for i, v in enumerate(top_vals):
            d.rectangle((x + label_w + i * cw + 2, y, x + label_w + (i + 1) * cw - 4, y + 38),
                        fill=(226, 232, 240), outline=LINE)
            text(d, (x + label_w + i * cw + 8, y + 8), str(v), F_SMALL, MUTED)
        text(d, (x, y + 70), bot_label, F_SMALL, bot_color)
        for j, v in enumerate(bot_vals):
            d.rectangle((x + label_w + j * cw + 2, y + 62, x + label_w + (j + 1) * cw - 4, y + 100),
                        fill=(254, 243, 199), outline=bot_color)
            text(d, (x + label_w + j * cw + 8, y + 70), str(v), F_SMALL, bot_color)

    seq = list(range(8))
    land = [5, 0, 3, 6, 1, 4, 7, 2]          # 寫入 k 的落點（亂跳）
    inv = [0] * 8
    for k, v in enumerate(land):
        inv[v] = k                            # dst → src

    def shape(x, y, w, title, desc, color, draw_body):
        rounded(d, (x, y, x + w, y + 250), 16, WHITE, color, 3)
        text(d, (x + 20, y + 14), title, F_CARD, color)
        text(d, (x + 20, y + 50), desc[0], F_CARD_S, MUTED)
        text(d, (x + 20, y + 78), desc[1], F_CARD_S, MUTED)
        draw_body(x + 20, y + 118, w - 40)

    def writes(order, shuffled_blocks=False):
        def body(x, y, w):
            top = [f"k{i}" for i in range(8)] if False else seq
            grid(x, y, w, "k", "dst", top, order)
        return body

    def by_dst():
        def body(x, y, w):
            grid(x, y, w, "dst", "src", seq, inv)
        return body

    shape(60, 170, 400, "range（連續）",
          ["src 0..n → dst 0..n", "pair 4.90 ms（12-bit PSRAM）"], GREEN, writes(seq))
    shape(480, 170, 400, "完全隨機（落點亂跳）",
          ["寫入 k 的 dst 由 random table 決定", "pair 19.97 ms（12-bit PSRAM）"], RED, writes(land))
    shape(900, 170, 400, "inverse（dst 升序）",
          ["寫入按 dst 0..n 順序", "來源隨機讀 → 1.87 ms"], AMBER, by_dst())
    shape(1320, 170, 420, "block（16 px 一段）",
          ["段內連續、段次序隨機", "1.86 ms（≈ 500 組的真實形狀）"], BLUE,
          writes([2, 0, 3, 1, 6, 4, 7, 5]))

    # bars
    y0 = 470
    text(d, (60, y0), "實測用時（ESP32-S3 240 MHz，EffectPerf 0xF069，3 次穩定）", F_SECTION)
    bars = [
        ("random12（dst 亂跳）", 19.97, RED, "PSRAM 隨機寫"),
        ("packed12（2×32-bit store）", 20.60, RED, "同 PSRAM"),
        ("random8（canvas PSRAM）", 10.65, AMBER, "固定 PSRAM 政策"),
        ("range12（連續）", 4.90, GREEN, "對照組"),
        ("inverse8（dst 升序）", 1.87, BLUE, "production 現在的形狀"),
        ("block8（16 px 段）", 1.86, BLUE, "500 組 proxy"),
        ("random8（canvas internal）", 0.97, GREEN, "internal 對照"),
    ]
    x0, bw = 60, 780
    maxv = 21.0
    yy = y0 + 60
    for (name, v, color, note) in bars:
        text(d, (x0, yy + 2), name, F_CARD_S, INK)
        d.rounded_rectangle((x0 + 420, yy + 2, x0 + 420 + int(bw * v / maxv), yy + 34), 6, fill=color)
        vx = x0 + 430 + int(bw * v / maxv)
        text(d, (vx, yy + 6), f"{v:.2f} ms", F_CARD_S, color)
        text(d, (1420, yy + 8), note, F_SMALL, MUTED)
        yy += 52

    # full summary
    rounded(d, (60, 910, W - 60, 1100), 16, (239, 246, 255), BLUE, 3)
    text(d, (90, 926), "整條 CPU（gen + pair + conv，唔包 render）", F_CARD, BLUE)
    text(d, (90, 972), "無 wave table、12-bit random：37.86 ms／幀（26 fps）", F_CARD_S, MUTED)
    text(d, (90, 1008), "wave table ★：gen 15.6 → 2.0 ms；random8 internal：full 16.57 → 4.41 ms", F_CARD_S, MUTED)
    text(d, (90, 1044), "固定 PSRAM 之後：pair 唔可以 random write；dst 升序（inverse／block）→ full 4.59 ms。", F_CARD_S, MUTED)
    os.makedirs(OUT, exist_ok=True)
    img.save(os.path.join(OUT, "pairing-shapes.png"))


if __name__ == "__main__":
    fig_buffer_policy()
    fig_test_rig()
    fig_pairing_shapes()
    print("wrote", os.path.abspath(OUT))
