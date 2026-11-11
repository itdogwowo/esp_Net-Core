#!/usr/bin/env python3
"""板子清單與設定檢查（對應 ports/<chip>/<board>/）

為什麼要有這支：
    ESP-IDF 的板子差異分成三層，只有 L3 能靠 config/*.json 換：

        L1 硬體事實（編譯期）  flash 大小/頻率/模式、PSRAM、CPU 頻率、有冇 Ethernet
                               → 一定要重新 build；寫在 ports/<board>/sdkconfig.defaults
        L2 能力碎片（編譯期）  ETH / WS / UDP 這種「功能包」
                               → ports/_profiles/*.defaults，由 board.json 組合
        L3 執行期（同一 binary）gpio、服務開關、NC4 路線、數量
                               → ports/<board>/data/config/<layer>.json（一層一個檔）

    這支工具就是讓 L1/L2 不會寫錯、不會跟 L3 搞混：
      list     所有板子與能力一覽
      check    board.json ↔ sdkconfig.defaults 一致性；擋下「把 build-time 的東西寫進 config/*.json」
      sync     把 board.json 指定的能力碎片寫進 sdkconfig.defaults（marker 區塊）
      new      開一塊新板：建目錄 + 三個檔 + README 骨架
      resolve  給 tools/board.sh 用：查板子 → 目錄 / target / 晶片

用法：
    python3 tools/board.py list
    python3 tools/board.py check            # 全部板子
    python3 tools/board.py check p4         # 只查一塊
    python3 tools/board.py sync p4
    python3 tools/board.py new --chip S3 --board ESP32-S3-Matrix \\
        --flash 16 --flash-freq 80 --psram 8 --psram-mode octal --psram-speed 80 \\
        --cpu 240 --console usb-serial-jtag --eth none --wifi
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PORTS = ROOT / "ports"
PROFILES = PORTS / "_profiles"
CHIPS = PORTS / "_chips"
COMMON_DEFAULTS = ROOT / "firmware" / "sdkconfig.defaults"

CHIP_DIR = {"s3": "esp32s3", "p4": "esp32p4", "c6": "esp32c6", "c3": "esp32c3",
            "s2": "esp32s2", "h2": "esp32h2"}
CHIP_ALIAS = {v: k for k, v in CHIP_DIR.items()}

# 這些鍵屬於 L1（硬體事實），出現在共通 defaults 或 config.json 就是放錯層
BOARD_FACT_RE = re.compile(
    r"^(ESPTOOLPY_FLASHSIZE_|ESPTOOLPY_FLASHFREQ_|ESPTOOLPY_FLASHMODE_|"
    r"SPIRAM|ESP_CONSOLE_|ESP_DEFAULT_CPU_FREQ_MHZ_)"
)

PSRAM_MODE = {"octal": "OCT", "oct": "OCT", "hex": "HEX", "hexadecimal": "HEX",
              "quad": "QUAD", "qio": "QUAD"}
CONSOLE_SYM = {
    "uart0": "ESP_CONSOLE_UART_DEFAULT",
    "uart": "ESP_CONSOLE_UART_DEFAULT",
    "usb-serial-jtag": "ESP_CONSOLE_USB_SERIAL_JTAG",
    "usb_serial_jtag": "ESP_CONSOLE_USB_SERIAL_JTAG",
    "usb-cdc": "ESP_CONSOLE_USB_CDC",
    "usb_cdc": "ESP_CONSOLE_USB_CDC",
}
# 每個 config 檔允許的頂層鍵（檔名 → 允許的鍵）；一個層一個檔
RUNTIME_KEYS_BY_FILE = {
    "system.json":   {"version", "_comment", "cID", "log_debug_mode",
                      "frame_interval_ms", "boot_strict", "num_pixels", "nc4"},
    "network.json":  {"version", "_comment", "hostname", "priority", "lan", "wifi", "bt", "espnow"},
    "hardware.json": {"version", "_comment", "_gpio_convention", "ETH",
                      "PIN", "SPI", "I2C", "I80", "UART", "I2S", "PWM",
                      "SDIO", "ENC", "XL9555"},
    "driver.json":   {"version", "_comment", "USB", "TFT", "SDcard",
                      "WS2812", "APA102", "PCA9685", "Audio", "PCM5102",
                      "GT1151Q", "HUSB238", "uartMotor"},
    "tasks.json":    {"version", "_comment", "tasks", "override"},
}

MARK_OPEN = "# >>> profile: {name}"
MARK_CLOSE = "# <<< profile: {name}"
MARK_HEAD = ("# ─── 能力碎片（由 tools/board.py sync 產生，不要手改這段）───\n"
             "# 來源：ports/_profiles/*.defaults；board.json 的 profiles 決定要哪些\n")


# ─────────────────────────── 讀取 ───────────────────────────

def board_dirs() -> list[Path]:
    return sorted(p.parent for p in PORTS.glob("*/*/board.json"))


def load_board(d: Path) -> dict:
    return json.loads((d / "board.json").read_text(encoding="utf-8"))


def parse_kconfig(path: Path) -> dict[str, str | None]:
    """解析 sdkconfig / sdkconfig.defaults 的鍵值（含 '# X is not set'）。"""
    out: dict[str, str | None] = {}
    if not path.exists():
        return out
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        m = re.match(r"^CONFIG_([A-Za-z0-9_]+)=(.*)$", line)
        if m:
            out[m.group(1)] = m.group(2).strip()
            continue
        m = re.match(r"^# CONFIG_([A-Za-z0-9_]+) is not set$", line)
        if m:
            out[m.group(1)] = None
    return out


def effective_config(d: Path) -> dict[str, str | None]:
    """建置時真正的值：共通 defaults → 板子覆蓋（後者勝）。"""
    cfg = parse_kconfig(COMMON_DEFAULTS)
    cfg.update(parse_kconfig(d / "sdkconfig.defaults"))
    return cfg


def rel(p: Path) -> str:
    return str(p.relative_to(ROOT))


# ─────────────────────────── 檢查 ───────────────────────────

class Report:
    def __init__(self) -> None:
        self.errors: list[str] = []
        self.warns: list[str] = []

    def err(self, msg: str) -> None:
        self.errors.append(msg)

    def warn(self, msg: str) -> None:
        self.warns.append(msg)


def check_board(d: Path, cfg_common: dict, cfg_port: dict) -> Report:
    r = Report()
    b = load_board(d)
    name = b.get("name", d.name)

    # --- board.json 結構 ---
    for k in ("name", "chip", "flash", "psram", "cpu_mhz", "console"):
        if k not in b:
            r.err(f"board.json 缺必要欄位：{k}")
    chip = b.get("chip", "")
    if chip and CHIP_DIR.get(d.parent.name.lower()) not in (None, chip):
        r.err(f"board.json chip={chip} 與目錄 ports/{d.parent.name}/ 不符")
    if chip and chip not in CHIP_ALIAS:
        r.warn(f"未見過的 chip={chip}（新增晶片請更新 CHIP_DIR）")

    # --- L1 一致性：board.json vs 實際 Kconfig ---
    def want(sym: str, value: str | None, label: str) -> None:
        got = cfg_port.get(sym, "<未設定>")
        if value is None:
            if got not in ("<未設定>", None):
                r.err(f"{label}：board.json 說沒有，但 sdkconfig 設了 CONFIG_{sym}={got}")
        elif got != value:
            r.err(f"{label}：期望 CONFIG_{sym}={value}，實際 {got}")

    flash = b.get("flash") or {}
    if flash.get("size_mb"):
        want(f"ESPTOOLPY_FLASHSIZE_{flash['size_mb']}MB", "y", "flash 大小")
    if flash.get("freq_mhz"):
        want(f"ESPTOOLPY_FLASHFREQ_{flash['freq_mhz']}M", "y", "flash 頻率")
    if flash.get("mode"):
        want(f"ESPTOOLPY_FLASHMODE_{str(flash['mode']).upper()}", "y", "flash 模式")

    psram = b.get("psram") or {}
    if psram.get("size_mb"):
        want("SPIRAM", "y", "PSRAM 啟用")
        mode = PSRAM_MODE.get(str(psram.get("mode", "")).lower())
        if mode is None:
            r.err(f"psram.mode={psram.get('mode')!r} 看不懂（用 octal / hex / quad）")
        else:
            want(f"SPIRAM_MODE_{mode}", "y", "PSRAM 模式")
        if psram.get("speed_mhz"):
            want(f"SPIRAM_SPEED_{psram['speed_mhz']}M", "y", "PSRAM 速度")
    else:
        if cfg_port.get("SPIRAM") == "y":
            r.err("board.json 說沒有 PSRAM，但 sdkconfig 開了 CONFIG_SPIRAM=y")

    if b.get("cpu_mhz"):
        want(f"ESP_DEFAULT_CPU_FREQ_MHZ_{b['cpu_mhz']}", "y", "CPU 頻率")

    con = CONSOLE_SYM.get(str(b.get("console", "")).lower())
    if con is None:
        r.err(f"console={b.get('console')!r} 看不懂（用 uart0 / usb-serial-jtag / usb-cdc）")
    else:
        want(con, "y", "console")

    feats = b.get("features") or {}
    eth = feats.get("eth")
    if eth:
        want("ETH_ENABLED", "y", "Ethernet")
    elif cfg_port.get("ETH_ENABLED") == "y":
        r.err("board.json 說沒有 Ethernet，但 sdkconfig 開了 CONFIG_ETH_ENABLED=y")

    # --- L2 能力碎片：宣告了就要同步、同步了就要一致 ---
    text = (d / "sdkconfig.defaults").read_text(encoding="utf-8") if (d / "sdkconfig.defaults").exists() else ""
    declared = b.get("profiles") or []
    for prof in declared:
        src = PROFILES / f"{prof}.defaults"
        if not src.exists():
            r.err(f"board.json 指定了能力碎片 {prof!r}，但 ports/_profiles/{prof}.defaults 不存在")
            continue
        block = _extract_block(text, prof)
        if block is None:
            r.err(f"能力碎片 {prof!r} 還沒同步進 sdkconfig.defaults → 跑 tools/board.py sync")
        elif block.strip() != src.read_text(encoding="utf-8").strip():
            r.err(f"能力碎片 {prof!r} 與 ports/_profiles/{prof}.defaults 不同步 → 跑 tools/board.py sync")
    for prof in _blocks_in(text):
        if prof not in declared:
            r.warn(f"sdkconfig.defaults 有 {prof!r} 的區塊，但 board.json 沒宣告（sync 會移除）")

    # --- build-time driver.json：每個 profile 都要有 _chips/<name>.json ---
    drv_path = d / "driver.json"
    if drv_path.exists():
        try:
            drv = json.loads(drv_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as e:
            r.err(f"driver.json 不是合法 JSON：{e}")
            drv = None
        if isinstance(drv, dict):
            def walk(o):
                if isinstance(o, dict):
                    for k, v in o.items():
                        if k == "profile" and isinstance(v, str):
                            yield v
                        else:
                            yield from walk(v)
                elif isinstance(o, list):
                    for v in o:
                        yield from walk(v)
            for prof in walk(drv):
                if not (CHIPS / f"{prof}.json").exists():
                    r.err(f"driver.json 引用 profile {prof!r}，但 ports/_chips/{prof}.json 不存在")
    else:
        r.warn("沒有 build-time driver.json（只有 USB 配置的板子可以沒有）")

    # --- L3 config/*.json：一個層一個檔；不准放 L1 的東西 ---
    cfg_dir = d / "data" / "config"
    cfg_files = sorted(cfg_dir.glob("*.json")) if cfg_dir.is_dir() else []
    if cfg_files:
        for cfg_json in cfg_files:
            try:
                data = json.loads(cfg_json.read_text(encoding="utf-8"))
            except json.JSONDecodeError as e:
                r.err(f"data/config/{cfg_json.name} 不是合法 JSON：{e}")
                continue
            allowed = RUNTIME_KEYS_BY_FILE.get(cfg_json.name)
            if allowed is None:
                r.warn(f"data/config/{cfg_json.name} 不在已知層清單"
                       f"（{'/'.join(sorted(RUNTIME_KEYS_BY_FILE))}）")
                continue
            for k in data:
                if k not in allowed:
                    r.warn(f"data/config/{cfg_json.name} 有未知的鍵 {k!r}")
            for bad in ("flash_size", "flash_freq", "flash_mode", "psram", "psram_size",
                        "cpu_mhz", "chip", "partition_table"):
                if bad in data:
                    r.err(f"data/config/{cfg_json.name} 出現 {bad!r}"
                          f" —— 這屬於 build-time，要寫在 board.json / sdkconfig.defaults")
    else:
        r.warn("沒有 data/config/*.json（P4 這類沒有 USB 的板子可以沒有，但 S3 會沒有出廠預置）")

    # --- board.json 裡有沒有未處理的鍵 ---
    known = {"name", "chip", "flash", "psram", "cpu_mhz", "console", "profiles",
             "features", "verified", "notes", "_comment"}
    for k in b:
        if k not in known:
            r.warn(f"board.json 有未知欄位 {k!r}")

    # --- 共通 defaults 不准偷渡板子事實 ---
    for sym in cfg_common:
        if BOARD_FACT_RE.match(sym):
            r.err(f"firmware/sdkconfig.defaults 設了板子專屬的 CONFIG_{sym} —— 應該搬到 port 的 sdkconfig.defaults")

    return r


def _blocks_in(text: str) -> list[str]:
    return re.findall(r"^# >>> profile: (\S+)", text, re.MULTILINE)


def _extract_block(text: str, name: str) -> str | None:
    pat = re.compile(
        re.escape(MARK_OPEN.format(name=name)) + r"\n(.*?)\n" + re.escape(MARK_CLOSE.format(name=name)),
        re.DOTALL,
    )
    m = pat.search(text)
    return m.group(1) if m else None


# ─────────────────────────── sync ───────────────────────────

def sync_board(d: Path) -> None:
    b = load_board(d)
    declared = b.get("profiles") or []
    target = d / "sdkconfig.defaults"

    text = target.read_text(encoding="utf-8") if target.exists() else ""

    # 砍掉舊的碎片區塊：從標頭開始整段丟掉（標頭之後只會有 marker 區塊）
    idx = text.find(MARK_HEAD.splitlines()[0])
    if idx != -1:
        text = text[:idx]
    # 保險：清掉任何殘留的 marker 區塊
    text = re.sub(r"^# >>> profile: .*?\n.*?^# <<< profile: .*?\n",
                  "", text, flags=re.DOTALL | re.MULTILINE)

    text = re.sub(r"\n{3,}", "\n\n", text).rstrip() + "\n"

    if declared:
        chunks = [MARK_HEAD]
        for name in declared:
            src = PROFILES / f"{name}.defaults"
            if not src.exists():
                raise SystemExit(f"找不到能力碎片：ports/_profiles/{name}.defaults")
            body = src.read_text(encoding="utf-8").strip("\n")
            chunks.append(f"{MARK_OPEN.format(name=name)}\n{body}\n{MARK_CLOSE.format(name=name)}\n")
        text = text.rstrip() + "\n\n" + "".join(chunks)

    target.write_text(text, encoding="utf-8")


# ─────────────────────────── 指令 ───────────────────────────

def find_boards(query: str | None) -> list[Path]:
    dirs = board_dirs()
    if not query:
        return dirs
    q = query.lower().rstrip("/")
    hits = []
    for d in dirs:
        b = load_board(d)
        names = {b.get("name", "").lower(), d.name.lower(), f"{d.parent.name.lower()}/{d.name.lower()}"}
        if q in names:
            hits.append(d)
            continue
        # 用晶片目錄名查（該晶片只有一塊板時）
        if q == d.parent.name.lower() and len([x for x in dirs if x.parent == d.parent]) == 1:
            hits.append(d)
    if not hits:
        raise SystemExit(f"找不到板子：{query}（用 tools/board.py list 看有哪些）")
    return hits


def cmd_list(_args) -> int:
    dirs = board_dirs()
    if not dirs:
        print("ports/ 下沒有任何 board.json")
        return 1
    print(f"{'板子':<26} {'target':<9} {'flash':<16} {'psram':<20} {'eth':<12} console")
    print("─" * 104)
    for d in dirs:
        b = load_board(d)
        f, p = (b.get("flash") or {}), (b.get("psram") or {})   # psram 可以是 null
        flash = f"{f.get('size_mb', '?')}MB@{f.get('freq_mhz', '?')}M {f.get('mode', '')}"
        psram = (f"{p.get('size_mb')}MB {p.get('mode')}@{p.get('speed_mhz')}M"
                 if p.get("size_mb") else "—")
        eth = (b.get("features") or {}).get("eth") or "—"
        print(f"{b.get('name', d.name):<26} {b.get('chip', '?'):<9} {flash:<16} {psram:<20} {eth:<12} {b.get('console', '?')}")
    print(f"\n{len(dirs)} 塊板子；能力碎片：{[p.stem for p in sorted(PROFILES.glob('*.defaults'))]}")
    return 0


def cmd_check(args) -> int:
    cfg_common = parse_kconfig(COMMON_DEFAULTS)

    # sdkconfig 只會在「不存在」時由 defaults 產生；改了 defaults 但留著舊 sdkconfig 會靜默不生效。
    # 只比對「目前 sdkconfig 那個 target」的板子，否則新增一塊板就會誤報。
    gen = ROOT / "firmware" / "sdkconfig"
    if gen.exists():
        target = (parse_kconfig(gen).get("IDF_TARGET") or "").strip('"')
        relevant = [COMMON_DEFAULTS]
        for d in board_dirs():
            if load_board(d).get("chip") == target:
                relevant.append(d / "sdkconfig.defaults")
        newest = max((p.stat().st_mtime for p in relevant if p.exists()), default=0)
        if gen.stat().st_mtime < newest:
            print(f"⚠ firmware/sdkconfig（target={target}）比 sdkconfig.defaults 舊 —— 改了不會生效。")
            print("  先 rm firmware/sdkconfig（tools/board.sh build 也會在換 target 時重設），再 build。\n")

    total_err = total_warn = 0
    for d in find_boards(args.board):
        b = load_board(d)
        r = check_board(d, cfg_common, effective_config(d))
        status = "✘" if r.errors else ("△" if r.warns else "✔")
        print(f"{status} {rel(d)}  ({b.get('chip')}, {b.get('name')})")
        for m in r.errors:
            print(f"    ERROR {m}")
        for m in r.warns:
            print(f"    warn  {m}")
        total_err += len(r.errors)
        total_warn += len(r.warns)
    print(f"\n{total_err} 個錯誤、{total_warn} 個警告")
    return 1 if total_err else 0


def cmd_sync(args) -> int:
    for d in find_boards(args.board):
        sync_board(d)
        print(f"sync {rel(d)}")
    return 0


def cmd_resolve(args) -> int:
    d = find_boards(args.board)[0]
    b = load_board(d)
    print(f"{rel(d)} {b['chip']} {CHIP_ALIAS.get(b['chip'], b['chip']).upper()}")
    return 0


def cmd_new(args) -> int:
    chip_dir = args.chip.upper()
    chip = CHIP_DIR.get(chip_dir.lower(), args.chip.lower())
    d = PORTS / chip_dir / args.board
    if d.exists():
        raise SystemExit(f"已存在：{rel(d)}")

    profiles = []
    if args.eth and args.eth != "none":
        profiles.append("eth-rmii" if args.eth == "rmii" else f"eth-{args.eth}")
    if args.ws:
        profiles.append("nc4-ws")
    if args.udp:
        profiles.append("nc4-udp")

    board = {
        "_comment": "硬體事實。改這裡 = 要重新 build；runtime 可改的放 data/config/*.json。",
        "name": args.board,
        "chip": chip,
        "flash": {"size_mb": args.flash, "freq_mhz": args.flash_freq, "mode": args.flash_mode},
        "psram": ({"size_mb": args.psram, "mode": args.psram_mode, "speed_mhz": args.psram_speed}
                  if args.psram else {"size_mb": 0}),
        "cpu_mhz": args.cpu,
        "console": args.console,
        "profiles": profiles,
        "features": {"eth": args.eth if args.eth != "none" else None,
                     "wifi": bool(args.wifi), "usb": bool(args.usb)},
        "verified": None,
        "notes": "TODO：這塊板子是什麼、腳位、驗證過什麼",
    }

    (d / "data").mkdir(parents=True)
    (d / "board.json").write_text(json.dumps(board, ensure_ascii=False, indent=2) + "\n")
    (d / "sdkconfig.defaults").write_text(
        f"# {args.board} — 硬體事實（改這裡要重新 build）\n"
        f"# 由 tools/board.py new 產生；tools/board.py check 會驗證與 board.json 一致\n\n"
        f"# ── flash ──\n"
        f"CONFIG_ESPTOOLPY_FLASHSIZE_{args.flash}MB=y\n"
        f"CONFIG_ESPTOOLPY_FLASHFREQ_{args.flash_freq}M=y\n"
        f"CONFIG_ESPTOOLPY_FLASHMODE_{args.flash_mode.upper()}=y\n"
        + (f"\n# ── PSRAM ──\nCONFIG_SPIRAM=y\n"
           f"CONFIG_SPIRAM_MODE_{PSRAM_MODE[args.psram_mode.lower()]}=y\n"
           f"CONFIG_SPIRAM_SPEED_{args.psram_speed}M=y\n" if args.psram else "")
        + f"\n# ── CPU ──\nCONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_{args.cpu}=y\n"
        + f"\n# ── console ──\nCONFIG_{CONSOLE_SYM[args.console.lower()]}=y\n"
    )
    cfg_dir = d / "data" / "config"
    cfg_dir.mkdir(parents=True, exist_ok=True)
    (cfg_dir / "system.json").write_text(json.dumps({
        "version": 1,
        "_comment": "System 層：身份、開機、NC4 通道與解碼路線。",
        "hostname": "", "frame_interval_ms": 20, "num_pixels": 0,
    }, ensure_ascii=False, indent=2) + "\n")
    (cfg_dir / "network.json").write_text(json.dumps({
        "version": 1,
        "_comment": "Network 層：只放 IP / WiFi。",
        "wifi": {"enable": 0, "ssid": "", "ssid_pw": ""},
    }, ensure_ascii=False, indent=2) + "\n")
    (d / "README.md").write_text(
        f"# {args.board}\n\n"
        f"| 項目 | 值 |\n|---|---|\n"
        f"| 晶片 | `{chip}` |\n"
        f"| Flash | {args.flash} MB @ {args.flash_freq} MHz {args.flash_mode.upper()} |\n"
        f"| PSRAM | {f'{args.psram} MB {args.psram_mode} @ {args.psram_speed} MHz' if args.psram else '無'} |\n"
        f"| CPU | {args.cpu} MHz |\n"
        f"| Console | {args.console} |\n"
        f"| 能力碎片 | {', '.join(profiles) if profiles else '—'} |\n\n"
        f"## 這塊板子是什麼\n\nTODO\n\n"
        f"## 驗證過什麼\n\n- [ ] build\n- [ ] 開機 log\n- [ ] 主要功能\n"
    )

    sync_board(d)
    print(f"建立 {rel(d)}")
    print("下一步：")
    print(f"  1. 編輯 {rel(d)}/board.json 的 notes / README")
    print(f"  2. python3 tools/board.py check {args.board}")
    print(f"  3. tools/board.sh build {args.board}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("list", help="列出所有板子與能力").set_defaults(fn=cmd_list)

    p = sub.add_parser("check", help="檢查一致性")
    p.add_argument("board", nargs="?", help="只查這塊（預設全部）")
    p.set_defaults(fn=cmd_check)

    p = sub.add_parser("sync", help="把能力碎片寫進 sdkconfig.defaults")
    p.add_argument("board", nargs="?")
    p.set_defaults(fn=cmd_sync)

    p = sub.add_parser("resolve", help="查板子 → 目錄 / target / 晶片（給 board.sh 用）")
    p.add_argument("board")
    p.set_defaults(fn=cmd_resolve)

    p = sub.add_parser("new", help="新增一塊板子")
    p.add_argument("--chip", required=True, help="S3 / P4 / C6 …")
    p.add_argument("--board", required=True, help="板子目錄名，例如 ESP32-S3-Matrix")
    p.add_argument("--flash", type=int, required=True, help="flash 大小 MB")
    p.add_argument("--flash-freq", type=int, default=80, help="flash 頻率 MHz（預設 80）")
    p.add_argument("--flash-mode", default="dio", choices=["dio", "qio", "dout", "qout"])
    p.add_argument("--psram", type=int, default=0, help="PSRAM 大小 MB（0 = 沒有）")
    p.add_argument("--psram-mode", default="octal", choices=["octal", "hex", "quad"])
    p.add_argument("--psram-speed", type=int, default=80)
    p.add_argument("--cpu", type=int, default=240)
    p.add_argument("--console", default="usb-serial-jtag",
                   choices=["uart0", "usb-serial-jtag", "usb-cdc"])
    p.add_argument("--eth", default="none", help="none / rmii …")
    p.add_argument("--wifi", action="store_true")
    p.add_argument("--usb", action="store_true")
    p.add_argument("--ws", action="store_true", help="加上 NC4 over WebSocket 能力碎片")
    p.add_argument("--udp", action="store_true", help="加上 NC4 over UDP 能力碎片")
    p.set_defaults(fn=cmd_new)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
