#!/usr/bin/env bash
# pixel_test — 渲染器核心的 host 測試（環 + 編碼器；不需要 ESP-IDF）
#
#   ./run.sh            建置並跑
#   CC=clang ./run.sh   指定編譯器
#
# 驗：環語義（滿了不覆蓋 / 假消費 / 重用 / 換場）、
#     編碼位元（GRB / GRBW / SPI / APA102）、抖動平均、多 lane 合成、
#     mock / direct 後端走同一份 codec。
# 測試與 firmware 編的是同一批 .c（不是兩份互相對照的程式）。
set -euo pipefail
cd "$(dirname "$0")/../../.."

CC=${CC:-cc}
PXF=firmware/components/sys/pixel
OUT="tools/hosttest/pixel_test/build"
mkdir -p "$OUT"

"$CC" -std=c17 -D_DEFAULT_SOURCE -Wall -Wextra -Wno-unused-parameter \
    -I"$PXF/include" -Itools/hosttest/pixel_test/include \
    -o "$OUT/pixel_test" \
    "$PXF/nc_pixel_q.c" \
    "$PXF/nc_pixel_encode.c" \
    "$PXF/nc_pixel_backend_mock.c" \
    "$PXF/nc_pixel_backend_direct.c" \
    tools/hosttest/pixel_test/pixel_test.c

"$OUT/pixel_test"
