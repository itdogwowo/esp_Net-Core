#!/usr/bin/env python3
"""板子清單與設定檢查（ports/hardware/ ＋ ports/project/）

為什麼要分兩層：

    ports/hardware/<chip>/<board>/   這塊板「有什麼」——硬體事實、腳位、匯流排、晶片
    ports/project/<chip>/<board>/    這塊板「跑什麼」——項目、通道、路由、任務、預置

    L1 硬體事實（編譯期）  flash 大小/頻率/模式、PSRAM、CPU 頻率、console、ETH
                           → hardware/board.json + hardware/sdkconfig.defaults
    L2 能力碎片（編譯期）  ETH／WiFi／NC4-WS／UDP 一律 → hardware/board.json 的 profiles
                           project 層只放 runtime 設定，冇任何 build 設定
    L3 執行期（同一 binary）gpio、服務開關、NC4 路線、數量
                           → project/data/config/<layer>.json

同一塊硬體可以掛多個 project；project.json 的 "hardware" 指回 hardware 層。
project ＝ 部署／用戶設定（project.json ＋ data/config/*.json），**不含 build 設定**：
唔應該有 `sdkconfig.defaults`、亦唔應該有 `profiles`（能力碎片全部屬 hardware）。

用法：
    python3 tools/board.py list
    python3 tools/board.py check            # 全部
    python3 tools/board.py check p4         # 只查一塊
    python3 tools/board.py sync p4
    python3 tools/board.py resolve p4       # 給 make.py 用
    python3 tools/board.py new --chip S3 --board ESP32-S3-Matrix ...
    python3 tools/board.py new --project demo --hardware P4/ESP32-P4-ETH
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PORTS = ROOT / "ports"
HW = PORTS / "hardware"
PJ = PORTS / "project"
PROFILES = PORTS / "_profiles"
CHIPS = PORTS / "_chips"
COMMON_DEFAULTS = ROOT / "firmware" / "sdkconfig.defaults"

CHIP_DIR = {"s3": "esp32s3", "p4": "esp32p4", "c6": "esp32c6", "c3": "esp32c3",
            "s2": "esp32s2", "h2": "esp32h2", "c5": "esp32c5", "c61": "esp32c61"}
CHIP_ALIAS = {v: k for k, v in CHIP_DIR.items()}

# ── 命名規範（2026-11-16；完整說明見 ports/README.md §10）──
#   硬體板目錄名 = <CHIP>-<Identity>，晶片永遠在最前；ASCII 英數 ＋ '-'。
#   品牌／型號／板本不放最前、也不進目錄名（寫 board.json 的 brand／model／rev）。
#   改名必須加 aliases，舊 build 指令才不會斷。
BOARD_NAME_RE = re.compile(r"^[A-Z][A-Za-z0-9]*(-[A-Za-z0-9]+)*$")
CHIP_NAME_PREFIX = {
    "esp32": "ESP32", "esp32s2": "ESP32-S2", "esp32s3": "ESP32-S3",
    "esp32c3": "ESP32-C3", "esp32c5": "ESP32-C5", "esp32c6": "ESP32-C6",
    "esp32c61": "ESP32-C61", "esp32h2": "ESP32-H2", "esp32p4": "ESP32-P4",
}


def chip_prefix(chip: str) -> str | None:
    return CHIP_NAME_PREFIX.get(chip)


def board_name_problem(name: str, chip: str) -> str | None:
    """命名不符公約就回傳原因；合格回 None。"""
    if not BOARD_NAME_RE.match(name):
        return ("只准 ASCII 英數與 '-'，每個 '-' 分隔的詞要用大寫字母開頭"
                f"（例：ESP32-S3-Mini-Nano）；現在是 {name!r}")
    pref = chip_prefix(chip)
    if pref and not (name == pref or name.startswith(pref + "-")):
        return f"要以晶片前綴 {pref!r} 開頭（例：{pref}-ETH）；現在是 {name!r}"
    return None

# 這些鍵屬於 L1（硬體事實），出現在共通 defaults 或 runtime config 就是放錯層
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
    "none": "ESP_CONSOLE_NONE",
}

# 每個 runtime 檔允許的頂層鍵（檔名 → 允許的鍵）；一個層一個檔
RUNTIME_KEYS_BY_FILE = {
    "system.json":   {"version", "_comment", "cID", "log_debug_mode", "hostname",
                      "frame_interval_ms", "boot_strict", "num_pixels", "nc4", "usb"},
    "network.json":  {"version", "_comment", "hostname", "priority", "lan", "wifi", "bt", "espnow"},
    "tasks.json":    {"version", "_comment", "tasks", "override"},
    # hardware.json 是 ncportgen 從 wiring.json 產生的「完整硬體預置」；
    # 它出現在 project 層是為了讓用戶在 /data/config 看得到、改得到。
    # 改 wiring 之後跑 tools/sync_hardware_presets.py 同步。
    "hardware.json": {"version", "_comment", "_gpio_convention", "ETH", "USB", "PIN", "SPI",
                      "I2C", "I80", "UART", "I2S", "PWM", "SDIO", "ENC", "XL9555"},
}

MARK_OPEN = "# >>> profile: {name}"
MARK_CLOSE = "# <<< profile: {name}"
MARK_HEAD = ("# ─── 能力碎片（由 tools/board.py sync 產生，不要手改這段）───\n"
             "# 來源：ports/_profiles/*.defaults；{layer} 的 profiles 決定要哪些\n")


# ─────────────────────────── 讀取 ───────────────────────────

def hardware_dirs() -> list[Path]:
    return sorted(p.parent for p in HW.glob("*/*/board.json"))


def project_dirs() -> list[Path]:
    return sorted(p.parent for p in PJ.glob("*/*/project.json"))


# 舊名字：很多工具只關心「有哪些硬體板」
def board_dirs() -> list[Path]:
    return hardware_dirs()


def load_board(d: Path) -> dict:
    return json.loads((d / "board.json").read_text(encoding="utf-8"))


def load_project(d: Path) -> dict:
    return json.loads((d / "project.json").read_text(encoding="utf-8"))


def aliases_of(meta: dict) -> list[str]:
    """board.json／project.json 的 aliases：舊名／別名，只影響解析，不影響路徑。

    這裡刻意不 raise：格式錯的 aliases 由 check 報，讀取端只取可用的字串。
    """
    raw = meta.get("aliases") if isinstance(meta, dict) else None
    if not isinstance(raw, list):
        return []
    return [str(x).strip() for x in raw if isinstance(x, str) and str(x).strip()]


def alias_problems(meta: dict, label: str) -> list[str]:
    """aliases 欄位的格式檢查（路徑／撞名由 cmd_check 另外做）。"""
    out: list[str] = []
    raw = meta.get("aliases")
    if raw is None:
        return out
    if not isinstance(raw, list):
        return [f"{label} 的 aliases 必須是字串陣列"]
    for a in raw:
        if not isinstance(a, str) or not a.strip():
            out.append(f"{label} 的 aliases 有空值或非字串：{a!r}")
        elif "/" in a:
            out.append(f"{label} 的 alias {a!r} 不可以含 '/'（alias 只係名，唔係路徑）")
    return out


def hw_name(d: Path) -> str:
    """傳給 CMake 的 -DPORT：晶片目錄/板子目錄（相對於 ports/hardware/）。"""
    return f"{d.parent.name}/{d.name}"


def pj_name(d: Path | None) -> str:
    return "" if d is None else f"{d.parent.name}/{d.name}"


def rel(p: Path) -> str:
    return str(p.relative_to(ROOT))


def projects_for_hardware(d: Path) -> list[Path]:
    """這塊硬體板掛了哪些 project（project.json.hardware 指回它）。

    回傳順序：同名 project（慣例上的 default）排最前，其餘按目錄名。
    """
    want = hw_name(d)
    out: list[Path] = []
    for p in project_dirs():
        try:
            if load_project(p).get("hardware") == want:
                out.append(p)
        except json.JSONDecodeError:
            continue
    out.sort(key=lambda p: (p.name != d.name, p.name))
    return out


def project_is_default(p: Path) -> bool:
    """project.json 有寫 default:true 嗎？"""
    try:
        return bool(load_project(p).get("default"))
    except json.JSONDecodeError:
        return False


def default_project_for_hardware(d: Path) -> Path | None:
    """這塊硬體板的 default project。

    規則（順序）：
      1. project.json 寫 `default: true` 的那個（每塊板最多一個，check 會擋）
      2. 否則，同名 project（`ports/project/<chip>/<board>/`）＝慣例上的 default
      3. 都沒有 → None（build 時就是純硬體，冇 project 預置）
    """
    ps = projects_for_hardware(d)
    flagged = [p for p in ps if project_is_default(p)]
    if flagged:
        return flagged[0]        # 多過一個由 check 報錯；解析先用第一個
    same = PJ / d.parent.name / d.name
    return same if same in ps else None


def project_for_hardware(d: Path) -> Path | None:
    """（保留舊名）回傳這塊板的 default project。"""
    return default_project_for_hardware(d)


def project_match_keys(p: Path) -> set[str]:
    try:
        pj = load_project(p)
    except json.JSONDecodeError:
        pj = {}
    return {p.name.lower(), str(pj.get("name", "")).lower(),
            *{a.lower() for a in aliases_of(pj)}}


def find_project_for_hardware(d: Path, query: str) -> Path | None:
    """用短名找這塊板的一個 project。

    先精確比 name／目錄／alias（不分大小寫）；再接受 `<硬體板>-<用途>` 的後綴
    （所以 `--project demo` 會命中 `ESP32-P4-ETH-Demo`）。多過一個候選 → None。
    """
    q = query.strip().lower()
    ps = projects_for_hardware(d)
    if not q or not ps:
        return None
    exact = [p for p in ps if q in project_match_keys(p)]
    if len(exact) == 1:
        return exact[0]
    if len(exact) > 1:
        return None
    suffix = [p for p in ps if any(k.endswith("-" + q) for k in project_match_keys(p))]
    return suffix[0] if len(suffix) == 1 else None


def hardware_for_project(d: Path) -> Path | None:
    hw = load_project(d).get("hardware")
    if not hw:
        return None
    cand = HW / hw
    return cand if (cand / "board.json").is_file() else None


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
    """建置時真正的 L1 值：共通 defaults → 板子覆蓋（後者勝）。"""
    cfg = parse_kconfig(COMMON_DEFAULTS)
    cfg.update(parse_kconfig(d / "sdkconfig.defaults"))
    return cfg


# ─────────────────────────── 檢查 ───────────────────────────

class Report:
    def __init__(self) -> None:
        self.errors: list[str] = []
        self.warns: list[str] = []

    def err(self, msg: str) -> None:
        self.errors.append(msg)

    def warn(self, msg: str) -> None:
        self.warns.append(msg)


def _check_profiles(d: Path, declared: list[str], label: str, r: Report) -> None:
    """宣告了哪些碎片，就要同步進這一層的 sdkconfig.defaults。"""
    path = d / "sdkconfig.defaults"
    text = path.read_text(encoding="utf-8") if path.exists() else ""
    for prof in declared:
        src = PROFILES / f"{prof}.defaults"
        if not src.exists():
            r.err(f"{label} 指定了能力碎片 {prof!r}，但 ports/_profiles/{prof}.defaults 不存在")
            continue
        block = _extract_block(text, prof)
        if block is None:
            r.err(f"{label} 的能力碎片 {prof!r} 還沒同步進 {rel(path)} → 跑 tools/board.py sync")
        elif block.strip() != src.read_text(encoding="utf-8").strip():
            r.err(f"{label} 的能力碎片 {prof!r} 與 ports/_profiles/{prof}.defaults 不同步 → 跑 tools/board.py sync")
    for prof in _blocks_in(text):
        if prof not in declared:
            r.warn(f"{rel(path)} 有 {prof!r} 的區塊，但 {label} 沒宣告（sync 會移除）")


def _check_seeds(pdir: Path, declared: list[str], r: Report) -> None:
    cfg_dir = pdir / "data" / "config"
    for name in declared:
        p = cfg_dir / f"{name}.json"
        if not p.is_file():
            r.err(f"project.json 宣告 seed {name!r}，但 {rel(p)} 不存在")
    files = sorted(cfg_dir.glob("*.json")) if cfg_dir.is_dir() else []
    for cfg_json in files:
        try:
            data = json.loads(cfg_json.read_text(encoding="utf-8"))
        except json.JSONDecodeError as e:
            r.err(f"{rel(cfg_json)} 不是合法 JSON：{e}")
            continue
        if not isinstance(data, dict):
            r.err(f"{rel(cfg_json)} 頂層不是物件")
            continue
        allowed = RUNTIME_KEYS_BY_FILE.get(cfg_json.name)
        if allowed is None:
            r.warn(f"{rel(cfg_json)} 不在已知層清單"
                   f"（{'/'.join(sorted(RUNTIME_KEYS_BY_FILE))}）")
            continue
        for k in data:
            if k not in allowed:
                r.warn(f"{rel(cfg_json)} 有未知的鍵 {k!r}")
        for bad in ("flash_size", "flash_freq", "flash_mode", "psram", "psram_size",
                    "cpu_mhz", "chip", "partition_table"):
            if bad in data:
                r.err(f"{rel(cfg_json)} 出現 {bad!r}"
                      f" —— 這屬於 build-time，要寫在 hardware/board.json 或 sdkconfig.defaults")
        # link schema（目標）：line = kind + index；kind 是 composite（eth_tcp／wifi_tcp），
        # 不再寫 iface／service；name 可以由 kind+index 生成，亦可以用 name 覆寫。
        if cfg_json.name == "system.json":
            nc4 = data.get("nc4")
            links = nc4.get("links") if isinstance(nc4, dict) else None
            if isinstance(links, list):
                seen_idents: dict[tuple[str, int], int] = {}
                for i, l in enumerate(links):
                    if not isinstance(l, dict):
                        continue
                    kind = l.get("kind")
                    if isinstance(kind, str) and kind in ("tcp", "udp", "ws", "http"):
                        r.warn(f"{rel(cfg_json)} links[{i}] kind={kind!r} 未帶 interface"
                               f" → 用 composite kind（eth_{kind}／wifi_{kind}）")
                    if "iface" in l:
                        r.warn(f"{rel(cfg_json)} links[{i}] 有 iface；"
                               f"目標 schema 的 iface 由 composite kind 決定，請移除")
                    if "service" in l:
                        r.warn(f"{rel(cfg_json)} links[{i}] 有 service；"
                               f"service 已移除（claim 由 kind／port 決定）")
                    if "index" not in l:
                        r.warn(f"{rel(cfg_json)} links[{i}] 缺 index；"
                               f"line identity = kind + index")
                    elif isinstance(kind, str) and isinstance(l.get("index"), int):
                        key = (kind, l["index"])
                        if key in seen_idents:
                            r.err(f"{rel(cfg_json)} links[{i}] 與 links[{seen_idents[key]}] "
                                  f"的 (kind,index)={key} 重複；line identity 必須唯一")
                        else:
                            seen_idents[key] = i
                    cfg = l.get("cfg")
                    if isinstance(cfg, dict) and ("port" in cfg or "rx" in cfg):
                        r.warn(f"{rel(cfg_json)} links[{i}] 用舊 cfg.port/cfg.rx"
                               f" → 搬到 link 頂層")
                        if "port" in cfg and "port" in l and cfg["port"] != l["port"]:
                            r.err(f"{rel(cfg_json)} links[{i}] 的 port 與 cfg.port 不一致")
    if (cfg_dir / "hardware.json").is_file():
        # 允許：它是 generated snapshot（sync_hardware_presets.py 寫的），
        # 不是手寫層；內容仍由 wiring.json 決定。
        pass


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
        r.err(f"board.json chip={chip} 與目錄 ports/hardware/{d.parent.name}/ 不符")
    if chip and chip not in CHIP_ALIAS:
        r.warn(f"未見過的 chip={chip}（新增晶片請更新 CHIP_DIR）")

    # --- 命名規範（ports/README.md §10）---
    if str(name) != d.name:
        r.err(f"board.json name={name!r} 要同目錄名 {d.name!r} 一致（改名要連目錄一齊改）")
    prob = board_name_problem(str(name), str(chip))
    if prob:
        r.err(f"命名不符 ports 公約：{prob}")
    for msg in alias_problems(b, "board.json"):
        r.err(msg)
    for k in ("brand", "model", "rev"):
        if k in b and b[k] is not None and not isinstance(b[k], str):
            r.err(f"board.json {k} 必須是字串或 null")

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
        r.err(f"console={b.get('console')!r} 看不懂（用 uart0 / usb-serial-jtag / usb-cdc / none）")
    else:
        want(con, "y", "console")

    feats = b.get("features") or {}
    eth = feats.get("eth")
    if eth:
        want("ETH_ENABLED", "y", "Ethernet")
    elif cfg_port.get("ETH_ENABLED") == "y":
        r.err("board.json 說沒有 Ethernet，但 sdkconfig 開了 CONFIG_ETH_ENABLED=y")

    # --- 分割表：宣告了就要存在（路徑相對於 firmware/，IDF 的 project dir）---
    part_name = cfg_port.get("PARTITION_TABLE_CUSTOM_FILENAME")
    if part_name:
        part_path = (ROOT / "firmware" / part_name.strip('"')).resolve()
        if not part_path.is_file():
            r.err(f"sdkconfig 的分割表路徑不存在：{rel(part_path) if part_path.is_relative_to(ROOT) else part_path}")
    elif (d / "partitions.csv").is_file():
        r.warn(f"{rel(d)}/partitions.csv 存在，但 sdkconfig 沒有指向它")

    # --- L2 能力碎片：全部在 hardware 層（project 冇 build 設定）---
    _check_profiles(d, b.get("profiles") or [], "hardware/board.json", r)

    # --- hardware 不應該再有 runtime 預置（那是 project 層）---
    if (d / "data" / "config").is_dir():
        r.err(f"{rel(d)}/data/config/ 屬於 project 層，硬體層不應該有 runtime 預置")

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

    # --- project 層（每塊硬體可以有多個 project；其中一個可係 default）---
    pdirs = projects_for_hardware(d)
    defaults = [p for p in pdirs if project_is_default(p)]
    if len(defaults) > 1:
        r.err("project 層有多個 default: true → "
              + "、".join(rel(p) for p in defaults)
              + "（每塊硬體板最多一個 default）")
    if not pdirs:
        r.warn("沒有對應的 project 層（ports/project/<chip>/<board>/project.json）；"
               "build 時就係純硬體（冇 project 預置）")
    elif not defaults and not any(p.name == d.name for p in pdirs):
        r.warn("有 project 但冇 default（冇 project.json 的 default:true，亦冇同名 project）；"
               "make.py build <硬體板> 會變成純硬體 —— 要一個 default 就加 default:true")
    for pdir in pdirs:
        try:
            p = load_project(pdir)
        except json.JSONDecodeError as e:
            r.err(f"{rel(pdir)}/project.json 不是合法 JSON：{e}")
            continue
        if not isinstance(p, dict):
            r.err(f"{rel(pdir)}/project.json 頂層不是物件")
            continue
        if p.get("hardware") != hw_name(d):
            r.err(f"{rel(pdir)}/project.json hardware={p.get('hardware')!r}，但這塊是 {hw_name(d)!r}")
        if "default" in p and not isinstance(p["default"], bool):
            r.err(f"{rel(pdir)}/project.json default 必須是 true/false")
        pname = str(p.get("name") or pdir.name)
        if pname != pdir.name:
            r.warn(f"{rel(pdir)}/project.json name={pname!r} 與目錄名 {pdir.name!r} 不同"
                   f"（公約：project 名 = 目錄名）")
        elif pdir.name != d.name and not pdir.name.startswith(d.name + "-"):
            r.warn(f"{rel(pdir)}：同一 chip 下多個 project 建議用 <硬體板>-<用途>"
                   f"（例如 {d.name}-Demo），避免撞名")
        for msg in alias_problems(p, "project.json"):
            r.err(msg)
        if "profiles" in p:
            r.err(f"{rel(pdir)}/project.json 有 profiles：能力碎片屬 hardware 層，"
                  f"搬去 hardware/board.json 的 profiles（project 只放 runtime 設定）")
        if (pdir / "sdkconfig.defaults").is_file():
            r.err(f"{rel(pdir)}/sdkconfig.defaults 係 build 設定，唔應該放 project 層；"
                  f"搬去 hardware/sdkconfig.defaults")
        _check_seeds(pdir, p.get("seed") or [], r)
        known_p = {"version", "name", "hardware", "seed", "aliases", "default", "_comment"}
        for k in p:
            if k not in known_p:
                r.warn(f"project.json 有未知欄位 {k!r}")

    # --- board.json 裡有沒有未處理的鍵 ---
    known = {"name", "chip", "flash", "psram", "cpu_mhz", "console", "profiles",
             "features", "verified", "notes", "brand", "model", "rev", "aliases", "_comment"}
    for k in b:
        if k not in known:
            r.warn(f"board.json 有未知欄位 {k!r}")

    # --- 共通 defaults 不准偷渡板子事實 ---
    for sym in cfg_common:
        if BOARD_FACT_RE.match(sym):
            r.err(f"firmware/sdkconfig.defaults 設了板子專屬的 CONFIG_{sym} —— 應該搬到 port 的 sdkconfig.defaults")

    _ = name
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

def _sync_layer(d: Path, declared: list[str], layer: str) -> None:
    target = d / "sdkconfig.defaults"
    original = target.read_text(encoding="utf-8") if target.exists() else ""
    text = original

    idx = text.find(MARK_HEAD.format(layer=layer).splitlines()[0])
    if idx != -1:
        text = text[:idx]
    text = re.sub(r"^# >>> profile: .*?\n.*?^# <<< profile: .*?\n",
                  "", text, flags=re.DOTALL | re.MULTILINE)
    text = re.sub(r"\n{3,}", "\n\n", text).rstrip() + "\n"

    if not declared and not text.strip():
        if target.exists():
            target.unlink()
        return

    if declared:
        chunks = [MARK_HEAD.format(layer=layer)]
        for name in declared:
            src = PROFILES / f"{name}.defaults"
            if not src.exists():
                raise SystemExit(f"找不到能力碎片：ports/_profiles/{name}.defaults")
            body = src.read_text(encoding="utf-8").strip("\n")
            chunks.append(f"{MARK_OPEN.format(name=name)}\n{body}\n{MARK_CLOSE.format(name=name)}\n")
        text = text.rstrip() + "\n\n" + "".join(chunks)

    # 內容冇變就唔好寫：write_text 會更新 mtime，令 doctor 的
    # "build 的 sdkconfig 比 defaults 舊" 出現假警告（sync 一次全部板都變 stale）。
    if text != original:
        target.write_text(text, encoding="utf-8")


def sync_board(d: Path) -> None:
    """只同步 hardware 層。project 係 runtime 設定，冇 build 設定可以同步。"""
    _sync_layer(d, load_board(d).get("profiles") or [], "hardware/board.json")


# ─────────────────────────── 指令 ───────────────────────────

def find_ports(query: str | None) -> list[tuple[Path, Path | None]]:
    """回傳 [(hardware 目錄, project 目錄或 None)]。query 可以命中任一層。"""
    hw = hardware_dirs()
    pj = project_dirs()
    pairs: list[tuple[Path, Path | None]] = []
    for d in hw:
        pairs.append((d, project_for_hardware(d)))
    if not query:
        return pairs

    q = query.lower().rstrip("/")
    if q.startswith("hw:"):
        q = q[3:]
    elif q.startswith("pj:") or q.startswith("project:"):
        q = q.split(":", 1)[1]

    def hw_hit(d: Path) -> bool:
        b = load_board(d)
        names = {str(b.get("name", "")).lower(), d.name.lower(), hw_name(d).lower()}
        names |= {a.lower() for a in aliases_of(b)}
        return q in names or (q == d.parent.name.lower() and
                              len([x for x in hw if x.parent == d.parent]) == 1)

    def pj_hit(d: Path) -> bool:
        p = load_project(d)
        names = {str(p.get("name", "")).lower(), d.name.lower(), pj_name(d).lower()}
        names |= {a.lower() for a in aliases_of(p)}
        return q in names or (q == d.parent.name.lower() and
                              len([x for x in pj if x.parent == d.parent]) == 1)

    hits = [(d, project_for_hardware(d)) for d in hw if hw_hit(d)]
    for p in pj:
        if not pj_hit(p):
            continue
        h = hardware_for_project(p)
        if h is None:
            raise SystemExit(f"{rel(p)}/project.json 的 hardware 指向不存在的板子")
        if all(h != hd for hd, _ in hits):
            hits.append((h, p))
    if not hits:
        by_chip = [hw_name(d) for d in hw if d.parent.name.lower() == q]
        if by_chip:
            raise SystemExit(
                f"{q.upper()} 有 {len(by_chip)} 塊板，請指定其中一塊：\n  "
                + "\n  ".join(by_chip))
        raise SystemExit(f"找不到板子／項目：{query}（用 tools/board.py list 看有哪些）")
    return hits


def cmd_list(_args) -> int:
    hw = hardware_dirs()
    if not hw:
        print("ports/hardware/ 下沒有任何 board.json")
        return 1
    print(f"{'硬體板':<24} {'default project':<28} {'target':<9} {'flash':<15} {'psram':<19} eth")
    print("─" * 122)
    for d in hw:
        b = load_board(d)
        ps_for_hw = projects_for_hardware(d)
        default = default_project_for_hardware(d)
        extra = len(ps_for_hw) - (1 if default else 0)
        proj = (default.name + ("*" if project_is_default(default) else "")) if default else "—"
        if extra > 0:
            proj += f" (+{extra})"
        f, ps = (b.get("flash") or {}), (b.get("psram") or {})
        flash = f"{f.get('size_mb', '?')}MB@{f.get('freq_mhz', '?')}M {f.get('mode', '')}"
        psram = (f"{ps.get('size_mb')}MB {ps.get('mode')}@{ps.get('speed_mhz')}M"
                 if ps.get("size_mb") else "—")
        eth = (b.get("features") or {}).get("eth") or "—"
        print(f"{b.get('name', d.name):<24} {proj:<28} "
              f"{b.get('chip', '?'):<9} {flash:<15} {psram:<19} {eth}")
    print(f"\n{len(hw)} 塊硬體板、{len(project_dirs())} 個 project；"
          f"`*` = project.json 有 default:true（其餘係同名 project 慣例）；"
          f"能力碎片：{[p.stem for p in sorted(PROFILES.glob('*.defaults'))]}")
    return 0


def cmd_check(args) -> int:
    cfg_common = parse_kconfig(COMMON_DEFAULTS)

    # 新版每個 build dir 有自己的 sdkconfig（artifacts/build/<board>/sdkconfig）。
    # 它只在「不存在」時由 defaults 產生；改了 defaults 但留著舊 sdkconfig 會靜默不生效。
    legacy = ROOT / "firmware" / "sdkconfig"
    if legacy.exists():
        print("⚠ 舊 firmware/sdkconfig 仍在 —— 新版 make.py 已經不用它；")
        print("  跑 python3 make.py clean 會清掉（不會影響 artifacts/ 裡的 .bin）。\n")
    defaults = [COMMON_DEFAULTS]
    for d in hardware_dirs():
        defaults.append(d / "sdkconfig.defaults")
        p = project_for_hardware(d)
        if p is not None:
            defaults.append(p / "sdkconfig.defaults")
    newest = max((p.stat().st_mtime for p in defaults if p.exists()), default=0)
    for sdk in sorted((ROOT / "artifacts" / "build").glob("*/sdkconfig")):
        if sdk.stat().st_mtime < newest:
            print(f"⚠ {rel(sdk)} 比 sdkconfig.defaults 舊 —— 改了不會生效。")
            print("  跑 python3 make.py build <board> --fresh（或 make.py clean），再 build。\n")

    total_err = total_warn = 0

    # --- alias 撞名：同一個名指向兩塊板／兩個 project → build 會挑錯板 ---
    seen: dict[str, str] = {}
    dup: list[str] = []
    for d, p in find_ports(None):
        b = load_board(d)
        names = {d.name, str(b.get("name") or d.name), *aliases_of(b)}
        if p is not None:
            try:
                pr = load_project(p)
            except json.JSONDecodeError:
                pr = {}
            names |= {p.name, str(pr.get("name") or p.name), *aliases_of(pr)}
        for n in {x.lower() for x in names}:
            prev = seen.get(n)
            if prev and prev != d.name:
                dup.append(f"{n!r} 同時指向 {prev} 與 {d.name}")
            else:
                seen[n] = d.name
    for msg in sorted(set(dup)):
        print(f"✘ alias 撞名：{msg}")
        total_err += 1

    for d, _p in find_ports(args.board):
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
    for d, _p in find_ports(args.board):
        sync_board(d)
        print(f"sync {rel(d)}")
    return 0


def cmd_resolve(args) -> int:
    d, p = find_ports(args.board)[0]
    b = load_board(d)
    print(f"{hw_name(d)} {b['chip']} {CHIP_ALIAS.get(b['chip'], b['chip']).upper()} {pj_name(p) or '-'}")
    return 0


def cmd_projects(args) -> int:
    """列出每塊硬體板掛了哪些 project（default 會標出來）。"""
    boards = hardware_dirs()
    if args.board:
        boards = [d for d, _p in find_ports(args.board)]
    if not boards:
        print("ports/hardware/ 下沒有任何 board.json")
        return 1
    for d in boards:
        b = load_board(d)
        default = default_project_for_hardware(d)
        print(f"{b.get('name', d.name)}  ({hw_name(d)})")
        ps = projects_for_hardware(d)
        if not ps:
            print("  （冇 project；make.py build 這塊板 = 純硬體，冇 project 預置）")
            print()
            continue
        for pdir in ps:
            try:
                pj = load_project(pdir)
            except json.JSONDecodeError:
                print(f"  ✘ {pdir.name}（project.json 壞）")
                continue
            if default is not None and pdir == default:
                if project_is_default(pdir):
                    mark = "[default]"
                else:
                    mark = "[default]"   # 同名 project 的慣例 default
            else:
                mark = "         "
            seed = ",".join(pj.get("seed") or []) or "—"
            print(f"  {mark:<10} {pdir.name:<30} seed={seed}")
        print()
    return 0


def cmd_new(args) -> int:
    if args.project:
        if not args.hardware:
            raise SystemExit("--project 需要 --hardware（要掛在哪塊硬體板上）")
        # 只加一個 project，掛在既有硬體上。
        # 命名公約：project 名 = <硬體板>-<用途>（flat 目錄，同一 chip 下唔會撞名）。
        # 所以 `--project Demo --hardware ESP32-P4-ETH` → ESP32-P4-ETH-Demo；
        # 想自己控制全名就照公約寫（有前綴就原樣用）。
        hw = find_ports(args.hardware)[0][0]
        hwdir = hw.name
        name = args.project
        prefixed = False
        if name != hwdir and not name.startswith(hwdir + "-"):
            name = f"{hwdir}-{name}"
            prefixed = True
        if getattr(args, "default", False):
            existing = [p for p in projects_for_hardware(hw) if project_is_default(p)]
            if existing:
                raise SystemExit(
                    f"{hwdir} 已經有 default project：{rel(existing[0])}\n"
                    f"  要換 default：先改它的 project.json 將 default 改 false（或移走它）")
        if prefixed:
            print(f"→ project 名自動加硬體前綴：{name}")
        d = PJ / hw.parent.name / name
        if d.exists():
            raise SystemExit(f"已存在：{rel(d)}")
        pj = {
            "_comment": "Project 層：這塊硬體上「跑什麼」——只放 runtime 設定"
                        "（project.json ＋ data/config），唔放 build 設定。"
                        "硬體事實與能力碎片一律在 ports/hardware/。",
            "version": 1, "name": name, "hardware": hw_name(hw),
            "seed": ["system", "network"],
        }
        if getattr(args, "default", False):
            pj["default"] = True
        if args.alias:
            pj["aliases"] = list(args.alias)
        cfg_dir = d / "data" / "config"
        cfg_dir.mkdir(parents=True)
        (d / "project.json").write_text(json.dumps(pj, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        (cfg_dir / "system.json").write_text(json.dumps({
            "version": 1, "_comment": "System 層：身份、NC4 通道與解碼路線。",
            "frame_interval_ms": 20, "num_pixels": 0,
        }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        (cfg_dir / "network.json").write_text(json.dumps({
            "version": 1, "_comment": "Network 層：只放 IP / WiFi。",
            "wifi": {"enable": 0, "ssid": "", "ssid_pw": ""},
        }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"建立 {rel(d)}")
        return 0

    if not args.chip or not args.board:
        raise SystemExit("new 需要 --chip 與 --board（或 --project + --hardware 只加 project）")
    chip_dir = args.chip.upper()
    chip = CHIP_DIR.get(chip_dir.lower(), args.chip.lower())
    prob = board_name_problem(args.board, chip)
    if prob:
        raise SystemExit(f"--board 不符命名規範：{prob}\n"
                         f"  規則見 ports/README.md §10（<CHIP>-<Identity>，例 ESP32-S3-Mini-Nano）")
    d = HW / chip_dir / args.board
    if d.exists():
        raise SystemExit(f"已存在：{rel(d)}")

    profiles = []
    if args.eth and args.eth != "none":
        profiles.append("eth-rmii" if args.eth == "rmii" else f"eth-{args.eth}")
    # Wi-Fi 有兩種完全不同的 build 路徑：
    #   · 晶片有原生 radio（S3／C3／C6…）→ 一個 CONFIG_NETCORE_WIFI=y，不用碎片
    #   · P4 沒有 radio → 一定要 wifi-hosted-* 碎片（外掛 C5／C6）
    native_wifi = bool(args.wifi) and chip != "esp32p4"
    if args.wifi and not native_wifi:
        profiles.append(args.wifi if str(args.wifi).startswith("wifi-") else "wifi-hosted-c6")

    board = {
        "_comment": "硬體事實（F.spec）。改這裡 = 要重新 build；runtime 可改的放 project 層。",
        "name": args.board, "chip": chip,
        "flash": {"size_mb": args.flash, "freq_mhz": args.flash_freq, "mode": args.flash_mode},
        "psram": ({"size_mb": args.psram, "mode": args.psram_mode, "speed_mhz": args.psram_speed}
                  if args.psram else {"size_mb": 0}),
        "cpu_mhz": args.cpu, "console": args.console,
        "profiles": profiles,
        "features": {"eth": args.eth if args.eth != "none" else None,
                     "wifi": bool(args.wifi), "usb": bool(args.usb)},
        "verified": None,
        "notes": "TODO：這塊板子是什麼、腳位、驗證過什麼",
    }
    if args.brand:
        board["brand"] = args.brand
    if args.model:
        board["model"] = args.model
    if args.rev:
        board["rev"] = args.rev
    if args.alias:
        board["aliases"] = list(args.alias)
    d.mkdir(parents=True)
    (d / "board.json").write_text(json.dumps(board, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
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
        + ("\n# ── Wi-Fi（晶片原生 radio；runtime 仍由 network.json 的 wifi.enable 決定）──\n"
           "CONFIG_NETCORE_WIFI=y\n" if native_wifi else ""),
        encoding="utf-8",
    )
    (d / "wiring.json").write_text(json.dumps({
        "version": 1, "_comment": "板級 pins／buses（F.wiring）。這是產生 firmware 預設的唯一來源。",
        "ETH": {"enable": 0 if not args.eth or args.eth == "none" else 1,
                "mdc": -1, "mdio": -1, "ref_clk": -1, "phy_type": "IP101", "phy_addr": 1},
        "PIN": {"enable": 0, "list": []},
        "SPI": {"enable": 0, "list": []},
        "I2C": {"enable": 0, "list": []},
        "I80": {"enable": 0, "list": []},
        "UART": {"enable": 0, "list": []},
        "I2S": {"enable": 0, "list": []},
        "PWM": {"enable": 0, "list": []},
        "SDIO": {"enable": 0, "list": []},
        "ENC": {"enable": 0, "list": []},
        "XL9555": {"enable": 0, "bus": {"type": "i2c", "index": -1}, "addr": "0x20", "list": []},
    }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (d / "README.md").write_text(
        f"# {args.board}\n\n"
        + (f"> 品牌：{args.brand}｜型號：{args.model or '—'}｜板本：{args.rev or '—'}\n\n"
           if (args.brand or args.model or args.rev) else "")
        + f"| 項目 | 值 |\n|---|---|\n"
        f"| 晶片 | `{chip}` |\n| Flash | {args.flash} MB @ {args.flash_freq} MHz {args.flash_mode.upper()} |\n"
        f"| PSRAM | {f'{args.psram} MB {args.psram_mode} @ {args.psram_speed} MHz' if args.psram else '無'} |\n"
        f"| CPU | {args.cpu} MHz |\n| Console | {args.console} |\n"
        f"| 硬體能力 | {', '.join(profiles) if profiles else '—'} |\n\n"
        f"## 這塊板子是什麼\n\nTODO\n\n## 驗證過什麼\n\n- [ ] build\n- [ ] 開機 log\n- [ ] 主要功能\n",
        encoding="utf-8",
    )

    pdir = PJ / chip_dir / args.board
    (pdir / "data" / "config").mkdir(parents=True, exist_ok=True)
    pj = {"_comment": "Project 層：這塊硬體上「跑什麼」——只放 runtime 設定"
                      "（project.json ＋ data/config），唔放 build 設定。"
                      "硬體事實與能力碎片一律在 ports/hardware/。",
          "version": 1, "name": args.board, "hardware": hw_name(d),
          "seed": ["system", "network"]}
    if args.alias:
        pj["aliases"] = list(args.alias)
    (pdir / "project.json").write_text(json.dumps(pj, ensure_ascii=False, indent=2) + "\n",
                                       encoding="utf-8")
    (pdir / "data" / "config" / "system.json").write_text(json.dumps({
        "version": 1, "_comment": "System 層：身份、NC4 通道與解碼路線。",
        "frame_interval_ms": 20, "num_pixels": 0,
    }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (pdir / "data" / "config" / "network.json").write_text(json.dumps({
        "version": 1, "_comment": "Network 層：只放 IP / WiFi。",
        "wifi": {"enable": 0, "ssid": "", "ssid_pw": ""},
    }, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    sync_board(d)
    print(f"建立 {rel(d)} 與 {rel(pdir)}")
    print("下一步：")
    print(f"  1. 編輯 {rel(d)}/wiring.json（腳位／匯流排）與 board.json")
    print(f"  2. python3 tools/board.py check {args.board}")
    print(f"  3. python3 make.py build {args.board}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("list", help="列出硬體板與 project").set_defaults(fn=cmd_list)

    p = sub.add_parser("check", help="檢查一致性")
    p.add_argument("board", nargs="?", help="只查這塊（預設全部）")
    p.set_defaults(fn=cmd_check)

    p = sub.add_parser("sync", help="把能力碎片寫進兩層的 sdkconfig.defaults")
    p.add_argument("board", nargs="?")
    p.set_defaults(fn=cmd_sync)

    p = sub.add_parser("resolve", help="查板子 → 硬體/project/target（給 make.py 用）")
    p.add_argument("board")
    p.set_defaults(fn=cmd_resolve)

    p = sub.add_parser("projects", help="列出每塊硬體板的 project（含 default）")
    p.add_argument("board", nargs="?")
    p.set_defaults(fn=cmd_projects)

    p = sub.add_parser("new", help="新增硬體板或 project")
    p.add_argument("--chip", help="S3 / P4 / C6 …")
    p.add_argument("--board", help="硬體板目錄名，例如 ESP32-S3-Matrix")
    p.add_argument("--project", help="只新增 project（配 --hardware）")
    p.add_argument("--hardware", help="既有硬體板（配 --project）")
    p.add_argument("--flash", type=int, default=4, help="flash 大小 MB")
    p.add_argument("--flash-freq", type=int, default=80, help="flash 頻率 MHz（預設 80）")
    p.add_argument("--flash-mode", default="dio", choices=["dio", "qio", "dout", "qout"])
    p.add_argument("--psram", type=int, default=0, help="PSRAM 大小 MB（0 = 沒有）")
    p.add_argument("--psram-mode", default="octal", choices=["octal", "hex", "quad"])
    p.add_argument("--psram-speed", type=int, default=80)
    p.add_argument("--cpu", type=int, default=240)
    p.add_argument("--console", default="usb-serial-jtag",
                   choices=["uart0", "usb-serial-jtag", "usb-cdc", "none"])
    p.add_argument("--eth", default="none", help="none / rmii …")
    p.add_argument("--wifi", nargs="?", const="wifi-hosted-c6", default=None,
                   help="加上 Wi-Fi 硬體能力碎片（可指定 wifi-hosted-c5/c6）")
    p.add_argument("--usb", action="store_true")
    p.add_argument("--brand", default=None, help="品牌（DFRobot / Waveshare / null = 白牌）")
    p.add_argument("--model", default=None, help="官方產品名（可以含空格，例如 FireBeetle 2 ESP32-P4）")
    p.add_argument("--rev", default=None, help="板本／批次（例如 v1.0；不進目錄名）")
    p.add_argument("--alias", action="append", default=[], metavar="NAME",
                   help="舊名／別名（可重複）；改名時一定要舊名")
    p.add_argument("--default", action="store_true",
                   help="將這個 project 設為那塊硬體板的 default（最多一個）")
    p.set_defaults(fn=cmd_new)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
