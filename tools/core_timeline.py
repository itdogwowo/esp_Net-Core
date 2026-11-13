#!/usr/bin/env python3
"""Generate docs/00_pm/core-timeline.svg — 核心時間線 ＋ 每階段緩衝佔用。

    python tools/core_timeline.py [out.svg]

為什麼要這張圖
-------------
boot.svg 講「有幾多階段、各自做乜」；buffers.svg 講「一條通道有幾層緩衝」。
但兩個問題一直冇一張圖答得到：

    ① 開機嗰 207 ms 裡面，**core1 到底做過乜**？
    ② 每個階段**真正落地幾多位元組**緩衝（唔係「宣告」，係 malloc／xTaskCreate）？

這張圖上半部＝時間線（左 core0／右 core1，向下＝時間），下半部＝另一個區域，
逐階段標返佢配咗幾多內部 RAM／PSRAM。

數字來源（全部可追溯，冇一個係估）
--------------------------------
· 毫秒：2026-11-08 的九階實測（docs/00_pm/status.md §7.23）。capability／hwcheck／
  pool 三個階段係之後才拆出嚟，**當時唔存在** → 標「未量測」，唔會假裝係 0.0 ms。
· 位元組：main.c 的 plan 宣告（nc4.rx 4,192、nc4.reply0/1 2,615、uart.rx 1,024、
  tcp.rx 16,410、fwd.cX 4×8,221）＋ 各處 xTaskCreate 的 stack 常數。
· 「0」唔等於冇成本：IDF 自己的（NVS 頁快取、FAT／WL、ETH DMA 51.2 KB、TinyUSB、
  Wi-Fi ~50 KB）刻意唔計入，因為嗰啲唔係我們的表可以控制；圖右下角有寫明。
"""
import sys
from pathlib import Path

W = 1720

# ── palette（與 arch／boot／buffers 同一套）──────────────────────────────
INK      = "#17191C"
INK2     = "#3D4247"
MUTED    = "#6E747B"
FAINT    = "#9AA0A6"
RULE     = "#DCD8CE"
PAPER    = "#FBFAF6"
WHITE    = "#FFFFFF"

C0       = "#1F5F8B"   # core 0
C1       = "#A8661B"   # core 1
GATE     = "#B23B2E"   # 放行線
WAIT     = "#C9A227"   # 等閘（hatch）

C_PREP   = "#8A6A2F"
C_CAP    = "#1F5F8B"
C_HW     = "#5A6169"
C_MEM    = "#7A4E8C"
C_TASK   = "#A8661B"
C_LINK   = "#2E6B4F"

SANS = "'Segoe UI','Noto Sans TC','Microsoft JhengHei','PingFang TC',sans-serif"
MONO = "'Cascadia Mono','Consolas','SF Mono',monospace"

out = []


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


# ── 量度（粗略但夠用：CJK 全寬、拉丁半寬）────────────────────────────────
def cw(ch, fs, mono=False):
    o = ord(ch)
    if o >= 0x2E80 or ch in "，。、；：（）「」『』【】！？—…·｜＋＝":
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


# ── primitives ─────────────────────────────────────────────────────────
def rect(x, y, w, h, fill=WHITE, stroke=RULE, sw=1, rx=5, op=1.0, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    out.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" '
               f'rx="{rx}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}" '
               f'opacity="{op}"{d}/>')


def line(x1, y1, x2, y2, color=RULE, sw=1, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    out.append(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" '
               f'stroke="{color}" stroke-width="{sw}"{d}/>')


def text(x, y, s, fs=12, fill=INK, weight=400, mono=False, anchor="start", op=1.0):
    fam = MONO if mono else SANS
    out.append(f'<text x="{x:.1f}" y="{y:.1f}" font-family="{fam}" font-size="{fs}" '
               f'font-weight="{weight}" fill="{fill}" text-anchor="{anchor}" '
               f'opacity="{op}">{esc(s)}</text>')


def vtext(x, y, s, fs=11, fill=MUTED, weight=500):
    """旋轉 −90°（由下往上讀），用於左側的時段脊柱。"""
    out.append(f'<text x="0" y="0" transform="translate({x:.1f},{y:.1f}) rotate(-90)" '
               f'font-family="{SANS}" font-size="{fs}" font-weight="{weight}" '
               f'fill="{fill}" text-anchor="middle">{esc(s)}</text>')


def rich(x, y, parts, fs=12, anchor="start"):
    out.append(f'<text x="{x:.1f}" y="{y:.1f}" font-family="{SANS}" font-size="{fs}" '
               f'text-anchor="{anchor}">')
    for t, fill, weight, mono in parts:
        f = MONO if mono else SANS
        out.append(f'<tspan font-family="{f}" fill="{fill}" font-weight="{weight}">'
                   f'{esc(t)}</tspan>')
    out.append("</text>")


def badgetxt(x, y, s, fs=10.5, fg=INK2, bg="#F1EFE8", bd=RULE, anchor="start"):
    w = measure(s, fs, True) + 14
    h = fs + 9
    bx = x if anchor == "start" else x - w
    out.append(f'<rect x="{bx:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" '
               f'rx="{h/2:.1f}" fill="{bg}" stroke="{bd}" stroke-width="1"/>')
    text(bx + w / 2, y + h / 2 + fs * 0.36, s, fs, fg, 500, mono=True, anchor="middle")
    return w


# ── 幾何 ───────────────────────────────────────────────────────────────
AX_RAIL  = 132.0          # 時間軸
SPINE_X  = 17.0           # 時段脊柱
C0_X, C0_W = 150.0, 700.0
C1_X, C1_W = 866.0, 520.0
RG_X, RG_W = 1404.0, 276.0
ROWS_Y   = 132.0
ROW_MIN  = 40.0
MS_SCALE = 1.0            # 1 px = 1 ms（大階段才拉長；細階段夾到 ROW_MIN）
MS_FLOOR = 40.0

BUF_Y     = 900.0         # 下半部：緩衝區域（build() 會按時間線高度重算）
BUF_ROW_H = 34.0
BAR_X_I, BAR_W_I, BAR_MAX_I = 622.0, 300.0, 22000.0
BAR_X_P, BAR_W_P, BAR_MAX_P = 1090.0, 430.0, 56000.0
VAL_X_I = 984.0
VAL_X_P = 1582.0

# ── 內容清單：順序＝執行順序（= main.c 的 stages[]）─────────────────────
# name, ms, core0 行, core1 行 (None = 閒置), 顏色,
# 內部 bytes, PSRAM bytes, 緩衝說明行
ROWS = [
    ("banner+nvs", 9.66,
     ["版本橫幅；nvs_flash_init；讀開機計數"],
     None, C_PREP, 0, 0,
     ["NVS 頁快取（IDF 的，唔計）"]),

    ("storage+cfg", 126.92,
     ["掛 FAT／WL 的 /data；寫 seed；config 三層解析（system／network／hardware）",
      "→ 全開機最貴嘅一步，佔掉 61%"],
     None, C_PREP, 0, 0,
     ["FAT／WL 的內部表（IDF 的，唔計）"]),

    ("capability", None,
     ["nc_core_probe() → ctx.cores（1／2）",
      "usb_resolve() → ctx.usb_mode／usb_link／usb_log／usb_msc"],
     None, C_CAP, 0, 0,
     ["0 B：只寫 nc_boot_ctx 五個欄位，一個 heap_caps_malloc 都冇"]),

    ("hwcheck", None,
     ["hardware.json 的 claim 對帳（腳位衝突開機即見）"],
     None, C_HW, 0, 0,
     ["0 B：只稽核，唔配置"]),

    ("hardware", 0.33,
     ["ETH／PHY、SD、UART 腳位；nc4_router_set() 收下路由表"],
     None, C_HW, 6144, 0,
     ["tcp_nc4 接受連線任務 6,144 B 棧（P4-ETH 才有；C3 冇）"]),

    ("pool", None,
     ["nc4.pool 已停用 → 舊設定全部忽略（唔配記憶體）"],
     None, C_MEM, 0, 0,
     ["0 B：留返個位，但已經冇人用"]),

    ("declare", 10.45,
     ["nc_task_declare_all()：decode0／1、nc4.rx、reply0／1、uart.rx、tcp.rx",
      "只登記「要幾大」，仲未 malloc"],
     None, C_MEM, 0, 0,
     ["0 B：帳面而已 —— 好處係 commit 之前就知夠唔夠"]),

    ("commit", 0.76,
     ["nc_plan_commit()：每個 class 一次 carve → 緩衝真正落地",
      "nc_wifi_boot()（有 Wi-Fi 才會配）"],
     ["wifi_mgr 6,144 B 棧（nc_wifi_boot，如有 Wi-Fi）"],
     C_MEM, 21626, 5230,
     ["nc4.rx 4,192 ＋ uart.rx 1,024 ＋ tcp.rx 16,410 ＝ 內部 21,626",
      "reply0／1 2,615×2 ＝ PSRAM 5,230（BULK 類；冇 PSRAM 就落返內部）"]),

    ("start", 5.39,
     ["nc_task_start_all()：建 decode0／decode1 → entry 跑 owner_init 就停低等閘",
      "印任務帳本"],
     ["decode1 已建立，但**等閘**（loop 未開）"],
     C_TASK, 6144, 0,
     ["decode0 3,072 ＋ decode1 3,072 棧（單核只建一條，省 3,072）"]),

    ("usb", 52.01,
     ["usb_init() 用 nc_core_run_on(cdc_core) → TinyUSB 都喺 core0",
      "列舉 CDC／MSC；nc_log_start() 換掉 log 出口"],
     None, C_LINK, 2560, 0,
     ["nc_log 2,560 B 棧 ＋ log 環 2,048／512（debug 8,192／1,024）"]),

    ("channels", 1.81,
     ["net／WS／UDP；CDC bus ＋ cdc.rx；UART bus；TCP bus",
      "nc_task_boot() 放行 → decode0 開 loop；最後才起 uart.rx producer"],
     ["decode1 放行（3,072）；uart.rx producer 1,536（只有 ETH 板）",
      "nc4_udp 6,144（如有 UDP）"],
     C_LINK, 4864, 0,
     ["cdc.rx 1,280 ＋ uart.rx producer 1,536 ＋ 槽環 256×4×2 ＝ 2,048"]),

    ("account", 0.19,
     ["印「開機剩餘：內部 …／PSRAM …（含最大連續）」→ 之後只剩 1 秒心跳"],
     None, MUTED, 0, 0,
     ["0 B：只讀 heap 數字"]),
]

FWD_ROW = ("runtime", None,
           ["跨核轉寄：decodeX 要送去另一核 → fwd.c0／fwd.c1（按需建立）"],
           ["decode1 收到嘅要由 core1 交返 core0 出 USB，反之亦然"],
           C_MEM, 0, 32884,
           ["nc_buf PSRAM 4 格 × 8,221 B（16 名字 ＋ 8,205 幀）＝ 32,884／核",
            "唔轉寄就一個 byte 都唔配（單核／冇 PSRAM 直接唔建）"])


def build():
    global BUF_Y
    # ── 逐行高度（時間線）────────────────────────────────────────────
    spans = []
    y = ROWS_Y
    for name, ms, c0, c1, col, bi, bp, btxt in ROWS:
        lines = max(len(c0), len(c1) if c1 else 1)
        need = 46 + 15 * (lines - 1)          # 標題 + 內容行（每行 15 px）＋ 下緣留白
        h = max(ROW_MIN, (ms or 0) * MS_SCALE if ms else 0.0, need)
        h = float(round(h))
        spans.append((y, y + h))
        y += h
    tl_bottom = y
    ry = tl_bottom + 12                        # runtime 帶
    rh = 96.0
    BUF_Y = ry + 22 + rh + 56

    cum = []                       # 每行起始的累計毫秒
    t = 0.0
    for name, ms, *_ in ROWS:
        cum.append(t)
        t += ms or 0.0
    total_ms = t

    H = int(BUF_Y + 40 + BUF_ROW_H * (len(ROWS) + 1) + 215)

    # ── defs ────────────────────────────────────────────────────────
    out.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
               f'viewBox="0 0 {W} {H}">')
    out.append('<defs>'
               f'<pattern id="hatch" width="7" height="7" patternUnits="userSpaceOnUse" '
               f'patternTransform="rotate(45)">'
               f'<rect width="7" height="7" fill="#FBF6E6"/>'
               f'<line x1="0" y1="0" x2="0" y2="7" stroke="{WAIT}" stroke-width="1.6"/>'
               '</pattern>'
               '<marker id="arw" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" '
               f'markerHeight="7" orient="auto"><path d="M0 0 L10 5 L0 10 z" fill="{FAINT}"/>'
               '</marker>'
               '</defs>')
    out.append(f'<rect width="{W}" height="{H}" fill="{PAPER}"/>')

    # ── 標題 ────────────────────────────────────────────────────────
    text(40, 48, "核心時間線：開機每一刻邊個核做嘢，每個階段又佔幾多緩衝", 25, INK, 600)
    text(40, 72,
         f"向下＝時間。實測 {total_ms:.1f} ms（九階量測，2026-11-08）｜左 core0／右 core1｜"
         "下方另一個區域逐階段標緩衝佔用（plan 宣告 ＋ 任務棧常數，非估算）",
         12, MUTED)

    # ── 欄頭 ────────────────────────────────────────────────────────
    head_y = ROWS_Y - 14
    text(AX_RAIL, head_y, "累計 ms", 10.5, FAINT, 500, mono=True, anchor="end")
    text(C0_X + 12, head_y, "core 0", 13, C0, 700)
    text(C0_X + 78, head_y, "app_main 喺呢個核跑 → 十二個階段全部順序喺呢邊", 10.5, MUTED)
    text(C1_X + 12, head_y, "core 1", 13, C1, 700)
    text(C1_X + 78, head_y, "大部分人以為嘅「雙核開機」其實好靜", 10.5, MUTED)

    # ── 時段脊柱（左側）────────────────────────────────────────────
    periods = [("準備", 0, 2, C_PREP), ("決定", 2, 6, C_CAP),
               ("落地", 6, 9, C_MEM), ("開通道", 9, 12, C_LINK)]
    for pname, a, b, col in periods:
        y0, y1 = spans[a][0], spans[b - 1][1]
        out.append(f'<rect x="{SPINE_X:.1f}" y="{y0:.1f}" width="13" '
                   f'height="{y1 - y0 - 4:.1f}" rx="6.5" fill="{col}" opacity="0.5"/>')
        vtext(SPINE_X + 9.5, (y0 + y1) / 2, pname, 11, "#FFFFFF", 700)

    # ── 時間軸 rail ＋ 累計刻度 ─────────────────────────────────────
    line(AX_RAIL, spans[0][0], AX_RAIL, tl_bottom, MUTED, 1.2)
    for i, (name, ms, c0, c1, col, bi, bp, btxt) in enumerate(ROWS):
        y0, y1 = spans[i]
        line(AX_RAIL - 5, y0, AX_RAIL, y0, MUTED, 1.2)
        if ms is not None:
            text(AX_RAIL - 10, y0 + 11, f"{cum[i]:.1f}", 10.5, INK2, 500, mono=True, anchor="end")
            text(AX_RAIL - 10, y0 + 24, f"+{ms:.2f}", 9.5, FAINT, 400, mono=True, anchor="end")
        else:
            text(AX_RAIL - 10, y0 + 14, "n/a", 10, FAINT, 400, mono=True, anchor="end")
            text(AX_RAIL - 10, y0 + 26, "未量測", 9, FAINT, 400, anchor="end")
    line(AX_RAIL - 5, tl_bottom, AX_RAIL, tl_bottom, MUTED, 1.2)
    text(AX_RAIL - 10, tl_bottom + 13, f"{total_ms:.1f}", 10.5, INK, 600, mono=True, anchor="end")

    # ── 兩條核道 ＋ 逐階段方塊 ─────────────────────────────────────
    for i, (name, ms, c0, c1, col, bi, bp, btxt) in enumerate(ROWS):
        y0, y1 = spans[i]
        pad = 3.0
        bx0, bh = y0 + pad, y1 - y0 - pad * 2

        # core 0：一定有內容
        rect(C0_X, bx0, C0_W, bh, WHITE, RULE, 1, 5)
        out.append(f'<rect x="{C0_X:.1f}" y="{bx0:.1f}" width="3.5" height="{bh:.1f}" '
                   f'rx="1.75" fill="{col}"/>')
        text(C0_X + 14, bx0 + 17, f"B{i + 1:02d}  {name}", 12.5, INK, 600, mono=True)
        if ms is not None:
            text(C0_X + C0_W - 12, bx0 + 17, f"{ms:.2f} ms", 10, MUTED, 500,
                 mono=True, anchor="end")
        else:
            text(C0_X + C0_W - 12, bx0 + 17, "未量測", 9.5, FAINT, 400, anchor="end")
        for k, ln in enumerate(c0):
            text(C0_X + 14, bx0 + 34 + k * 15, ln, 11.5, INK2)

        # core 1：多數係「冇嘢」
        idle = c1 is None
        rect(C1_X, bx0, C1_W, bh, "#FDFAF3" if idle else "#FDF6EC",
             RULE, 1, 5, 0.9 if idle else 1.0, "3 3" if idle else None)
        if idle:
            text(C1_X + 14, bx0 + bh / 2 + 4, "—  冇任務（全部喺 core0 順序做）", 10.5, FAINT)
        else:
            out.append(f'<rect x="{C1_X:.1f}" y="{bx0:.1f}" width="3.5" height="{bh:.1f}" '
                       f'rx="1.75" fill="{C1}"/>')
            for k, ln in enumerate(c1):
                text(C1_X + 14, bx0 + 18 + k * 15, ln.replace("**", ""), 10.5,
                     INK2 if "**" not in ln else C1, 500 if "**" in ln else 400)

    # ── 等閘：start 建任務 → channels 才放行 ───────────────────────
    # 條帶畫喺兩條核道之間的縫，唔會蓋住任何階段的方塊。
    gate_y = spans[10][0]                       # channels 行頂 = usb 結束
    wait_top = spans[8][0]                      # start 行頂
    for sx in (C0_X + C0_W + 6, C1_X + C1_W + 6):
        out.append(f'<rect x="{sx:.1f}" y="{wait_top + 3:.1f}" width="8" '
                   f'height="{gate_y - wait_top - 6:.1f}" fill="url(#hatch)" '
                   f'stroke="{WAIT}" stroke-width="0.8" rx="2"/>')
    line(AX_RAIL, gate_y, C1_X + C1_W + 14, gate_y, GATE, 1.6, "7 4")
    badgetxt(C1_X + C1_W + 14, gate_y - 12, "放行 nc_task_boot()", 9.5, WHITE, GATE, GATE, "end")

    # ── runtime 帶 ─────────────────────────────────────────────────
    ry = tl_bottom + 12
    rh = 96.0
    text(AX_RAIL - 10, ry + 14, "→∞", 10.5, INK2, 600, mono=True, anchor="end")
    text(C0_X + 12, ry + 14, "runtime（開機完成之後，永久）", 12, INK, 600)

    rect(C0_X, ry + 22, C0_W, rh, "#F6FAFD", C0, 1, 6)
    text(C0_X + 12, ry + 40, "core0：decode0、cdc.rx、nc_log、TinyUSB、tcp_nc4、app_main 心跳", 11, C0, 600)
    text(C0_X + 12, ry + 58,
         "decode0 3,072｜cdc.rx 1,280｜nc_log 2,560｜tcp_nc4 6,144｜app_main 心跳（1 s，prio 1）",
         10, INK2)
    text(C0_X + 12, ry + 76,
         "任務棧小計 13,056 B（心跳用 IDF 主任務棧，唔另外計）", 10, MUTED)
    text(C0_X + 12, ry + 94,
         "USB 全部喺呢邊：router_defaults() 第一條就寫死 core 0", 10, FAINT)

    rect(C1_X, ry + 22, C1_W, rh, "#FEF9F1", C1, 1, 6)
    text(C1_X + 12, ry + 40, "core1：decode1、uart.rx、wifi_mgr、nc4_udp", 11, C1, 600)
    text(C1_X + 12, ry + 58,
         "decode1 3,072｜uart.rx 1,536｜wifi_mgr 6,144｜nc4_udp 6,144", 10, INK2)
    text(C1_X + 12, ry + 76,
         "任務棧小計 16,896 B（Wi-Fi／UDP 可關，關掉就 4,608）", 10, MUTED)
    text(C1_X + 12, ry + 94,
         "只有 routes[] 指去 core1 的線才會開；冇 ETH 就連 decode1 都唔建", 10, FAINT)

    # runtime 右側（面板下面嘅空位）：幾時才真正並行
    rect(RG_X, ry + 22, RG_W, rh, WHITE, RULE, 1, 6)
    text(RG_X + 14, ry + 42, "幾時才真正並行？", 11.5, INK, 600)
    text(RG_X + 14, ry + 60, "有 Wi-Fi：core1 由 148 ms 起喺背景連線", 9, MUTED)
    text(RG_X + 14, ry + 74, "冇 Wi-Fi：要等到 205.5 ms 放行先有 loop", 9, MUTED)
    text(RG_X + 14, ry + 96, "冇 Wi-Fi 嘅板，「雙核開機」其實係", 9.5, C1, 600)
    text(RG_X + 14, ry + 110, "「單核開機 ＋ 單核待命」。", 9.5, C1, 600)

    # ── 右側參考欄：核心分配原則 ────────────────────────────────────
    p2 = ROWS_Y + 62 + 7 * 30 + 52
    p3 = p2 + 42 + 4 * 27 + 26
    rgh = p3 + 140 - ROWS_Y
    rect(RG_X, ROWS_Y, RG_W, rgh, WHITE)
    text(RG_X + 14, ROWS_Y + 22, "邊個決定去邊個核", 12.5, INK, 600)
    text(RG_X + 14, ROWS_Y + 40, "唔係編譯期常數，係開機時嘅決定", 10.5, MUTED)
    items = [
        ("USB／CDC", "core 0 寫死", C0),
        ("decodeN", "routes[].core", C0),
        ("UART", "有 ETH→core1，冇→core0", C1),
        ("wifi_mgr", "nc_core_clamp(1)", C1),
        ("nc4_udp", "釘 core1", C1),
        ("net_* producer", "釘 core0", C0),
        ("單核板", "nc_core_clamp() 收成 0", MUTED),
    ]
    for k, (nm, val, col) in enumerate(items):
        yy = ROWS_Y + 62 + k * 30
        out.append(f'<rect x="{RG_X + 14:.1f}" y="{yy - 8:.1f}" width="3" height="24" '
                   f'rx="1.5" fill="{col}"/>')
        text(RG_X + 24, yy + 1, nm, 10.5, INK, 600, mono=True)
        text(RG_X + 24, yy + 15, val, 9.5, MUTED)

    text(RG_X + 14, ROWS_Y + 62 + 7 * 30 + 10,
         "單核板上寫 core1：收斂成 0，只印一行警告 ——", 9.5, FAINT)
    text(RG_X + 14, ROWS_Y + 62 + 7 * 30 + 24,
         "唔會喺 FreeRTOS 裡 assert → reboot loop。", 9.5, FAINT)

    # ── 面板下半：core1 幾時先有嘢做 ＋ 開機期間 workload ─────────────
    line(RG_X + 14, p2, RG_X + RG_W - 14, p2, RULE, 1)
    text(RG_X + 14, p2 + 22, "core1 幾時先有嘢做？", 12.5, INK, 600)
    gates = [
        ("有 ETH（P4-ETH）", "channels：decode1 ＋ uart.rx", C1),
        ("有 Wi-Fi", "commit：wifi_mgr（先起，後連線）", C1),
        ("有 UDP 通道", "channels：nc4_udp（釘死 core1）", C1),
        ("以上都冇", "core1 整段開機冇任務", MUTED),
    ]
    for k, (nm, val, col) in enumerate(gates):
        yy = p2 + 42 + k * 27
        out.append(f'<circle cx="{RG_X + 20:.1f}" cy="{yy - 3:.1f}" r="3" fill="{col}"/>')
        text(RG_X + 30, yy, nm, 9.5, INK, 600, mono=True)
        text(RG_X + 30, yy + 13, val, 9, MUTED)

    # workload 條：207.5 ms 之內邊個核真係做緊嘢
    line(RG_X + 14, p3, RG_X + RG_W - 14, p3, RULE, 1)
    text(RG_X + 14, p3 + 22, f"開機 {total_ms:.1f} ms 之內，邊個核做緊嘢", 12.5, INK, 600)
    bw = RG_W - 60
    for k, (nm, frac, col, lab) in enumerate((
            ("core0", 1.0, C0, f"{total_ms:.1f} ms 全程"),
            ("core1", 0.012, C1, "≈ 0 ms" ))):
        yy = p3 + 44 + k * 30
        text(RG_X + 14, yy + 4, nm, 10, INK, 600, mono=True)
        out.append(f'<rect x="{RG_X + 52:.1f}" y="{yy - 6:.1f}" width="{bw:.1f}" '
                   f'height="15" rx="3" fill="#F1EFE8" stroke="{RULE}" stroke-width="0.8"/>')
        out.append(f'<rect x="{RG_X + 52:.1f}" y="{yy - 6:.1f}" width="{max(2.0, bw * frac):.1f}" '
                   f'height="15" rx="3" fill="{col}" opacity="0.85"/>')
        text(RG_X + 60, yy + 4, lab, 9,
             WHITE if frac > 0.3 else MUTED, 600, mono=True)
    text(RG_X + 14, p3 + 106,
         "core1 唯一嘅開機工作係「被建立」——", 9, FAINT)
    text(RG_X + 14, p3 + 118,
         "loop 要等 channels 的 nc_task_boot() 放行。", 9, FAINT)

    # ══ 下半部：緩衝區域 ═══════════════════════════════════════════
    text(40, BUF_Y + 22, "緩衝佔用：每個階段真正落地幾多位元組", 18, INK, 600)
    text(40, BUF_Y + 42,
         "「落地」＝ malloc／xTaskCreate 真係發生喺呢一步。declare 只記帳，commit 才 carve。"
         "IDF 自己嘅（NVS／FAT／ETH DMA／TinyUSB／Wi-Fi）唔計在內，見右下角。",
         10.5, MUTED)

    hy = BUF_Y + 66
    text(40, hy, "階段", 10.5, MUTED, 600)
    text(250, hy, "落到咩緩衝（名字 ＋ 大小）", 10.5, MUTED, 600)
    text(BAR_X_I, hy, "內部 RAM（MALLOC_CAP_INTERNAL）　全長 22,000 B", 10.5, C_MEM, 600)
    text(BAR_X_P, hy, "PSRAM（MALLOC_CAP_SPIRAM）　全長 56,000 B", 10.5, C1, 600)

    def buf_row(y, name, col, bi, bp, btxt, note=None, dim=False):
        if y > BUF_Y + 70:
            line(40, y - 4, W - 40, y - 4, "#EDEAE2", 1)
        # 名字
        out.append(f'<rect x="40" y="{y + 4:.1f}" width="3" height="20" rx="1.5" '
                   f'fill="{col}" opacity="{0.45 if dim else 1}"/>')
        text(50, y + 18, name, 11.5, FAINT if dim else INK, 600, mono=True)
        # 說明（最多兩行）
        for k, ln in enumerate(btxt[:2]):
            text(250, y + 14 + k * 15, ln, 10.5, MUTED if dim else INK2)
        # 內部 / PSRAM 兩條 bar（先畫淡色軌道，令長度可以互相比較）
        for x0, wmax, vmax, val, bcol, vx in (
                (BAR_X_I, BAR_W_I, BAR_MAX_I, bi, C_MEM, VAL_X_I),
                (BAR_X_P, BAR_W_P, BAR_MAX_P, bp, C1, VAL_X_P)):
            out.append(f'<rect x="{x0:.1f}" y="{y + 7:.1f}" width="{wmax:.1f}" height="16" '
                       f'rx="3" fill="#F2F0E9"/>')
            if val <= 0:
                text(vx, y + 19, "0", 10, FAINT, 400, mono=True, anchor="end")
            else:
                bw = max(3.0, wmax * val / vmax)
                out.append(f'<rect x="{x0:.1f}" y="{y + 7:.1f}" width="{bw:.1f}" height="16" '
                           f'rx="3" fill="{bcol}" opacity="0.9"/>')
                text(vx, y + 19, f"{val:,}", 10.5, INK, 600, mono=True, anchor="end")

    yy = BUF_Y + 78
    for i, (name, ms, c0, c1, col, bi, bp, btxt) in enumerate(ROWS):
        buf_row(yy, f"B{i + 1:02d} {name}", col, bi, bp, btxt)
        yy += BUF_ROW_H
    buf_row(yy, "—  " + FWD_ROW[0], FWD_ROW[4], FWD_ROW[5], FWD_ROW[6], FWD_ROW[7],
            dim=True)
    yy += BUF_ROW_H

    # 合計
    tot_i = sum(r[5] for r in ROWS)
    tot_p = sum(r[6] for r in ROWS)
    line(40, yy + 4, W - 40, yy + 4, INK2, 1.2)
    text(50, yy + 24, "合計（雙核 P4-ETH）", 11.5, INK, 700)
    text(250, yy + 24,
         "內部 41,338 B（40.4 KB）＋ PSRAM 5,230 B（5.1 KB）；轉寄按需再加 32,884 B／核",
         10.5, INK2)
    text(VAL_X_I, yy + 24, f"{tot_i:,}", 11, INK, 700, mono=True, anchor="end")
    text(VAL_X_P, yy + 24, f"{tot_p:,}", 11, INK, 700, mono=True, anchor="end")

    text(50, yy + 48,
         "單核 C3 同一份表：nc4.jtag 4,192 ＋ reply0 2,615（冇 PSRAM → BULK 落返內部）"
         "＋ uart.rx 1,024 ＋ decode0 3,072 ＋ nc_log 2,560 ＝ 13,463 B（13.1 KB）",
         10.5, C0)
    text(50, yy + 66,
         "少咗乜：decode1（3,072）、tcp.rx（16,410）、tcp_nc4（6,144）、cdc.rx（1,280，"
         "JTAG 係 PULL 冇 producer）、fwd（32,884）—— 差 27.3 KB 就係「雙核值唔值」。",
         10.5, C0)

    # 右下角：唔計入嘅東西
    nb_x, nb_y, nb_w = 1050.0, yy + 42, 628.0
    rect(nb_x, nb_y, nb_w, 62, "#FCFBF7", RULE, 1, 5, 1, "4 3")
    text(nb_x + 12, nb_y + 18, "刻意唔計（IDF 擁有，唔受我們的表控制）", 10.5, MUTED, 600)
    text(nb_x + 12, nb_y + 36,
         "NVS 頁快取｜FAT／WL 掛載表｜ETH DMA 16×1,600×2 ≈ 51.2 KB｜TinyUSB ≈ 2–3 KB 任務", 9.5, FAINT)
    text(nb_x + 12, nb_y + 51,
         "Wi-Fi（啟用時 ~50 KB）｜TCP／WS producer 4,096×N｜fx 4,096（只測試映像）", 9.5, FAINT)

    # ── 簡寫對照（圖上為了排版把 usb_ 前綴省掉；這裡寫返全名）────────────
    # 放喺圖例上一行：讀者掃到 B03 兩個 usb_* 欄位時，唔需要再翻 boot.svg。
    rich(40, H - 36, [
        ("簡寫", MUTED, 600, False),
        ("：B01–B12 ＝ 開機階段（數字只代表先後）｜"
         "ctx ＝ nc_boot_ctx（跨階段唯一一份，只五個欄位）｜"
         "usb_mode／link／log／msc ＝ USB 模式／線名／log 閘／MSC 閘｜"
         "MSC ＝ Mass Storage Class（把 FAT 當隨身碟）｜"
         "capability 係階段名，唔係 NC_CAP_* 能力位", FAINT, 400, False),
    ], fs=9.5)

    # ── 圖例 ────────────────────────────────────────────────────────
    ly = H - 16
    lx = 40.0
    out.append(f'<rect x="{lx}" y="{ly - 9}" width="9" height="11" rx="2" fill="{C0}"/>')
    text(lx + 14, ly, "core0", 10.5, MUTED)
    lx += 14 + measure("core0", 10.5) + 20
    out.append(f'<rect x="{lx}" y="{ly - 9}" width="9" height="11" rx="2" fill="{C1}"/>')
    text(lx + 14, ly, "core1", 10.5, MUTED)
    lx += 14 + measure("core1", 10.5) + 20
    out.append(f'<rect x="{lx}" y="{ly - 9}" width="9" height="11" rx="2" fill="url(#hatch)"/>')
    text(lx + 14, ly, "已建立但等閘（未放行）", 10.5, MUTED)
    lx += 14 + measure("已建立但等閘（未放行）", 10.5) + 20
    out.append(f'<line x1="{lx}" y1="{ly - 4}" x2="{lx + 22}" y2="{ly - 4}" '
               f'stroke="{GATE}" stroke-width="1.6" stroke-dasharray="7 4"/>')
    text(lx + 28, ly, "放行線 nc_task_boot()", 10.5, MUTED)
    lx += 28 + measure("放行線 nc_task_boot()", 10.5) + 20
    text(lx, ly, "n/a ＝ 該階段係事後才拆出，當時冇量過（唔當 0）", 10.5, FAINT)
    text(W - 40, ly, "tools/core_timeline.py → docs/00_pm/core-timeline.svg", 10, FAINT,
         anchor="end")

    out.append("</svg>")
    return H


def main():
    h = build()
    dst = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("docs/00_pm/core-timeline.svg")
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text("\n".join(out), encoding="utf-8")
    print(f"{dst}  ({W}×{h})")


if __name__ == "__main__":
    main()
