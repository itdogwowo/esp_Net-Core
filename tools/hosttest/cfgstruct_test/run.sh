#!/bin/sh
# config 型別化（JSON → struct）的 host 測試（POSIX）。需要 cc。
#
# 真的 cJSON ＋ 真的 nc_cfg_load.c；esp_log / esp_err 換成 stdout 替身。
set -e
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
OUT="${TMPDIR:-/tmp}/cfgstruct_test_build"
CJSON="$ROOT/firmware/managed_components/espressif__cjson/cJSON"
mkdir -p "$OUT"

# cJSON 是第三方程式碼：用 -w 關掉它的警告（我們自己的檔案仍然 -Werror）。
cc -std=c11 -w -I"$CJSON" -c "$CJSON/cJSON.c" -o "$OUT/cJSON.o"

cc -std=c11 -Wall -Wextra -Werror \
   -I"$ROOT/tools/hosttest/cfgstruct_test/include" \
   -I"$ROOT/firmware/components/sys/config/include" \
   -I"$ROOT/firmware/components/sys/config" \
   -I"$CJSON" \
   -DPRESET_SYSTEM="\"$ROOT/ports/S3/ESP32-S3-N16R8/data/config/system.json\"" \
   -DPRESET_HARDWARE_P4="\"$ROOT/ports/P4/ESP32-P4-ETH/data/config/hardware.json\"" \
   -DPRESET_HARDWARE_S3="\"$ROOT/ports/S3/ESP32-S3-N16R8/data/config/hardware.json\"" \
   "$ROOT/tools/hosttest/cfgstruct_test/main.c" \
   "$ROOT/tools/hosttest/cfgstruct_test/stubs.c" \
   "$ROOT/firmware/components/sys/config/nc_cfg.c" \
   "$ROOT/firmware/components/sys/config/nc_cfg_patch.c" \
   "$ROOT/firmware/components/sys/config/nc_cfg_load.c" \
   "$ROOT/firmware/components/sys/config/nc_cfg_defaults.c" \
   "$OUT/cJSON.o" \
   -o "$OUT/cfgstruct_test"
"$OUT/cfgstruct_test"
