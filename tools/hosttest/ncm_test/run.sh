#!/usr/bin/env bash
# ncm.py 格式標準測試（不需要板子、不需要 IDF）
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
python3 "$ROOT/tools/hosttest/ncm_test/test_ncm.py"
