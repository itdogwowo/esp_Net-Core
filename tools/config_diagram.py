#!/usr/bin/env python3
"""生成 config 行為圖（docs/00_pm/config-layers.svg、schema-registry.svg）。

    python tools/config_diagram.py            # 兩張都出
    python tools/config_diagram.py --only a   # 只出 config-layers

為什麼要有生成器：圖上的 key 數、範圍、量測數字全部來自程式／schema，
改完 code 重跑，圖唔會同實作講兩套。

圖 A（config-layers）答：
    ① 設定由 build 到 runtime 經過哪幾層？邊層最高權威？
    ② 開機 B02 做咩？cfg struct 之後邊個消費者讀？
    ③ state.ncst 同 RAM store 是咩角色（快取，不是權威）？
    ④ 三種改法（FAT JSON／build 預置／runtime set）分別幾時生效？

圖 B（schema-registry）答：
    ① 可以設定什麼（units／keys／範圍）——直接讀 schema/state-keys.json。
    ② 一個 JSON 怎樣變成 runtime 表＋API（生成物／--check）。
    ③ 誰在讀寫（driver／boot 對拍／status／test）。
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCHEMA = ROOT / "schema" / "state-keys.json"
OUT_A = ROOT / "docs/00_pm/config-layers.svg"
OUT_B = ROOT / "docs/00_pm/schema-registry.svg"

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

F_BUILD = "#F3EFF8"; S_BUILD = "#B79BC9"   # build 層（L0／預置）
F_LIVE = "#EAF1F7";  S_LIVE = "#8FB0CB"    # FAT live（L1）
F_CACHE = "#EDF4EF"; S_CACHE = "#94B7A2"   # state 快取
F_USE = "#F5F4EF";   S_USE = "#BEB8A8"     # cfg struct／consumer
F_WARN = "#FBF3E6";  S_WARN = "#C9A227"    # 「要 rebuild」提示

SANS = "'Segoe UI','Noto Sans TC','Microsoft JhengHei','PingFang TC',sans-serif"
MONO = "'Cascadia Mono','Consolas','SF Mono',monospace"

out: list[str] = []


def esc(s: str) -> str:
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def cw(ch: str, fs: float, mono: bool = False) -> float:
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


def measure(s: str, fs: float, mono: bool = False) -> float:
    return sum(cw(c, fs, mono) for c in s)


def rect(x, y, w, h, fill=WHITE, stroke=RULE, sw=1, rx=6, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    out.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{rx}" '
               f'fill="{fill}" stroke="{stroke}" stroke-width="{sw}"{d}/>')


def text(x, y, s, fs=12, fill=INK, weight=400, mono=False, anchor="start"):
    fam = MONO if mono else SANS
    out.append(f'<text x="{x:.1f}" y="{y:.1f}" font-family="{fam}" font-size="{fs}" '
               f'font-weight="{weight}" fill="{fill}" text-anchor="{anchor}">{esc(s)}</text>')


def wrap(x, y, s, fs=10.5, width=None, fill=INK2, lh=15, weight=400, mono=False):
    """自動折行（以 CJK 寬度估算）；回傳最後一行的 y。"""
    limit = width if width is not None else (R - M - x)
    line = ""
    for ch in s:
        if measure(line + ch, fs, mono) > limit and line:
            text(x, y, line, fs, fill, weight, mono)
            y += lh
            line = ch
        else:
            line += ch
    if line:
        text(x, y, line, fs, fill, weight, mono)
        y += lh
    return y


def arrow_down(x, y1, y2, color=FAINT):
    out.append(f'<line x1="{x:.1f}" y1="{y1:.1f}" x2="{x:.1f}" y2="{y2 - 6:.1f}" '
               f'stroke="{color}" stroke-width="1.4"/>')
    out.append(f'<path d="M {x - 4:.1f} {y2 - 7:.1f} L {x:.1f} {y2 - 1:.1f} '
               f'L {x + 4:.1f} {y2 - 7:.1f}" fill="none" stroke="{color}" stroke-width="1.4"/>')


def arrow_right(x1, y, x2, color=FAINT):
    out.append(f'<line x1="{x1:.1f}" y1="{y:.1f}" x2="{x2 - 6:.1f}" y2="{y:.1f}" '
               f'stroke="{color}" stroke-width="1.4"/>')
    out.append(f'<path d="M {x2 - 7:.1f} {y - 4:.1f} L {x2 - 1:.1f} {y:.1f} '
               f'L {x2 - 7:.1f} {y + 4:.1f}" fill="none" stroke="{color}" stroke-width="1.4"/>')


def svg_open(h):
    out.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{h}" '
               f'viewBox="0 0 {W} {h}" font-family="{SANS}">')
    rect(0, 0, W, h, PAPER, PAPER, 0, 0)


def legend(y, items):
    lx = M
    for label, f, s in items:
        rect(lx, y, 13, 11, f, s, 1, 3)
        text(lx + 19, y + 10, label, 10.5, MUTED)
        lx += 19 + measure(label, 10.5) + 26


def card(x, y, w, h, title, lines, fill, stroke, tag=None, tag_color=None):
    rect(x, y, w, h, fill, stroke, 1.3)
    text(x + 14, y + 23, title, 12.5, INK, 700)
    if tag:
        tw = measure(tag, 9.5) + 14
        rect(x + w - tw - 12, y + 10, tw, 17, WHITE, tag_color or stroke, 1, 8)
        text(x + w - tw - 5, y + 22, tag, 9.5, tag_color or stroke, 600)
    ty = y + 44
    for ln in lines:
        ty = wrap(x + 14, ty, ln, 10.5, width=w - 28, lh=15)
    return y + h


# ───────────────────────── 圖 A：config 行為 ─────────────────────────
def build_layers():
    global out
    out = []
    H = 1030
    svg_open(H)
    text(M, 52, "設定從哪裡來、哪一步生效：build 事實 → 預置 → FAT live → cfg → state 快取", 25, INK, 600)
    text(M, 78, "顏色 = 這層幾時定；箭頭 = 資料流；虛線 = 只在某個條件下走。"
                "最右的 FAT live 永遠最高權威，state.ncst 只是快取。", 12.5, MUTED)
    legend(92, [("build 層（L0：改它要 rebuild）", F_BUILD, S_BUILD),
                ("FAT live（L1：改完 reboot）", F_LIVE, S_LIVE),
                ("state 快取（可重建）", F_CACHE, S_CACHE),
                ("cfg struct／消費者", F_USE, S_USE),
                ("要 rebuild 才生效", F_WARN, S_WARN)])

    # ① 三個來源
    y = 150
    text(M, y - 12, "① 三個來源（權威由右至左：FAT live > build 預置 > 出廠 const）", 11.5, MUTED, 600)
    x1, w1 = M, 470
    card(x1, y, w1, 112, "硬體事實（L0）", [
        "board.json（flash／PSRAM／CPU／console）＋sdkconfig.defaults",
        "wiring.json（腳位）＋driver.json（LED device）",
        "→ ncportgen → nc_port.c（wiring／LED／activation const 表）",
    ], F_BUILD, S_BUILD, tag="rebuild")

    x2 = x1 + w1 + 20
    card(x2, y, w1, 112, "build 預置（嵌進固件）", [
        "ports/project/<chip>/<board>/data/config/*.json",
        "project.json 的 seed[]：hardware／system／network／tasks（＋driver／resources）",
        "只在裝置缺檔時寫入；已有檔案唔會被覆蓋",
    ], F_BUILD, S_BUILD, tag="rebuild")

    x3 = x2 + w1 + 20
    w3 = R - x3
    card(x3, y, w3, 112, "FAT live（/data/config/*.json，最高權威）", [
        "USB MSC／WebMaster／NC4 FILE 改；檔案存在就用它，缺欄位才回預設",
        "改完 reboot 生效（目前全部設定都係「重啟後生效」）",
        "壞檔 → 整層用預設（RAM）＋warn，**唔會改你嘅檔案**",
    ], F_LIVE, S_LIVE, tag="reboot 生效", tag_color=S_LIVE)

    # B02
    y2 = 292
    for cx in (x1 + w1 / 2, x2 + w1 / 2, x3 + w3 / 2):
        arrow_down(cx, y + 112, y2)
    card(M, y2, R - M, 112, "B02 nc_cfg_boot()：一次讀齊、型別化，之後全系統只讀 struct", [
        "開檔（system → hardware → tasks → resources → driver）→ cJSON → 型別化 struct；"
        "缺檔重建骨架、壞檔整層預設、缺欄位用預設（計 miss／bad／inv／unk）",
        "讀檔同時計 src_hash（表驅動 CRC）——state.ncst 用它判斷「config 有冇改過」",
        "實測 Matrix：5 個 JSON＋型別化＋hash ≈ 30.1 ms（B02 總共 64.6 ms）",
    ], F_USE, S_USE)

    # cfg structs
    y3 = 432
    text(M, y3 - 12, "② cfg struct（RAM）：之後所有人只讀呢層，唔再碰 JSON", 11.5, MUTED, 600)
    structs = [
        ("nc_system_cfg_t", ["links[]：kind／enable／port／rx", "routes[]：in → decodeN", "frame_interval_ms、num_pixels"]),
        ("nc_tasks_cfg_t", ["每個租戶的 level／affinity", "缺席自動補 [0,0]", "核心任務唔受管"]),
        ("nc_resources_cfg_t", ["buf／cmd／hw 的提供與需求", "缺檔 → 固件預設", "模組 provides 會合併"]),
        ("nc_hardware_cfg_t", ["ETH／SDIO／PIN／PWM／UART", "I80／SPI（typed）", "I2C／I2S／ENC 未 typed"]),
        ("nc_devices_cfg_t", ["driver.json 的 LED 段", "enable／backend／lanes／px／fmt", "其餘 device 未實作"]),
    ]
    n = len(structs)
    gap = 16
    bw = (R - M - gap * (n - 1)) / n
    for i, (title, lines) in enumerate(structs):
        bx = M + i * (bw + gap)
        card(bx, y3, bw, 96, title, lines, F_USE, S_USE)
        if i:
            arrow_right(bx - gap - 2, y3 + 48, bx - 2, INK2)

    # consumers
    y4 = 560
    text(M, y4 - 12, "③ 消費者：cfg 決定「有咩、幾大」；state 可以覆寫「值」", 11.5, MUTED, 600)
    uses = [
        ("nc_net_lines", ["links[].enable=0 → 唔宣告緩衝、唔建 bus", "iface 對唔上 → 跳過＋warn"]),
        ("nc_task", ["level=-1 → 唔 declare／唔開 loop", "affinity [0,0] → 同上"]),
        ("render_task（LED）", ["先 cfg／driver 預設，再用 state store 覆寫", "10 個 LED key；冇 key 就保留 cfg"]),
        ("activation（build）", ["nc_port.c 的 const 表：邊個 unit enabled", "改 enable 一定要 rebuild"]),
        ("status cfg", ["miss／bad／inv／unk、t{cfg_us,state_us}", "act{fat,match}、state{hit...}、schema"]),
    ]
    for i, (title, lines) in enumerate(uses):
        bx = M + i * (bw + gap)
        card(bx, y4, bw, 96, title, lines, F_USE, S_USE)
    for i in range(n):
        arrow_down(M + i * (bw + gap) + bw / 2, y3 + 96, y4, RULE)

    # state
    y5 = 690
    rect(M, y5, R - M, 162, WHITE, RULE, 1)
    text(M + 16, y5 + 26, "B02 之後：nc_state_boot_sync() —— 把 config 派生值寫成 state.ncst（快取）", 12.5, INK, 700)
    bx = M + 16
    card(bx, y5 + 40, 360, 84, "state.ncst（NCSTATE）", [
        "sparse：只存同預設唔同的 key（Matrix 18 筆／124 B）",
        "header 26 B＋自描述 record；有 CRC／src_hash／.bak",
    ], F_CACHE, S_CACHE)
    arrow_right(bx + 360 + 6, y5 + 88, bx + 420, INK2)

    bx2 = bx + 420
    card(bx2, y5 + 40, 360, 84, "RAM store（開機後常駐）", [
        "固定陣列 64 筆（~430 B internal、PSRAM 0）",
        "driver 經 nc_state_api 讀寫；set 只改這裡＋dirty",
    ], F_CACHE, S_CACHE)
    arrow_right(bx2 + 360 + 6, y5 + 88, bx2 + 420, INK2)

    bx3 = bx2 + 420
    card(bx3, y5 + 40, R - bx3 - 16, 84, "nc_state_api／schema（讀寫唯一入口）", [
        "get_u32/i32/bool/str、set_*（schema 驗型別＋範圍）",
        "查表：nc_state_key(id)／nc_state_key_name()",
    ], F_CACHE, S_CACHE)

    chips = [
        ("hit：hash 對＋值全中 → 唔寫（8.2 ms）", S_CACHE),
        ("值一樣但 hash 變 → 唔寫（8.3 ms）", S_CACHE),
        ("值有變／壞檔 → 重建（762 ms，一次性）", S_WARN),
    ]
    cx = M + 16
    for label, col in chips:
        tw = measure(label, 10) + 18
        rect(cx, y5 + 160 - 22, tw, 20, WHITE, col, 1, 10)
        text(cx + 9, y5 + 160 - 8, label, 10, col, 600)
        cx += tw + 12

    # 三種改法
    y6 = 884
    text(M, y6 - 10, "④ 三種改法（選錯層 = 改完唔生效）", 11.5, MUTED, 600)
    ways = [
        ("改 FAT live", ["USB／WebMaster 改 /data/config/*.json", "→ reboot；最高權威", "驗證：status cfg／status tasks"], F_LIVE, S_LIVE),
        ("改 build 預置", ["ports/project/.../data/config 或 sdkconfig.defaults", "→ rebuild（改 defaults 要 --fresh）", "→ OTA；再 fs_format 才會重新 seed"], F_WARN, S_WARN),
        ("runtime set（API）", ["nc_state_set_u32/i32/bool/str", "只改 RAM＋dirty；reboot 由 JSON 重算", "永久改仍然要改 JSON"], F_CACHE, S_CACHE),
    ]
    ww = (R - M - 2 * 20) / 3
    for i, (title, lines, f, s) in enumerate(ways):
        bx = M + i * (ww + 20)
        card(bx, y6, ww, 104, title, lines, f, s)

    text(M, 1006, "權威鏈：FAT live（L1）> build 預置 > 出廠 const（L0）；state.ncst 係派生快取，"
                  "任何時候都可以由前兩者重建。", 11, MUTED)
    out.append("</svg>\n")
    OUT_A.write_text("\n".join(out), encoding="utf-8")
    print(f"wrote {OUT_A.relative_to(ROOT)}")


# ───────────────────── 圖 B：schema registry ─────────────────────
def load_schema():
    data = json.loads(SCHEMA.read_text(encoding="utf-8"))
    units = {u["name"]: u for u in data.get("units", [])}
    groups: dict[str, list[dict]] = {name: [] for name in units}
    for k in data.get("keys", []):
        groups.setdefault(k["unit"], []).append(k)
    return data, units, groups


def fmt_range(k: dict) -> str:
    typ = k["type"]
    if typ == "enum":
        vals = k.get("values", [])
        return f"enum {len(vals)} 值（{vals[0]}…{vals[-1]}）" if vals else "enum"
    if typ == "str":
        return f"str ≤ {k.get('max_len', 0)} 字"
    return f"{k.get('min')}..{k.get('max')}"


def build_registry():
    global out
    out = []
    data, units, groups = load_schema()
    n_keys = len(data.get("keys", []))
    n_units = len(data.get("units", []))
    H = 900
    svg_open(H)
    text(M, 52, "schema registry：一份 JSON 決定「可以設定什麼」，生成的表＋API 負責讀寫", 25, INK, 600)
    text(M, 78, f"schema/state-keys.json v{data.get('version')}：{n_keys} 個 key、{n_units} 個 unit。"
                "改能力只改這個檔 → 跑生成器 → rebuild；firmware 冇第二份名單。", 12.5, MUTED)
    legend(92, [("schema 來源／生成物", F_BUILD, S_BUILD),
                ("runtime 表／API", F_CACHE, S_CACHE),
                ("消費者", F_USE, S_USE),
                ("未入 schema（仍然係 cfg struct）", F_WARN, S_WARN)])

    # 左：來源 → 生成
    y = 132
    text(M, y - 12, "① 唯一來源 → 生成三份 C 檔", 11.5, MUTED, 600)
    lw = 420
    card(M, y, lw, 104, "schema/state-keys.json", [
        "每個 key：id／unit／type／min／max／enum／path",
        "生成器驗：unit nibble、範圍、enum 連續、id／名重複",
    ], F_BUILD, S_BUILD)
    arrow_down(M + lw / 2, y + 104, y + 140)
    card(M, y + 140, lw, 84, "tools/gen_state_keys.py", [
        "無參數 → 寫三份檔；--check → 驗同步（run_all 開頭跑）",
    ], F_BUILD, S_BUILD)
    arrow_down(M + lw / 2, y + 224, y + 260)
    files = [
        ("nc_state_keys.h", ["id ＋ NC_UNIT_*（C 名）"]),
        ("nc_state_schema_data.[ch]", ["runtime 表：型別／min／max／enum／path"]),
        ("nc_state_schema + nc_state_api", ["查表／驗範圍；get_*／set_*"]),
    ]
    fy = y + 260
    for i, (title, lines) in enumerate(files):
        card(M, fy, lw, 70, title, lines, F_CACHE if i == 2 else F_BUILD,
             S_CACHE if i == 2 else S_BUILD)
        fy += 82

    # 中：units
    mx = M + lw + 40
    mw = 760
    text(mx, y - 12, "② 可以設定什麼（直接由 schema 生成，唔會漂移）", 11.5, MUTED, 600)
    order = [u["name"] for u in sorted(data["units"], key=lambda x: x["id"])]
    cw2 = (mw - 16) / 2
    ch2 = 150
    for idx, uname in enumerate(order):
        u = units[uname]
        keys = groups.get(uname, [])
        gx = mx + (idx % 2) * (cw2 + 16)
        gy = y + (idx // 2) * (ch2 + 16)
        rect(gx, gy, cw2, ch2, WHITE, S_USE, 1.2)
        text(gx + 12, gy + 22, f"{uname}（{len(keys)} key）", 12.5, INK, 700)
        text(gx + 12, gy + 40, u.get("desc", ""), 10, MUTED)
        ly = gy + 60
        for k in keys[:4]:
            label = k["name"].replace("NC_KEY_", "")
            text(gx + 12, ly, label, 10.5, INK2, 600, mono=True)
            text(gx + 12 + 190, ly, fmt_range(k), 10, MUTED, mono=True)
            ly += 17
        if len(keys) > 4:
            text(gx + 12, ly, f"… 其餘 {len(keys) - 4} 個", 10, FAINT)

    # 右：消費者
    rx = mx + mw + 40
    rw = R - rx
    text(rx, y - 12, "③ 誰在讀寫", 11.5, MUTED, 600)
    consumers = [
        ("render_task（LED）", ["10 個 LED key：先 cfg，再用 state 覆寫"]),
        ("boot 對拍", ["每筆 record 對 schema；bad≠0 = code／schema drift"]),
        ("status cfg", [f"{{ver,crc,keys:{n_keys},units:{n_units},bad,dirty}}"]),
        ("host test", ["stateSchema_test：查表／型別／範圍／enum／讀寫"]),
        ("config UI（未來）", ["查 nc_state_key(id) 拿型別／範圍再顯示"]),
    ]
    cy = y
    for title, lines in consumers:
        card(rx, cy, rw, 76, title, lines, F_USE, S_USE)
        cy += 88

    # 中段：讀寫語義（get／set／失敗碼／dirty）
    ay = 470
    rect(mx, ay, mw, 168, WHITE, S_CACHE, 1.2)
    text(mx + 14, ay + 26, "nc_state_api：讀寫的唯一入口（型別／範圍由 schema 決定）", 12.5, INK, 700)
    text(mx + 14, ay + 48, "get_u32／i32／bool／str：schema 型別對唔上、sparse 冇值 → false（呼叫端保留自己的預設）",
         10.5, INK2)
    text(mx + 14, ay + 68, "set_u32／i32／bool／str：先驗型別＋範圍＋enum＋str 長度；成功改 RAM store＋dirty",
         10.5, INK2)
    text(mx + 14, ay + 88, "失敗碼（明示，唔靜默截斷）：unknown_key／wrong_type／out_of_range／not_ready／no_space",
         10.5, INK2)
    text(mx + 14, ay + 108, "持久化：唔會自動寫盤；reboot 由 /data/config/*.json 重算（L1 仍然最高權威）",
         10.5, MUTED)
    code_y = ay + 128
    label = "status cfg → \"schema\":{\"ver\":2,\"crc\":…,\"keys\":25,\"units\":4,\"bad\":0,\"dirty\":0}"
    tw = measure(label, 10, mono=True) + 20
    rect(mx + 14, code_y, tw, 26, F_CACHE, S_CACHE, 1, 6)
    text(mx + 24, code_y + 17, label, 10, INK2, 600, mono=True)

    # 底部：三種設定方法 + 未入 schema
    y2 = 700
    text(M, y2 - 10, "④ 怎樣設定（同圖 A 對應）", 11.5, MUTED, 600)
    ways = [
        ("FAT JSON（最高權威）", ["/data/config/*.json 改值 → reboot", "driver 由 state 讀，cfg 由 JSON 來"], F_LIVE, S_LIVE),
        ("build 預置／Kconfig", ["ports/project/.../data/config／sdkconfig.defaults", "rebuild（--fresh）；再 seed 才上板"], F_WARN, S_WARN),
        ("runtime API（RAM）", ["nc_state_set_*：驗 schema、改 RAM＋dirty", "reboot 仍然以 JSON 為權威"], F_CACHE, S_CACHE),
    ]
    ww = (R - M - 2 * 20) / 3
    for i, (title, lines, f, s) in enumerate(ways):
        bx = M + i * (ww + 20)
        card(bx, y2, ww, 92, title, lines, f, s)

    text(M, y2 + 112, "schema 未覆蓋（仍然係 cfg struct，改完 reboot 生效）："
                      "links／routes（system.json）、tasks level／affinity（tasks.json）、resources、"
                      "ETH／SDIO／PIN／PWM／UART（hardware.json）。", 10.5, MUTED)
    text(M, y2 + 134, "加一個 key：schema json 加一行 → gen_state_keys.py → rebuild；"
                      "boot 要寫入就 nc_state_boot.c 的 build_records() 加一行取值。", 10.5, MUTED)
    out.append("</svg>\n")
    OUT_B.write_text("\n".join(out), encoding="utf-8")
    print(f"wrote {OUT_B.relative_to(ROOT)}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", choices=["a", "b"], help="只出其中一張")
    args = ap.parse_args()
    if args.only in (None, "a"):
        build_layers()
    if args.only in (None, "b"):
        build_registry()
    return 0


if __name__ == "__main__":
    sys.exit(main())
