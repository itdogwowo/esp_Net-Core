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

MAX = {"sdio": 2, "pin": 16, "pwm": 8, "uart": 3}

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
    lines.append("};")
    return "\n".join(lines), summary


def gen_preset(wiring: dict, hw_rel: str) -> dict:
    out = {
        "_comment": [
            "這份是 build 時由 ports/hardware/" + hw_rel + "/wiring.json 產生的出廠預置（鏡子）。",
            "它會被 build 嵌入、並在 /data/config/hardware.json 缺失時 seed 到裝置；"
            "**唔係** firmware 內建預設本身：真正的內建預設是 nc_port.c（同樣由 wiring.json 生成）。",
            "所以改這份只會改「用戶看到的值」；要改行為就改 wiring.json 再 rebuild"
            "（目前 typed 解析：ETH／SDIO／PIN／PWM／UART；SPI／I2C／I80／I2S／ENC／XL9555 未 parse）。",
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
    args = ap.parse_args()

    hw = Path(args.hardware).resolve()
    out = Path(args.out).resolve()
    board = load_json(hw / "board.json")
    wiring = load_json(hw / "wiring.json")
    chip = board.get("chip")
    if not chip:
        raise GenError(f"{hw}/board.json 缺 chip")
    cap = chip_caps(chip)

    hw_rel = f"{hw.parent.name}/{hw.name}"
    chip_c = gen_chip(cap)
    hw_c, summary = gen_hardware(wiring)
    preset = gen_preset(wiring, hw_rel)
    summary.update({"board": hw_rel, "chip": chip, "cores": num(cap.get("cores"), 1),
                    "usb": (cap.get("usb") or {}).get("link")})

    out.mkdir(parents=True, exist_ok=True)
    (out / "nc_port.h").write_text(
        "// 由 tools/ncportgen.py 產生，不要手改。\n"
        "// 來源：ports/hardware/" + hw_rel + "/{board,wiring}.json + _chips/" + str(chip) + ".json\n"
        "#pragma once\n\n"
        "#include \"nc_cfg.h\"\n\n"
        "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n"
        "const nc_cfg_chip_t *nc_port_chip(void);\n"
        "const nc_hardware_cfg_t *nc_port_hw_defaults(void);\n"
        "uint8_t nc_port_usb_kind(uint8_t idx);\n\n"
        "#ifdef __cplusplus\n}\n#endif\n",
        encoding="utf-8")
    (out / "nc_port.c").write_text(
        "// 由 tools/ncportgen.py 產生，不要手改。\n"
        "// 這是這塊硬體的「編譯期預設」：沒有 JSON、沒有 cJSON、沒有字串字典。\n"
        "#include \"nc_cfg.h\"\n#include \"nc_port.h\"\n\n"
        + chip_c + "\n\n" + hw_c + "\n\n"
        "const nc_cfg_chip_t *nc_port_chip(void) { return &s_chip; }\n"
        "const nc_hardware_cfg_t *nc_port_hw_defaults(void) { return &s_hw; }\n\n"
        + gen_usb_kinds(wiring) + "\n",
        encoding="utf-8")
    (out / "preset_hardware.json").write_text(
        json.dumps(preset, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (out / "nc_port_summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print(f"ncportgen: {hw_rel} → {out}  "
          f"(sdio {summary['counts']['sdio']}, pin {summary['counts']['pin']}, "
          f"pwm {summary['counts']['pwm']}, uart {summary['counts']['uart']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
