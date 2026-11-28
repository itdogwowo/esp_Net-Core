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
import zlib
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
        # 稀疏語義（2026-11-26）：元素 >=0 = 真的接燈；-1（或 null／省略）= 未用。
        # 有效腳數自動決定 bus 寬度：<=8 → 8-bit；9..16 → 16-bit。
        # wr／dc 唔再強制（直驅版「有登記才 route」；esp_lcd 版解析時自動由安全池補）。
        used = [x for x in (pin(v) for v in (it.get("data") or [])) if x >= 0]
        if not used:
            raise GenError(f"wiring.json I80.list[{i80_idx}].data 冇一支有效 GPIO")
        if len(used) > 16:
            raise GenError(f"wiring.json I80.list[{i80_idx}].data 有 {len(used)} 支有效腳"
                           "（上限 16：D0..D15）")
        width = 8 if len(used) <= 8 else 16
        bw = num(it.get("bus_width"), width)
        if bw != width:
            raise GenError(f"wiring.json I80.list[{i80_idx}].bus_width={bw} 與有效腳數 "
                           f"{len(used)} 推出的 {width} 唔一致（可以唔寫）")
        pad = ", ".join(str(x) for x in (used + [-1] * (width - len(used))))
        items.append(
            "        { "
            f".enable = true, .bus_width = {width}, .n_data = {width}, .n_used = {len(used)}, "
            f".data = {{ {pad} }}, "
            f".wr = {pin(it.get('wr'))}, .dc = {pin(it.get('dc'))}, .cs = {pin(it.get('cs'))}, "
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



def _gen_led_ws2812(driver: dict, wiring: dict,
                    suppressed: list[str] | None) -> tuple[str, dict]:
    """driver.json 的 LED_WS2812 段 →（暫時）legacy nc_led_cfg_t。

    新設定：`LED_WS2812 = { enable, list:[ {bus:{type:"i80",index:<line>},
    px, order, d_stay}, ... ] }`
      · 每個 list item = 一條線；item 的 bus.index 是第幾條 data line（0..N-1）。
      · S3 只有一個 I80 → legacy nc_led_cfg_t.bus 固定 0（hardware I80 list index），
        lanes = list item 數；px = 每條線的 px（目前 driver 要求全部一樣）。
      · timing 唔寫入 config：clk/bit_cells/t0h/t1h 全部 0 → codec 用 driver 預設。
    """
    section = driver.get("LED_WS2812")
    if not isinstance(section, dict):
        raise GenError("driver.json 的 LED_WS2812 要係物件："
                       '{ "enable": 1, "list": [ { "bus": {"type":"i80","index":0}, … } ] }')
    enable = bool(num(section.get("enable"), 0))
    items = section.get("list")
    if items is None:
        items = []
    if not isinstance(items, list):
        raise GenError("LED_WS2812.list 要係陣列")
    if not enable:
        if suppressed:
            return _suppressed_stub(suppressed), {
                "present": False, "enable": False, "backend": "i80", "bus": -1,
                "px": 0, "format": "ws2812", "lanes": 0, "wired": True,
                "suppressed_by": sorted(set(suppressed))}
        return ("// driver.json 有 LED_WS2812 但 enable=0 → 明確唔用燈（render 只留 mock lane）。\n"
                "static const nc_led_cfg_t s_led = { .present = true, .enable = false,\n"
                "    .backend = NC_LED_BACKEND_I80, .bus = -1 };\n"
                "const nc_led_cfg_t *nc_port_led(void) { return &s_led; }\n",
                {"present": True, "enable": False, "backend": "i80", "bus": -1,
                 "px": 0, "format": "ws2812", "lanes": 0, "wired": True})
    if not items:
        raise GenError("LED_WS2812.enable=1 但 list 係空（至少要一條線）")
    if len(items) > 16:
        raise GenError(f"LED_WS2812.list 有 {len(items)} 條線（I80 上限 16）")

    px0 = None
    line_px = []
    fmt = "ws2812"
    for i, it in enumerate(items):
        if not isinstance(it, dict):
            raise GenError(f"LED_WS2812.list[{i}] 要係物件")
        bus = it.get("bus")
        if not isinstance(bus, dict):
            raise GenError(f"LED_WS2812.list[{i}].bus 要係物件："
                           '{ "type": "i80", "index": 0 }')
        if str(bus.get("type") or "").lower() != "i80":
            raise GenError(f"LED_WS2812.list[{i}].bus.type 要係 \"i80\""
                           "（RMT 未實作；唔好靜默當 i80）")
        line = num(bus.get("index"), -1)
        if line != i:
            raise GenError(f"LED_WS2812.list[{i}].bus.index={line}（要順序 {i}；"
                           "index = 第幾條 data line）")
        px = num(it.get("px"), 0)
        if px < 1 or px > LED_PX_MAX:
            raise GenError(f"LED_WS2812.list[{i}].px={px}（1..{LED_PX_MAX}）")
        # 2026-11-28：每條線可以不同 px；內部對齊到**最長**那條。
        # 同一 transaction 會送 max px 給所有 active lane；短線多出的資料由
        # WS2812 鏈尾自然忽略（唔需要為短線額外填充 buffer）。
        line_px.append(px)
        if px0 is None or px > px0:
            px0 = px
        order = str(it.get("order") or "").upper()
        if order:
            if order not in ("GRB", "RGB"):
                if order in ("GRBW", "RGBW"):
                    fmt = "ws2812w"
                else:
                    raise GenError(f"LED_WS2812.list[{i}].order={order!r} 看不懂"
                                   "（GRB／RGB／GRBW／RGBW）")

    # px0 = 最長那條線的 px；所有 lane 都用它（auto-align）。
    # hardware 層：S3 只有一個 I80 → legacy bus=0；但一定要有足夠 data 腳。
    buses = ((wiring.get("I80") or {}).get("list") or [])
    if not buses:
        raise GenError("wiring.json 冇 I80.list（LED_WS2812 要 I80 資料腳）")
    used = [x for x in (num(v, -1) for v in (buses[0].get("data") or [])) if x >= 0]
    if len(used) < len(items):
        raise GenError(f"LED_WS2812 要 {len(items)} 條線，但 wiring.json I80[0].data "
                       f"只有 {len(used)} 支有效腳")
    lanes = len(items)
    body = [
        "// driver 層：LED_WS2812（來源 driver.json 的 LED_WS2812 段）。",
        "// 每條線可以不同 px／內容：frame=Σpx、DMA 時間=maxpx；短線尾出 reset。",
        "// timing 不在 config —— codec 用格式／driver 內建預設。",
        "static const nc_led_cfg_t s_led = {",
        "    .present = true,",
        "    .enable = true,",
        "    .backend = NC_LED_BACKEND_I80,",
        "    .bus = 0,",
        f"    .lanes = {lanes},",
        f"    .px = {px0},",
        f"    .fmt = {cstr(fmt)},",
        f"    .n_line = {lanes},",
        "    .line_px = {" + ", ".join(str(v) for v in line_px) + "},",
        "};",
        "const nc_led_cfg_t *nc_port_led(void) { return &s_led; }",
    ]
    return "\n".join(body) + "\n", {
        "present": True, "enable": True, "backend": "i80", "bus": 0,
        "px": px0, "format": fmt, "lanes": lanes, "wired": True,
        "line_px": line_px, "source": "LED_WS2812",
    }


def gen_led(driver: dict | None, wiring: dict,
            suppressed: list[str] | None = None) -> tuple[str, dict]:
    """driver.json 的 LED 段 → nc_led_cfg_t（build-time 生成）。

    腳位**不是這裡發明**：I80 的 D0..Dn／WR／DC 來自 wiring.json 的 I80.list[index]，
    SPI 的 MOSI／SCLK 來自 SPI.list[index]。driver.json 只引用 bus。

    suppressed = project 的 sdkconfig 覆寫了 pixel 的符號名（見 project_pixel_override）：
    那時仍然驗證 JSON（錯就 build 失敗），但唔會生成（讓位俾 project 的 Kconfig）。
    """
    # 2026-11-28：新系列 LED_WS2812（每 item 一條線）優先；舊 LED 段仍然接受。
    if isinstance(driver, dict) and "LED_WS2812" in driver:
        return _gen_led_ws2812(driver, wiring, suppressed)

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
            used = [x for x in (num(v, -1) for v in (b.get("data") or [])) if x >= 0]
            if not (1 <= len(used) <= 16):
                raise GenError(f"wiring.json I80[{bus_idx}].data 有效腳 {len(used)} 支"
                               "（要 1..16；-1／null＝未用）")
            width = 8 if len(used) <= 8 else 16
            if lanes == 0:
                lanes = 1
            if lanes < 1 or lanes > len(used):
                raise GenError(f"LED.lanes={lanes} 超出 wiring.json I80[{bus_idx}] 的有效腳數 "
                               f"{len(used)}（最多 {len(used)} 條）")
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
            "（目前 typed 解析：ETH／SDIO／PIN／PWM／UART／SPI／I80；I2C／I2S／ENC／XL9555 未 parse）。",
            "燈鏈（LED）不在這份：它是 driver 層，寫在該板的 driver.json，"
            "build 時生成 nc_led_cfg_t（見 nc_port.c）。",
            "硬體事實（flash/psram/CPU）在 board.json，不在這裡。",
        ],
        "version": 1,
    }
    for k, v in wiring.items():
        if k in ("version", "_comment"):
            continue
        out[k] = v
    return out


def gen_preset_devices(driver: dict | None, suppressed: list[str] | None) -> dict:
    """driver.json → /data/config/devices.json 的出廠預置（runtime 可改）。

    內容＝driver.json 的 device 段（LED／TFT／PCA9685…），但**唔含腳位**：
    device 只寫 bus:{type,index}，腳位仍然只在 wiring.json／hardware.json。
    project 覆寫 pixel（suppressed）時 LED 段讓位（唔宣告），render 會照舊用 Kconfig。
    """
    out = {
        "version": 1,
        "_comment": [
            "應用硬件（device）層：呢份係 build 時由 ports/hardware/.../driver.json 產生的預置。",
            "現場可以直接改 /data/config/devices.json（USB 或 WebMaster），**reboot 生效**，唔使重 build。",
            "規則：device 只寫 bus:{type,index}＋型號參數；**腳位只在 hardware.json**（I80/SPI/…）。",
            "目前 firmware 真正支援：LED（ws2812 via i80）。其餘 device 寫 enable=1 會開機大聲 warn（唔會靜默）。",
            "呢個檔缺少都開得機：render 會退回 build-time 的 driver.json 設定。",
        ],
    }
    if isinstance(driver, dict):
        for k, v in driver.items():
            if k in ("version", "_comment"):
                continue
            if suppressed and k == "LED":
                out["_led_note"] = "project 覆寫 pixel → LED 由 Kconfig 決定（呢份唔宣告 LED）"
                continue
            out[k] = v
    return out


def gen_activation(wiring: dict, driver: dict | None, led_sum: dict) -> list[dict]:
    """生成期決定「邊個 unit 會激活＋次序＋依賴」（runtime 唔探測硬件）。

    enabled 來源：bus 睇 wiring.json 的 enable；device 睇 driver.json 的 enable
    （LED 由 gen_led 結果決定，已計 project 讓位）。project 覆寫大於 board 的規則
    仍然喺 project_pixel_override() 處理。
    """
    # (unit, level, deps)
    units = [
        ("pin", 0, []), ("sdio", 0, []), ("eth", 0, []), ("usb", 0, []),
        # bus 自己擁有 GPIO，唔依賴 PIN（PIN 只係通用 label 表）
        ("spi", 1, []), ("i2c", 1, []), ("uart", 1, []),
        ("pwm", 1, []), ("i2s", 1, []), ("i80", 1, []), ("enc", 1, []),
        ("led", 2, ["i80"]), ("apa102", 2, ["spi"]), ("tft", 2, ["spi"]),
        ("sd", 2, ["sdio"]), ("pca9685", 2, ["i2c"]), ("xl9555", 2, ["i2c"]),
        ("gt1151q", 2, ["i2c"]), ("husb238", 2, ["i2c"]), ("pcm5102", 2, ["i2s"]),
        ("uart_motor", 2, ["uart"]), ("audio", 2, []),
    ]
    bus_key = {"pin": "PIN", "sdio": "SDIO", "eth": "ETH", "usb": "USB",
               "spi": "SPI", "i2c": "I2C", "uart": "UART", "pwm": "PWM",
               "i2s": "I2S", "i80": "I80", "enc": "ENC"}
    drv_key = {"led": "LED", "apa102": "APA102", "tft": "TFT", "sd": "SDcard",
               "pca9685": "PCA9685", "xl9555": "XL9555", "gt1151q": "GT1151Q",
               "husb238": "HUSB238", "pcm5102": "PCM5102", "uart_motor": "uartMotor",
               "audio": "Audio"}
    act: list[dict] = []
    for unit, level, deps in units:
        enabled = False
        if unit in bus_key:
            v = wiring.get(bus_key[unit])
            enabled = bool((v or {}).get("enable")) if isinstance(v, dict) else False
        elif unit in drv_key:
            if unit == "led":
                enabled = bool(led_sum.get("present") and led_sum.get("enable"))
            else:
                v = (driver or {}).get(drv_key[unit])
                enabled = bool((v or {}).get("enable")) if isinstance(v, dict) else False
        # 依賴唔 enabled → 呢個 unit 唔可以 enabled（生成期就報，唔使開機才發現）
        missing = [d for d in deps if not any(a["unit"] == d and a["enabled"] for a in act)]
        act.append({
            "unit": unit,
            "level": level,
            "enabled": bool(enabled),
            "deps": deps,
            "missing_deps": missing if enabled else [],
        })
    return act


def gen_activation_c(activation: list[dict], src_hash: int) -> str:
    """生成期激活表 → nc_port.c 的 const 表（firmware 開機照表；src_hash 版本核對）。"""
    rows = []
    for i, a in enumerate(activation):
        if i >= 24:
            raise GenError("activation 超過 NC_PORT_ACT_MAX=24")
        rows.append('    { "%s", %d, %d },' % (a["unit"], a["level"], 1 if a["enabled"] else 0))
    body = "\n".join(rows)
    return (
        "// 生成期激活表：邊個 unit 會激活、level（0 平台／1 bus／2 device）、enabled。\n"
        "// firmware 開機只照呢張表；enable=0 → 零 GPIO／零 buffer。\n"
        "// src_hash = crc32(名稱+內容) 逐個來源檔累加；跑 tools/ncportgen.py 時寫死。\n"
        f"static const nc_port_act_t s_act[] = {{\n{body}\n}};\n"
        "const nc_port_act_t *nc_port_activation(size_t *n)\n"
        "{\n"
        f"    if (n != NULL) *n = {len(activation)}u;\n"
        "    return s_act;\n"
        "}\n\n"
        f"uint32_t nc_port_activation_src_hash(void) {{ return 0x{src_hash & 0xFFFFFFFF:08x}u; }}\n"
    )


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
    summary["driver_preset"] = "preset_driver.json"
    summary["activation_preset"] = "preset_activation.json"
    summary.update({"board": hw_rel, "chip": chip, "cores": num(cap.get("cores"), 1),
                    "usb": (cap.get("usb") or {}).get("link")})
    summary["led"] = led_sum

    # 生成期激活表：同一份資料 → preset_activation.json（人睇）＋ nc_port.c 的 const 表
    # （firmware 開機照表執行；src_hash 核對表同來源 JSON 係唔係同一版本）。
    activation = gen_activation(wiring, driver, led_sum)
    src_files = []
    for name in ("wiring.json", "driver.json"):
        fp = hw / name
        if fp.is_file():
            src_files.append((name, fp.read_bytes()))
    if project is not None and (project / "sdkconfig.defaults").is_file():
        src_files.append((project.name + "/sdkconfig.defaults",
                          (project / "sdkconfig.defaults").read_bytes()))
    src_hash = 0
    for name, data in src_files:
        src_hash = zlib.crc32(name.encode("utf-8") + b"\0" + data, src_hash)
    # 預置 JSON 文字先產生（同一份 bytes 既寫檔又計 hash）：
    # firmware 開機用同一條 hashing 規則（檔名+NUL+內容，逐檔累加）比對 FAT；
    # 唔同 = FAT 改過（用戶現場微調）→ activation 表係 build-time，enable 變更要 rebuild。
    hw_text = json.dumps(preset, ensure_ascii=False, indent=2) + "\n"
    devices = gen_preset_devices(driver, project_pixel_override(project))
    drv_text = json.dumps(devices, ensure_ascii=False, indent=2) + "\n"
    cfg_hash = zlib.crc32(b"hardware.json\x00" + hw_text.encode("utf-8"))
    cfg_hash = zlib.crc32(b"driver.json\x00" + drv_text.encode("utf-8"), cfg_hash)
    activation_c = gen_activation_c(activation, src_hash) + (
        "// FAT 上的 hardware.json／driver.json（如果同 build-time 預置一樣）應得的 hash。\n"
        "// 開機比對：唔同 = 用戶現場改過 config → activation 表係舊 build。\n"
        f"uint32_t nc_port_config_hash(void) {{ return 0x{cfg_hash & 0xFFFFFFFF:08x}u; }}\n"
    )

    out.mkdir(parents=True, exist_ok=True)
    (out / "nc_port.h").write_text(
        "// 由 tools/ncportgen.py 產生，不要手改。\n"
        "// 來源：ports/hardware/" + hw_rel + "/{board,wiring,driver}.json + _chips/" + str(chip) + ".json\n"
        "#pragma once\n\n"
        "#include \"nc_cfg.h\"\n\n"
        "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n"
        "#include <stddef.h>\n\n"
        "// 生成期激活表（nc_port.c 內 const；級別：0 平台／1 bus／2 device）。\n"
        "#define NC_PORT_ACT_MAX 24\n"
        "typedef struct {\n"
        "    const char *unit;\n"
        "    uint8_t level;\n"
        "    uint8_t enabled;\n"
        "} nc_port_act_t;\n"
        "const nc_port_act_t *nc_port_activation(size_t *n);\n"
        "uint32_t nc_port_activation_src_hash(void);\n"
        "uint32_t nc_port_config_hash(void);   // build-time 預置 hardware+driver 的 hash\n\n"
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
        + gen_usb_kinds(wiring) + "\n" + activation_c + "\n",
        encoding="utf-8")
    (out / "preset_hardware.json").write_text(hw_text, encoding="utf-8")
    # 應用硬件（device）層預置：runtime /data/config/driver.json 可改；
    # 缺檔／壞檔時 render 退回 build-time 的 nc_port_led()。
    (out / "preset_driver.json").write_text(drv_text, encoding="utf-8")
    # 人睇的鏡像（同 nc_port.c 的 const 表同源；src_hash 相同）
    (out / "preset_activation.json").write_text(
        json.dumps({"version": 1,
                    "src_hash": "0x%08x" % (src_hash & 0xFFFFFFFF),
                    "src_files": [{"name": n, "crc32": "0x%08x" % (zlib.crc32(d) & 0xFFFFFFFF)}
                                  for n, d in src_files],
                    "units": activation},
                   ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
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
