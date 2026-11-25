#!/usr/bin/env bash
set -eu
T="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$T/../../.." && pwd)"
mkdir -p "$T/build"
cc -std=c17 -Wall -Wextra -Werror -O2 \
   -I"$ROOT/firmware/components/tasks/pixel_render_task" \
   "$ROOT/firmware/components/tasks/pixel_render_task/nc_fps_clock.c" \
   "$T/fpsclock_test.c" -o "$T/build/fpsclock_test"
"$T/build/fpsclock_test"
