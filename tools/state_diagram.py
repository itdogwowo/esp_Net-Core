#!/usr/bin/env python3
"""生成狀態系統的三合一圖（docs/00_pm/state-flow.svg，帶 SMIL 動畫）。

    python tools/state_diagram.py

為什麼有動畫：三件事都是「隨時間變化」的——
  ① 緩存佔用：activation 過濾之後由 18 KB 降到 1.7 KB；
  ② 開機次序：B02 → state merge → apply_to_cfg → B05 → B08 的時序；
  ③ 路線圖：S0／S1 已完成，S2 之後是下一步。
靜態圖只能講「結果」，動畫可以講「先後」。

數字全部由程式／schema 讀（同 config_diagram.py 同一個原則：圖唔會同實作講兩套）。
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCHEMA = ROOT / "schema" / "state-keys.json"
OUT = ROOT / "docs/00_pm/state-flow.svg"

# ── 量測／推導值（來源見 docs/00_pm/status.md §7.291）────────────────
KEY_NOW = 25                 # 現時 schema key 數
KEY_MATRIX = 380             # hw-matrix.md 的大表
KEY_TARGET = 565             # 大表＋9 類缺失資源
STORE_ENTRY = 32             # 舊 entry（u32 key＋bytes[16]＋u/i 分開）
ENTRY_SLIM = 12              # 2026-11-27 瘦身後（key4＋type1＋pad3＋union4）
STORE_MAX = 96               # 現時 NC_STATE_STORE_MAX（2026-11-27：64→96）
STORE_MAX_OLD = 64           # 舊值（圖上做對照）
# 典型啟動（Matrix 真實 config）：sys 12 + PIN 6×3=18 + I80 22 + LED 10
#                              + UART 6 + PWM 2 = 70
TYPICAL_ACTIVE = 70
BLOB_NOW = 50                # 板上實測 bytes
CFG_US = 24.4                # 開機 parse（實測 24,343–24,463 µs）
STATE_US = 8.5               # state merge（實測 8,429–8,664 µs）

W = 1680
M = 44
R = W - M

INK = "#17191C"
INK2 = "#3D4247"
MUTED = "#6E747B"
FAINT = "#9AA0A6"
RULE = "#DCD8CE"
PAPER = "#FBFAF6"
WHITE = "#FFFFFF"

F_OK = "#EDF4EF";  S_OK = "#6E9E7C"
F_NEXT = "#FBF3E6"; S_NEXT = "#C9A227"
F_TODO = "#F2F1EC"; S_TODO = "#BEB8A8"
F_BAD = "#FBEDEA";  S_BAD = "#C0674F"
F_INFO = "#EAF1F7"; S_INFO = "#8FB0CB"
F_DEV = "#F3EFF8";  S_DEV = "#B79BC9"

FONT = "'PingFang TC','Noto Sans TC','Microsoft JhengHei','Segoe UI',sans-serif"
MONO = "'SF Mono','JetBrains Mono','Consolas',monospace"


def esc(s: str) -> str:
    return (s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;"))


def text(x, y, s, fs=12, fill=INK, weight=400, mono=False, anchor="start", extra=""):
    fam = MONO if mono else FONT
    return (f'<text x="{x:.1f}" y="{y:.1f}" font-family="{fam}" font-size="{fs}" '
            f'font-weight="{weight}" fill="{fill}" text-anchor="{anchor}"{extra}>'
            f'{esc(s)}</text>')


def rect(x, y, w, h, fill=WHITE, stroke=RULE, sw=1, rx=6, extra=""):
    return (f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{rx}" '
            f'fill="{fill}" stroke="{stroke}" stroke-width="{sw}"{extra}/>')


def bar(x, y, w, h, fill, stroke, dur, delay=0.2):
    """會長大的 bar：width 由 0 動畫到 w。"""
    return (f'<rect x="{x:.1f}" y="{y:.1f}" width="0" height="{h:.1f}" rx="3" '
            f'fill="{fill}" stroke="{stroke}" stroke-width="1">'
            f'<animate attributeName="width" from="0" to="{w:.1f}" '
            f'dur="{dur}s" begin="{delay}s" fill="freeze"/></rect>')


def arrow(x1, y1, x2, y2, color=FAINT, sw=1.4):
    return (f'<path d="M {x1:.1f} {y1:.1f} L {x2:.1f} {y2:.1f}" fill="none" '
            f'stroke="{color}" stroke-width="{sw}" marker-end="url(#ah)"/>')


def box(x, y, w, h, title, lines, fill, stroke, tag=None, tagfill=None, fs=11.5):
    out = [rect(x, y, w, h, fill, stroke, 1.2)]
    if tag:
        out.append(rect(x + 10, y - 9, 52, 18, tagfill or stroke, tagfill or stroke, 0, 9))
        out.append(text(x + 36, y + 4, tag, 9.5, WHITE, 700, anchor="middle"))
    out.append(text(x + 12, y + 20, title, fs, INK, 700))
    for i, ln in enumerate(lines):
        out.append(text(x + 12, y + 39 + i * 15, ln, 10, INK2))
    return "".join(out)


def svg_open(h: int) -> list[str]:
    return [
        f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
        f'width="{W}" height="{h}" viewBox="0 0 {W} {h}">',
        "<defs>",
        f'<marker id="ah" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" '
        f'orient="auto-start-reverse"><path d="M 0 0 L 10 5 L 0 10 z" fill="{FAINT}"/></marker>',
        f'<marker id="ahg" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" '
        f'orient="auto-start-reverse"><path d="M 0 0 L 10 5 L 0 10 z" fill="{S_OK}"/></marker>',
        "</defs>",
        rect(0, 0, W, h, PAPER, PAPER, 0, 0),
    ]


# ── ① 緩存佔用 ────────────────────────────────────────────────────
def panel_cache(y: int) -> str:
    x, w = M, 800
    h = 372
    s = [rect(x, y, w, h, WHITE, RULE), text(x + 18, y + 28, "① 緩存佔用：過濾 × 瘦身", 15, INK, 700)]
    s.append(text(x + 18, y + 48,
                  f"entry：舊 {STORE_ENTRY} B（含 bytes[16]）→ 新 {ENTRY_SLIM} B（只存數值；字串唔入 state）",
                  10.5, MUTED))

    # 全部入 store 的極端情況用文字講（18 KB 會壓扁整條 bar 的刻度）
    s.append(rect(x + 18, y + 60, w - 36, 34, F_BAD, S_BAD, 1, 6))
    s.append(text(x + 30, y + 76,
                  f"如果全部 {KEY_TARGET} key 都入 store：{KEY_TARGET*STORE_ENTRY:,} B"
                  f"（＝上限的 {KEY_TARGET*STORE_ENTRY/(STORE_MAX*ENTRY_SLIM+4):.1f} 倍）→ 一定爆",
                  10.5, S_BAD, 700))
    s.append(text(x + 30, y + 90, "所以一定要 activation 過濾（S4）＋ entry 瘦身（S5，已做）", 9.5, INK2))

    bx, bw = x + 170, 470
    scale_max = 2500
    scale = bw / scale_max

    def row(yy, label, keys, entry, fill, stroke, note, delay, limit=None):
        wpx = keys * entry * scale
        out = [text(bx - 14, yy + 13, label, 11, INK2, 600, anchor="end")]
        out.append(rect(bx, yy, bw, 22, "#F4F2EC", RULE, 0.8, 3))
        out.append(bar(bx, yy, max(3, wpx), 22, fill, stroke, 1.4, delay))
        out.append(text(bx + max(3, wpx) + 8, yy + 15,
                        f"{keys} × {entry} B = {keys*entry:,} B", 10.5, stroke, 700))
        if limit is not None:
            out.append(text(bx + max(3, wpx) + 8, yy + 30, limit, 9, S_BAD))
        out.append(text(bx, yy + 36, note, 9.5, MUTED))
        return "".join(out)

    s.append(text(x + 18, y + 120, f"現時上限：{STORE_MAX} 筆 × {ENTRY_SLIM} B = {STORE_MAX*ENTRY_SLIM:,} B"
                                   f"（s_store 實測 {STORE_MAX*ENTRY_SLIM+4:,} B；舊 {STORE_MAX_OLD} 筆）",
                  10.5, INK, 700))
    s.append(row(y + 132, "過濾後（70 筆）", TYPICAL_ACTIVE, STORE_ENTRY, F_BAD, S_BAD,
                 "S4 只做過濾仍然爆：2,240 B > 772 B，而且 70 > 64 筆", 0.25,
                 limit="← 超上限"))
    s.append(row(y + 196, "瘦身後（70 筆）", TYPICAL_ACTIVE, ENTRY_SLIM, F_OK, S_OK,
                 f"入 {STORE_MAX} 筆上限（{STORE_MAX*ENTRY_SLIM+4:,} B），餘 {STORE_MAX-TYPICAL_ACTIVE} 筆", 0.55))
    s.append(row(y + 252, "板上實測（5 筆）", 5, ENTRY_SLIM, F_INFO, S_INFO,
                 "SuperMini 現時真正有值的 key（sys 3 ＋ device 2）", 0.85))

    # 上限虛線（舊 64 筆＋新 96 筆）
    for val, lab, col in ((STORE_MAX_OLD*ENTRY_SLIM, f"舊 {STORE_MAX_OLD*ENTRY_SLIM} B", S_BAD),
                          (STORE_MAX*ENTRY_SLIM+4, f"新 {STORE_MAX*ENTRY_SLIM+4:,} B", S_OK)):
        lx = bx + val * scale
        s.append(f'<line x1="{lx:.1f}" y1="{y+128}" x2="{lx:.1f}" y2="{y+286}" '
                 f'stroke="{col}" stroke-width="1.3" stroke-dasharray="5 4">'
                 f'<animate attributeName="opacity" values="1;0.4;1" dur="2.8s" repeatCount="indefinite"/></line>')
        s.append(text(lx + 4, y + 126, lab, 9, col))

    # FAT blob
    s.append(text(x + 18, y + 316, "FAT blob（state.ncst）：現在 50 B；LED_FMT 由 6 B 字串變 1 B index", 9.5, MUTED))
    s.append(text(x + 18, y + 332, "開機暫存 s_recs 768 B → PSRAM（s_recs 只剩 4 B pointer）；state 佔 SRAM 2,820→1,160 B",
                  9.5, MUTED))
    return "".join(s)


# ── ② 開機次序 ────────────────────────────────────────────────────
def panel_boot(y: int) -> str:
    x, w = M + 820, 772
    h = 372
    s = [rect(x, y, w, h, WHITE, RULE), text(x + 18, y + 28, "② 開機次序（B02 → B05 → B08）", 15, INK, 700)]
    s.append(text(x + 18, y + 48, f"實測：cfg parse {CFG_US} ms、state merge {STATE_US} ms（hit）", 10.5, MUTED))

    cx = x + 210
    steps = [
        (y + 62,  "B02", "parse /data/config/*.json", f"{CFG_US} ms → cfg structs", F_INFO, S_INFO),
        (y + 124, "state", "造 records（只 enabled unit）＋對拍", f"{STATE_US} ms；hit＝值一樣", F_OK, S_OK),
        (y + 186, "S2", "apply_to_cfg（override 蓋上）", "B05 之前；否則 pin／freq 靜默失效", F_NEXT, S_NEXT),
        (y + 248, "B05", "install driver（讀 cfg）", "I80／SPI／LED 真正初始化", F_DEV, S_DEV),
        (y + 310, "B08", "render on_start → nc_state_bind_*()", "loop 內直接讀位址，唔再查 key", F_DEV, S_DEV),
    ]
    bw, bh = 330, 52
    for i, (yy, tag, title, sub, f, st) in enumerate(steps):
        s.append(rect(cx - bw / 2, yy, bw, bh, f, st, 1.2))
        s.append(text(cx - bw / 2 + 12, yy + 16, tag, 9.5, st, 700, mono=True))
        s.append(text(cx - bw / 2 + 12, yy + 33, title, 11, INK, 600))
        s.append(text(cx - bw / 2 + 12, yy + 46, sub, 9.5, MUTED))
    # 主路徑：左側獨立軌道（唔會壓住 box 內文），再逐段入 box
    rail = cx - bw / 2 - 22
    ys = [y + 62, y + 124, y + 186, y + 248, y + 310]
    for i in range(len(ys) - 1):
        y_mid = (ys[i] + bh + ys[i + 1]) / 2
        s.append(f'<path d="M {rail:.1f} {ys[i]+bh/2:.1f} L {rail:.1f} {ys[i+1]+bh/2:.1f}" '
                 f'fill="none" stroke="{RULE}" stroke-width="1.2"/>')
        s.append(f'<path d="M {rail:.1f} {ys[i+1]+bh/2:.1f} L {cx-bw/2-2:.1f} {ys[i+1]+bh/2:.1f}" '
                 f'fill="none" stroke="{RULE}" stroke-width="1.2" marker-end="url(#ah)"/>')
        _ = y_mid
    path = f"M {rail:.1f} {ys[0]+bh/2:.1f} L {rail:.1f} {ys[-1]+bh/2:.1f}"
    s.append(f'<path id="bootpath" d="{path}" fill="none" stroke="{S_OK}" stroke-opacity="0.35" '
             f'stroke-width="2" stroke-dasharray="4 6">'
             f'<animate attributeName="stroke-dashoffset" from="0" to="-40" dur="1.1s" repeatCount="indefinite"/></path>')
    s.append(f'<circle r="5.5" fill="{S_OK}">'
             f'<animateMotion dur="5.5s" repeatCount="indefinite">'
             f'<mpath xlink:href="#bootpath"/></animateMotion></circle>')

    # 右側：activation 閘 + hit/miss 分支
    gx = x + w - 250
    s.append(rect(gx, y + 66, 232, 122, F_TODO, S_TODO, 1))
    s.append(text(gx + 12, y + 86, "activation 閘（S4a/S4b）", 10.5, INK, 700))
    for i, ln in enumerate(["enabled = cfg.enable && deps_ok",
                            "表：全部能力（flash XIP，0 RAM）",
                            "store：只載 enabled 的 key",
                            "未啟動 → 冇 key → 要改 config＋reboot"]):
        s.append(text(gx + 12, y + 105 + i * 19, ln, 9.5, INK2))
    s.append(arrow(gx - 18, y + 128, cx + bw / 2 + 4, y + 128, S_TODO, 1.2))

    s.append(rect(gx, y + 206, 232, 92, F_BAD, S_BAD, 1))
    s.append(text(gx + 12, y + 226, "miss 分支（值真係變）", 10.5, S_BAD, 700))
    for i, ln in enumerate(["重建 records → 寫 state.ncst",
                            "舊檔 → .bak（atomic）",
                            "下次開機再對拍 → hit"]):
        s.append(text(gx + 12, y + 245 + i * 17, ln, 9.5, INK2))
    return "".join(s)


# ── ③ 路線圖 ──────────────────────────────────────────────────────
def panel_roadmap(y: int) -> str:
    x, w = M, R - M
    h = 300
    s = [rect(x, y, w, h, WHITE, RULE), text(x + 18, y + 28, "③ 路線圖：由「能跑」到「覆蓋全部硬件」", 15, INK, 700)]

    steps = [
        ("S0", "key id u32 8/8/8/8", "PIN 48／P4 55 放得落", "done"),
        ("S1", "值集 CRC＋hit 只看值", "改註解唔清回寫值", "done"),
        ("S2", "apply_to_cfg（B05 前）", "override 真正生效", "next"),
        ("S3", "commit＋persist 位", "STATUS_SET 落盤", "todo"),
        ("S4", "activation 搬 boot-time", "store 只載 enabled", "todo"),
        ("S5", "大表補 9 類（+185）", "entry 12 B ✔｜RMT／ADC…", "todo"),
        ("S6", "模板＋preset 驗證", "565 key 可 seed", "todo"),
    ]
    cy = y + 96
    lane_x0, lane_x1 = x + 70, x + w - 70
    gap = (lane_x1 - lane_x0) / (len(steps) - 1)

    # 底線
    s.append(f'<line x1="{lane_x0:.1f}" y1="{cy}" x2="{lane_x1:.1f}" y2="{cy}" '
             f'stroke="{RULE}" stroke-width="3"/>')
    # 進度線（走到 S1）
    done_end = lane_x0 + gap
    s.append(f'<line x1="{lane_x0:.1f}" y1="{cy}" x2="{done_end:.1f}" y2="{cy}" '
             f'stroke="{S_OK}" stroke-width="4" stroke-linecap="round">'
             f'<animate attributeName="x2" from="{lane_x0:.1f}" to="{done_end:.1f}" dur="1.6s" fill="freeze"/></line>')

    colors = {"done": (F_OK, S_OK), "next": (F_NEXT, S_NEXT), "todo": (F_TODO, S_TODO)}
    for i, (tag, title, sub, st) in enumerate(steps):
        px = lane_x0 + i * gap
        f, sc = colors[st]
        # 節點
        s.append(f'<circle cx="{px:.1f}" cy="{cy}" r="13" fill="{f}" stroke="{sc}" stroke-width="2.2"/>')
        if st == "next":
            s.append(f'<circle cx="{px:.1f}" cy="{cy}" r="13" fill="none" stroke="{sc}" stroke-width="2">'
                     f'<animate attributeName="r" values="13;20;13" dur="2.2s" repeatCount="indefinite"/>'
                     f'<animate attributeName="opacity" values="0.9;0;0.9" dur="2.2s" repeatCount="indefinite"/></circle>')
        s.append(text(px, cy + 5, tag, 10.5, sc, 700, anchor="middle"))
        # 標籤
        s.append(text(px, cy + 40, title, 11, INK, 600, anchor="middle"))
        s.append(text(px, cy + 58, sub, 9.5, MUTED, anchor="middle"))
        # 狀態點
        mark = {"done": "✔ 完成", "next": "← 現在", "todo": "待做"}[st]
        s.append(text(px, cy + 82, mark, 9.5, sc if st != "todo" else MUTED, 700, anchor="middle"))

    # 四個不變量（用 pill 表示；跨步驟都唔准破）
    pills = [
        ("值一樣 → 唔重建", "省 762 ms WL 寫盤", S_OK, F_OK),
        ("未啟動 → 冇 key", "要改 config＋reboot", S_BAD, F_BAD),
        ("改註解 → 唔清 override", "值集 CRC，唔係 bytes", S_INFO, F_INFO),
        ("每步 25/25 ＋ 上板一項", "唔靜默降級（鐵律 9）", S_NEXT, F_NEXT),
    ]
    pw2 = (w - 36 - 3 * 14) / 4
    py = y + 196
    for i, (t1, t2, sc, fc) in enumerate(pills):
        px = x + 18 + i * (pw2 + 14)
        s.append(rect(px, py, pw2, 46, fc, sc, 1.1, 8))
        s.append(text(px + 12, py + 20, t1, 10.5, INK, 700))
        s.append(text(px + 12, py + 36, t2, 9.5, MUTED))

    s.append(text(x + 18, y + h - 22,
                  "規則：每步一個 commit、25/25 host test、上板驗一項；上面四個不變量跨步驟都唔准破。",
                  10, MUTED))
    return "".join(s)


def build() -> str:
    h = 900
    s = svg_open(h)
    s.append(text(M, 62, "統一狀態系統：緩存、啟動次序、路線圖", 26, INK, 800))
    s.append(text(M, 90,
                  "2026-11-27 · NCSTATE · key id u32（8/8/8/8）· 值集 CRC · activation 過濾（未做）"
                  " · 圖由 tools/state_diagram.py 生成 · 開圖有動畫：bar 生長／光點沿開機路徑／S2 呼吸",
                  11.5, MUTED))
    s.append(text(R, 62, "esp_Net-Core", 12, FAINT, 600, mono=True, anchor="end"))
    s.append(panel_cache(124))
    s.append(panel_boot(124))
    s.append(panel_roadmap(520))
    # 底部一句
    s.append(text(M, 860,
                  f"現時 schema {KEY_NOW} key（4 unit）；大表 {KEY_MATRIX}；補齊 {KEY_TARGET}。"
                  f" entry {STORE_ENTRY}→{ENTRY_SLIM} B；s_store 2,052→{STORE_MAX*ENTRY_SLIM+4:,} B（{STORE_MAX} 筆）；"
                  f"開機暫存搬 PSRAM → state 佔 SRAM 2,820→{STORE_MAX*ENTRY_SLIM+4+4:,} B。",
                  11, INK2))
    s.append("</svg>")
    return "\n".join(s)


def main() -> int:
    if not SCHEMA.exists():
        print(f"找唔到 {SCHEMA}", file=sys.stderr)
        return 1
    data = json.loads(SCHEMA.read_text(encoding="utf-8"))
    keys = len(data.get("keys", []))
    if keys != KEY_NOW:
        print(f"注意：schema 有 {keys} 個 key（圖寫 {KEY_NOW}）→ 更新 KEY_NOW", file=sys.stderr)
    OUT.write_text(build(), encoding="utf-8")
    print(f"wrote {OUT.relative_to(ROOT)}（{OUT.stat().st_size} B）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
