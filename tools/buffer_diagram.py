#!/usr/bin/env python3
"""Generate docs/00_pm/buffers.svg — 核 ↔ 通道 ↔ 緩衝 的關係圖。

Single source of truth: 手寫的內容清單 + 固定幾何，輸出獨立 SVG（無外部字型／圖片）。
    python tools/buffer_diagram.py [out.svg]

圖在答三條問題
--------------
    ① 一幀資料從硬件進來之後，經過哪幾塊記憶體才會被解析？
    ② 每塊緩衝是誰的（plan / heap / PSRAM）、哪一核寫、哪一核讀？
    ③ PUSH 與 PULL 兩種通道，緩衝鏈差在哪裡（有沒有 producer、有沒有 slots）？

數值全部來自程式（nc_cfg_defaults.c 與各模組常數），改程式要重跑。
"""
import sys
from pathlib import Path

W = 1680

INK      = "#17191C"
INK2     = "#3D4247"
MUTED    = "#6E747B"
FAINT    = "#9AA0A6"
RULE     = "#DCD8CE"
PAPER    = "#FBFAF6"
WHITE    = "#FFFFFF"

# 緩衝的「來源」用顏色分（這張圖最重要的一層編碼）
F_PLAN_I = "#F3EFF8"; S_PLAN_I = "#B79BC9"   # plan INTERNAL
F_PLAN_P = "#EAF1F7"; S_PLAN_P = "#8FB0CB"   # plan PSRAM（BULK）
F_HEAP   = "#F5F4EF"; S_HEAP   = "#BEB8A8"   # runtime heap
F_DRV    = "#EDF4EF"; S_DRV    = "#94B7A2"   # 驅動／硬件 FIFO
F_NONE   = "#FAF9F5"; S_NONE   = "#DCD8CE"

C_CORE0  = "#1F5F8B"
C_CORE1  = "#A8661B"

SANS = "'Segoe UI','Noto Sans TC','Microsoft JhengHei','PingFang TC',sans-serif"
MONO = "'Cascadia Mono','Consolas','SF Mono',monospace"

out = []


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def cw(ch, fs, mono=False):
    o = ord(ch)
    if o >= 0x2E80 or ch in "，。、；：（）「」『』【】！？—…·｜":
        return fs * 1.0
    if mono:
        return fs * 0.60
    if ch == " ":
        return fs * 0.28
    if ch in "ilj|!.,:;'`[](){}":
        return fs * 0.30
    if ch in "mwMW@%":
        return fs * 0.85
    if ch.isupper() or ch.isdigit():
        return fs * 0.58
    return fs * 0.53


def measure(s, fs, mono=False):
    return sum(cw(c, fs, mono) for c in s)


def rect(x, y, w, h, fill=WHITE, stroke=RULE, sw=1, rx=6, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    out.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{rx}" '
               f'fill="{fill}" stroke="{stroke}" stroke-width="{sw}"{d}/>')


def text(x, y, s, fs=12, fill=INK, weight=400, mono=False, anchor="start"):
    fam = MONO if mono else SANS
    out.append(f'<text x="{x:.1f}" y="{y:.1f}" font-family="{fam}" font-size="{fs}" '
               f'font-weight="{weight}" fill="{fill}" text-anchor="{anchor}">{esc(s)}</text>')


def arrow(x1, y, x2, color=FAINT):
    out.append(f'<line x1="{x1:.1f}" y1="{y:.1f}" x2="{x2 - 6:.1f}" y2="{y:.1f}" '
               f'stroke="{color}" stroke-width="1.3"/>')
    out.append(f'<path d="M {x2 - 7:.1f} {y - 4:.1f} L {x2 - 1:.1f} {y:.1f} '
               f'L {x2 - 7:.1f} {y + 4:.1f}" fill="none" stroke="{color}" stroke-width="1.3"/>')


# ── 內容清單 ───────────────────────────────────────────────────────────
# 每行一條通道：(通道, 入口, 暫存, 解析環, 解碼, 出口, fill/stroke 對)
ROWS = [
    ("CDC（OTG）", C_CORE0,
     ("TinyUSB", ["CDC-ACM · USB FS", "主機寫入 OUT 端點"], F_DRV, S_DRV),
     ("CDC.slots", ["4 × 256 B = 1 KB", "heap（nc_buf，runtime）", "寫：cdc.rx（同核）"], F_HEAP, S_HEAP),
     ("usb.rx", ["4,192 B · plan INTERNAL", "STREAM，容得落 4,115 B 幀", "本通道的 parser 環"], F_PLAN_I, S_PLAN_I),
     ("decode0", ["PUSH：producer kick 叫醒", "parser 解析 → handler", "棧 3,072 B · prio 4"], F_PLAN_I, C_CORE0),
     ("回覆 + TX", ["回覆幀 2,615 B（plan BULK）", "TinyUSB TX FIFO", "s_tx_reply 仲裁：回覆優先"], F_PLAN_P, S_PLAN_P)),

    ("usb_jtag（JTAG）", C_CORE0,
     ("USB-Serial-JTAG", ["303A:1001 · 無 OTG 的板", "LL 直讀 RX FIFO"], F_DRV, S_DRV),
     ("（無暫存）", ["PULL：冇 producer task", "decode 自己讀 FIFO", "一次 256 B"], F_NONE, S_NONE),
     ("usb.rx", ["4,192 B · plan INTERNAL", "chunk 256 × budget 8", "每輪最多搬 2,048 B"], F_PLAN_I, S_PLAN_I),
     ("decode0", ["每 10 ms 輪詢一次", "parser 解析 → handler", "單核：只有這一個"], F_PLAN_I, C_CORE0),
     ("回覆 + TX", ["回覆幀 2,615 B（plan BULK）", "LL TX FIFO · 100 ms 上限", "s_jtag_tx_lock 同 log 共用"], F_PLAN_P, S_PLAN_P)),

    ("UART", C_CORE1,
     ("UART 驅動", ["921600 8N1", "driver 環 1 KB"], F_DRV, S_DRV),
     ("UART.slots", ["4 × 256 B = 1 KB", "heap（nc_buf，runtime）", "寫：uart.rx（同核）"], F_HEAP, S_HEAP),
     ("uart.rx", ["1,024 B · plan INTERNAL", "收得落 ≤1,011 B 指令", "要 4 KB 就要改 config"], F_PLAN_I, S_PLAN_I),
     ("decode1（預設）", ["PUSH，owner_init 裝 driver", "單核時同 decode0", "雙核：core1"], F_PLAN_I, C_CORE1),
     ("回覆 + TX", ["回覆幀 2,615 B（plan BULK）", "nc_uart_drv_send · TX 環 1 KB", "4 KB 轉寄阻塞 ~44 ms"], F_PLAN_P, S_PLAN_P)),

    ("TCP（P4）", C_CORE0,
     ("lwIP socket", ["乙太網／Wi-Fi", "非阻塞 recv"], F_DRV, S_DRV),
     ("（無暫存）", ["PULL：decode 直接 recv", "frame_mode 時零複製", "整幀進插槽"], F_NONE, S_NONE),
     ("tcp.rx", ["16,410 B · plan INTERNAL", "2 幀：跨讀需要餘裕", "chunk 8,192"], F_PLAN_I, S_PLAN_I),
     ("decode0（P4 預設）", ["PULL：冇 kick，每輪問一次", "parser 解析 → handler", "同 core0 的 HTTP／WS 共用"], F_PLAN_I, C_CORE0),
     ("回覆 + TX", ["回覆幀 2,615 B（plan BULK）", "tcp_nc4_send", "s_tcp_tx_lock 整幀獨佔"], F_PLAN_P, S_PLAN_P)),
]

# ── 幾何 ───────────────────────────────────────────────────────────────
L, R = 44, W - 44
GAP = 24
COLW = (R - L - GAP * 4) / 5
TOP = 156
ROWH = 108
ROWGAP = 14
PANEL_Y = TOP + (ROWH + ROWGAP) * len(ROWS) + 16
PANEL_H = 196
H = PANEL_Y + PANEL_H + 34

HDR = ["① 入口（硬件／驅動）", "② 暫存（有 producer 才有）", "③ 解析環（plan 給）",
       "④ 誰解析（核）", "⑤ 出口（回覆與 TX）"]


def cell(x, y, w, h, title, lines, fill, stroke, core_color=None):
    rect(x, y, w, h, fill, stroke, 1.2)
    if core_color:
        out.append(f'<rect x="{x}" y="{y}" width="4.5" height="{h}" rx="2" fill="{core_color}"/>')
    text(x + 14, y + 22, title, 12.5, INK, 600, mono=True)
    for i, ln in enumerate(lines[:3]):
        text(x + 14, y + 40 + i * 15, ln, 10.5, INK2 if i == 0 else MUTED)


def build():
    out.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
               f'viewBox="0 0 {W} {H}" font-family="{SANS}">')
    rect(0, 0, W, H, PAPER, PAPER, 0, 0)

    text(L, 52, "核 ↔ 通道 ↔ 緩衝：一幀資料要經過哪幾塊記憶體", 25, INK, 600)
    text(L, 78, "四條通道的鏈一樣長，但 PUSH 多一個 producer 與 slots，PULL 直接讀硬件。"
                "顏色 = 這塊緩衝從哪來。", 12.5, MUTED)

    # 圖例
    lx = L
    for lbl, f, s in [("plan INTERNAL（宣告、開機配一次）", F_PLAN_I, S_PLAN_I),
                      ("plan PSRAM／BULK（有 PSRAM 才放）", F_PLAN_P, S_PLAN_P),
                      ("heap（runtime，可丟）", F_HEAP, S_HEAP),
                      ("驅動／硬件 FIFO", F_DRV, S_DRV)]:
        rect(lx, 92, 13, 11, f, s, 1, 3)
        text(lx + 19, 102, lbl, 10.5, MUTED)
        lx += 19 + measure(lbl, 10.5) + 26

    # 欄標題
    for i, h in enumerate(HDR):
        text(L + i * (COLW + GAP), TOP - 12, h, 11.5, MUTED, 600)

    for r, (name, core_col, *cells) in enumerate(ROWS):
        y = TOP + r * (ROWH + ROWGAP)
        for c, (title, lines, f, s) in enumerate(cells):
            x = L + c * (COLW + GAP)
            if c == 0:
                # 入口格：加通道名
                rect(x, y, COLW, ROWH, f, s, 1.2)
                text(x + 14, y + 20, name, 12, INK, 700)
                text(x + 14, y + 40, title, 11, INK2, 600, mono=True)
                for i, ln in enumerate(lines[:2]):
                    text(x + 14, y + 58 + i * 15, ln, 10.5, MUTED)
            elif c == 3:
                cell(x, y, COLW, ROWH, title, lines, f, s, core_color=core_col)
            else:
                cell(x, y, COLW, ROWH, title, lines, f, s)
            # 箭頭
            if c > 0:
                arrow(x - GAP + 2, y + ROWH / 2, x - 2)

    # ── 底部：跨核 + 單核 ────────────────────────────────────────────
    rect(L, PANEL_Y, R - L, PANEL_H, WHITE)

    text(L + 16, PANEL_Y + 26, "跨核轉寄：唯一允許的跨核 notify", 12.5, INK, 600)
    y = PANEL_Y + 44
    rect(L + 16, y, 250, 54, F_PLAN_P, S_PLAN_P, 1.2)
    text(L + 28, y + 20, "decode0", 12, INK, 700)
    text(L + 28, y + 38, "解析完，目的不是本核的 link", 10.5, MUTED)
    arrow(L + 272, y + 27, L + 320)
    rect(L + 322, y, 330, 54, F_PLAN_P, S_PLAN_P, 1.2)
    text(L + 336, y + 20, "fwd.c1（PSRAM）", 12, INK, 700, mono=True)
    text(L + 336, y + 38, "4 格 × 8,221 B（16 名 + 8,205 幀）= 32,884 B", 10.5, MUTED)
    arrow(L + 658, y + 27, L + 706)
    rect(L + 708, y, 250, 54, F_PLAN_P, S_PLAN_P, 1.2)
    text(L + 722, y + 20, "decode1", 12, INK, 700)
    text(L + 722, y + 38, "每輪先清 queue，再送 link", 10.5, MUTED)
    text(L + 974, y + 22, "佇列滿就丟（來源核唔可以停）", 10.5, MUTED)
    text(L + 974, y + 40, "drops 計在 STATUS mem.fwd", 10.5, MUTED)

    text(L + 16, PANEL_Y + 122, "單核（C3／C6／H2）會縮成什麼", 12.5, INK, 600)
    text(L + 16, PANEL_Y + 142,
         "decode1 與 decoder1 唔建立；nc4.reply1 唔宣告（省 2,615 B）；全部 link 的 core 都收斂到 0；"
         "fwd.c1 永遠用不到。", 11, INK2)
    text(L + 16, PANEL_Y + 162,
         "usb.rx 仍然是 4,192 B —— 它跟核數無關，跟「主機的 chunk 有幾大」有關。", 11, MUTED)
    text(L + 16, PANEL_Y + 182,
         "heap 那兩層（slots、read chunk）在單核一樣存在：它們由「有冇 producer」決定，唔係由核數。", 11, MUTED)

    out.append("</svg>")


def main():
    build()
    dst = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("docs/00_pm/buffers.svg")
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text("\n".join(out), encoding="utf-8")
    print(f"{dst}  ({W}×{H})")


if __name__ == "__main__":
    main()
