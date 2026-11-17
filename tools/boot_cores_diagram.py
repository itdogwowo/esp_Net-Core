#!/usr/bin/env python3
"""Generate docs/00_pm/boot-cores.svg — 開機決策 → 啟動／唔啟動 → 核落點。

    python tools/boot_cores_diagram.py [out.svg]

三塊：
  D. 決策表：邊個條件決定「啟動什麼」
  C. 核落點：邊個 object 釘喺 core0／core1，由邊個決定
  S. 唔啟動矩陣：什麼情況下某一條線／服務／loop 唔會出現
"""
import sys
from pathlib import Path

W = 1960
INK, INK2, MUTED, FAINT = "#17191C", "#3D4247", "#6E747B", "#9AA0A6"
RULE, PAPER, WHITE = "#DCD8CE", "#FBFAF6", "#FFFFFF"
C_PREP, C_CAP, C_HW, C_MEM, C_TASK, C_LINK, C_BAD = (
    "#8A6A2F", "#1F5F8B", "#5A6169", "#7A4E8C", "#A8661B", "#2E6B4F", "#A04A3A")
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


def rect(x, y, w, h, fill=WHITE, stroke=RULE, sw=1, rx=5, op=1.0):
    out.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" '
               f'rx="{rx}" fill="{fill}" stroke="{stroke}" stroke-width="{sw}" opacity="{op}"/>')


def line(x1, y1, x2, y2, color=RULE, sw=1):
    out.append(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" '
               f'stroke="{color}" stroke-width="{sw}"/>')


def text(x, y, s, fs=12, fill=INK, weight=400, mono=False, anchor="start"):
    fam = MONO if mono else SANS
    out.append(f'<text x="{x:.1f}" y="{y:.1f}" font-family="{fam}" font-size="{fs}" '
               f'font-weight="{weight}" fill="{fill}" text-anchor="{anchor}">{esc(s)}</text>')


def panel(x, y, w, h, tag, title, sub=None):
    rect(x, y, w, h, WHITE, RULE, 1, 6, 0.72)
    text(x + 18, y + 24, tag, 10.5, FAINT, 500, mono=True)
    text(x + 18, y + 48, title, 15.5, INK, 600)
    if sub:
        text(x + 18, y + 68, sub, 11.2, MUTED)


# ── 決策表 ─────────────────────────────────────────────────────────────
DECISIONS = [
    ("D1", "boot record（NVS）：provisioned／fmt_pending／fw_hash",
     "first 或 fmt_pending → 格式化 data；否則掛載；已 provisioned 唔自動再 format",
     "B02；storage 失敗 → 總閘（見底）", C_PREP),
    ("D2", "/data/config/*.json 存在？可 parse？version 對？",
     "缺檔 → seed F.preset（再冇就 k_min）；壞檔 → 該層用內建預設，degraded",
     "B02；之後所有人都讀 struct，唔再開檔", C_PREP),
    ("D3", "nc_core_probe()（IDF 晶片實際核數）",
     "1 核 → config decode1→decode0、tasks core1→core0；decoder 只建 1 個；全部 clamp core0\n"
     "2 核 → 可按 routes 派 decode0／decode1",
     "B03；影響 B07 decoder 數、B08 line core、B11 任務核", C_CAP),
    ("D4", "links[] 有 enable 的 cdc／usb_jtag？× build 有冇該 transport",
     "JTAG／OTG／NONE；JTAG log 永遠開、OTG 跟 usb.cdc_log；MSC 只 OTG＋usb.msc=1",
     "B03 寫 ctx；B10 照做（冇 runtime 切換）", C_CAP),
    ("D5", "NC_HAS_ETH（晶片 EMAC）＋ hardware ETH.present／enable＋mdc/mdio/ref_clk 完整",
     "全部成立 → 起 netif／PHY（s_eth_ok=1）；否則 s_eth_ok=0",
     "B05；之後 ETH 通道／legacy TCP 全部跟它", C_HW),
    ("D6", "system.routes[] 有效？（in 的 link enable；out 第一個 decodeN）",
     "每線 core＝第一個 decodeN（單核 clamp、decode1 重寫）；冇有效 routes → 預設表",
     "B05 router → B07／B08／B11 的核全部由這張表", C_HW),
    ("D7", "任何 link：composite kind（eth_tcp／wifi_tcp…）＋ enable ＋ port≠0？",
     "有 → 新 nc_net_lines 接管；冇 → legacy（nc4_line_net／main tcp accept／ws／udp）",
     "B08／B11 二選一，唔會兩條路同時起", C_LINK),
    ("D8", "逐條 line：composite kind？enable？port≠0？service claim（port 政策）？routes 有核？decoder 存在？buffer 配到？",
     "全過 → 建 listener／bus；HTTP→core0；NC4／WS→routes core；任一唔過 → 跳過該線（其他照起）；\n"
     "routes[].out 指到不存在 link → 只忽略該格，不令整張表失敗",
     "B08 宣告、B11 開 listener", C_LINK),
    ("D9", "任務：kind＝SYSTEM？tenant 的 tasks.json level／affinity？loop enabled()？",
     "SYSTEM 永遠照 C 宣告（config 管唔到）；tenant level≥0 且 affinity 有 1 → loop 開；\n"
     "否則 on_start 照跑、loop 唔開（缺條目＝(0,0)）",
     "B07 declare、B09 on_start、B11 nc_task_boot", C_TASK),
    ("D10", "CONFIG_NETCORE_WIFI 編入？network.json wifi.enable？mode sta/ap？有 credentials？",
     "全過 → 起 WiFi；否則唔初始化（0 B heap 以外）",
     "B08；WiFi manager task＝core1（單核 clamp 0）", C_TASK),
    ("D11", "NC_ENABLE_TESTS？fx 任務 started？WAMR 後端有冇接？",
     "測試 build：掃 /data/pixel/effects、/data/tasks 的 .fxe attach；release 唔掃；\n"
     "WAMR 未接 → 登記得到、跑不到",
     "B09；效果目前只在測試映像出現；FXE 執行後端未完成", C_MEM),
    ("D12", "network.json rawbench=1？",
     "開 :5000／:5001 raw 測試 endpoint；release 要關",
     "B11；繞過 NC4，只作量測", C_BAD),
]

# ── 核落點 ─────────────────────────────────────────────────────────────
CORE_RULES = [
    "app_main（B01→B12）固定 core0；每階段都係順序跑。",
    "每條 line 的核 = routes[].out 第一個 decodeN；HTTP service 固定 core0。",
    "producer 必須同 bus / decoder 同核（kick 同核）；B11 會驗，跨核＝ERROR。",
    "SYSTEM task：照 C 宣告 core（clamp）；tenant：tasks.json affinity 可改（只可一核=1）。",
    "WiFi manager＝core1（clamp）；單核板全部落 core0。",
    "測試任務／fx：宣告／manifest 的 core（只有 NC_ENABLE_TESTS）。",
]

CORE0 = [
    ("app_main", "B01–B12 全部階段（順序）", C_PREP),
    ("decode0", "條件：routes 有 decode0（幾乎一定有）", C_CAP),
    ("line tasks", "route core0 的 TCP／UDP／WS，core 由 routes", C_LINK),
    ("httpd", "HTTP service（:80）固定 core0", C_LINK),
    ("cdc.rx", "USB 線的核（decoder_core；單 decoder→core0）", C_LINK),
    ("uart.rx", "UART 線的核（decode 起咗才建 producer）", C_LINK),
    ("心跳", "B12 之後 app_main 每秒 heartbeat", C_TASK),
    ("測試任務", "probe／hello／bench… 依宣告（測試 build）", C_TASK),
]
CORE1 = [
    ("decode1", "條件：cores>1＋routes 有 decode1＋有線", C_CAP),
    ("line tasks", "route core1 的 TCP／UDP／WS", C_LINK),
    ("cdc.rx", "USB 線 route core1（PUSH，同核）", C_LINK),
    ("uart.rx", "UART 線 route core1", C_LINK),
    ("wifi_mgr", "WiFi 起咗 → nc_core_clamp(1)；單核→core0", C_TASK),
    ("fx / rgb matrix", "測試任務，依宣告／manifest core", C_MEM),
]

# ── 唔啟動矩陣 ─────────────────────────────────────────────────────────
SKIP = [
    ("ETH／TCP", "NC_HAS_ETH=0；hardware ETH.enable=0（例如 FireBeetle 冇 PHY）；"
                 "腳位 -1／不完整；PHY init 失敗", C_HW),
    ("新 net line", "enable=0；唔 managed；port=0；service claim 失敗；"
                    "routes 冇 decodeN；decoder 唔存在；buffer 配唔到", C_LINK),
    ("legacy 網絡", "有 managed line → 新路徑接管，legacy 唔用；"
                    "或者根本冇 ETH／WiFi 可用", C_LINK),
    ("USB 線／stack", "links[] 冇 enable 的 cdc／usb_jtag；build 冇該 transport；"
                      "OTG 但 cdc 未登記＋msc=0 → 連 TinyUSB 都唔裝", C_CAP),
    ("MSC（交碟）", "USB=NONE／JTAG；或 usb.msc=0（預設 0，/data 留 APP）", C_CAP),
    ("WiFi", "CONFIG_NETCORE_WIFI=n；network.json 缺；enable=0；mode 唔支援；"
             "冇 credentials；hosted 握手失敗", C_TASK),
    ("任務 loop", "tenant level=-1；affinity=(0,0)；enabled() false；layer 超界；"
                  "config 缺條目 → on_start 照跑、loop 唔開", C_TASK),
    ("效果 .fxe", "release build（冇 NC_ENABLE_TESTS）；或 fx 任務未 started", C_MEM),
    ("rawbench", "network.json rawbench≠1（預設唔開）", C_BAD),
]

# ── 幾何 ───────────────────────────────────────────────────────────────
L, R = 40, W - 40
D_Y, D_H = 120, 100 + len(DECISIONS) * 44
C_Y = D_Y + D_H + 16
C_H = 420
S_Y = C_Y + C_H + 16
S_H = 92 + len(SKIP) * 40
F_Y = S_Y + S_H + 16
F_H = 150
H = F_Y + F_H + 40


def build():
    out.clear()
    out.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
               f'viewBox="0 0 {W} {H}" font-family="{SANS}">')
    rect(0, 0, W, H, PAPER, PAPER, 0, 0)

    text(L, 52, "開機：邊個決定啟動什麼、喺邊個核、什麼情況唔啟動", 25, INK, 600)
    text(L, 78, "所有決策都喺 B01–B11 之間發生；冇 runtime 熱切換 —— 改 config 之後重啟，全部重算。",
         12.5, MUTED)
    text(R, 78, "階段明細見 boot.svg", 11, FAINT, anchor="end")

    # ── D. 決策表 ──────────────────────────────────────────────────
    panel(L, D_Y, R - L, D_H, "D  DECISIONS", "決策點：條件 → 結果 → 影響（◇D# 對應 boot.svg）")
    cx = [L + 20, L + 120, L + 800, L + 1400]
    hy = D_Y + 92
    text(cx[0], hy, "ID", 10.5, MUTED, 600, mono=True)
    text(cx[1], hy, "條件（輸入／來源）", 10.5, MUTED, 600)
    text(cx[2], hy, "決定（結果）", 10.5, MUTED, 600)
    text(cx[3], hy, "影響／落點", 10.5, MUTED, 600)
    line(L + 14, hy + 6, R - 14, hy + 6, RULE)
    y = hy + 26
    for did, cond, res, impact, col in DECISIONS:
        rect(L + 14, y - 16, R - L - 28, 38, "#FCFBF7", RULE, 1, 4)
        out.append(f'<rect x="{L + 14}" y="{y - 16}" width="4" height="38" rx="2" fill="{col}"/>')
        text(cx[0], y + 6, did, 11, col, 700, mono=True)
        text(cx[1], y - 2, cond, 10.6, INK)
        rlines = res.split("\n")
        for k, rl in enumerate(rlines):
            text(cx[2], y - 2 + k * 15, rl, 10.6, INK2)
        text(cx[3], y - 2, impact, 10.4, MUTED)
        y += 44

    # ── C. 核落點 ──────────────────────────────────────────────────
    panel(L, C_Y, R - L, C_H, "C  CORES", "核落點：每個 object 釘喺邊個核、由邊個決定",
          "先有 routes 才有 decode 核；producer 一律跟 bus 同核（跨核 kick 係設計禁止）。")
    # 規則
    rx0, ry0 = L + 20, C_Y + 88
    for i, rule in enumerate(CORE_RULES):
        col = 0 if i < 3 else 1
        x = rx0 + (0 if i < 3 else 940)
        yy = ry0 + (i if i < 3 else i - 3) * 19
        text(x + 10, yy, "·", 12, C_CAP, 700)
        text(x + 22, yy, rule, 10.8, INK2)
    # lanes
    lane_y = ry0 + 66
    lane_h = C_H - 88 - 76
    lane_w = (R - L - 60) / 2
    for idx, (label, items, col) in enumerate((("core 0", CORE0, C_CAP), ("core 1", CORE1, C_TASK))):
        x = L + 20 + idx * (lane_w + 20)
        rect(x, lane_y, lane_w, lane_h, "#FCFBF7", RULE, 1, 6, 0.9)
        text(x + 14, lane_y + 24, label, 14, col, 700, mono=True)
        iy = lane_y + 48
        for name, cond, c in items:
            text(x + 14, iy, name, 11.2, INK, 600, mono=True)
            text(x + 14 + measure(name, 11.2, True) + 14, iy, cond, 10.4, MUTED)
            iy += 26

    # ── S. skip matrix ─────────────────────────────────────────────
    panel(L, S_Y, R - L, S_H, "S  NOT STARTED", "什麼情況唔啟動：逐項條件（任何一項成立就唔出現）")
    sy = S_Y + 90
    for name, cond, col in SKIP:
        rect(L + 14, sy - 16, R - L - 28, 34, "#FCFBF7", RULE, 1, 4)
        out.append(f'<rect x="{L + 14}" y="{sy - 16}" width="4" height="34" rx="2" fill="{col}"/>')
        text(L + 30, sy + 6, name, 11, col, 600, mono=True)
        text(L + 260, sy - 1, cond, 10.6, INK2)
        sy += 40

    # ── F. 總閘／語義 ───────────────────────────────────────────────
    panel(L, F_Y, R - L, F_H, "F  總閘", "B02 是總閘：storage+cfg 失敗，後面全部 SKIP")
    text(L + 20, F_Y + 92,
         "· ph_storage 回非 ESP_OK → NC_CAP_STORAGE／CONFIG 唔 give → B03（needs CONFIG）之後全部 SKIP；"
         "B12 account 仍會跑，然後只剩 app_main 心跳。", 11.2, INK2)
    text(L + 20, F_Y + 114,
         "· config 檔壞 ≠ B02 FAIL：degraded 仍然 give CONFIG，之後用內建預設開機；"
         "所以「壞檔」同「冇碟」後果唔同。", 11.2, INK2)
    text(L + 20, F_Y + 136,
         "· B10 USB 失敗／B11 單線失敗都只係局部退化（記錄＋跳過），唔會令前面階段重跑。",
         11.2, INK2)

    text(R, H - 18, "來源：firmware/main/main.c · boot.c · nc_task.c · nc_net_lines.c · nc_wifi.c · nc4_router.c",
         10.2, FAINT, anchor="end")
    out.append("</svg>")


def main():
    build()
    dst = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("docs/00_pm/boot-cores.svg")
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text("\n".join(out), encoding="utf-8")
    print(f"{dst}  ({W}×{H})")


if __name__ == "__main__":
    main()
