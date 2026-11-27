#!/usr/bin/env python3
"""緩衝位置＋干擾路徑圖（統計前要知的事）。

    python3 tools/buffer_locations.py     # → docs/00_pm/buffer-locations.svg

數字來源：板上 status mem（SuperMini、release）＋ plan 宣告 class ＋ Kconfig 政策。
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs/00_pm/buffer-locations.svg"

INK, INK2, MUTED = "#1b2430", "#43536a", "#7c8ba1"
RULE, WHITE = "#d8dfe8", "#ffffff"
F_INT, S_INT = "#eaf2fd", "#2f6fbf"
F_DMA, S_DMA = "#fef7e6", "#b9770e"
F_PS, S_PS = "#e9f7ef", "#1e8449"
F_FL, S_FL = "#f3edfb", "#6c3fb0"
F_RED, S_RED = "#fdecea", "#c0392b"

W, H = 1680, 1100
M = 34


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def text(x, y, s, size=11, fill=INK, weight=400, anchor="start"):
    return (f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{fill}" '
            f'font-weight="{weight}" text-anchor="{anchor}" '
            f'font-family="Inter, Noto Sans CJK TC, PingFang TC, sans-serif">{esc(s)}</text>')


def rect(x, y, w, h, fill, stroke=None, sw=1, r=10):
    st = f' stroke="{stroke}" stroke-width="{sw}"' if stroke else ""
    return f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{r}" fill="{fill}"{st}/>'


def panel1(y):
    x, w, h = M, W - 2 * M, 516
    s = [rect(x, y, w, h, WHITE, RULE, 1.2, 14),
         text(x + 20, y + 30, "① 緩衝住喺邊：四條記憶體通道（實測）", 16, INK, 700)]
    lanes = [
        ("Internal SRAM（512 KB，CPU 快）", F_INT, S_INT,
         [("static .bss/.data", 143559, "RTOS／lwIP／WiFi／驅動"),
          ("usb.rx", 4128, "parser STREAM（plan INTERNAL）"),
          ("i80.prd 3×192", 576, "環（Kconfig 選 internal）"),
          ("mock.prd 3×192", 576, "測試後端環"),
          ("task stacks", 0, "main 8 KB／ncm 8 KB（tests）")]),
        ("DMA-capable internal（細池）", F_DMA, S_DMA,
         [("I80／SPI 後端槽", 0, "≤48 KB 才放 internal；S3 唔可以 DMA 讀 PSRAM"),
          ("（現時 0 條 plan DMA 宣告）", 0, "dma bytes=0（status mem）")]),
        ("PSRAM（2 MB，慢、要 cache）", F_PS, S_PS,
         [("nc4.reply0", 2615, "回覆 JSON 砌在這裡（每條 decoder）"),
          ("ncm.arena", 0, "有模組才 32 KB 起（現時 0）"),
          ("s_recs 暫存", 768, "state 開機對稿（一次）")]),
        ("Flash（XIP，0 RAM）", F_FL, S_FL,
         [("schema 字典", 12404, "286 key／16 unit，開機讀一次"),
          ("code＋const", 0, "XIP 直接執行；同 flash cache 爭")]),
    ]
    ly = y + 52
    for title, f, st, items in lanes:
        s.append(rect(x + 20, ly, w - 40, 104, f, st, 1.1, 10))
        s.append(text(x + 34, ly + 20, title, 11, st, 700))
        ix = x + 34
        iy = ly + 42
        for j, (nm, b, desc) in enumerate(items):
            col = ix + (j % 3) * ((w - 80) / 3)
            row = iy + (j // 3) * 34
            size = f"{b:,} B" if b else "—"
            s.append(text(col, row, f"{nm}（{size}）", 9.5, INK, 600))
            s.append(text(col, row + 12, desc, 8.5, INK2))
        ly += 112
    s.append(text(x + 20, y + h - 14,
                  "重點：環由 CPU 寫／讀；DMA 讀的是後端自己的 internal 槽 —— 所以環放 PSRAM 都唔會直接俾 DMA 讀。",
                  9.5, MUTED))
    return "".join(s)


def panel2(y):
    x, w, h = M, W - 2 * M, 250
    s = [rect(x, y, w, h, WHITE, RULE, 1.2, 14),
         text(x + 20, y + 30, "② 干擾路徑：邊個影響邊個（統計量測會遇到）", 16, INK, 700)]
    rows = [
        ("PSRAM 帶寬", "reply0（PSRAM）↔ 環（PSRAM 專案）", "兩個都係 PSRAM 讀寫 → 量 RTT／FPS 時會互相拖慢"),
        ("Internal 爭用", "parser／stack ↔ Wi-Fi／driver", "Wi-Fi 起來食 internal 大塊；pool 縮水＝cache miss 增加"),
        ("DMA 池", "I80／SPI 槽 ↔ 其他 DMA 用戶", "S3 只有 internal DMA；槽位搶唔到就逐格降級"),
        ("Flash cache", "schema／資產 JSON ↔ code fetch", "開機讀字典／資產時同 XIP 爭 cache（只影響開機）"),
        ("鎖／所有權", "nc_buf 格狀態、FAT 鎖、bus 鎖", "量測同時做 FILE／MSC，RTT 一定被拖"),
        ("量測自身", "0xF061／0xF081 命令", "多一次 PSRAM 寫＋USB 傳輸；RTT 量測要扣"),
    ]
    ry = y + 56
    for i, (a, b, c) in enumerate(rows):
        yy = ry + i * 31
        s.append(rect(x + 20, yy - 15, w - 40, 27, F_WHITE := "#f7f9fc", None, 0, 6))
        s.append(text(x + 32, yy + 4, a, 10.5, S_PS, 700))
        s.append(text(x + 190, yy + 4, b, 10.5, INK))
        s.append(text(x + 610, yy + 4, c, 9.5, INK2))
    return "".join(s)


def panel3(y):
    x, w, h = M, W - 2 * M, 170
    s = [rect(x, y, w, h, WHITE, RULE, 1.2, 14),
         text(x + 20, y + 30, "③ 統計規矩（避免量測互相污染）", 16, INK, 700)]
    rules = [
        "每次量測記錄 where（S／D／P）＋同場仲有邊個高頻寬模組（例如 Wi-Fi、MSC、效果生產者）。",
        "同一時間只量一個子系統；要比較就同一塊板、同一個 ring policy（Kconfig）＋同一個 px。",
        "PSRAM 類量測（reply／大環）要標明「有冇同時跑 pixel producer」——否則 RTT 同 FPS 唔可比。",
        "量 RTT 時扣掉 0xF061／0xF081 自身的 PSRAM 寫入與 USB 傳輸；用 min/median，唔用平均。",
    ]
    for i, r in enumerate(rules):
        s.append(text(x + 24, y + 58 + i * 24, "· " + r, 10, INK2))
    return "".join(s)


def main():
    head = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">'
            f'<defs><marker id="a" viewBox="0 0 10 10" refX="8" refY="5" markerWidth="6" markerHeight="6" orient="auto">'
            f'<path d="M 0 0 L 10 5 L 0 10 z" fill="{MUTED}"/></marker></defs>'
            f'<rect width="{W}" height="{H}" fill="#f7f9fc"/>'
            + text(M, 46, "緩衝位置圖：住邊、邊個影響邊個", 26, INK, 700)
            + text(M, 72, "2026-11-27 · status mem＋plan class＋Kconfig · 圖由 tools/buffer_locations.py 生成", 11, MUTED)
            + text(W - M, 46, "esp_Net-Core", 11, MUTED, anchor="end"))
    body = panel1(96) + panel2(632) + panel3(896)
    OUT.write_text(head + body + "</svg>\n", encoding="utf-8")
    print(f"wrote {OUT.relative_to(ROOT)}（{OUT.stat().st_size} B）")


if __name__ == "__main__":
    main()
