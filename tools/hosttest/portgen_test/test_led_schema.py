#!/usr/bin/env python3
"""LED／driver 層 schema 的 host 測試（唔需要 IDF、唔需要板子）。

三件事：
  1. tools/ncportgen.py 真的會由 driver.json ＋ wiring.json 生成 nc_led_cfg_t；
  2. project sdkconfig 覆寫 pixel 時，board 的 LED 段會讓位（Kconfig 贏）；
  3. tools/board.py 的分層守門會抓到「driver 層寫 GPIO／硬件段／bus 指唔到」。

用法： python3 tools/hosttest/portgen_test/test_led_schema.py
"""
from __future__ import annotations

import importlib.util
import json
import shutil
import subprocess
import sys
from contextlib import contextmanager
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
PORTS = ROOT / "ports"
PY = sys.executable

fails: list[str] = []


def ok(name: str, cond: bool, detail: str = "") -> None:
    if cond:
        print(f"  OK   {name}")
    else:
        print(f"  FAIL {name}" + (f"   {detail}" if detail else ""))
        fails.append(name)


def gen(hardware: str, out: Path, project: str | None = None) -> subprocess.CompletedProcess:
    cmd = [PY, str(ROOT / "tools" / "ncportgen.py"),
           "--hardware", str(PORTS / "hardware" / hardware), "--out", str(out)]
    if project:
        cmd += ["--project", str(PORTS / "project" / project)]
    return subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                          errors="replace")


# ── board.py 當模組用（測 helper，不需要動真檔）──
spec = importlib.util.spec_from_file_location("ncboard", ROOT / "tools" / "board.py")
ncboard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ncboard)

MATRIX_DIR = PORTS / "hardware" / "S3" / "ESP32-S3-Matrix"
WIRING = {
    "I80": {"enable": 1, "list": [
        {"data": [14, 38, 39, 40, 41, 42, 47, 48], "wr": 1, "dc": 2, "cs": -1,
         "freq": 80000000}]},
    "SPI": {"enable": 0, "list": []},
    "I2C": {"enable": 0, "list": []},
    "UART": {"enable": 0, "list": []},
}


def check(drv: dict, wiring: dict = WIRING, cfg: dict | None = None):
    r = ncboard.Report()
    on = ncboard._check_driver_layers(MATRIX_DIR, drv, wiring, cfg or {}, r)
    return r, on


@contextmanager
def _workdir():
    """工作目錄放 artifacts/（同其他 host test 一樣；唔用系統 temp，Windows sandbox 會擋）。"""
    d = ROOT / "artifacts" / "hosttest_led"
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True, exist_ok=True)
    yield d


def main() -> int:
    with _workdir() as tmp:

        # ── 1. 真板：硬件（I80）與裝置（LED）分兩層生成 ──
        print("ncportgen：Matrix 的 I80 ＋ LED 段")
        p = gen("S3/ESP32-S3-Matrix", tmp / "matrix")
        ok("Matrix 生成成功", p.returncode == 0, p.stderr.strip())
        c = (tmp / "matrix" / "nc_port.c").read_text(encoding="utf-8")
        ok("硬件層有 n_i80 = 1", ".n_i80 = 1" in c)
        # 2026-11-26 稀疏 schema：Matrix 只登記 GPIO14；未用槽 = -1，n_used 記錄真腳數
        ok("稀疏 data：只 1 支有效（GPIO14），其餘 -1",
           ".data = { 14, -1, -1, -1, -1, -1, -1, -1 }" in c)
        ok("n_used = 1、bus 寬度自動 = 8", ".bus_width = 8, .n_data = 8, .n_used = 1" in c)
        ok("冇登記 wr／dc（直驅唔需要；有登記才 route）", ".wr = -1, .dc = -1" in c)
        ok("driver 層只引用 bus", ".backend = NC_LED_BACKEND_I80" in c and ".bus = 0" in c)
        ok("driver 層冇再寫 pins", ".pins" not in c)
        ok("px = 64（per lane）", ".px = 64" in c)
        ok("timing 有寫入", ".clk_hz = 2500000u" in c and ".bit_cells = 3" in c)
        h = (tmp / "matrix" / "nc_port.h").read_text(encoding="utf-8")
        ok("nc_port.h 有 nc_port_led", "nc_port_led" in h)
        # 同一份值也寫在 sdkconfig：兩邊要一致，否則「改咗唔生效」
        kv = ncboard.parse_kconfig(MATRIX_DIR / "sdkconfig.defaults")
        want = [int(x) for x in kv["NETCORE_PIXEL_I80_PINS"].strip('"').split(",")]
        # 稀疏 schema：Kconfig 字串只放真正接燈的腳（Matrix 只有 14）
        ok("wiring.json 與 sdkconfig 的腳位一致（只真腳）", want == [14], str(want))

        # ── 2. project 覆寫 pixel → board 的 LED 段讓位 ──
        print("ncportgen：project 覆寫 pixel（Show2400）")
        p = gen("S3/ESP32-S3-Zero", tmp / "z2400", "S3/ESP32-S3-Zero-Show2400")
        ok("生成成功", p.returncode == 0, p.stderr.strip())
        c = (tmp / "z2400" / "nc_port.c").read_text(encoding="utf-8")
        ok("LED present=false（讓位）", ".present = false" in c)
        ok("有講明讓位原因", "讓位" in c and "NETCORE_PIXEL_MOCK_ONLY" in c)

        # ── 3. project 冇覆寫 → 照用 JSON ──
        print("ncportgen：project 冇覆寫 pixel（Zero-Show）")
        p = gen("S3/ESP32-S3-Zero", tmp / "zshow", "S3/ESP32-S3-Zero-Show")
        c = (tmp / "zshow" / "nc_port.c").read_text(encoding="utf-8")
        ok("Zero 的 8 支有效腳 = 1,38,39,40,41,42,47,48",
           ".data = { 1, 38, 39, 40, 41, 42, 47, 48 }" in c)
        ok("Zero 有登記 wr／dc = 2／4（esp_lcd 板）", ".wr = 2, .dc = 4" in c)

        # ── 3b. board 冇燈（enable=0）但 project 有燈 → 讓位要贏過「明確關閉」 ──
        # 迴歸：SuperMini 的 board LED 係 enable=0，textkit 用 project sdkconfig 開燈。
        # 如果生成 present=true/enable=false，render 會只留 mock lane → 燈唔會亮。
        print("ncportgen：board 冇燈但 project 有燈（SuperMini＋textkit）")
        p = gen("S3/ESP32-S3-SuperMini", tmp / "sm_textkit",
                "S3/ESP32-S3-SuperMini-LED-Textkit")
        ok("生成成功", p.returncode == 0, p.stderr.strip())
        c = (tmp / "sm_textkit" / "nc_port.c").read_text(encoding="utf-8")
        ok("讓位：s_led present=false（單行 stub）",
           "static const nc_led_cfg_t s_led = { .present = false, .enable = false };" in c)
        ok("唔可以當成「明確關閉」（冇 disable 註解）", "明確唔用燈" not in c)
        ok("有講明讓位原因", "讓位" in c and "NETCORE_PIXEL_" in c)

        # ── 4. 壞 schema：bus 指唔到 → build 要失敗（唔可以靜靜當冇事）──
        print("ncportgen：壞 schema 要 build 失敗")
        bad = tmp / "bad"
        (bad / "hw").mkdir(parents=True)
        (bad / "hw" / "board.json").write_text(json.dumps(
            {"name": "BadLed", "chip": "esp32s3",
             "flash": {"size_mb": 4}, "psram": {}, "cpu_mhz": 240, "console": "uart0"},
            ensure_ascii=False), encoding="utf-8")
        (bad / "hw" / "wiring.json").write_text(json.dumps(
            {"version": 1, "I80": {"enable": 0, "list": []}}), encoding="utf-8")
        (bad / "hw" / "driver.json").write_text(json.dumps(
            {"LED": {"enable": 1, "backend": "i80",
                     "bus": {"type": "i80", "index": 0},
                     "lanes": 1, "px": 64, "format": "ws2812"}}, ensure_ascii=False),
            encoding="utf-8")
        p = subprocess.run([PY, str(ROOT / "tools" / "ncportgen.py"),
                            "--hardware", str(bad / "hw"), "--out", str(bad / "out")],
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
        ok("bus 指唔到 → 非 0 退出", p.returncode != 0)
        ok("錯誤訊息提到 I80.list", "I80.list" in (p.stderr + p.stdout))

        # ── 5. board.py 分層守門 ──
        print("board.py：分層守門")
        good_led = {"enable": 1, "backend": "i80", "bus": {"type": "i80", "index": 0},
                    "lanes": 1, "px": 64, "format": "ws2812",
                    "timing": {"pclk_hz": 2500000, "bit_cells": 3, "t0h": 1, "t1h": 2}}
        r, on = check({"LED": good_led})
        ok("合法 LED 段：0 error", not r.errors, str(r.errors))
        ok("合法 LED 段：回 True", on is True)

        r, _ = check({"I80": {"enable": 0, "list": []}})
        ok("driver 層寫 I80 → error", any("hardware 層" in e for e in r.errors), str(r.errors))

        r, _ = check({"PCA9685": {"enable": 1,
                                  "list": [{"GPIO": {"i2c": 0}}]}})
        ok("舊單線寫法 → warn 而唔係 error",
           not r.errors and any("舊單線" in w for w in r.warns), f"{r.errors} {r.warns}")

        r, _ = check({"TFT": {"enable": 1, "bus": {"type": "spi", "index": 0},
                              "profile": "ST7789"}})
        ok("bus index 指唔到 → error", any("SPI.list[0]" in e for e in r.errors), str(r.errors))

        r, _ = check({"TFT": {"enable": 0, "bus": {"type": "spi", "index": -1},
                              "profile": "ST7789"}})
        ok("enable=0 而 bus=-1 → 0 error（未接是合法）", not r.errors, str(r.errors))

        bad_order = dict(good_led, order="RGB")
        r, _ = check({"LED": bad_order})
        ok("order 與 format 唔一致 → error", any("order" in e for e in r.errors), str(r.errors))

        r, on = check({"LED": {"enable": 0, "backend": "rmt",
                               "bus": {"type": "i80", "index": -1},
                               "px": 0, "format": "ws2812"}})
        ok("enable=0 的 template（index:-1）→ 0 error", not r.errors, str(r.errors))
        ok("enable=0 → 回 False", on is False)

        r, _ = check({"LED": dict(good_led, backend="parlio")})
        ok("parlio 未有 driver → warn", any("未有 firmware 後端" in w for w in r.warns),
           str(r.warns))

        r, _ = check({"LED": dict(good_led, px=0)})
        ok("px=0 → error", any("px=" in e for e in r.errors), str(r.errors))

        r, _ = check({"LED": dict(good_led, format="apa102")})
        ok("i80 + apa102（時脈格式）→ error",
           any("時脈格式" in e for e in r.errors), str(r.errors))

        # runtime 覆蓋路徑：nc_cfg_load.c 一定要有 apply_i80／apply_spi，否則
        # /data/config/hardware.json 的 I80 又變回死段（正是今次修的東西）。
        load_c = (ROOT / "firmware/components/sys/config/nc_cfg_load.c").read_text(encoding="utf-8")
        ok("nc_cfg_load.c 有 apply_i80 且被呼叫",
           "static void apply_i80(" in load_c and "apply_i80(root, out, st);" in load_c)
        ok("nc_cfg_load.c 有 apply_spi 且被呼叫",
           "static void apply_spi(" in load_c and "apply_spi(root, out, st);" in load_c)

        r, _ = check({"LED": dict(good_led, **{"timing": {"t0h": 3, "t1h": 2}})})  # type: ignore[arg-type]
        ok("t0h >= t1h → error", any("t0h < t1h" in e for e in r.errors), str(r.errors))

        # Kconfig 對帳：兩邊寫唔同就要報
        r, _ = check({"LED": good_led},
                     cfg={"NETCORE_PIXEL_I80_PINS": '"14,38,39,40,41,42,47,48,9,9"'})
        ok("driver.json 與 sdkconfig 腳位唔一致 → error",
           any("NETCORE_PIXEL_I80_PINS" in e for e in r.errors), str(r.errors))

    print()
    if fails:
        print(f"{len(fails)} 個 FAIL：")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("test_led_schema OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
