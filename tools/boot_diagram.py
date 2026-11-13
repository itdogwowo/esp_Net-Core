#!/usr/bin/env python3
"""Generate docs/00_pm/boot.svg — 開機 12 個階段：做什麼、用什麼模塊、契約。

Single source of truth: 手寫的內容清單 + 固定幾何，輸出獨立 SVG（無外部字型／圖片）。
    python tools/boot_diagram.py [out.svg]

為什麼要這張圖
--------------
boot.h 的契約是「程式裡的一張表」，但看程式要跳來跳去。這張圖把三件事放埋一齊：
    ① 邊個階段　② 它做什麼、用哪些模塊　③ 它要等什麼、跑完之後給什麼
改程式之後重跑一次，圖就跟著改 —— 圖唔會自己漂走。
"""
import sys
from pathlib import Path

W = 1680

# ── palette（與 arch_diagram.py 同一套）──────────────────────────────────
INK      = "#17191C"
INK2     = "#3D4247"
MUTED    = "#6E747B"
FAINT    = "#9AA0A6"
RULE     = "#DCD8CE"
PAPER    = "#FBFAF6"
WHITE    = "#FFFFFF"

C_PREP   = "#8A6A2F"   # 準備（NVS／storage）
C_CAP    = "#1F5F8B"   # 能力（核數／USB）
C_HW     = "#5A6169"   # 硬體
C_MEM    = "#7A4E8C"   # 記憶體（plan）
C_TASK   = "#A8661B"   # 任務
C_LINK   = "#2E6B4F"   # 通道

SANS = "'Segoe UI','Noto Sans TC','Microsoft JhengHei','PingFang TC',sans-serif"
MONO = "'Cascadia Mono','Consolas','SF Mono',monospace"

out = []


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


# ── 量度（粗略但夠用：CJK 全寬、拉丁半寬）────────────────────────────────
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


# ── primitives ─────────────────────────────────────────────────────────
def rect(x, y, w, h, fill=WHITE, stroke=RULE, sw=1, rx=5, op=1.0):
    out.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" '
               f'rx="{rx}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}" '
               f'opacity="{op}"/>')


def line(x1, y1, x2, y2, color=RULE, sw=1):
    out.append(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" '
               f'stroke="{color}" stroke-width="{sw}"/>')


def text(x, y, s, fs=12, fill=INK, weight=400, mono=False, anchor="start"):
    fam = MONO if mono else SANS
    out.append(f'<text x="{x:.1f}" y="{y:.1f}" font-family="{fam}" font-size="{fs}" '
               f'font-weight="{weight}" fill="{fill}" text-anchor="{anchor}">{esc(s)}</text>')


def rich(x, y, parts, fs=12, anchor="start"):
    """parts = [(text, fill, weight, mono)] → 一行多色（用 tspan）。"""
    fam = SANS
    out.append(f'<text x="{x:.1f}" y="{y:.1f}" font-family="{fam}" font-size="{fs}" '
               f'text-anchor="{anchor}">')
    for t, fill, weight, mono in parts:
        f = MONO if mono else SANS
        out.append(f'<tspan font-family="{f}" fill="{fill}" font-weight="{weight}">'
                   f'{esc(t)}</tspan>')
    out.append("</text>")


def badge(x, y, s, fs=11, fg=INK2, bg="#F1EFE8", bd=RULE):
    w = measure(s, fs, True) + 14
    h = fs + 9
    out.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" '
               f'rx="{h/2:.1f}" fill="{bg}" stroke="{bd}" stroke-width="1"/>')
    text(x + w / 2, y + h / 2 + fs * 0.35, s, fs, fg, 500, mono=True, anchor="middle")
    return w


# ── 內容清單：順序 = 執行順序（與 main.c 的 stages[] 一致）──────────────
# (階段名, [做什麼…每行一句], [模塊 badge], needs, gives, 之後誰用它, 顏色)
ROWS = [
    ("banner+nvs", ["NVS 開機、log 擷取、版本橫幅"],
     ["nvs_flash", "nclog", "state"], "∅", "nvs", "下一個階段等它", C_PREP),

    ("storage+cfg", ["掛 FAT／WL、寫 seed、讀 config 三層、cID、SD"],
     ["store", "config", "sdmmc"], "nvs", "storage, config",
     "三層 config 之後人人都讀", C_PREP),

    ("capability", ["探測核數；決定 USB 模式與 link 名"],
     ["core", "config"], "config", "cores, usb", "decoder 個數、router／任務的 core 夾", C_CAP),

    ("hwcheck", ["按 hardware.json claim 腳位（衝突開機即見）"],
     ["hw"], "config", "∅", "只稽核：即報，唔阻開機", C_HW),

    ("hardware", ["PWM、ETH 起來；寫路由表（線 → 核）"],
     ["drv_pwm", "esp_eth", "nc4_router"], "config", "router", "決定每條線跑哪個核", C_HW),

    ("pool", ["共用池已停用，只印一行說明"],
     ["—"], "∅", "∅", "—", C_MEM),

    ("declare", ["建 decoder × 核數；登記任務；向 plan 要記憶體"],
     ["nc4", "task_manager", "plan"], "config, cores, usb, router", "declared",
     "commit 才有東西可 carve", C_MEM),

    ("commit", ["plan 一次 carve；Wi-Fi 兩道閘"],
     ["plan", "wifi"], "declared", "plan", "start 派資源靠它", C_MEM),

    ("start", ["派資源（nc_buf）、跑 on_start、註冊指令"],
     ["task_manager", "mem", "action"], "plan, config", "tasks", "usb／channels 等它", C_TASK),

    ("usb", ["TinyUSB 裝 CDC+MSC，或 JTAG log；交碟給 host"],
     ["tinyusb", "store", "nclog", "core"], "usb, router", "∅",
     "log 管道與 /data 歸屬從此定案", C_LINK),

    ("channels", ["建 CDC／jtag／UART／TCP bus；decode loop 開跑"],
     ["nc4_bus", "drv_uart", "bus_net"], "tasks, usb, router", "channels",
     "decode loop 開始收幀", C_LINK),

    ("account", ["印剩餘 heap（內部／PSRAM）"],
     ["esp_heap"], "∅", "∅", "—", C_LINK),
]
# ── 幾何 ───────────────────────────────────────────────────────────────
L, R = 44, W - 44
COL_IDX, COL_DO, COL_MOD, COL_CON, COL_USE = L + 8, L + 176, L + 640, L + 950, L + 1288
ROW_H = 74
TOP = 140
PANEL_A_Y = TOP + ROW_H * len(ROWS) + 26
PANEL_A_H = 108
PANEL_B_Y = PANEL_A_Y + PANEL_A_H + 14
PANEL_B_H = 116
H = PANEL_B_Y + PANEL_B_H + 34


def build():
    out.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
               f'viewBox="0 0 {W} {H}" font-family="{SANS}">')
    rect(0, 0, W, H, PAPER, PAPER, 0, 0)

    # 標題
    text(L, 52, "開機：12 個階段、10 個依賴位元、1 個 ctx", 25, INK, 600)
    text(L, 78, "boot.h 是契約（needs／gives）、boot.c 是執行器、main.c 是實作。"
                "下面每一行 = main.c 的 stages[] 一行。", 12.5, MUTED)
    text(R, 78, "來源：firmware/main/boot.h · boot.c · main.c", 11, FAINT, 400, anchor="end")

    # 表頭
    hy = TOP - 12
    text(COL_IDX, hy, "① 階段（B01…，順序即執行序）", 11.5, MUTED, 600)
    text(COL_DO, hy, "② 做什麼", 11.5, MUTED, 600)
    text(COL_MOD, hy, "③ 用什麼模塊", 11.5, MUTED, 600)
    text(COL_CON, hy, "④ 契約   needs → gives", 11.5, MUTED, 600)
    text(COL_USE, hy, "⑤ 之後誰用它", 11.5, MUTED, 600)
    line(L, TOP - 2, R, TOP - 2, RULE)

    for i, (name, what, mods, needs, gives, use, color) in enumerate(ROWS):
        y = TOP + i * ROW_H
        # 左側色條
        out.append(f'<rect x="{L}" y="{y + 6}" width="4" height="{ROW_H - 14}" '
                   f'rx="2" fill="{color}"/>')
        # 序號
        # 階段 ID（B01…）＝ 對外溝通用嘅 token，與 log 的 boot[B02] 一致
        text(COL_IDX, y + 24, f"B{i + 1:02d}", 11, FAINT, 500, mono=True)
        text(COL_IDX + 36, y + 24, name, 13, INK, 600, mono=True)
        # 做什麼
        for k, ln in enumerate(what[:2]):
            text(COL_DO, y + 22 + k * 17, ln, 12, INK2)
        # 模塊 badges
        bx = COL_MOD
        for m in mods:
            bx += badge(bx, y + 12, m) + 6
        # 契約
        rich(COL_CON, y + 24,
             [(needs, MUTED, 400, True), ("  →  ", FAINT, 400, False), (gives, INK, 600, True)],
             fs=12)
        text(COL_USE, y + 24, use, 11, MUTED)
        if i < len(ROWS) - 1:
            line(L + 12, y + ROW_H - 6, R - 12, y + ROW_H - 6, RULE, 1)

    # 箭頭（階段之間）
    for i in range(len(ROWS) - 1):
        y0 = TOP + i * ROW_H + ROW_H - 8
        out.append(f'<path d="M {L + 34} {y0 - 2} L {L + 34} {y0 + 6}" stroke="{FAINT}" '
                   f'stroke-width="1.2"/>')
        out.append(f'<path d="M {L + 31} {y0 + 3} L {L + 34} {y0 + 7} L {L + 37} {y0 + 3}" '
                   f'fill="none" stroke="{FAINT}" stroke-width="1.2"/>')

    # ── 面板 A：ctx ──────────────────────────────────────────────────
    rect(L, PANEL_A_Y, R - L, PANEL_A_H, WHITE)
    text(L + 16, PANEL_A_Y + 24, "ctx（nc_boot_ctx）—— 跨階段的資料只有這五個欄位", 12.5, INK, 600)
    text(L + 16, PANEL_A_Y + 44,
         "寫入者只有 capability 一個；讀者全部經 nc_boot_ctx() 拿，沒有第二份 static。",
         11.5, MUTED)

    rich(L + 16, PANEL_A_Y + 72,
         [("cores", INK, 600, True), ("  (1／2)   ", MUTED, 400, False),
          ("讀：", FAINT, 400, False),
          ("nc4_decoders()、router 的 decodeN 夾、任務管理器的 loop core 夾", INK2, 400, False)],
         fs=11.5)
    rich(L + 16, PANEL_A_Y + 92,
         [("usb_mode／usb_link／usb_log／usb_msc   ", MUTED, 400, False),
          ("讀：", FAINT, 400, False),
          ("declare、ph_usb、ph_channels、router_defaults、usb_init", INK2, 400, False)],
         fs=11.5)

    # ── 面板 B：失敗會點 ────────────────────────────────────────────
    rect(L, PANEL_B_Y, R - L, PANEL_B_H, WHITE)
    text(L + 16, PANEL_B_Y + 24, "失敗會點：一條鏈講清楚，唔會靜靜帶錯狀態往下跑", 12.5, INK, 600)

    boxes = [
        ("階段 FAIL", "回非 ESP_OK", C_MEM),
        ("唔會 give", "後面的階段驗 needs", C_CAP),
        ("下游 SKIP", "log 寫明「缺 config|router」", C_TASK),
        ("開機照完", "最後印「已具備 0x03ff」", C_LINK),
    ]
    bw = (R - L - 32 - 3 * 10) / 4
    for i, (t1, t2, col) in enumerate(boxes):
        x = L + 16 + i * (bw + 10)
        y = PANEL_B_Y + 38
        rect(x, y, bw, 44, "#FCFBF7", RULE)
        out.append(f'<rect x="{x}" y="{y}" width="4" height="44" rx="2" fill="{col}"/>')
        text(x + 14, y + 19, t1, 12, INK, 600)
        text(x + 14, y + 34, t2, 10.5, MUTED)

    text(L + 16, PANEL_B_Y + 104,
         "0x03ff = 10 個位元全開（NVS|storage|config|cores|usb|router|declared|plan|tasks|channels）；"
         "只跑到 config 會係 0x0017。", 10.5, FAINT)

    # 圖例
    ly = H - 14
    lx = L
    for name, col in [("準備", C_PREP), ("能力", C_CAP), ("硬體", C_HW),
                      ("記憶體", C_MEM), ("任務", C_TASK), ("通道", C_LINK)]:
        out.append(f'<rect x="{lx}" y="{ly - 8}" width="4" height="11" rx="2" fill="{col}"/>')
        text(lx + 9, ly, name, 10.5, MUTED)
        lx += 9 + measure(name, 10.5) + 18

    out.append("</svg>")


def main():
    build()
    dst = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("docs/00_pm/boot.svg")
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text("\n".join(out), encoding="utf-8")
    print(f"{dst}  ({W}×{H})")


if __name__ == "__main__":
    main()
