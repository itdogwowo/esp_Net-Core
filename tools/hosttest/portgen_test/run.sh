#!/bin/sh
# ports 兩層 → firmware 預設的 host 測試（不需要 IDF；有 cc 就順便編譯 generated C）。
set -e
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
OUT="${TMPDIR:-/tmp}/ncportgen_test"
PYTHON="${PYTHON:-$(command -v python || command -v python3)}"
rm -rf "$OUT"
mkdir -p "$OUT"

gen() {  # gen <hardware-rel> <out-subdir>
    "$PYTHON" "$ROOT/tools/ncportgen.py" \
        --hardware "$ROOT/ports/hardware/$1" --out "$OUT/$2" >/dev/null
}

echo "  P4/ESP32-P4-ETH"
gen P4/ESP32-P4-ETH p4eth
grep -q '.phy_type = NC_CFG_PHY_IP101' "$OUT/p4eth/nc_port.c"
grep -q '.mdc = 31' "$OUT/p4eth/nc_port.c"
grep -q '.cores = 2' "$OUT/p4eth/nc_port.c"
grep -q '.usb_kind = NC_LINK_USBJTAG' "$OUT/p4eth/nc_port.c"

echo "  S3/ESP32-S3-N16R8"
gen S3/ESP32-S3-N16R8 s3n16
grep -q '.n_sdio = 1' "$OUT/s3n16/nc_port.c"
grep -q '.clk = 7' "$OUT/s3n16/nc_port.c"
grep -q '.d = { 15, 16, 4, 5 }' "$OUT/s3n16/nc_port.c"

echo "  S3/ESP32-S3-Matrix（無 PSRAM、無 ETH）"
gen S3/ESP32-S3-Matrix s3mx
grep -q '.enable = false' "$OUT/s3mx/nc_port.c"
grep -q '.n_sdio = 0' "$OUT/s3mx/nc_port.c"

echo "  C3/ESP32-C3-DevKitM（單核、只有 USJ）"
gen C3/ESP32-C3-DevKitM c3
grep -q '.cores = 1' "$OUT/c3/nc_port.c"
grep -q '.usb_otg = false' "$OUT/c3/nc_port.c"
grep -q '.usb_serial_jtag = true' "$OUT/c3/nc_port.c"

echo "  preset_hardware.json 與 wiring.json 語義相同"
"$PYTHON" - "$ROOT" "$OUT" <<'PY'
import json, sys
from pathlib import Path
root, out = Path(sys.argv[1]), Path(sys.argv[2])
for rel, sub in [("P4/ESP32-P4-ETH", "p4eth"), ("S3/ESP32-S3-N16R8", "s3n16"),
                 ("S3/ESP32-S3-Matrix", "s3mx"), ("C3/ESP32-C3-DevKitM", "c3")]:
    a = json.loads((root / "ports/hardware" / rel / "wiring.json").read_text(encoding="utf-8"))
    b = json.loads((out / sub / "preset_hardware.json").read_text(encoding="utf-8"))
    for k in a:
        if k in ("version", "_comment"):
            continue
        assert a[k] == b.get(k), f"{rel}: {k} 不一致"
print("  OK")
PY

echo "  LED 段（driver.json → nc_led_cfg_t）＋分層守門"
"$PYTHON" "$ROOT/tools/hosttest/portgen_test/test_led_schema.py"

if command -v cc >/dev/null 2>&1; then
    echo "  generated C 編譯（-Werror，不 link IDF）"
    CJSON="$ROOT/firmware/managed_components/espressif__cjson/cJSON"
    for sub in p4eth s3n16 s3mx c3; do
        cc -std=c11 -Wall -Wextra -Werror -c "$OUT/$sub/nc_port.c" \
           -I"$ROOT/tools/hosttest/cfgstruct_test/include" \
           -I"$ROOT/firmware/components/sys/config/include" \
           -I"$CJSON" -o "$OUT/$sub/nc_port.o"
    done
else
    echo "  （沒有 cc：跳過 generated C 編譯）"
fi

echo "  portgen_test OK"
