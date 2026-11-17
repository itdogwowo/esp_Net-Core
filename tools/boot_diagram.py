#!/usr/bin/env python3
"""Generate docs/00_pm/boot.svg — 開機 11 個階段（詳細版）。

Single source of truth: 手寫內容清單 + 固定幾何，輸出獨立 SVG（無外部字型／圖片）。
    python tools/boot_diagram.py [out.svg]

2026-11-16 詳細版：每一階段寫清楚「做什麼／決定什麼／產出去哪裡／失敗會怎樣」，
決策點用 ◇D-id 表示；完整決策表、核心落點、不啟動條件見 boot-cores.svg。
"""
import sys
from pathlib import Path

W = 1960

INK = "#17191C"
INK2 = "#3D4247"
MUTED = "#6E747B"
FAINT = "#9AA0A6"
RULE = "#DCD8CE"
PAPER = "#FBFAF6"
WHITE = "#FFFFFF"

C_PREP = "#8A6A2F"
C_CAP = "#1F5F8B"
C_HW = "#5A6169"
C_MEM = "#7A4E8C"
C_TASK = "#A8661B"
C_LINK = "#2E6B4F"
C_BAD = "#A04A3A"

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


def rich(x, y, parts, fs=12):
    out.append(f'<text x="{x:.1f}" y="{y:.1f}" font-family="{SANS}" font-size="{fs}">')
    for t, fill, weight, mono in parts:
        f = MONO if mono else SANS
        out.append(f'<tspan font-family="{f}" fill="{fill}" font-weight="{weight}">{esc(t)}</tspan>')
    out.append("</text>")


def wrap(s, fs, width, max_lines=2):
    """粗略按寬度斷行（CJK 全寬、拉丁半寬）；超出 max_lines 就加 …。"""
    lines, cur = [], ""
    for ch in s:
        if cur and measure(cur + ch, fs) > width:
            lines.append(cur)
            cur = ch
            if len(lines) >= max_lines:
                break
        else:
            cur += ch
    if cur:
        if len(lines) >= max_lines:
            lines[-1] = lines[-1] + "…"
        else:
            lines.append(cur)
    return lines


def badge(x, y, s, fs=10.5, fg=INK2, bg="#F1EFE8", bd=RULE):
    w = measure(s, fs, True) + 12
    h = fs + 8
    out.append(f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" '
               f'rx="{h/2:.1f}" fill="{bg}" stroke="{bd}" stroke-width="1"/>')
    text(x + w / 2, y + h / 2 + fs * 0.35, s, fs, fg, 500, mono=True, anchor="middle")
    return w


# ── 內容：每階段 = 做什麼（明細） / 決定（◇D…） / 產出 / 失敗・跳過 ──────────
ROWS = [
    dict(id="B01", name="banner+nvs", color=C_PREP,
         what=["清 BOOT hold（rtc／gpio hold_dis）→ NVS init → 裝 log hook（最早）",
               "印 IDF／chip／cores／PSRAM／頻率 banner"],
         decide=["◇D1 開機生命週期在 B02 才讀；本階段無分支"],
         out=["ctx：nvs 可用", "log ring（RAM，consumer 等 B09）", "NC_CAP_NVS"],
         fail=["NVS 失敗 → warning，log 退 console；唔阻開機"]),
    dict(id="B02", name="storage+cfg", color=C_PREP,
         what=["nc_boot_init：讀 boot record（provisioned／format_pending／fw_hash）",
               "◇D1 需要格式化？→ 抹 data 分區重建 FAT；否則掛載（first 掛唔上→格式化）",
               "seed：缺檔才寫 F.preset（generated hardware ＋ build 揀的 project；冇→k_min）",
               "nc_cfg_boot：system／hardware／tasks → struct；network 由各模塊自己讀",
               "cID（config 有就用；冇→MAC 推導並寫回）／SD／卷標／NVS frame_align"],
         decide=["◇D1 格式化？", "◇D2 config 缺／壞 → 預設", "◇D3 單核 merge 在 B03"],
         out=["F.live：/data/config 檔案", "cID／SD 狀態", "NC_CAP_STORAGE｜CONFIG"],
         fail=["storage 失敗：ERROR 但仍繼續（config 用預設）",
               "壞檔：整層用預設、degraded 標記，不改用戶檔"]),
    dict(id="B03", name="capability", color=C_CAP,
         what=["nc_core_probe() → ctx.cores（1／2）",
               "◇D3 單核：Routes decode1→decode0、tasks core1→core0（只改 RAM，唔寫檔）",
               "usb_resolve()：links[] 找 enable 的 kind:\"usb\"（解析成 cdc／usb_jtag）；◇D4 決定 JTAG／OTG／NONE",
               "log：JTAG 永遠開；MSC 只有 OTG＋link.msc=1；S3 兩者共用 PHY，OTG 時 console 要改 none／UART0"],
         decide=["◇D3 核數", "◇D4 USB 人格", "◇D5（ETH 在 B05）"],
         out=["ctx：cores／usb_mode／usb_link", "usb_log／usb_msc／usb_core",
              "NC_CAP_CORES｜USB"],
         fail=["links[] 冇 USB link → USB=NONE（0 B）",
               "kind 在這個 build 冇實作 → NONE＋ERROR"]),
    dict(id="B04", name="hwcheck", color=C_HW,
         what=["nc_hw_init()；由 typed hardware 收集 claim：",
               "SDIO（clk／cmd／data）、ETH（mdc／mdio／ref_clk）、PIN、PWM、UART（tx／rx／en）"],
         decide=["無分支：只稽核"],
         out=["hw claim 帳本", "腳位衝突 → 開機即 log"],
         fail=["衝突只 log ERROR；仍然開機（相關 driver 會 fail）"]),
    dict(id="B05", name="hardware", color=C_HW,
         what=["pwm_boot()：hardware PWM.list enable → LEDC install",
               "◇D5 NC_HAS_ETH（晶片）＋ hardware ETH.enable＋腳位完整 → 起 netif／PHY；否則 s_eth_ok=0",
               "router_from_cfg()：system.routes；◇D6 每行第一個 decodeN 定核（單核 clamp）",
               "冇有效 routes → router_defaults()：USB→core0；ETH ok→TCP core0／UART core1；否則 UART core0",
               "USB 線的組：ctx.usb_core = router_core(usb link)（可能 NONE）"],
         decide=["◇D5 ETH 起唔起", "◇D6 routes → 線→核"],
         out=["ctx.usb_core", "router 表", "eth_ok／pwm", "NC_CAP_ROUTER"],
         fail=["ETH 未編／enable=0／腳位缺 → 唔起 netif（唔阻其他通道）",
               "router 無效 → 用預設表＋warning"]),
    dict(id="B06", name="declare", color=C_MEM,
         what=["s_ndec = (cores>1 且 router 有 core1 線) ? 2 : 1；每核建一個 decoder",
               "套 cid／master_cid；actions_init（統計→一次配置→封存）",
               "登記任務：nc4 L0、net L1、gpio L2；測試 build 另加 probe／ncm／rgb／hello／bench",
               "nc_task_declare_all()：讀 tasks.json level／affinity（◇D9），只計記憶體，未 malloc"],
         decide=["◇D9 任務 level／affinity", "decoder 個數＝核數＋router"],
         out=["decoder[] 物件", "plan 帳面（未 carve）", "NC_CAP_DECLARED"],
         fail=["decoder0 建失敗 → 收不到任何指令"]),
    dict(id="B07", name="commit", color=C_MEM,
         what=["探測每 class 最大連續 → plan commit／carve（一次）",
               "◇D7 managed net lines？有 → nc_net_lines_declare()：逐線 service claim、buffer、bus（◇D8）",
               "之後才 nc_wifi_boot()：◇D10 編譯閘＋network.json wifi.enable；sta／ap；credentials",
               "冇 managed line → legacy 路徑（nc4_line_net declare）"],
         decide=["◇D7 managed vs legacy", "◇D8 每條 line 建唔建", "◇D10 WiFi 起唔起"],
         out=["plan：已 carve", "net lines 清單（核心已綁）", "WiFi 狀態", "NC_CAP_PLAN"],
         fail=["單條 line 失敗 → 跳過該線，其他照起", "WiFi 唔起 → 仍可 ETH／USB"]),
    dict(id="B08", name="start", color=C_TASK,
         what=["nc_task_start_all()：gather 資源 → on_start → prepare loops（◇D9 決定開唔開）",
               "loop 未開；只建立 task 物件／資源／指令表",
               "測試 build：ncm 任務起咗 → 掃 /data/pixel/effects、/data/tasks 的 .mode attach（◇D11）",
               "WAMR 未接：.mode 只登記／驗證，未真正執行（NCM 下一步）",
               "印任務帳本"],
         decide=["◇D9 loop enabled", "◇D11 .mode 只在測試 build 掃"],
         out=["on_start 已跑、資源已派", "tasks.json 補寫", "NC_CAP_TASKS"],
         fail=["on_start 失敗 → 任務唔 started，loop 唔開"]),
    dict(id="B09", name="usb", color=C_LINK,
         what=["JTAG：nc_log_start() 走 USJ（NC4＋log 共用 TX）；/data 留 APP",
               "OTG：cdc_wanted＝link enable＋usb_core≠NONE；冇 CDC 又冇 MSC → 唔裝 TinyUSB",
               "usb_init() → CDC log（usb.cdc_log）／MSC（link.msc=1 → 交 FAT 給 host）",
               "NONE：冇 USB → 只 console；/data 留 APP"],
         decide=["◇D4 的結果照做（冇 runtime 切換）", "◇D12 MSC 交唔交碟"],
         out=["USB stack／log 出口", "/data 歸屬（APP 或 host）"],
         fail=["init 失敗 → /data 留 APP、ERROR；不重試"]),
    dict(id="B10", name="channels", color=C_LINK,
         what=["managed：nc_net_lines_start()——HTTP→httpd（core0）、WS／TCP→listen＋task（core=ln）",
               "UDP→bind；legacy：WiFi line／ETH WS＋UDP／main tcp accept fallback",
               "建 USB bus（OTG PUSH＋cdc.rx；JTAG PULL；UART PUSH）／legacy TCP bus",
               "最後 nc_task_boot()：按 layer 開所有 enabled loop → decode 開始收幀",
               "uart.rx producer 最後才起（等 decode 的 owner_init 裝好 driver）"],
         decide=["◇D7／D8 的結果照做", "◇D9 loop enabled 才開"],
         out=["listener／bus／producer", "tasks loop 開跑", "NC_CAP_CHANNELS"],
         fail=["單線 bind／task 失敗 → log＋跳過；decode 照跑"]),
    dict(id="B11", name="account", color=C_LINK,
         what=["印內部／PSRAM：free ＋ 最大連續（兩個數字分開睇）",
               "之後 app_main 只剩 1 秒心跳（heap 快照）"],
         decide=["無分支"],
         out=["開機帳", "—"],
         fail=["—"]),
]

TOP = 150
ROW_H = 118
L, R = 40, W - 40
COL_ID, COL_WHAT, COL_DEC, COL_OUT, COL_FAIL = L + 16, L + 250, L + 880, L + 1340, L + 1620
PANEL_Y = TOP - 26
PANEL_H = 11 * ROW_H + 74
NOTES_Y = PANEL_Y + PANEL_H + 16
NOTES_H = 150
H = NOTES_Y + NOTES_H + 40


def build():
    out.clear()
    out.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
               f'viewBox="0 0 {W} {H}" font-family="{SANS}">')
    rect(0, 0, W, H, PAPER, PAPER, 0, 0)

    text(L, 52, "開機：11 個階段，每一階段做什麼、決定什麼、失敗會怎樣", 25, INK, 600)
    text(L, 78, "每一行 = main.c stages[] 一行（順序即執行序）。永遠嘗試全部階段；"
                "FAIL 只記錄、缺 needs 就 SKIP，唔會中途 reboot。", 12.5, MUTED)
    text(R, 78, "決策點 ◇D#：完整條件／核落點見 boot-cores.svg", 11, FAINT, anchor="end")

    rect(L, PANEL_Y, R - L, PANEL_H, WHITE, RULE, 1, 6, 0.72)
    # 表頭
    hy = TOP - 12
    text(COL_ID, hy, "① 階段（B01…；順序）", 11.5, MUTED, 600)
    text(COL_WHAT, hy, "② 做什麼（實際順序）", 11.5, MUTED, 600)
    text(COL_DEC, hy, "③ 決定／分支", 11.5, MUTED, 600)
    text(COL_OUT, hy, "④ 產出／去哪裡", 11.5, MUTED, 600)
    text(COL_FAIL, hy, "⑤ 失敗・跳過", 11.5, MUTED, 600)
    line(L + 8, TOP - 2, R - 8, TOP - 2, RULE)

    for i, r in enumerate(ROWS):
        y = TOP + i * ROW_H
        out.append(f'<rect x="{L + 8}" y="{y + 6}" width="4" height="{ROW_H - 14}" '
                   f'rx="2" fill="{r["color"]}"/>')
        text(COL_ID, y + 22, r["id"], 10.5, FAINT, 500, mono=True)
        text(COL_ID + 34, y + 22, r["name"], 12.5, INK, 600, mono=True)
        for k, ln in enumerate(r["what"]):
            text(COL_WHAT, y + 20 + k * 17, ln, 11.6, INK2)
        for k, ln in enumerate(r["decide"]):
            text(COL_DEC, y + 20 + k * 17, ln, 11.2, C_CAP)
        out_lines = []
        for msg in r["out"]:
            out_lines.extend(wrap(msg, 11.2, COL_FAIL - COL_OUT - 12, 2))
        for k, ln in enumerate(out_lines[:4]):
            text(COL_OUT, y + 20 + k * 15, ln, 11.2, INK2)
        fail_lines = []
        for msg in r["fail"]:
            fail_lines.extend(wrap(msg, 10.6, R - COL_FAIL - 10, 2))
        for k, ln in enumerate(fail_lines[:3]):
            text(COL_FAIL, y + 20 + k * 15, ln, 10.6, MUTED)
        if i < len(ROWS) - 1:
            line(L + 20, y + ROW_H - 8, R - 20, y + ROW_H - 8, RULE)
            out.append(f'<path d="M {COL_ID + 8} {y + ROW_H - 14} L {COL_ID + 8} {y + ROW_H - 4}" '
                       f'stroke="{FAINT}" stroke-width="1.1"/>')
            out.append(f'<path d="M {COL_ID + 5} {y + ROW_H - 8} L {COL_ID + 8} {y + ROW_H - 4} '
                       f'L {COL_ID + 11} {y + ROW_H - 8}" fill="none" stroke="{FAINT}" stroke-width="1.1"/>')

    # ── 底部：語義＋能力位＋入口 ─────────────────────────────────────
    rect(L, NOTES_Y, R - L, NOTES_H, WHITE, RULE, 1, 6, 0.72)
    text(L + 18, NOTES_Y + 26, "階段語義（boot.c 執行器）", 12.5, INK, 600)
    rich(L + 18, NOTES_Y + 52,
         [("needs 缺失 → ", MUTED, 400, False), ("SKIP", C_BAD, 600, False),
          ("（寫明缺什麼）；", MUTED, 400, False), ("fn 失敗 → ", MUTED, 400, False),
          ("FAIL", C_BAD, 600, False), ("（記錄，不中斷）；optional 失敗 → ", MUTED, 400, False),
          ("WARN", C_TASK, 600, False), ("。", MUTED, 400, False)], fs=11.5)
    rich(L + 18, NOTES_Y + 74,
         [("能力位 0x03ff = ", MUTED, 400, False),
          ("NVS(1)｜STORAGE(2)｜CONFIG(4)｜CORES(8)｜USB(0x10)｜ROUTER(0x20)｜"
           "DECLARED(0x40)｜PLAN(0x80)｜TASKS(0x100)｜CHANNELS(0x200)", INK2, 400, True)],
         fs=11)
    rich(L + 18, NOTES_Y + 96,
         [("ctx（nc_boot_ctx）＝ ", MUTED, 400, False),
          ("cores｜usb_mode｜usb_link｜usb_log｜usb_msc｜usb_core｜ready", INK2, 400, True),
          ("；跨階段只准經 ctx。", MUTED, 400, False)], fs=11)
    rich(L + 18, NOTES_Y + 118,
         [("硬性次序：", MUTED, 400, False),
          ("NVS→log hook；掛 FAT→讀 config；decode 起咗→才建 producer；"
           "line buffer carve→才開 Wi-Fi／listener。", INK2, 400, False)], fs=11)
    text(R - 18, NOTES_Y + 26, "來源：firmware/main/boot.c · main.c stages[] · boot.h",
         10.5, FAINT, anchor="end")

    # 圖例
    ly = H - 12
    lx = L + 8
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
