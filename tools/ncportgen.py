#!/usr/bin/env python3
"""把 hardware 層的 JSON 編成 firmware 預設（build-time codegen）。

為什麼要有這支：
    firmware 以前把 wiring.json／hardware.json 原文嵌進 image，開機再 cJSON 解析。
    同一份資料因此有兩份（JSON 文字 + 解析後的 struct），而且 JSON 裡大量
    `_comment`／重複的字典鍵（"mode":"OUT"、"pull":"UP"、"type":"i2c"）全部進 flash。

    這支在 build 時做一次「字典 → 數值」：
        "OUT"/"IN"        → 0/1
        "UP"/"DOWN"       → 1/2
        "spi"/"i2c"/"uart"→ enum（driver 層用）
        "IP101"/...       → enum（PHY）
    輸出的是 C designated initializer，不是 JSON：
        · flash 只剩數值與 label 字串（無重複的鍵名、無註解）
        · struct 的欄位寬度＝真實型別，alignment 由編譯器處理
        · nc_cfg_defaults_hardware() 直接回傳這份 static const，零解析

    同時產生 preset_hardware.json：這是給使用者看的「這塊板完整硬體預設」，
    開機 seed 到 /data/config/hardware.json；用戶改它只覆蓋差異，不改 firmware。

用法：
    python3 tools/ncportgen.py --hardware ports/hardware/P4/ESP32-P4-ETH --out build/gen

輸出：
    nc_port.h / nc_port.c           firmware 的 static const 預設
    preset_hardware.json            /data/config/hardware.json 的出廠內容
    nc_port_summary.json            給 tools/board.py check / 測試看的摘要
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CHIPS = ROOT / "ports" / "hardware" / "_chips"

MAX = {"sdio": 2, "pin": 16, "pwm": 8, "uart": 3, "i80": 2, "spi": 2}

LINK_KIND = {
    "cdc": "NC_LINK_CDC", "uart": "NC_LINK_UART", "tcp": "NC_LINK_TCP",
    "ws": "NC_LINK_WS", "udp": "NC_LINK_UDP", "http": "NC_LINK_HTTP",
    "usb_jtag": "NC_LINK_USBJTAG",
}

ETH_DRIVER = {
    "rmii": "NC_ETH_DRIVER_RMII",
    "spi": "NC_ETH_DRIVER_SPI",
    "w5500": "NC_ETH_DRIVER_SPI",
}

PHY = {
    "IP101": "NC_CFG_PHY_IP101", "LAN8720": "NC_CFG_PHY_LAN8720",
    "DP83848": "NC_CFG_PHY_DP83848", "RTL8201": "NC_CFG_PHY_RTL8201",
    "GENERIC": "NC_CFG_PHY_GENERIC",
}

# ── driver 層（driver.json 的 LED 段）───────────────────────────────
# 分層規矩：**腳位只出現在 wiring.json**（I80／SPI 段）；driver.json 只寫
# 「用邊條 bus、幾條 lane、每條幾顆、乜格式」。這支在 build 時把兩邊併埋。
LED_BACKEND = {
    "none": "NC_LED_BACKEND_NONE",
    "i80": "NC_LED_BACKEND_I80",
    "spi": "NC_LED_BACKEND_SPI",
    "rmt": "NC_LED_BACKEND_RMT",
    "parlio": "NC_LED_BACKEND_PARLIO",
    "direct": "NC_LED_BACKEND_DIRECT",
    "mock": "NC_LED_BACKEND_MOCK",
}
# 有 firmware 後端的；其餘只是 schema（寫得到、但 render 會降級 → board.py check 會 warn）。
LED_BACKEND_WIRED = {"i80", "spi", "direct", "mock"}

# 格式 → 線材次序（跟 sw/pixel/nc_pixel_encode.c 的 format 表一致）。
# 次序由**格式**決定，不是 driver.json 可調的；寫 order 只會被對帳（寫錯就 build 失敗），
# 免得同一件事有兩個真相。apa102 舊寫法 "BGRW" 的 W 其實是亮度頭，所以兩種都收。
LED_FMT_ORDER = {
    "ws2812":  ("GRB",),
    "ws2812w": ("GRBW",),
    "ws2815":  ("GRB",),
    "ws2801":  ("RGB",),
    "apa102":  ("BGR", "BGRW"),
}
LED_PX_MAX = 16384     # 跟 Kconfig NETCORE_PIXEL_LEDS 的上限一致

# project 的 sdkconfig.defaults 一旦寫了以下任何一個，就代表「這個 project 自己決定
# pixel 用什麼」（外接燈具是 project 的事，見 ports/README.md §6）——那時 board 的
# driver.json LED 段會**讓位**，render 用 Kconfig，跟未遷移之前一模一樣。
# 呢個規則係 build-time 判定的：唔需要在 firmware 猜「呢個符號係唔係 project 寫的」。
PROJECT_PIXEL_KEYS = (
    "CONFIG_NETCORE_PIXEL_I80_PINS",
    "CONFIG_NETCORE_PIXEL_I80_LANES",
    "CONFIG_NETCORE_PIXEL_I80_BUS_WIDTH",
    "CONFIG_NETCORE_PIXEL_LEDS",
    "CONFIG_NETCORE_PIXEL_FORMAT",
    "CONFIG_NETCORE_PIXEL_SPI",
    "CONFIG_NETCORE_PIXEL_SPI_MOSI",
    "CONFIG_NETCORE_PIXEL_SPI_SCLK",
    "CONFIG_NETCORE_PIXEL_SPI_HZ",
    "CONFIG_NETCORE_PIXEL_MOCK_ONLY",
)


def project_pixel_override(project: Path | None) -> list[str]:
    """project 的 sdkconfig.defaults 有冇覆寫 pixel？回覆命中的符號名（空 = 冇）。"""
    if project is None:
        return []
    path = project / "sdkconfig.defaults"
    if not path.is_file():
        return []
    hits = []
    try:
        text = path.read_text(encoding="utf-8")
    except OSError:
        return []
    for line in text.splitlines():
        s = line.strip()
        if not s.startswith("CONFIG_") or s.startswith("#"):
            continue
        name = s.split("=", 1)[0]
        if name in PROJECT_PIXEL_KEYS:
            hits.append(name)
    return hits


class GenError(SystemExit):
    pass


def load_json(path: Path) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise GenError(f"找不到 {path}")
    except json.JSONDecodeError as e:
        raise GenError(f"{path} 不是合法 JSON：{e}")
    if not isinstance(data, dict):
        raise GenError(f"{path} 頂層不是物件")
    return data


def cstr(s: str | None) -> str:
    """C 字串字面值；只轉義 C 必需的字元，UTF-8 原樣保留。"""
    if s is None:
        return "NULL"
    out = []
    for ch in str(s):
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif ord(ch) < 0x20:
            out.append(f"\\x{ord(ch):02x}")
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def num(v, default=0) -> int:
    if isinstance(v, bool):
        return 1 if v else 0
    if isinstance(v, (int, float)):
        return int(v)
    return default


def pin(v) -> int:
    """null／缺欄位 = -1（未接）；數字照用。"""
    return -1 if v is None else num(v, -1)


def mode_id(s) -> int:
    return 1 if str(s).upper() == "IN" else 0


def pull_id(s) -> int:
    u = str(s).upper()
    return 1 if u == "UP" else (2 if u == "DOWN" else 0)


def chip_caps(chip: str) -> dict:
    path = CHIPS / f"{chip}.json"
    if not path.is_file():
        raise GenError(f"晶片 {chip!r} 沒有 ports/hardware/_chips/{chip}.json")
    return load_json(path)


def gen_chip(cap: dict) -> str:
    usb = cap.get("usb") or {}
    kind = usb.get("kind")
    kind_sym = LINK_KIND.get(str(kind), "NC_LINK_CDC") if kind else "NC_LINK_CDC"
    link = usb.get("link")
    return "\n".join([
        "static const nc_cfg_chip_t s_chip = {",
        f"    .id = {cstr(cap.get('id'))},",
        f"    .cores = {num(cap.get('cores'), 1)},",
        f"    .usb_otg = {'true' if usb.get('otg') else 'false'},",
        f"    .usb_otg_hs = {'true' if usb.get('otg_hs') else 'false'},",
        f"    .usb_serial_jtag = {'true' if usb.get('serial_jtag') else 'false'},",
        f"    .usb_shared_phy = {'true' if usb.get('shared_phy') else 'false'},",
        f"    .usb_link = {cstr(link) if link else 'NULL'},",
        f"    .usb_kind = {kind_sym},",
        f"    .usb_rx_region = {num(usb.get('rx_region'), 4128)},",
        "};",
    ])


def gen_hardware(wiring: dict) -> tuple[str, dict]:
    summary: dict = {"present": {}, "counts": {}}
    lines = ["static const nc_hardware_cfg_t s_hw = {",
             "    .version = NC_CFG_SCHEMA_VERSION,"]

    eth = wiring.get("ETH")
    if isinstance(eth, dict):
        summary["present"]["ETH"] = True
        phy = str(eth.get("phy_type") or "IP101").upper()
        phy_sym = PHY.get(phy, "NC_CFG_PHY_UNKNOWN")
        drv = str(eth.get("driver") or "rmii").lower()
        drv_sym = ETH_DRIVER.get(drv, "NC_ETH_DRIVER_RMII")
        gpio = eth.get("GPIO") if isinstance(eth.get("GPIO"), dict) else eth
        lines += [
            "    .eth = {",
            "        .present = true,",
            f"        .enable = {'true' if num(eth.get('enable')) else 'false'},",
            f"        .driver = {drv_sym},",
            f"        .mdc = {pin(gpio.get('mdc'))},",
            f"        .mdio = {pin(gpio.get('mdio'))},",
            f"        .ref_clk = {pin(gpio.get('ref_clk'))},",
            f"        .spi_idx = {num(gpio.get('spi', eth.get('spi', eth.get('bus'))), -1)},",
            f"        .cs = {pin(gpio.get('cs'))},",
            f"        .rst = {pin(gpio.get('rst'))},",
            f"        .int_pin = {pin(gpio.get('int'))},",
            f"        .phy_addr = {num(eth.get('phy_addr'), -1 if drv == 'spi' else 1)},",
            f"        .phy_type = {phy_sym},",
            "    },",
        ]
    else:
        summary["present"]["ETH"] = False
        lines += ["    .eth = { .present = false, .enable = false, .driver = NC_ETH_DRIVER_RMII,",
                  "             .mdc = -1, .mdio = -1, .ref_clk = -1,",
                  "             .spi_idx = -1, .cs = -1, .rst = -1, .int_pin = -1,",
                  "             .phy_addr = 1, .phy_type = NC_CFG_PHY_UNKNOWN },"]

    def section(name: str, key: str) -> tuple[bool, list]:
        sec = wiring.get(name)
        if not isinstance(sec, dict) or not num(sec.get("enable")):
            summary["counts"][key] = 0
            return False, []
        lst = sec.get("list")
        if not isinstance(lst, list):
            return True, []
        return True, lst

    # ── SDIO ──
    en, lst = section("SDIO", "sdio")
    items = []
    for it in lst[:MAX["sdio"]]:
        if not isinstance(it, dict):
            continue
        g = it.get("GPIO") if isinstance(it.get("GPIO"), dict) else {}
        data = g.get("data") if isinstance(g.get("data"), list) else []
        d = [pin(data[i]) if i < len(data) else -1 for i in range(4)]
        items.append(
            "        { .enable = true, "
            f".freq_hz = {num(it.get('freq'), 40000000)}u, .width = {num(it.get('width'), 4)}, "
            f".clk = {pin(g.get('sck'))}, .cmd = {pin(g.get('cmd'))}, "
            f".d = {{ {d[0]}, {d[1]}, {d[2]}, {d[3]} }} }},")
    if len(lst) > MAX["sdio"]:
        raise GenError(f"SDIO list 有 {len(lst)} 條，上限 {MAX['sdio']}")
    summary["counts"]["sdio"] = len(items)
    lines.append(f"    .n_sdio = {len(items)},")
    lines.append("    .sdio = { " + ("\n" + "\n".join(items) + "\n    " if items else "{ 0 }") + " },")

    # ── PIN（GPIO=null 的條目照 apply_pin 的規則跳過）──
    en, lst = section("PIN", "pin")
    items = []
    for it in lst:
        if not isinstance(it, dict) or not isinstance(it.get("GPIO"), int):
            continue
        items.append(
            "        { "
            f".pin = {pin(it.get('GPIO'))}, .mode = {mode_id(it.get('mode', 'OUT'))}, "
            f".pull = {pull_id(it.get('pull', 'NONE'))}, .initial = {num(it.get('initial'))}, "
            f".label = {cstr(it.get('label') or '')} }},")
    if len(items) > MAX["pin"]:
        raise GenError(f"PIN 有 {len(items)} 條可用，上限 {MAX['pin']}")
    summary["counts"]["pin"] = len(items)
    lines.append(f"    .n_pin = {len(items)},")
    lines.append("    .pin = { " + ("\n" + "\n".join(items) + "\n    " if items else "{ 0 }") + " },")

    # ── PWM ──
    en, lst = section("PWM", "pwm")
    items = []
    for it in lst:
        if not isinstance(it, dict) or not isinstance(it.get("GPIO"), int):
            continue
        bits = max(1, min(14, num(it.get("resolution"), 12)))
        items.append(
            "        { "
            f".pin = {pin(it.get('GPIO'))}, .freq_hz = {num(it.get('freq'), 1000)}u, "
            f".resolution_bits = {bits}, .duty = {num(it.get('duty'))}, "
            f".d_stay = {num(it.get('dStay'))}, .label = {cstr(it.get('label') or '')} }},")
    if len(items) > MAX["pwm"]:
        raise GenError(f"PWM 有 {len(items)} 條可用，上限 {MAX['pwm']}")
    summary["counts"]["pwm"] = len(items)
    lines.append(f"    .n_pwm = {len(items)},")
    lines.append("    .pwm = { " + ("\n" + "\n".join(items) + "\n    " if items else "{ 0 }") + " },")

    # ── UART ──
    en, lst = section("UART", "uart")
    items = []
    for it in lst:
        if not isinstance(it, dict):
            continue
        g = it.get("GPIO") if isinstance(it.get("GPIO"), dict) else {}
        tx, rx = pin(g.get("tx")), pin(g.get("rx"))
        if tx < 0 or rx < 0:
            continue
        items.append(
            "        { "
            f".enable = true, .id = {num(it.get('id'), 1)}, .baud = {num(it.get('baudrate'), 115200)}u, "
            f".rxbuf = {num(it.get('rxbuf'), 16384)}u, .txbuf = {num(it.get('txbuf'), 16384)}u, "
            f".tx = {tx}, .rx = {rx}, .en = {pin(g.get('en'))}, "
            f".en_settle_ms = {num(it.get('en_settle_ms'), 1)} }},")
    if len(items) > MAX["uart"]:
        raise GenError(f"UART 有 {len(items)} 條可用，上限 {MAX['uart']}")
    summary["counts"]["uart"] = len(items)
    lines.append(f"    .n_uart = {len(items)},")
    lines.append("    .uart = { " + ("\n" + "\n".join(items) + "\n    " if items else "{ 0 }") + " },")

    # ── I80（舊專案原形狀：enable／data[]／wr／cs／freq；dc 是 esp_lcd 必要）──
    # data[] 長度決定 bus_width（8 或 16），唔另外寫一個會漂移的欄位。
    # 每一條 list 一定要編得出一條 bus —— 咁 wiring 的 index 同 nc_i80_cfg_t.i80[] 的
    # index 永遠一樣，driver.json 的 bus.index 唔會錯位。
    en, lst = section("I80", "i80")
    items = []
    for i80_idx, it in enumerate(lst):
        if not isinstance(it, dict):
            raise GenError(f"wiring.json I80.list[{i80_idx}] 唔係物件")
        data = [pin(x) for x in (it.get("data") or [])]
        if any(d < 0 for d in data) or len(data) not in (8, 16):
            raise GenError(f"wiring.json I80.list[{i80_idx}].data 有 {len(data)} 支／含有未接腳位"
                           "（要 8 或 16 支有效 GPIO：D0..D7／D0..D15）")
        wr, dc = pin(it.get("wr")), pin(it.get("dc"))
        if wr < 0 or dc < 0:
            raise GenError(f"wiring.json I80.list[{i80_idx}] 的 wr／dc 未填"
                           "（I80 硬性要求；送燈時是 dummy）")
        bw = num(it.get("bus_width"), len(data))
        if bw != len(data):
            raise GenError(f"wiring.json I80.list[{i80_idx}].bus_width={bw} 與 data 的 "
                           f"{len(data)} 支唔一致（bus_width 由 data 長度決定，可以唔寫）")
        pad = ", ".join(str(x) for x in data)
        items.append(
            "        { "
            f".enable = true, .bus_width = {bw}, .n_data = {len(data)}, "
            f".data = {{ {pad} }}, "
            f".wr = {wr}, .dc = {dc}, .cs = {pin(it.get('cs'))}, "
            f".freq_hz = {num(it.get('freq'), 80000000)}u }},")
    if len(lst) > MAX["i80"]:
        raise GenError(f"I80 list 有 {len(lst)} 條，上限 {MAX['i80']}")
    summary["counts"]["i80"] = len(items)
    lines.append(f"    .n_i80 = {len(items)},")
    lines.append("    .i80 = { " + ("\n" + "\n".join(items) + "\n    " if items else "{ 0 }") + " },")

    # ── SPI（時脈式燈鏈／面板用；miso 可以 -1）──
    # 每一條 list 一定要編得出一條 bus（同 I80 一樣），driver.json 的 bus.index 才不會錯位。
    en, lst = section("SPI", "spi")
    items = []
    for spi_idx, it in enumerate(lst):
        if not isinstance(it, dict):
            raise GenError(f"wiring.json SPI.list[{spi_idx}] 唔係物件")
        g = it.get("GPIO") if isinstance(it.get("GPIO"), dict) else {}
        sck, mosi = pin(g.get("sck")), pin(g.get("mosi"))
        if sck < 0 or mosi < 0:
            raise GenError(f"wiring.json SPI.list[{spi_idx}] 的 sck／mosi 未填"
                           "（要用 SPI 就填好；未接請整段 enable=0）")
        items.append(
            "        { "
            f".enable = true, .baud = {num(it.get('baudrate'), 80000000)}u, "
            f".phase = {num(it.get('phase'))}, .polarity = {num(it.get('polarity'))}, "
            f".sck = {sck}, .mosi = {mosi}, .miso = {pin(g.get('miso'))} }},")
    if len(items) > MAX["spi"]:
        raise GenError(f"SPI 有 {len(items)} 條可用，上限 {MAX['spi']}")
    summary["counts"]["spi"] = len(items)
    lines.append(f"    .n_spi = {len(items)},")
    lines.append("    .spi = { " + ("\n" + "\n".join(items) + "\n    " if items else "{ 0 }") + " },")
    lines.append("};")
    return "\n".join(lines), summary


def _wiring_bus(wiring: dict, kind: str, index: int) -> dict:
    lst = (wiring.get(kind) or {}).get("list") or []
    if index < 0 or index >= len(lst):
        raise GenError(f"wiring.json 的 {kind}.list 冇 index {index}"
                       f"（這塊板有 {len(lst)} 條；index 由 0 開始）")
    bus = lst[index]
    if not isinstance(bus, dict):
        raise GenError(f"wiring.json 的 {kind}.list[{index}] 唔係物件")
    return bus


def _suppressed_stub(suppressed: list[str]) -> str:
    """project 覆寫了 pixel → 這塊板的 LED 段完全讓位（render 用 Kconfig）。"""
    note = ("// driver.json 有 LED 段，但這個 project 的 sdkconfig.defaults 覆寫了\n"
            + "".join(f"//   {k}\n" for k in sorted(set(suppressed)))
            + "// → 外接燈具屬 project，render 用 Kconfig（board 的 LED 段讓位）。\n")
    return (note
            + "static const nc_led_cfg_t s_led = { .present = false, .enable = false };\n"
            "const nc_led_cfg_t *nc_port_led(void) { return &s_led; }\n")


def gen_led(driver: dict | None, wiring: dict,
            suppressed: list[str] | None = None) -> tuple[str, dict]:
    """driver.json 的 LED 段 → nc_led_cfg_t（build-time 生成）。

    腳位**不是這裡發明**：I80 的 D0..Dn／WR／DC 來自 wiring.json 的 I80.list[index]，
    SPI 的 MOSI／SCLK 來自 SPI.list[index]。driver.json 只引用 bus。

    suppressed = project 的 sdkconfig 覆寫了 pixel 的符號名（見 project_pixel_override）：
    那時仍然驗證 JSON（錯就 build 失敗），但唔會生成（讓位俾 project 的 Kconfig）。
    """
    present = isinstance(driver, dict) and "LED" in driver
    led = (driver or {}).get("LED") if present else None
    if not present:
        return ("// driver.json 冇 LED 段 → render 用 Kconfig（未遷移的板子行為不變）\n"
                "static const nc_led_cfg_t s_led = { .present = false, .enable = false };\n"
                "const nc_led_cfg_t *nc_port_led(void) { return &s_led; }\n",
                {"present": False})
    if not isinstance(led, dict):
        raise GenError("driver.json 的 LED 要係物件："
                       '{ "enable": 1, "backend": "i80", "bus": {"type":"i80","index":0}, … }')
    if "list" in led:
        raise GenError("driver.json 的 LED 只可以係一個物件（v1 一組燈鏈；"
                       "多組要等 render 支援多後端，唔好寫 list）")

    enable = bool(num(led.get("enable"), 0))
    backend_s = str(led.get("backend") or "").lower()
    if backend_s not in LED_BACKEND or backend_s == "none":
        raise GenError(f"driver.json LED.backend={led.get('backend')!r} 看不懂（用 "
                       + " / ".join(k for k in LED_BACKEND if k != "none") + "）")
    backend = LED_BACKEND[backend_s]

    if not enable:
        # 明確關閉（enable=0）：只檢查「名字對不對」，唔解析腳位 ——
        # 腳位可以未定（板子未有燈／等接線），寫 index:-1 係合法嘅。
        fmt_s = str(led.get("format") or "").lower()
        if fmt_s and fmt_s not in LED_FMT_ORDER:
            raise GenError(f"LED.format={led.get('format')!r} 看不懂（用 "
                           + " / ".join(LED_FMT_ORDER) + "）")
        # ⚠ 讓位要贏過「enable=0」：project 覆寫 pixel 時，board 段（即使係 enable=0）
        # 都唔可以講「明確唔用燈」——否則 render 會只留 mock，外接燈鏈就唔會亮。
        # 這正是 SuperMini（board 冇燈）＋ textkit（project 有燈）的情況。
        if suppressed:
            return _suppressed_stub(suppressed), {
                "present": False, "enable": False, "backend": backend_s, "bus": -1,
                "px": num(led.get("px"), 0), "format": fmt_s,
                "lanes": num(led.get("lanes"), 0),
                "wired": backend_s in LED_BACKEND_WIRED,
                "suppressed_by": sorted(set(suppressed))}
        body = [
            "// driver.json 有 LED 段但 enable=0 → 明確唔用燈（render 只留 mock lane，",
            "// 唔會退回去讀 Kconfig 亂開 i80）。腳位未定是合法的。",
            "static const nc_led_cfg_t s_led = {",
            "    .present = true,",
            "    .enable = false,",
            f"    .backend = {backend},",
            "    .bus = -1,",
            f"    .px = {num(led.get('px'), 0)},",
            f"    .fmt = {cstr(fmt_s)},",
            "};",
            "const nc_led_cfg_t *nc_port_led(void) { return &s_led; }",
        ]
        return "\n".join(body) + "\n", {"present": True, "enable": False,
                                        "backend": backend_s, "bus": -1,
                                        "px": num(led.get("px"), 0), "format": fmt_s,
                                        "lanes": num(led.get("lanes"), 0),
                                        "wired": backend_s in LED_BACKEND_WIRED}

    bus = led.get("bus")
    bus_type = ""
    bus_idx = -1
    if bus is not None and not isinstance(bus, dict):
        raise GenError('driver.json LED.bus 要係物件：{ "type": "i80", "index": 0 }')
    if isinstance(bus, dict):
        bus_type = str(bus.get("type") or "").lower()
        bus_idx = num(bus.get("index"), -1)

    lanes = num(led.get("lanes"), 0)
    width = 0

    if backend_s in ("i80", "spi"):
        kind = "I80" if backend_s == "i80" else "SPI"
        if not isinstance(bus, dict) or bus_type != backend_s:
            raise GenError(f'LED.backend="{backend_s}" 一定要用 bus 引用 hardware 層：'
                           f'{{ "type": "{backend_s}", "index": 0 }}')
        b = _wiring_bus(wiring, kind, bus_idx)
        if backend_s == "i80":
            # 腳位本身由 gen_hardware 編成 nc_i80_cfg_t；這裡只驗「引用得到、lanes 合理」。
            data = [num(x, -1) for x in (b.get("data") or [])]
            width = len(data)
            if width not in (8, 16):
                raise GenError(f"wiring.json I80[{bus_idx}].data 有 {width} 支（要 8 或 16）")
            if lanes == 0:
                lanes = 1
            if lanes < 1 or lanes > width:
                raise GenError(f"LED.lanes={lanes} 超出 wiring.json I80[{bus_idx}] 的 "
                               f"bus_width={width}（最多 {width} 條）")
        else:   # spi
            if lanes not in (0, 1):
                raise GenError(f"LED.lanes={lanes}：SPI 只有一條 MOSI，只可以 1 條 lane")
            lanes = 1
    else:   # direct / mock：唔掛 bus；rmt / parlio 仲未實作，寫住 bus 都收（方便預留）
        if lanes == 0:
            lanes = 1
        if bus is not None and backend_s in ("direct", "mock"):
            raise GenError(f'LED.backend="{backend_s}" 唔掛匯流排，請移除 bus')

    px = num(led.get("px"), 0)
    if px < 1 or px > LED_PX_MAX:
        raise GenError(f"LED.px={px}（一條 lane 幾顆；1..{LED_PX_MAX}）")

    fmt = str(led.get("format") or "").lower()
    if fmt not in LED_FMT_ORDER:
        raise GenError(f"LED.format={led.get('format')!r} 看不懂（用 "
                       + " / ".join(LED_FMT_ORDER) + "）")
    order = led.get("order")
    if isinstance(order, str) and order.strip():
        if order.strip().upper() not in LED_FMT_ORDER[fmt]:
            raise GenError(f"LED.order={order!r} 與 format={fmt!r} 唔一致（{fmt} 是 "
                           f"{'/'.join(LED_FMT_ORDER[fmt])}）；線材次序由格式決定，"
                           "兩邊寫法唔同就 build 失敗，免得有兩個真相")

    tm = led.get("timing") or {}
    if not isinstance(tm, dict):
        raise GenError("LED.timing 要係物件：{ \"pclk_hz\": …, \"bit_cells\": …, \"t0h\": …, \"t1h\": … }")
    clk = num(tm.get("pclk_hz", tm.get("clk_hz")), 0)
    bit_cells = num(tm.get("bit_cells"), 0)
    t0h = num(tm.get("t0h"), 0)
    t1h = num(tm.get("t1h"), 0)
    if clk and not (1000000 <= clk <= 20000000):
        raise GenError(f"LED.timing.pclk_hz={clk}（1 MHz..20 MHz）")
    if bit_cells and not (1 <= bit_cells <= 16):
        raise GenError(f"LED.timing.bit_cells={bit_cells}（1..16）")
    if t0h and t0h < 1:
        raise GenError("LED.timing.t0h 一定要 ≥ 1（整組低電平會被燈當成 reset）")
    if bit_cells and t0h and t0h > bit_cells:
        raise GenError(f"LED.timing.t0h={t0h} > bit_cells={bit_cells}")
    if bit_cells and t1h and t1h > bit_cells:
        raise GenError(f"LED.timing.t1h={t1h} > bit_cells={bit_cells}")
    if t0h and t1h and t0h >= t1h:
        raise GenError(f"LED.timing 要 t0h < t1h（現在 {t0h} / {t1h}），否則兩個碼分不出來")

    body = [
        "// driver 層：燈鏈（來源 driver.json 的 LED 段）。",
        f"// 腳位唔在這裡：它們是 hardware 層（wiring.json → nc_hardware_cfg_t.i80／spi），"
        f"由 bus={bus_idx} 引用；",
        "// runtime 可以由 /data/config/hardware.json 覆蓋 —— 唔使重 build。",
        "static const nc_led_cfg_t s_led = {",
        "    .present = true,",
        "    .enable = true,",
        f"    .backend = {backend},",
        f"    .bus = {bus_idx},",
        f"    .lanes = {lanes},",
        f"    .px = {px},",
        f"    .fmt = {cstr(fmt)},",
        f"    .clk_hz = {clk}u,",
        f"    .bit_cells = {bit_cells},",
        f"    .t0h = {t0h},",
        f"    .t1h = {t1h},",
        "};",
        "const nc_led_cfg_t *nc_port_led(void) { return &s_led; }",
    ]
    summary = {"present": True, "enable": enable, "backend": backend_s, "bus": bus_idx,
               "lanes": lanes, "px": px, "format": fmt, "bus_width": width,
               "wired": backend_s in LED_BACKEND_WIRED}
    if suppressed:
        return (_suppressed_stub(suppressed),
                {**summary, "present": False, "suppressed_by": sorted(set(suppressed))})
    return "\n".join(body) + "\n", summary


def gen_preset(wiring: dict, hw_rel: str) -> dict:
    out = {
        "_comment": [
            "這份是 build 時由 ports/hardware/" + hw_rel + "/wiring.json 產生的出廠預置（鏡子）。",
            "它會被 build 嵌入、並在 /data/config/hardware.json 缺失時 seed 到裝置；"
            "**唔係** firmware 內建預設本身：真正的內建預設是 nc_port.c（同樣由 wiring.json 生成）。",
            "所以改這份只會改「用戶看到的值」；要改行為就改 wiring.json 再 rebuild"
            "（目前 typed 解析：ETH／SDIO／PIN／PWM／UART；SPI／I2C／I80／I2S／ENC／XL9555 未 parse）。",
            "燈鏈（LED）不在這份：它是 driver 層，寫在該板的 driver.json，"
            "build 時生成 nc_led_cfg_t（見 nc_port.c）。",
            "硬度事實（flash/psram/CPU）在 board.json，不在這裡。",
        ],
        "version": 1,
    }
    for k, v in wiring.items():
        if k in ("version", "_comment"):
            continue
        out[k] = v
    return out


def gen_usb_kinds(wiring: dict) -> str:
    """USB.list index → internal link kind（NC_LINK_CDC / NC_LINK_USBJTAG）。

    屬性只用 `used`：**唔可以用 `retain`**。xtensa GCC 15（IDF 6.0.3）冇實作
    `retain`，會報 `'retain' attribute ignored [-Werror=attributes]`，
    整個 build 直接失敗（2026-11-17 實測）。留住整個 object 係 main.c 的
    強引用負責（見 main.c 的 s_usb_kind_lookup）；呢度只保證函數一定被編出。
    """
    kind = {"otg-fs": "NC_LINK_CDC", "otg-hs": "NC_LINK_CDC",
            "usj-fs": "NC_LINK_USBJTAG"}
    vals = []
    for it in (wiring.get("USB") or {}).get("list", []):
        phy = str(it.get("phy") or "").lower()
        vals.append(kind.get(phy, "0xFF"))
    if not vals:
        return ("__attribute__((used)) uint8_t nc_port_usb_kind(uint8_t idx) { (void)idx; return 0xFF; }\n")
    body = ", ".join(vals)
    return (
        f"static const uint8_t s_usb_kinds[{len(vals)}] = {{ {body} }};\n"
        f"__attribute__((used)) uint8_t nc_port_usb_kind(uint8_t idx) {{\n"
        f"    if (idx >= {len(vals)}u) return 0xFF;\n"
        f"    return s_usb_kinds[idx];\n"
        f"}}\n"
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hardware", required=True, help="ports/hardware/<chip>/<board> 目錄")
    ap.add_argument("--out", required=True, help="輸出目錄")
    ap.add_argument("--project", default=None,
                    help="（選配）ports/project/<chip>/<board>；用來判斷 project 有沒有覆寫 pixel")
    args = ap.parse_args()

    hw = Path(args.hardware).resolve()
    out = Path(args.out).resolve()
    project = Path(args.project).resolve() if args.project else None
    board = load_json(hw / "board.json")
    wiring = load_json(hw / "wiring.json")
    # driver 層是選配：冇 driver.json = 這塊板冇掛外部裝置（build 正常）。
    drv_path = hw / "driver.json"
    driver = load_json(drv_path) if drv_path.is_file() else None
    if project is not None and (project / "driver.json").is_file():
        print(f"ncportgen: 警告 — {project.name}/driver.json 唔會被讀"
              "（driver 層目前只在 hardware 層；外接燈具用 project sdkconfig）",
              file=sys.stderr)
    chip = board.get("chip")
    if not chip:
        raise GenError(f"{hw}/board.json 缺 chip")
    cap = chip_caps(chip)

    hw_rel = f"{hw.parent.name}/{hw.name}"
    chip_c = gen_chip(cap)
    hw_c, summary = gen_hardware(wiring)
    led_c, led_sum = gen_led(driver, wiring, project_pixel_override(project))
    preset = gen_preset(wiring, hw_rel)
    summary.update({"board": hw_rel, "chip": chip, "cores": num(cap.get("cores"), 1),
                    "usb": (cap.get("usb") or {}).get("link")})
    summary["led"] = led_sum

    out.mkdir(parents=True, exist_ok=True)
    (out / "nc_port.h").write_text(
        "// 由 tools/ncportgen.py 產生，不要手改。\n"
        "// 來源：ports/hardware/" + hw_rel + "/{board,wiring,driver}.json + _chips/" + str(chip) + ".json\n"
        "#pragma once\n\n"
        "#include \"nc_cfg.h\"\n\n"
        "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n"
        "const nc_cfg_chip_t *nc_port_chip(void);\n"
        "const nc_hardware_cfg_t *nc_port_hw_defaults(void);\n"
        "uint8_t nc_port_usb_kind(uint8_t idx);\n"
        "const nc_led_cfg_t *nc_port_led(void);\n\n"
        "#ifdef __cplusplus\n}\n#endif\n",
        encoding="utf-8")
    (out / "nc_port.c").write_text(
        "// 由 tools/ncportgen.py 產生，不要手改。\n"
        "// 這是這塊硬體的「編譯期預設」：沒有 JSON、沒有 cJSON、沒有字串字典。\n"
        "#include \"nc_cfg.h\"\n#include \"nc_port.h\"\n\n"
        + chip_c + "\n\n" + hw_c + "\n\n" + led_c + "\n"
        "const nc_cfg_chip_t *nc_port_chip(void) { return &s_chip; }\n"
        "const nc_hardware_cfg_t *nc_port_hw_defaults(void) { return &s_hw; }\n\n"
        + gen_usb_kinds(wiring) + "\n",
        encoding="utf-8")
    (out / "preset_hardware.json").write_text(
        json.dumps(preset, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (out / "nc_port_summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    led_note = ""
    if led_sum.get("present"):
        led_note = (f"；LED {led_sum['backend']} lanes={led_sum['lanes']} px={led_sum['px']}"
                    f"{'' if led_sum['enable'] else '（enable=0）'}")
        if not led_sum["wired"]:
            print(f"ncportgen: 警告 — LED.backend={led_sum['backend']} 未有 firmware 後端，"
                  "上板只會降級到 mock", file=sys.stderr)
    elif led_sum.get("suppressed_by"):
        led_note = "；project 覆寫 pixel → LED 用 Kconfig"
    print(f"ncportgen: {hw_rel} → {out}  "
          f"(sdio {summary['counts']['sdio']}, pin {summary['counts']['pin']}, "
          f"pwm {summary['counts']['pwm']}, uart {summary['counts']['uart']}{led_note})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
