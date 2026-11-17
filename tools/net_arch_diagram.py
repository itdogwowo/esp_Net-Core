#!/usr/bin/env python3
"""Generate docs/00_pm/network-lines.svg — 網絡線路層（Line → Service → decoder）。

Single source of truth: 手寫內容清單 + 固定幾何，輸出獨立 SVG（無外部字型／圖片）。
    python tools/net_arch_diagram.py [out.svg]

2026-11-16 晚更新：line schema 回到 composite kind（eth_tcp／wifi_tcp）＋ index；
config 不寫 iface／service；name 由 kind+index 生成（可 name 覆寫）。service 仍由
well-known port claim（80=HTTP、8080=WS、其他=NC4）；HTTP 只做 static，冇 /config.json、
冇 /ota；WS 自寫 handshake／frame 之後照樣入 nc4_bus → decoder。
"""
import sys
from pathlib import Path
from html import escape

W, H = 1800, 1340
PAPER = "#F7F5EF"
INK = "#17191C"
INK2 = "#3D4247"
MUTED = "#6E747B"
FAINT = "#9AA0A6"
RULE = "#D8D3C8"
WHITE = "#FFFFFF"
C_CFG = "#8A5A20"
C_IF = "#2E6B4F"
C_LINE = "#1F5F8B"
C_SVC = "#6B4C8A"
C_HTTP = "#A04A3A"
C_OK = "#2E7D4F"

SANS = "'Noto Sans TC','PingFang TC','Microsoft JhengHei','Segoe UI',sans-serif"
MONO = "'SF Mono','Cascadia Mono','Consolas',monospace"

out = []


def add(s):
    out.append(s)


def esc(s):
    return escape(str(s))


def text(x, y, s, fs=14, fill=INK, weight="400", anchor="start", mono=False, ls=0):
    fam = MONO if mono else SANS
    add(f'<text x="{x}" y="{y}" font-family="{fam}" font-size="{fs}" '
        f'fill="{fill}" font-weight="{weight}" text-anchor="{anchor}" letter-spacing="{ls}">{esc(s)}</text>')


def rect(x, y, w, h, fill=WHITE, stroke=RULE, rx=8, sw=1, op=1):
    add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" '
        f'fill-opacity="{op}" stroke="{stroke}" stroke-width="{sw}"/>')


def line(x1, y1, x2, y2, color=RULE, sw=1.2, dash=None, arrow=False):
    d = f' stroke-dasharray="{dash}"' if dash else ''
    m = ' marker-end="url(#arrow)"' if arrow else ''
    add(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{color}" '
        f'stroke-width="{sw}"{d}{m} stroke-linecap="round"/>')


def pill(x, y, w, h, label, color, fs=11):
    rect(x, y, w, h, fill=color, stroke=color, rx=h / 2, sw=0, op=.10)
    text(x + w / 2, y + h / 2 + fs * .36, label, fs=fs, fill=color, weight="600", anchor="middle")


def panel(x, y, w, h, tag, title, sub=None):
    rect(x, y, w, h, fill=WHITE, op=.7)
    text(x + 20, y + 26, tag, fs=11, fill=FAINT, mono=True, ls=1.6)
    text(x + 20, y + 52, title, fs=16, weight="600")
    if sub:
        text(x + 20, y + 72, sub, fs=11.5, fill=MUTED)


add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">')
add('<defs><marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" '
    'orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="#5F6B76"/></marker></defs>')
add(f'<rect width="{W}" height="{H}" fill="{PAPER}"/>')

# ── header ──────────────────────────────────────────────────────────────
text(50, 62, "esp_Net-Core — 網絡線路與 service", fs=32, weight="700")
text(50, 90, "config（links／routes）→ Line（composite kind＋index）→ Service claim（NC4｜WS｜HTTP）→ decoder（per core）",
     fs=15, fill=INK2)
text(1750, 54, "2026-11-16 · 對照 nc_net_lines.c／nc4_web.c", fs=12, fill=FAINT, anchor="end")
line(50, 108, 1750, 108)

# ── 01 CONFIG ───────────────────────────────────────────────────────────
panel(50, 126, 1700, 160, "01  CONFIG", "line 全部由 config 決定：composite kind＋index，interface 已包含在 kind",
      "冇 name → 由 kind＋index 生成（wifi_tcp1）；routes 用生成名；name 仍可覆寫。")
rect(70, 210, 780, 58, fill="#FBF7EE", stroke="#E4D8C2", rx=5)
text(84, 232, '{"kind":"wifi_tcp","index":1,"port":6000}', fs=12.5, mono=True, fill=C_CFG)
text(84, 250, '→ name wifi_tcp1；rx 用預設（TCP 2×8205）。route 用 wifi_tcp1。', fs=12.5, mono=True, fill=C_CFG)
rect(870, 210, 860, 58, fill="#F3F6F9", stroke="#D5DFE8", rx=5)
text(884, 232, "managed line：composite kind → nc_net_lines 接管；唔再寫 iface／service。", fs=12.3, fill=C_LINE)
text(884, 250, "nc4.routes[].out 決定 decodeN；指到不存在 link 的 out 只忽略該格。", fs=12.3, fill=C_LINE)

# ── 02 LINE ─────────────────────────────────────────────────────────────
text(50, 330, "02  LINE", fs=11, fill=FAINT, mono=True, ls=1.6)
text(50, 356, "一條 managed line = 一個 listener／task／rx 緩衝；service 決定邊個消費", fs=16, weight="600")
cards = [
    ("TCP", "6000", "NC4", "socket · stream", "line slots → nc4_bus", "core = routes[].out"),
    ("UDP", "6001", "NC4", "socket · datagram", "line slots → nc4_bus", "core = routes[].out"),
    ("WS", "8080", "WS", "自寫 handshake／binary frame", "frame → nc4_bus", "不經 httpd"),
    ("HTTP", "80", "HTTP", "httpd", "靜態 /data/www", "唔經 NC4／decoder"),
]
x0, y0, cw, ch, gap = 50, 372, 410, 140, 20
for i, (kind, port, svc, sub, path, note) in enumerate(cards):
    x = x0 + i * (cw + gap)
    col = C_HTTP if kind == "HTTP" else C_LINE
    rect(x, y0, cw, ch, fill=WHITE, stroke=col, sw=1.3)
    rect(x, y0, 5, ch, fill=col, stroke=col, rx=0)
    text(x + 18, y0 + 30, kind, fs=18, weight="700", fill=col)
    text(x + cw - 18, y0 + 30, ":" + port, fs=14, fill=MUTED, anchor="end", mono=True)
    pill(x + 18, y0 + 42, 78, 22, svc, col, fs=11)
    text(x + 106, y0 + 58, sub, fs=12, fill=INK2)
    text(x + 18, y0 + 88, path, fs=11.5, fill=MUTED, mono=True)
    text(x + 18, y0 + 110, note, fs=11, fill=FAINT)
    if kind == "WS":
        pill(x + cw - 122, y0 + 42, 104, 22, "NO HTTPD", C_OK, fs=10)
    if kind == "HTTP":
        pill(x + cw - 132, y0 + 42, 114, 22, "STATIC ONLY", C_HTTP, fs=10)

# ── 03 SERVICE CLAIM ────────────────────────────────────────────────────
panel(50, 540, 1700, 190, "03  SERVICE CLAIM", "service 係「邊個消費這條線」，唔係 line 自己的一種 kind")
rows = [
    ("tcp :80", "HTTP（core 0）", "httpd；GET/HEAD /data/www/* ＋ fallback 頁；冇任何寫入 endpoint", C_HTTP),
    ("tcp :8080　or　kind=ws", "WS（core = routes）", "自寫 handshake／binary frame；frame 入 nc4_bus → decoder", C_OK),
    ("其他 tcp／udp", "NC4（core = routes）", "nc4_bus push slot → parser 逐幀 → decoder → action", C_LINE),
]
ry = 604
for cond, svc, behave, col in rows:
    rect(70, ry, 1660, 30, fill="#FCFBF7", stroke=RULE, rx=4)
    rect(70, ry, 4, 30, fill=col, stroke=col, rx=2)
    text(86, ry + 20, cond, fs=12.5, mono=True, fill=INK)
    text(410, ry + 20, svc, fs=12.5, fill=col, weight="600")
    text(760, ry + 20, behave, fs=12, fill=INK2)
    ry += 34
text(70, 724, "claim 現階段用 well-known port（80→HTTP、8080→WS、其他→NC4）；config 已冇 service 欄位。"
     "日後改成 service registry／claim API。", fs=11.2, fill=MUTED)

# ── 04A NC4 PATH ────────────────────────────────────────────────────────
panel(50, 736, 840, 246, "04A  NC4 PATH", "bytes → line slot → nc4_bus → parser → decoder → action")
steps = [
    ("socket／WS frame", "line task 讀", C_LINE),
    ("line slots", "links[].rx", C_SVC),
    ("nc4_bus", "PUSH · parser", C_SVC),
    ("decoder 0/1", "routes 指定", C_LINE),
    ("action → reply", "同一條線", C_OK),
]
sx, sy, bw, bh, sgap = 70, 806, 142, 78, 18
for i, (t, s, c) in enumerate(steps):
    x = sx + i * (bw + sgap)
    rect(x, sy, bw, bh, fill=WHITE, stroke=c, rx=6)
    text(x + 12, sy + 28, t, fs=12.5, weight="700", fill=c)
    text(x + 12, sy + 50, s, fs=10.5, fill=MUTED)
    if i < len(steps) - 1:
        line(x + bw + 3, sy + bh / 2, x + bw + sgap - 5, sy + bh / 2, color="#5F6B76", arrow=True)
text(70, 920, "TCP／UDP／WS 都走這條路；WS framing 在 line task，NC4 解析在 decoder core。", fs=11.5, fill=MUTED)
text(70, 942, "跨核轉寄由 routes[].out 的 link 決定（fwd.c0／c1）；kick 永遠同核。", fs=11.5, fill=MUTED)
text(70, 964, "HTTP 唔行這條路 —— 見 04B。", fs=11.5, fill=FAINT)

# ── 04B WEB PATH ────────────────────────────────────────────────────────
panel(920, 736, 830, 246, "04B  WEB PATH", "HTTP :80 → httpd → static handler（冇 NC4）")
rect(940, 806, 210, 78, fill="#FBF1EE", stroke=C_HTTP, rx=6)
text(952, 834, "httpd task", fs=13, weight="700", fill=C_HTTP)
text(952, 854, "core 0 · prio 10", fs=10.5, fill=MUTED, mono=True)
text(952, 872, "stack 8,192", fs=10.5, fill=MUTED, mono=True)
rect(1188, 806, 280, 78, fill=WHITE, stroke=C_HTTP, rx=6)
text(1200, 834, "static handlers", fs=13, weight="700", fill=C_HTTP)
text(1200, 854, "GET／HEAD /data/www/*", fs=10.5, fill=MUTED, mono=True)
text(1200, 872, "fallback 頁", fs=10.5, fill=MUTED, mono=True)
rect(1506, 806, 220, 78, fill="#F7F7F2", stroke=RULE, rx=6)
text(1518, 834, "NO NC4", fs=13, weight="700", fill=C_OK)
text(1518, 854, "不經 decoder", fs=10.5, fill=MUTED)
text(1518, 872, "冇 /config.json／/ota", fs=10.5, fill=MUTED)
line(1153, 845, 1183, 845, color="#5F6B76", arrow=True)
line(1471, 845, 1501, 845, color="#5F6B76", arrow=True)
text(940, 920, "HTTP 只係靜態 UI service：唔寫檔、唔改 config、唔做 OTA。", fs=11.5, fill=MUTED)
text(940, 942, "唯一 OTA 路徑＝NC4 0x22xx（line → bus → parser → decoder → ota_action）。", fs=11.5, fill=MUTED)
text(940, 964, "舊 nc4_ota_http.c／nc4_http.c 已刪；頁面只留靜態說明。", fs=11.5, fill=FAINT)

# ── 05 BOOT ─────────────────────────────────────────────────────────────
panel(50, 1004, 1700, 140, "05  BOOT ORDER", "line buffer 先 carve，B11 才開 listener／httpd")
boot = [
    ("B07", "declare", "decoder／資源", C_CFG),
    ("B08", "commit", "line buffers → Wi-Fi", C_SVC),
    ("B09", "start", "decode loops 就緒", C_LINE),
    ("B10", "usb", "USB／CDC", C_IF),
    ("B11", "channels", "nc_net_lines_start", C_OK),
    ("B12", "account", "剩餘記憶體", C_CFG),
]
bx, by, bw2, bh2 = 70, 1072, 250, 56
for i, (n, t, s, c) in enumerate(boot):
    x = bx + i * (bw2 + 18)
    hi = n in ("B08", "B11")
    rect(x, by, bw2, bh2, fill="#FCFBF7" if hi else WHITE, stroke=c, sw=1.6 if hi else 1)
    text(x + 12, by + 22, n, fs=11, fill=c, mono=True, weight="700")
    text(x + 48, by + 22, t, fs=13, weight="700")
    text(x + 12, by + 43, s, fs=10.5, fill=MUTED)

# ── 06 限制／驗證 ───────────────────────────────────────────────────────
panel(50, 1160, 830, 150, "06  限制／LEGACY", "線路層仍然有幾個缺口")
text(70, 1224, "· iface 已包含在 composite kind；socket bind 仍 INADDR_ANY，同一個 port 喺 ETH／WiFi 未可靠分離。", fs=11.2, fill=INK2)
text(70, 1246, "· 上限：NC_CFG_MAX_LINKS 8／NET_MAX_LINES 8／NC4_MAX_BUSES 8。", fs=11.2, fill=INK2)
text(70, 1268, "· legacy 路徑仍在：main tcp accept fallback、nc4_line_net、nc4_ws、nc4_udp（目標 compile out）。", fs=11.2, fill=INK2)
text(70, 1290, "· rawbench :5000／:5001 係測試 endpoint（rawbench=1 才開），release 要關。", fs=11.2, fill=INK2)

panel(920, 1160, 830, 150, "07  驗證（2026-11-16）", "有線係吞吐、無線係控制／備援")
text(940, 1224, "P4-ETH：HTTP 200 · TCP :6000／UDP :6001 STATUS · WS :8080 basic PASS。", fs=11.2, fill=INK2)
text(940, 1246, "S3 WiFi：TCP burst ≈411/s、UDP ≈134/s、WS 100 可（300+ timeout）。", fs=11.2, fill=INK2)
text(940, 1268, "P4-C6（hosted）：小量 request 可用；burst 被 SDIO staging（1536 < 2048）卡住。", fs=11.2, fill=INK2)
text(940, 1290, "有線 raw 7.4–8.8 MB/s；無線 raw 0.4–1.4 MB/s。", fs=11.2, fill=MUTED)

text(1750, 1324, "source：nc_net_lines.c · nc4_bus.c · nc4_web.c · system.json 的 nc4.links[]／routes[]",
     fs=10.5, fill=FAINT, anchor="end")
add('</svg>')

path = Path(sys.argv[1] if len(sys.argv) > 1 else "docs/00_pm/network-lines.svg")
path.parent.mkdir(parents=True, exist_ok=True)
path.write_text("\n".join(out), encoding="utf-8")
print(path)
