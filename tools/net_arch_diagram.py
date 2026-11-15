#!/usr/bin/env python3
"""Generate docs/00_pm/network-lines.svg — current Interface → Line → Service map."""
import sys
from pathlib import Path
from html import escape

W, H = 1800, 1260
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
def add(s): out.append(s)
def esc(s): return escape(str(s))
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
    rect(x, y, w, h, fill=color, stroke=color, rx=h/2, sw=0, op=.10)
    text(x+w/2, y+h/2+fs*.36, label, fs=fs, fill=color, weight="600", anchor="middle")

add(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">')
add('<defs><marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="#5F6B76"/></marker></defs>')
add(f'<rect width="{W}" height="{H}" fill="{PAPER}"/>')

# header
text(50, 62, "esp_Net-Core — 網絡線路層", fs=32, weight="700")
text(50, 90, "Interface → Line（iface + transport + port）→ Service（nc4 / web）", fs=15, fill=INK2)
text(1750, 54, "2026-11-15 · P4-ETH + S3-WiFi", fs=12, fill=FAINT, anchor="end")
line(50, 108, 1750, 108)

# 01 config
rect(50, 130, 1700, 122, fill=WHITE, op=.7)
text(70, 156, "01  CONFIG", fs=11, fill=FAINT, mono=True, ls=1.6)
text(70, 182, "line 的 iface／kind／port／service／rx 全部由 config 決定", fs=16, weight="600")
# config boxes
rect(70, 196, 510, 40, fill="#FBF7EE", stroke="#E4D8C2", rx=5)
text(84, 221, "system.json  nc4.links[] · nc4.routes[]", fs=13, mono=True, fill=C_CFG)
rect(596, 196, 410, 40, fill="#F2F7F3", stroke="#D6E2D8", rx=5)
text(610, 221, "network.json  lan / wifi / IP", fs=13, mono=True, fill=C_IF)
rect(1022, 196, 708, 40, fill="#F3F6F9", stroke="#D5DFE8", rx=5)
text(1036, 221, "managed line：iface 或 service 有寫 → 由 nc_net_lines 接管", fs=13, fill=C_LINE)

# 02 interfaces
rect(50, 276, 1700, 132, fill=WHITE, op=.7)
text(70, 302, "02  INTERFACE", fs=11, fill=FAINT, mono=True, ls=1.6)
text(70, 328, "System 提供 netif；line 只認 interface 名", fs=16, weight="600")
rect(70, 344, 800, 48, fill="#F2F7F3", stroke="#D6E2D8", rx=5)
text(86, 364, "ETH", fs=15, fill=C_IF, weight="700")
text(140, 364, "esp_eth + IP101 · P4-ETH", fs=13, fill=INK2)
text(86, 383, "10.10.1.50 / 16 · 32 MB PSRAM", fs=12, fill=MUTED, mono=True)
rect(890, 344, 840, 48, fill="#F2F7F3", stroke="#D6E2D8", rx=5)
text(906, 364, "WIFI", fs=15, fill=C_IF, weight="700")
text(976, 364, "esp_wifi · S3-Matrix（AP mode）", fs=13, fill=INK2)
text(906, 383, "192.168.4.1 · 4 MB flash · 2 MB embedded PSRAM（停用）", fs=12, fill=MUTED)

# 03 lines
text(50, 444, "03  LINE", fs=11, fill=FAINT, mono=True, ls=1.6)
text(50, 470, "一條 config line = 一個 listener／task／buffer；service 決定誰消費", fs=16, weight="600")
# four cards
cards = [
    ("TCP", "6000", "nc4", "socket · stream", "line slots → nc4_bus", "core = routes"),
    ("UDP", "6001", "nc4", "socket · datagram", "line slots → nc4_bus", "core = routes"),
    ("WS", "8080", "nc4", "自寫 handshake／frame", "line slots → nc4_bus", "不經 httpd"),
    ("HTTP", "80", "web", "httpd", "request buffer", "services: /, /config, /ota"),
]
x0, y0, cw, ch, gap = 50, 486, 410, 130, 20
for i, (kind, port, svc, sub, path, note) in enumerate(cards):
    x = x0 + i * (cw + gap)
    col = C_HTTP if kind == "HTTP" else C_LINE
    rect(x, y0, cw, ch, fill=WHITE, stroke=col, sw=1.3)
    rect(x, y0, 5, ch, fill=col, stroke=col, rx=0)
    text(x+18, y0+30, kind, fs=18, weight="700", fill=col)
    text(x+cw-18, y0+30, ":"+port, fs=14, fill=MUTED, anchor="end", mono=True)
    pill(x+18, y0+42, 78, 22, svc, col, fs=11)
    text(x+106, y0+58, sub, fs=12, fill=INK2)
    text(x+18, y0+86, path, fs=11.5, fill=MUTED, mono=True)
    text(x+18, y0+108, note, fs=11, fill=FAINT)
    if kind == "WS":
        pill(x+cw-122, y0+42, 104, 22, "NO HTTPD", C_OK, fs=10)
    if kind == "HTTP":
        pill(x+cw-122, y0+42, 104, 22, "WEB ONLY", C_HTTP, fs=10)

# 04 data paths
rect(50, 658, 830, 258, fill=WHITE, op=.7)
text(70, 684, "04A  NC4 PATH", fs=11, fill=FAINT, mono=True, ls=1.6)
text(70, 710, "bytes → parser → decoder → action", fs=16, weight="600")
steps = [
    ("socket recv", "line task 直接入 slot", C_LINE),
    ("line slots", "每 line 自己嘅 region", C_SVC),
    ("nc4_bus", "PUSH · parser 逐幀", C_SVC),
    ("decoder 0/1", "routes 指定；handler", C_LINE),
    ("reply", "同一條 line send", C_OK),
]
sx, sy, bw, bh, sgap = 70, 730, 142, 76, 18
for i, (t, s, c) in enumerate(steps):
    x = sx + i * (bw + sgap)
    rect(x, sy, bw, bh, fill=WHITE, stroke=c, rx=6)
    text(x+12, sy+28, t, fs=13, weight="700", fill=c)
    text(x+12, sy+48, s.split(" · ")[0], fs=10.5, fill=MUTED)
    if i < len(steps)-1:
        line(x+bw+3, sy+bh/2, x+bw+sgap-5, sy+bh/2, color="#5F6B76", arrow=True)
text(70, 842, "TCP／UDP／WS 都是這條路；WS framing 在 line task，NC4 解析在 decoder core。", fs=11.5, fill=MUTED)
text(70, 866, "action 回覆經同一條 line；不經 web service。", fs=11.5, fill=MUTED)

rect(900, 658, 850, 258, fill=WHITE, op=.7)
text(920, 684, "04B  WEB PATH", fs=11, fill=FAINT, mono=True, ls=1.6)
text(920, 710, "HTTP :80 → httpd → handler", fs=16, weight="600")
rect(920, 730, 220, 76, fill="#FBF1EE", stroke=C_HTTP, rx=6)
text(932, 758, "httpd task", fs=13, weight="700", fill=C_HTTP)
text(932, 778, "stack 6,144 · prio 10", fs=10.5, fill=MUTED, mono=True)
text(932, 794, "core = line core", fs=10.5, fill=MUTED)
rect(1170, 730, 240, 76, fill=WHITE, stroke=C_HTTP, rx=6)
text(1182, 758, "HTTP handlers", fs=13, weight="700", fill=C_HTTP)
text(1182, 778, "/ · /config.json", fs=10.5, fill=MUTED, mono=True)
text(1182, 794, "/ota · POST .bin", fs=10.5, fill=MUTED, mono=True)
rect(1440, 730, 290, 76, fill="#F7F7F2", stroke=RULE, rx=6)
text(1452, 758, "NO NC4", fs=13, weight="700", fill=C_OK)
text(1452, 778, "不經 decoder", fs=10.5, fill=MUTED)
text(1452, 794, "不佔 line slots", fs=10.5, fill=MUTED)
line(1143, 768, 1165, 768, color="#5F6B76", arrow=True)
line(1413, 768, 1435, 768, color="#5F6B76", arrow=True)
text(920, 842, "HTTP 只係 web service；WS 是 NC4 line，兩者唔綁埋。", fs=11.5, fill=MUTED)
text(920, 866, "HTTP 用 httpd／lwIP request buffer，不是 NC4 buffer。", fs=11.5, fill=MUTED)

# 05 boot
rect(50, 942, 1700, 150, fill=WHITE, op=.7)
text(70, 968, "05  BOOT ORDER", fs=11, fill=FAINT, mono=True, ls=1.6)
text(70, 994, "B08 先 carve line buffer，之後才開 Wi-Fi；B11 才開 listener／httpd", fs=16, weight="600")
boot = [
    ("B07", "declare", "decoder／資源", C_CFG),
    ("B08", "commit", "line buffers → WiFi", C_SVC),
    ("B09", "start", "decode loops", C_LINE),
    ("B10", "usb", "USB／CDC", C_IF),
    ("B11", "channels", "listener／:80 httpd", C_OK),
    ("B12", "account", "剩餘記憶體", C_CFG),
]
bx, by, bw2, bh2 = 70, 1010, 250, 58
for i, (n, t, s, c) in enumerate(boot):
    x = bx + i * (bw2 + 18)
    rect(x, by, bw2, bh2, fill=WHITE, stroke=c, rx=6)
    text(x+12, by+22, n, fs=11, fill=c, mono=True, weight="700")
    text(x+48, by+22, t, fs=13, weight="700")
    text(x+12, by+43, s, fs=10.5, fill=MUTED)

# 06 evidence / limitations
rect(50, 1112, 830, 116, fill="#F2F7F3", stroke="#CFE0D2", rx=8)
text(70, 1140, "P4-ETH  ·  實機驗證", fs=14, weight="700", fill=C_OK)
text(70, 1164, "HTTP :80 200 · TCP :6000 STATUS · UDP :6001 STATUS", fs=11.5, fill=INK2)
text(70, 1186, "WS :8080 basic PASS（自寫 handshake／frame）", fs=11.5, fill=INK2)
text(70, 1210, "@ 10.10.1.50", fs=10.5, fill=FAINT, mono=True)

rect(900, 1112, 850, 116, fill="#FBF7EE", stroke="#E4D8C2", rx=8)
text(920, 1140, "S3-WiFi  ·  AP mode", fs=14, weight="700", fill=C_CFG)
text(920, 1164, "TCP／UDP／WS／HTTP 四條 line 全建立；on-device selftest 四項 PASS", fs=11.5, fill=INK2)
text(920, 1186, "@ 192.168.4.1 · 4 MB flash · 2 MB embedded PSRAM（未啟用）", fs=11.5, fill=INK2)
text(920, 1210, "限制：單 session · INADDR_ANY 單 interface · buffer 未全入 plan", fs=11, fill=MUTED)

text(1750, 1242, "config → interface → line → service  ·  WS = line；HTTP = web service", fs=10.5, fill=FAINT, anchor="end")
add('</svg>')

path = Path(sys.argv[1] if len(sys.argv) > 1 else "docs/00_pm/network-lines.svg")
path.parent.mkdir(parents=True, exist_ok=True)
path.write_text("\n".join(out), encoding="utf-8")
print(path)
