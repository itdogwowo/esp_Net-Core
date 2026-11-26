#!/usr/bin/env bash
# 一次跑完所有 host 測試（不需要板子、不需要 IDF）。
#
# 用法： bash tools/hosttest/run_all.sh
#
# 每支子測試都有自己的 run.sh（Linux / WSL）；部分另有 run.bat（Windows / MSVC）。
# 全部都用 -Wall -Wextra -Werror，所以「跑得過」同時等於「冇警告」。
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# 先確認 state key header 同 schema/state-keys.json 同步（唔同步＝直接失敗）
if ! python3 "$ROOT/tools/gen_state_keys.py" --check; then
    echo "—— state key schema 唔同步"
    exit 1
fi
# 中央示範設定（ports/presets/）：JSON 結構、schema 範圍、四層四任務
if ! python3 "$ROOT/tools/preset_check.py" --quiet; then
    echo "—— ports/presets 檢查失敗"
    exit 1
fi
TESTS="cfg_test cfgstruct_test ncd_test task_test router_test link_test mod_test hub_test plan_test buf_test portgen_test ncm_test pixel_test pxEngine_test pxAsset_test pxProgram_test pxLayer_test pxShow_test pxScan_test pxLayout_test stateBlob_test stateFile_test stateSchema_test stateStore_test fpsclock_test"
fail=0
for t in $TESTS; do
    printf '%-12s ' "$t"
    rm -rf "$ROOT/tools/hosttest/$t/build"   # 強制重建：避免舊 binary 遮住 header 改動
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
