#!/usr/bin/env bash
# 一次跑完所有 host 測試（不需要板子、不需要 IDF）。
#
# 用法： bash tools/hosttest/run_all.sh
#
# 每支子測試都有自己的 run.sh（Linux / WSL）；部分另有 run.bat（Windows / MSVC）。
# 全部都用 -Wall -Wextra -Werror，所以「跑得過」同時等於「冇警告」。
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TESTS="cfg_test cfgstruct_test fxm_test task_test router_test link_test fx_test hub_test plan_test"
fail=0
for t in $TESTS; do
    printf '%-12s ' "$t"
    log="${TMPDIR:-/tmp}/hosttest_$t.log"
    if bash "$ROOT/tools/hosttest/$t/run.sh" >"$log" 2>&1; then
        echo OK
    else
        echo "FAIL"
        tail -20 "$log"
        fail=$((fail + 1))
    fi
done
if [ "$fail" -ne 0 ]; then
    echo "—— $fail 支測試失敗"
    exit 1
fi
echo "—— 全部通過"
