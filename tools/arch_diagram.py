#!/usr/bin/env python3
"""Generate docs/00_pm/architecture.svg — esp_Net-Core 抽象架構圖。

Single source of truth: 手寫幾何 + 內容清單，輸出獨立 SVG（無外部字型／圖片）。
    python tools/arch_diagram.py [out.svg]

行內標記：`code` = 等寬；**bold** = 粗體。
"""
import sys
from pathlib import Path

W, H = 1720, 1510

# ── palette ─────────────────────────────────────────────────────────────
INK      = "#17191C"
INK2     = "#3D4247"
MUTED    = "#6E747B"
FAINT    = "#9AA0A6"
RULE     = "#DCD8CE"
PAPER    = "#FBFAF6"
WHITE    = "#FFFFFF"

C_ASM    = "#A8661B"   # 組裝／任務
C_NC4    = "#1F5F8B"   # NC4 協議
C_BUF    = "#7A4E8C"   # 緩衝
C_LINK   = "#2E6B4F"   # 線路／驅動
C_HW     = "#5A6169"   # 硬體

SANS = "'Segoe UI','Noto Sans TC','Microsoft JhengHei','PingFang TC',sans-serif"
MONO = "'Cascadia Mono','Consolas','SF Mono',monospace"

out = []


def esc(s):
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


# ── text measuring / wrapping ───────────────────────────────────────────
def cw(ch, fs, mono=False):
    o = ord(ch)
    if o >= 0x2E80 or ch in "，。、；：（）「」『』【】！？—…·":
        return fs * (1.0 if not mono else 1.0)
    if mono:
        return fs * 0.6
    if ch in " ":
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


def parse(s):
    """`code` → mono, **bold** → bold. Returns tokens [(text, kind)]."""
    toks, i, buf, kind = [], 0, "", "sans"
    while i < len(s):
        if s.startswith("**", i):
            if buf:
                toks.append((buf, kind))
            buf, kind = "", ("bold" if kind != "bold" else "sans")
            i += 2
            continue
        if s[i] == "`":
            if buf:
                toks.append((buf, kind))
            buf = ""
            j = s.index("`", i + 1)
            toks.append((s[i + 1:j], "mono"))
            i = j + 1
            continue
        buf += s[i]
        i += 1
    if buf:
        toks.append((buf, kind))
    return toks


def wrap(tokens, fs, maxw, mono_fs=None):
    """Greedy wrap: CJK 逐字可斷，英文以空白斷詞，code 整段優先不斷。"""
    units = []  # (text, kind, width, breakable_before)
    for text, kind in tokens:
        f = mono_fs if (kind == "mono" and mono_fs) else fs
        if kind == "mono":
            units.append((text, kind, measure(text, f, True), True))
            continue
        word = ""
        for ch in text:
            if ch in " \t":
                if word:
                    units.append((word, kind, measure(word, f), True))
                    word = ""
                units.append((" ", kind, measure(" ", f), True))
                continue
            if ord(ch) >= 0x2E80:
                if word:
                    units.append((word, kind, measure(word, f), True))
                    word = ""
                units.append((ch, kind, measure(ch, f), True))
                continue
            word += ch
        if word:
            units.append((word, kind, measure(word, f), True))

    lines, cur, curw = [], [], 0.0
    for u in units:
        text, kind, wid, _ = u
        if text == " " and not cur:
            continue
        if curw + wid > maxw and cur:
            lines.append(cur)
            cur, curw = [], 0.0
            if text == " ":
                continue
        cur.append(u)
        curw += wid
    if cur:
        lines.append(cur)
    return lines


def text_el(x, y, lines, fs, fill=INK, lh=None, anchor="start", mono_fs=None,
            weight="normal", spacing=0):
    lh = lh or fs * 1.55
    s = [f'<text x="{x}" y="{y}" font-family="{SANS}" font-size="{fs}" '
         f'fill="{fill}" text-anchor="{anchor}"']
    if weight != "normal":
        s.append(f' font-weight="{weight}"')
    if spacing:
        s.append(f' letter-spacing="{spacing}"')
    s.append(">")
    for i, ln in enumerate(lines):
        s.append(f'<tspan x="{x}" dy="{0 if i == 0 else lh}">')
        for txt, kind, _w, _b in ln:
            if kind == "mono":
                f = mono_fs or fs - 0.5
                s.append(f'<tspan font-family="{MONO}" font-size="{f}">{esc(txt)}</tspan>')
            elif kind == "bold":
                s.append(f'<tspan font-weight="600">{esc(txt)}</tspan>')
            else:
                s.append(esc(txt))
        s.append("</tspan>")
    s.append("</text>")
    return "".join(s)


def para(x, y, text, fs, maxw, fill=INK2, lh=None, mono_fs=None, weight="normal"):
    lines = wrap(parse(text), fs, maxw, mono_fs)
    return text_el(x, y, lines, fs, fill, lh, mono_fs=mono_fs, weight=weight), lines


def rect(x, y, w, h, fill="none", stroke=RULE, rx=3, sw=1, op=1.0, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    return (f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" '
            f'fill="{fill}" fill-opacity="{op}" stroke="{stroke}" stroke-width="{sw}"{d}/>')


def line(x1, y1, x2, y2, stroke=RULE, sw=1, dash=None, op=1.0):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    return (f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{stroke}" '
            f'stroke-width="{sw}" stroke-opacity="{op}"{d}/>')


def circle_num(cx, cy, n, color=C_NC4, fs=10):
    return (f'<circle cx="{cx}" cy="{cy}" r="9.5" fill="{color}" fill-opacity="0.10" '
            f'stroke="{color}" stroke-width="1"/>'
            f'<text x="{cx}" y="{cy+3.4}" font-family="{MONO}" font-size="{fs}" '
            f'fill="{color}" text-anchor="middle">{n}</text>')


MARKERS = []


def arrow(x1, y1, x2, y2, color=C_NC4, sw=1.4, dash=None, head=True):
    mid = f"am{len(MARKERS)}"
    if head:
        MARKERS.append(
            f'<marker id="{mid}" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" '
            f'markerHeight="7" orient="auto-start-reverse">'
            f'<path d="M 0 0 L 10 5 L 0 10 z" fill="{color}"/></marker>')
    m = f' marker-end="url(#{mid})"' if head else ""
    d = f' stroke-dasharray="{dash}"' if dash else ""
    return (f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{color}" '
            f'stroke-width="{sw}" stroke-linecap="round"{m}{d}/>')


def panel(x, y, w, h, kicker, title, note=None):
    s = [rect(x, y, w, h, fill=WHITE, stroke=RULE, rx=4, op=0.75)]
    s.append(f'<text x="{x+22}" y="{y+24}" font-family="{MONO}" font-size="10.5" '
             f'fill="{FAINT}" letter-spacing="1.6">{esc(kicker)}</text>')
    s.append(f'<text x="{x+22}" y="{y+47}" font-family="{SANS}" font-size="16.5" '
             f'font-weight="600" fill="{INK}">{esc(title)}</text>')
    if note:
        s.append(f'<text x="{x+22+measure(title,16.5)*1.02+14}" y="{y+47}" '
                 f'font-family="{SANS}" font-size="12" fill="{MUTED}">{esc(note)}</text>')
    s.append(line(x + 22, y + 60, x + w - 22, y + 60, RULE, 1))
    return "".join(s)


# ════════════════════════════════════════════════════════════════════════
# canvas
out.append(f'<rect width="{W}" height="{H}" fill="{PAPER}"/>')

# header
out.append(f'<text x="44" y="66" font-family="{SANS}" font-size="33" font-weight="600" '
           f'fill="{INK}">esp_Net-Core — 現在的抽象架構</text>')
sub = ("一幀 NC4 由線路進來、到回覆出去，中間只經過三樣東西：緩衝、解碼、路由。"
       "其餘全部是「誰擁有這條線」的邊界。")
out.append(f'<text x="44" y="97" font-family="{SANS}" font-size="13.5" fill="{MUTED}">'
           f'{esc(sub)}</text>')
out.append(f'<text x="1676" y="52" font-family="{MONO}" font-size="11" fill="{FAINT}" '
           f'text-anchor="end">v0.1.2 · 2026-11-11</text>')
out.append(f'<text x="1676" y="70" font-family="{SANS}" font-size="11.5" fill="{FAINT}" '
           f'text-anchor="end">S3 已上板　·　P4 只有 ETH</text>')
out.append(line(44, 112, 1676, 112, RULE, 1))

# ════════════════════════════════════════════════════════════════════════
# PANEL 1 — 分層
PX, PY, PW, PH = 44, 132, 760, 772
out.append(panel(PX, PY, PW, PH, "01 / 靜態結構", "分層：七層，依賴只准向下"))

LAYERS = [
    ("L6", "組裝", C_ASM, "firmware/main",
     "`main.c` 決定「誰用哪條線」；11 個開機階段 ph_boot → ph_account",
     "腳位衝突、通道組裝、政策 —— 全部留在這一層，driver 不管"),
    ("L5", "任務／動作", C_ASM, "sys/task · components/action",
     "`nc_task` 管理器：SYSTEM／NORMAL／EFFECT、8 個 layer、20 格",
     "decode0／decode1 只是它的兩個 loop；長跑 loop 要 `nc_task_beat()`"),
    ("L4", "NC4 協議核心", C_NC4, "sys/nc4",
     "decoder 48 格 handler 表 ＋ 每條 bus 一個 parser（黏包狀態各自獨立）",
     "router：routes[].in → decodeN；out 可指 decodeN 或另一條 link（轉寄）"),
    ("L3", "緩衝", C_BUF, "sys/mem/nc_buf",
     "只有一個工具：FIFO／STREAM，宣告「格 × 格數」；mem.buf 14 條 · 643 KB",
     "開機要齊、永不 free、只管 carve —— 冇 free 就冇碎片"),
    ("L2", "線路", C_LINK, "bus/bus_core ＋ adapters",
     "`nc_bus_read`／`write`／`ioctl` ＋ `nc_bus_native()` 逃生門",
     "bus_uart ｜ bus_net：TCP／UDP／WS、httpd、網絡 OTA"),
    ("L1", "驅動", C_LINK, "driver/drv_*",
     "`drv_uart`（install／read／write／send）、`drv_pwm`（LEDC）",
     "USB／SD／ETH 仍然在 main.c，未抽（CDC 熱路徑最後做）"),
    ("L0", "硬體", C_HW, "ESP32-S3 / ESP32-P4",
     "WiFi／ETH／USB-CDC／UART／RS485／PSRAM",
     "P4 冇內建 Wi-Fi —— 同一份 firmware，只有 port 不同"),
]

by = PY + 70
bh, gap = 86, 9
for tag, name, color, path, l1, l2 in LAYERS:
    out.append(rect(PX + 16, by, PW - 32, bh, fill=color, op=0.050, stroke=color,
                    sw=1, rx=3))
    out.append(rect(PX + 16, by, 3.5, bh, fill=color, stroke=color, rx=1, sw=0))
    out.append(f'<text x="{PX+34}" y="{by+26}" font-family="{MONO}" font-size="11" '
               f'fill="{color}" letter-spacing="0.6">{tag}</text>')
    out.append(f'<text x="{PX+34}" y="{by+50}" font-family="{SANS}" font-size="14.5" '
               f'font-weight="600" fill="{INK}">{esc(name)}</text>')
    out.append(f'<text x="{PX+34}" y="{by+68}" font-family="{MONO}" font-size="9.5" '
               f'fill="{FAINT}">{esc(path)}</text>')
    out.append(line(PX + 170, by + 12, PX + 170, by + bh - 12, color, 1, op=0.35))
    inner = PW - 32 - 170 + 16 - 34
    a, _ = para(PX + 186, by + 29, l1, 12.4, inner - 20, INK2)
    b, _ = para(PX + 186, by + 52, l2, 12.2, inner - 20, MUTED)
    out.append(a + b)
    by += bh + gap

out.append(f'<text x="{PX+22}" y="{by+22}" font-family="{SANS}" font-size="11.5" '
           f'fill="{MUTED}">規則：每一層只 include 下面那層的 header；'
           f'由上而下的呼叫是單向的，回呼只有 callback 一種。</text>')

# ════════════════════════════════════════════════════════════════════════
# PANEL 2 — 一幀的旅程
PX2, PY2, PW2, PH2 = 828, 132, 848, 512
out.append(panel(PX2, PY2, PW2, PH2, "02 / 動態路徑", "一幀的旅程：五個站，全部通道共用"))

out.append(f'<text x="{PX2+22}" y="{PY2+82}" font-family="{SANS}" font-size="11" '
           f'font-weight="600" fill="{C_LINK}" letter-spacing="0.4">收 — 由線路到 handler</text>')

STATIONS = [
    ("1", "線路 adapter", C_LINK, ["`read()`／`recv()`", "driver 或 socket", "只答「怎麼讀」"]),
    ("2", "producer", C_ASM, ["prio 7 · 每線一條", "`push_begin()` 拿格", "recv 直接入 slot"]),
    ("3", "PUSH slots", C_BUF, ["4 格 × 2 KB（net）", "256 B（CDC／UART）", "滿 → `vTaskDelay(1)`"]),
    ("4", "decode loop", C_NC4, ["prio 8 · 每核一個", "`kick` 同核叫醒", "parser 逐幀黏包"]),
    ("5", "decoder", C_NC4, ["48 格 handler 表", "STATUS／action", "route 決策"]),
]
sx, sy, sw_, sh, sgap = PX2 + 22, PY2 + 96, 132, 96, 30
for i, (num, title, color, lines) in enumerate(STATIONS):
    x = sx + i * (sw_ + sgap)
    out.append(rect(x, sy, sw_, sh, fill=color, op=0.045, stroke=color, sw=1))
    out.append(circle_num(x + 15, sy + 16, num, color))
    out.append(f'<text x="{x+30}" y="{sy+20}" font-family="{SANS}" font-size="12.6" '
               f'font-weight="600" fill="{INK}">{esc(title)}</text>')
    for j, t in enumerate(lines):
        ell, _ = para(x + 12, sy + 42 + j * 17, t, 10.6, sw_ - 24, MUTED, lh=13)
        out.append(ell)
    if i < len(STATIONS) - 1:
        out.append(arrow(x + sw_ + 4, sy + sh / 2, x + sw_ + sgap - 4, sy + sh / 2,
                         C_LINK, 1.3))

# lane B
ly = sy + sh + 46
out.append(f'<text x="{PX2+22}" y="{ly-14}" font-family="{SANS}" font-size="11" '
           f'font-weight="600" fill="{C_ASM}" letter-spacing="0.4">回 — handler 之後只有兩個出口</text>')

# split connector: station 5 → both outlets
rx5 = sx + 4 * (sw_ + sgap) + sw_ / 2
bus_y = ly - 20
out.append(line(rx5, sy + sh, rx5, bus_y, C_NC4, 1, dash="3 3", op=0.55))
out.append(line(PX2 + 22 + 194, bus_y, rx5, bus_y, C_NC4, 1, dash="3 3", op=0.55))
for cx in (PX2 + 22 + 194, PX2 + 430 + 198):
    out.append(arrow(cx, bus_y, cx, ly - 2, C_NC4, 1.1, dash="3 3"))

# outlet 1
out.append(rect(PX2 + 22, ly, 388, 76, fill=C_NC4, op=0.04, stroke=RULE, sw=1))
out.append(f'<text x="{PX2+38}" y="{ly+22}" font-family="{SANS}" font-size="12.2" '
           f'font-weight="600" fill="{INK}">① 回原線</text>')
out.append(para(PX2 + 38, ly + 41,
                "handler 寫入 `nc4.replyN`（2,615 B ×2，per core）→ "
                "`send()` 由同一條線送出", 11.2, 356, INK2, lh=15)[0])

# outlet 2
out.append(rect(PX2 + 430, ly, 396, 76, fill=C_BUF, op=0.04, stroke=RULE, sw=1))
out.append(f'<text x="{PX2+446}" y="{ly+22}" font-family="{SANS}" font-size="12.2" '
           f'font-weight="600" fill="{INK}">② 轉寄去另一條線</text>')
out.append(para(PX2 + 446, ly + 41,
                "`routes[].out` → `nc_buf` FIFO（per 目的 × 來源核）→ "
                "`nc4_bus_kick_core()` → 目的核 send()", 11.2, 366, INK2, lh=15)[0])

# notes
ny = ly + 104
out.append(rect(PX2 + 22, ny, PW2 - 44, 1, fill=RULE, stroke="none", sw=0))
notes = [
    ("一條線就是一條線　", "TCP／UDP／WS 同 UART／CDC 走同一個 parser、同一組動詞，"
                     "冇「網絡專用」的分支。"),
    ("零複製　", "recv 直接入格；`copied` 18 B／幀（以前 36）。"
             "PUSH = 有資料才拿格，不會揸住格等 blocking read。"),
]
for k, (lead, body) in enumerate(notes):
    out.append(f'<text x="{PX2+22}" y="{ny+26+k*24}" font-family="{SANS}" font-size="11.4">'
               f'<tspan font-weight="600" fill="{INK}">{esc(lead)}</tspan>'
               f'<tspan fill="{MUTED}">{esc(body)}</tspan></text>')

# 載體 chips
cy = ny + 74
out.append(f'<text x="{PX2+22}" y="{cy}" font-family="{SANS}" font-size="10.6" '
           f'fill="{FAINT}" letter-spacing="0.3">同一套格，五種載體（0.1.2 mem.buf 實測）</text>')
CHIPS = [("CDC", "256 B × 4"), ("UART", "256 B × 4"), ("TCP", "2 KB × 4"),
         ("UDP", "2 KB × 4"), ("WS", "24 KB × 2")]
chx, chw, chg = PX2 + 22, 150, 10
for i, (name, spec) in enumerate(CHIPS):
    x = chx + i * (chw + chg)
    out.append(rect(x, cy + 8, chw, 40, fill=WHITE, stroke=RULE, sw=1, rx=3))
    out.append(f'<text x="{x+11}" y="{cy+24}" font-family="{MONO}" font-size="10.5" '
               f'fill="{C_BUF}" font-weight="600">{name}</text>')
    out.append(f'<text x="{x+11}" y="{cy+39}" font-family="{MONO}" font-size="9.8" '
               f'fill="{MUTED}">{spec}</text>')

# ════════════════════════════════════════════════════════════════════════
# PANEL 3 — 排程骨架
PX3, PY3, PW3, PH3 = 828, 664, 848, 240
out.append(panel(PX3, PY3, PW3, PH3, "03 / 排程", "雙核與優先級：decode 一定高過 producer"))

LAD = [
    ("24／22", "ipc1、esp_timer（IDF）", FAINT, False),
    ("15", "NC 核心保留區上界（NC_PRIO_BAND_MAX）", MUTED, False),
    ("8", "decode0／decode1 —— 整個系統的節拍", C_NC4, True),
    ("7", "bus producer：cdc.rx／uart.rx／net.rx", C_ASM, True),
    ("6／5", "其他任務上限、TinyUSB（IDF）", MUTED, False),
    ("2–0", "fx／nc_log／app_main／IDLE", FAINT, False),
]
ry = PY3 + 72
for num, label, color, hi in LAD:
    if hi:
        out.append(rect(PX3 + 20, ry - 12, 396, 21, fill=color, op=0.07, stroke="none"))
    out.append(f'<text x="{PX3+380}" y="{ry+3}" font-family="{MONO}" font-size="10.5" '
               f'fill="{color}" text-anchor="end">{num}</text>')
    out.append(f'<text x="{PX3+30}" y="{ry+3}" font-family="{SANS}" font-size="11.2" '
               f'fill="{INK2 if hi else MUTED}">{esc(label)}</text>')
    ry += 22

out.append(f'<text x="{PX3+22}" y="{ry+22}" font-family="{SANS}" font-size="10.4" '
           f'fill="{FAINT}">心跳：長跑 loop 每輪 nc_task_beat() 自報。</text>')

out.append(line(PX3 + 434, PY3 + 66, PX3 + 434, PY3 + PH3 - 20, RULE, 1, op=0.9))
RX = PX3 + 456
dual = [
    ("兩個 decode loop", "decode0＝core0、decode1＝core1；各有一把 decoder 與回覆幀。"),
    ("叫醒永遠同核", "producer 同 decode 綁同一個核 → 只是 notify，不經 core0 tick。"
                 "跨核只有轉寄時用 `kick_core()`。"),
    ("倒過來會出事", "實測 decode < producer：接手 478 µs、插槽被讀爆（slot_full +122）；"
                 "調正之後 62 µs、slot_full 0。"),
]
for k, (lead, body) in enumerate(dual):
    yy = PY3 + 72 + k * 55
    out.append(f'<text x="{RX}" y="{yy}" font-family="{SANS}" font-size="11.6" '
               f'font-weight="600" fill="{INK}">{esc(lead)}</text>')
    out.append(para(RX, yy + 16, body, 10.9, PW3 - 456 - 26, MUTED, lh=13.6)[0])

# ════════════════════════════════════════════════════════════════════════
# PANEL 4 — 六條界線
PX4, PY4, PW4, PH4 = 44, 924, 1632, 186
out.append(panel(PX4, PY4, PW4, PH4, "04 / 契約", "六條界線：架構是靠「不准知道什麼」撐住的"))

LINES = [
    ("driver 唔知道 NC4", "只回答「這條線怎麼讀、怎麼寫」。誰用、幾時用、腳位撞不撞，全部是組裝層的事。"),
    ("bus 唔知道 NC4", "`nc4_line_net` 與 LED／LCD 一樣只是用家；NC4 是幀協議，唔綁網絡協議。"),
    ("一個資源一個 owner", "`nc_hw` 只有目錄、command queue、snapshot；真正寫 GPIO 的只有 owner task。"),
    ("一個緩衝工具", "plan／pool／hub／環 四個名收成 `nc_buf`。冇 free 就冇碎片，帳只有一份。"),
    ("量測值不進 config", "DMA 大小、read_chunk、slot 數是量出來的，寫死在 firmware；config 只放部署差異。"),
    ("補值，不改值", "firmware 只補用戶看不到的東西（cID、內建任務、新 route），舊檔留 .bak，排版不動。"),
]
cw_, ch_, cgx, cgy = 520, 56, 16, 10
for i, (title, body) in enumerate(LINES):
    col, row = i % 3, i // 3
    x = PX4 + 22 + col * (cw_ + cgx)
    y = PY4 + 74 + row * (ch_ + cgy)
    out.append(f'<text x="{x}" y="{y+15}" font-family="{MONO}" font-size="9.5" '
               f'fill="{FAINT}">{i+1:02d}</text>')
    out.append(f'<text x="{x+22}" y="{y+15}" font-family="{SANS}" font-size="12.2" '
               f'font-weight="600" fill="{INK}">{esc(title)}</text>')
    out.append(para(x + 22, y + 34, body, 10.8, cw_ - 26, MUTED, lh=13)[0])

# ════════════════════════════════════════════════════════════════════════
# PANEL 5 — 啟動次序
PX5, PY5, PW5, PH5 = 44, 1130, 1632, 300
out.append(panel(PX5, PY5, PW5, PH5, "05 / 初始化次序",
                 "啟動次序：app_main 只有一張表，順序就是資料"))

STEPS = [
    ("banner+nvs", C_ASM, "NVS 先；log hook 最早"),
    ("storage+cfg", C_ASM, "掛 FAT；wiring 做 base"),
    ("hwcheck", C_HW, "腳位 claim＋驗，不 init"),
    ("hardware", C_HW, "PWM／ETH；router 定核"),
    ("pool", C_BUF, "已停用（nc_pool 退役）"),
    ("declare", C_BUF, "每核 decoder；任務登記"),
    ("commit", C_BUF, "carve；Wi-Fi＋net bus"),
    ("start", C_NC4, "on_start；掃 .fxe"),
    ("usb", C_NC4, "TinyUSB → FAT 交 host"),
    ("channels", C_NC4, "開 socket、建 bus、boot"),
    ("account", C_NC4, "印剩餘；之後只剩心跳"),
]
bx0, bw, bg, bh = PX5 + 22, 134, 9, 72
by0 = PY5 + 96
for i, (name, color, text) in enumerate(STEPS):
    x = bx0 + i * (bw + bg)
    dim = name == "pool"
    out.append(rect(x, by0, bw, bh, fill=color, op=0.035 if dim else 0.06,
                    stroke=RULE, sw=1))
    out.append(rect(x, by0, 3, bh, fill=color, stroke=color, rx=1,
                    sw=0, op=0.35 if dim else 1))
    out.append(f'<text x="{x+11}" y="{by0+17}" font-family="{MONO}" font-size="8.6" '
               f'fill="{FAINT}">{i+1:02d}</text>')
    out.append(f'<text x="{x+28}" y="{by0+17}" font-family="{MONO}" font-size="10.2" '
               f'fill="{color if not dim else FAINT}">{esc(name)}</text>')
    out.append(para(x + 11, by0 + 37, text, 9.9, bw - 20, MUTED if not dim else FAINT,
                    lh=13.4)[0])

# group brackets
GROUPS = [(0, 2, "① 掛載／配置", C_ASM), (2, 4, "② 硬件", C_HW),
          (4, 7, "③ 宣告／切記憶體", C_BUF), (7, 11, "④ 起", C_NC4)]
gb_y = by0 - 22
for a, b, label, color in GROUPS:
    x1 = bx0 + a * (bw + bg)
    x2 = bx0 + (b - 1) * (bw + bg) + bw
    out.append(line(x1, gb_y + 5, x1, gb_y + 11, color, 1, op=0.6))
    out.append(line(x2, gb_y + 5, x2, gb_y + 11, color, 1, op=0.6))
    out.append(line(x1, gb_y + 5, x2, gb_y + 5, color, 1, op=0.6))
    out.append(f'<text x="{(x1+x2)/2}" y="{gb_y}" font-family="{SANS}" font-size="10.2" '
               f'fill="{color}" text-anchor="middle" font-weight="600">{esc(label)}</text>')

# hard ordering notes
ny5 = by0 + bh + 22
out.append(rect(PX5 + 22, ny5 - 8, PW5 - 44, 1, fill=RULE, stroke="none", sw=0))
HARD = [
    ("硬性次序　", "NVS → 裝 log hook → 其他：log 要讀 NVS 設定；"
              "USB-Serial-JTAG 的 PHY 會被 OTG 拿走，遲裝就冇開機 log。"),
    ("硬性次序　", "掛 FAT → 讀 config：config 檔住在 data 分區；讀一次就入 struct，"
              "之後任何人用都唔再碰檔案（FAT 交 host 後讀唔到的舊問題因此消失）。"),
    ("硬性次序　", "decode 起咗 → 才建 producer：太早建會自旋；優先級一旦高過 decode，"
              "會餓死整個核（實測：USB 已列舉但完全冇回應）。"),
    ("易錯位　　", "Wi-Fi 在 commit（要先有記憶體）、nc_task_boot() 在 channels（要先有 bus）"
              "—— 兩個都唔在 start，睇階段表好容易以為喺 start。"),
]
for k, (lead, body) in enumerate(HARD):
    out.append(f'<text x="{PX5+22}" y="{ny5+20+k*21}" font-family="{SANS}" font-size="10.2">'
               f'<tspan font-weight="600" fill="{INK}">{esc(lead)}</tspan>'
               f'<tspan fill="{MUTED}">{esc(body)}</tspan></text>')
out.append(f'<text x="{PX5+PW5-22}" y="{ny5+20}" font-family="{MONO}" font-size="9.2" '
           f'fill="{FAINT}" text-anchor="end">firmware/main/main.c · nc_boot_run()</text>')

# ── footer ──────────────────────────────────────────────────────────────
out.append(line(44, 1462, 1676, 1462, RULE, 1))
foot_l = ("配置三層：`ports/<board>/wiring.json`（build-time 預設）→ "
          "`/data/config/hardware.json`（用戶覆蓋）→ NVS（執行期最高）")
out.append(para(44, 1482, foot_l, 11.2, 980, MUTED)[0])
out.append(f'<text x="1676" y="1482" font-family="{MONO}" font-size="10" fill="{FAINT}" '
           f'text-anchor="end">docs/03_notes · status §7.55–7.60</text>')

svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
       f'viewBox="0 0 {W} {H}">'
       f'<defs>{"".join(MARKERS)}</defs>{"".join(out)}</svg>')

dst = Path(sys.argv[1] if len(sys.argv) > 1 else "docs/00_pm/architecture.svg")
dst.parent.mkdir(parents=True, exist_ok=True)
dst.write_text(svg, encoding="utf-8")
print(f"wrote {dst} ({len(svg)/1024:.1f} KB)")
