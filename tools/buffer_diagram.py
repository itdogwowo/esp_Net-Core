#!/usr/bin/env python3
"""緩衝地圖：邊啲係「寫死」，邊啲係「掃描／設定驅動」。

    python3 tools/buffer_diagram.py            # → docs/00_pm/buffer-map.svg

數字來源：板上 `status mem`（2026-11-27、SuperMini、release）＋
`nm`（schema 表在 .flash.rodata）。要更新數字就改下面常數。
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs/00_pm/buffer-map.svg"

# ── 實測（status mem / nm）──
BSS, DATA = 104_272, 39_287
PLAN = [("nc4.reply0", 2615, "核數（1 decoder）", "psram"),
        ("usb.rx", 4128, "link 的 region（cfg）", "int"),
        ("i80.prd", 576, "px×lane（cfg）", "int"),
        ("mock.prd", 576, "測試後端", "int")]
# 2026-11-27 查清：usb0.rx 同 usb.rx 係**同一塊記憶體**（USJ parser 用 link 名再包一次），
# 唔係第二份；已用 nc_buf_find_region() 去重，帳目唔再重複計。
EXTRA: list = []
ARENA_ZERO = 0               # 冇模組 = 唔宣告（tests 映像實測）；有模組才 32 KB 起
ARENA_OLD = 512 * 1024
SCHEMA_FLASH = 12_404
STATIC = BSS + DATA
DYNAMIC = sum(b for _, b, _, _ in PLAN) + sum(b for _, b, _ in EXTRA)

INK, INK2, MUTED = "#1b2430", "#43536a", "#7c8ba1"
RULE, WHITE = "#d8dfe8", "#ffffff"
F_RED, S_RED = "#fdecea", "#c0392b"
F_BLUE, S_BLUE = "#eaf2fd", "#2f6fbf"
F_GREEN, S_GREEN = "#e9f7ef", "#1e8449"
F_AMBER, S_AMBER = "#fef7e6", "#b9770e"
F_PURPLE, S_PURPLE = "#f3edfb", "#6c3fb0"

W, H = 1680, 1000
M = 34


def esc(s: str) -> str:
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def text(x, y, s, size=11, fill=INK, weight=400, anchor="start"):
    return (f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{fill}" '
            f'font-weight="{weight}" text-anchor="{anchor}" '
            f'font-family="Inter, Noto Sans CJK TC, PingFang TC, sans-serif">{esc(s)}</text>')


def rect(x, y, w, h, fill, stroke=None, sw=1, r=10):
    st = f' stroke="{stroke}" stroke-width="{sw}"' if stroke else ""
    return f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{r}" fill="{fill}"{st}/>'


def fmt(n: int) -> str:
    return f"{n:,}"


def panel1(y: int) -> str:
    x, w, h = M, W - 2 * M, 430
    s = [rect(x, y, w, h, WHITE, RULE, 1.2, 14),
         text(x + 20, y + 30, "① 邊啲係寫死、邊啲係掃描／設定出嚟", 16, INK, 700)]
    # 三個來源欄
    cols = [
        ("編譯期寫死（.bss／.data）", STATIC, F_RED, S_RED,
         ["FreeRTOS／lwIP／Wi-Fi／驅動靜態", "固定表、小陣列、decoder 結構", "任務 stack（main 3–8 KB、ncm 8 KB）"]),
        ("設定／掃描驅動（plan ＋ nc_buf）", DYNAMIC, F_BLUE, S_BLUE,
         ["per-link rx（hardware.json）", "回覆幀（decoder 數 = 核數）", "px×lane 環（cfg）＋模組 arena（掃描）"]),
        ("flash，0 RAM（.flash.rodata）", SCHEMA_FLASH, F_GREEN, S_GREEN,
         ["schema 字典 286 key／16 unit", "路徑／描述字串", "唔佔 SRAM，XIP 直接讀"]),
    ]
    cw = (w - 40 - 2 * 16) / 3
    for i, (title, nbytes, f, st, lines) in enumerate(cols):
        cx = x + 20 + i * (cw + 16)
        cy = y + 50
        s.append(rect(cx, cy, cw, 176, f, st, 1.2, 10))
        s.append(text(cx + 14, cy + 24, title, 11.5, st, 700))
        s.append(text(cx + 14, cy + 54, fmt(nbytes) + " B", 22, INK, 700))
        for j, ln in enumerate(lines):
            s.append(text(cx + 14, cy + 82 + j * 18, "· " + ln, 9.5, INK2))

    # 堆疊條（比例）
    by = y + 250
    s.append(text(x + 20, by - 8, "靜態 vs 動態（真機實測）", 11, INK2, 700))
    total = STATIC + DYNAMIC
    bw = w - 40
    sw_ = bw * STATIC / total
    s.append(rect(x + 20, by, sw_, 34, F_RED, S_RED, 1))
    s.append(f'<rect x="{x+20:.1f}" y="{by}" width="0" height="34" fill="{F_RED}" stroke="{S_RED}">'
             f'<animate attributeName="width" from="0" to="{sw_:.1f}" dur="1.4s" fill="freeze"/></rect>')
    s.append(rect(x + 20 + sw_, by, bw - sw_, 34, F_BLUE, S_BLUE, 1))
    s.append(text(x + 30, by + 23, f"靜態 {fmt(STATIC)} B（{STATIC*100/total:.1f}%）", 11, S_RED, 700))
    s.append(text(x + w - 20, by - 8,
                  f"動態 {fmt(DYNAMIC)} B（{DYNAMIC*100/total:.1f}%）", 10.5, S_BLUE, 700, anchor="end"))
    # 結論
    s.append(rect(x + 20, by + 52, bw, 92, F_AMBER, S_AMBER, 1.1, 10))
    s.append(text(x + 34, by + 76, "答案：真正「寫死」＝ .bss／.data 143.6 KB（93%）——全部係 RTOS／網絡／驅動骨架，唔關業務事。", 11, INK, 700))
    s.append(text(x + 34, by + 98, "業務緩衝（plan 7,895 B）全部由 cfg／掃描決定；arena 由「掃到幾多模組」決定（冇模組 32 KB）。", 10.5, INK2))
    s.append(text(x + 34, by + 118, "usb.rx／usb0.rx 係同一塊（USJ parser 用 link 名再包一次）——已加 nc_buf_find_region() 去重，帳目唔再計兩次。", 10.5, INK2))
    return "".join(s)


def panel2(y: int) -> str:
    x, w, h = M, W - 2 * M, 210
    s = [rect(x, y, w, h, WHITE, RULE, 1.2, 14),
         text(x + 20, y + 30, "② 開機時序：幾時掃描、幾時先申請", 16, INK, 700)]
    steps = [
        ("B02", "讀 cfg", "hardware／system／driver\n決定 rx region、px×lane", F_BLUE, S_BLUE),
        ("B06", "掃描 .nce／.nct", "只讀 header＋manifest\n累加檔案大小 → arena", F_GREEN, S_GREEN),
        ("B06", "declare", "bytes = cfg 值／掃描量\n（唔用 MAX_* 乘）", F_GREEN, S_GREEN),
        ("B07", "plan commit", "每個 class 一次 carve\n逐項成功／失敗", F_AMBER, S_AMBER),
        ("B08", "包裝成 nc_buf", "nc_buf_at(plan 指標)\n零額外分配", F_PURPLE, S_PURPLE),
    ]
    cw, gap = 236, 24
    for i, (tag, title, sub, f, st) in enumerate(steps):
        cx = x + 20 + i * (cw + gap)
        cy = y + 52
        s.append(rect(cx, cy, cw, 116, f, st, 1.2, 10))
        s.append(text(cx + 12, cy + 22, tag, 10.5, st, 700))
        s.append(text(cx + 12, cy + 44, title, 12, INK, 700))
        for j, ln in enumerate(sub.split("\n")):
            s.append(text(cx + 12, cy + 68 + j * 16, ln, 9.5, INK2))
        if i < len(steps) - 1:
            ax = cx + cw + 4
            s.append(f'<path d="M {ax} {cy+58} h {gap-8}" stroke="{MUTED}" stroke-width="2" '
                     f'marker-end="url(#arr)"/>')
    s.append(text(x + 20, y + h - 16,
                  "重點：plan 的 bytes 喺 declare 之前就已經係「實際數字」；冇任何一步係「先要一大塊再算」。",
                  10, MUTED))
    return "".join(s)


def panel3(y: int) -> str:
    x, w, h = M, W - 2 * M, 236
    s = [rect(x, y, w, h, WHITE, RULE, 1.2, 14),
         text(x + 20, y + 30, "③ 逐項清單：邊個可以彈性、邊個唔可以", 16, INK, 700)]
    rows = [("緩衝", "bytes", "來源", "彈性"),
            (".bss／.data", fmt(STATIC), "編譯期", "✗ 固定（RTOS／lwIP／WiFi 骨架）"),
            ("nc4.reply0", "2,615", "decoder 數（核數）", "△ 單核只宣告 1 條（已省 2,615）"),
            ("usb.rx", "4,128", "link region（cfg）", "△ 改 hardware.json 的 rx.region 就變"),
            ("i80.prd", "576", "px×lane（cfg）", "△ 跟 cfg，唔會超配"),
            ("mock.prd", "576", "測試後端", "△ release 仍然存在 → 可條件編譯省 576"),
            ("ncm.arena", "0", "掃描 .nce／.nct", "✓ 0 模組 = 唔宣告（原 32 KB）；有模組 32 KB 起～512 KB"),
            ("schema 表", "12,404", "schema JSON（生成期）", "✓ flash，0 SRAM")]
    ry = y + 54
    colx = [x + 24, x + 220, x + 330, x + 560]
    for i, r in enumerate(rows):
        yy = ry + i * 20
        if i == 0:
            for j, c in enumerate(r):
                s.append(text(colx[j], yy, c, 10.5, INK, 700))
            s.append(f'<line x1="{x+20:.1f}" y1="{yy+6:.1f}" x2="{x+w-20:.1f}" y2="{yy+6:.1f}" stroke="{RULE}"/>')
            continue
        flexible = r[3].startswith("✓") or r[3].startswith("△")
        col = S_GREEN if r[3].startswith("✓") else (S_AMBER if r[3].startswith("?") else (INK2 if flexible else S_RED))
        s.append(text(colx[0], yy, r[0], 10, INK, 600))
        s.append(text(colx[1], yy, r[1], 10, INK2))
        s.append(text(colx[2], yy, r[2], 10, INK2))
        s.append(text(colx[3], yy, r[3], 10, col, 600 if flexible else 400))
    return "".join(s)


def main() -> None:
    defs = '''<defs>
  <marker id="arr" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="6" markerHeight="6" orient="auto">
    <path d="M 0 0 L 10 5 L 0 10 z" fill="#7c8ba1"/>
  </marker>
</defs>'''
    head = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">'
            f'{defs}<rect width="{W}" height="{H}" fill="#f7f9fc"/>'
            + text(M, 46, "緩衝地圖：邊啲寫死、邊啲掃描", 26, INK, 700)
            + text(M, 72, "2026-11-27 · 真機 status mem＋nm · 圖由 tools/buffer_diagram.py 生成 · "
                          "開圖有動畫：靜態／動態條生長", 11, MUTED)
            + text(W - M, 46, "esp_Net-Core", 11, MUTED, anchor="end"))
    body = panel1(96) + panel2(536) + panel3(760)
    foot = text(M, H - 12, f"靜態 {fmt(STATIC)} B ＋ 動態 {fmt(DYNAMIC)} B ＋ flash 字典 {fmt(SCHEMA_FLASH)} B"
                           f" · arena 0 模組 = {fmt(ARENA_ZERO)} B（舊 32 KB 起）", 10, MUTED)
    OUT.write_text(head + body + foot + "</svg>\n", encoding="utf-8")
    print(f"wrote {OUT.relative_to(ROOT)}（{OUT.stat().st_size} B）")


if __name__ == "__main__":
    main()
