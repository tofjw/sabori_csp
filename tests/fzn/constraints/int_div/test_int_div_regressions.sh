#!/bin/bash
# IntDivConstraint regression tests
# (1) 2026-05-01: y が完全に正のとき forward bound 計算で y_min 端点が考慮されない bug

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
SOLVER="$PROJECT_ROOT/build/src/fzn/fzn_sabori"

FAILED=0
PASSED=0
pass() { echo "PASS: $1"; PASSED=$((PASSED + 1)); }
fail() { echo "FAIL: $1"; [ -n "$2" ] && echo "  $2"; FAILED=$((FAILED + 1)); }

# --- Test 1: forward bound with y fully positive ---
# x=814 (固定), y∈[814,8140], z∈[0,10] のとき z=1 (y=814 のとき) が必ず存在する。
# 修正前は z 上限が誤って 0 と計算され、z=1 が伝播段階で除外されていた。
output=$("$SOLVER" -a "$SCRIPT_DIR/forward_y_positive_only.fzn" 2>/dev/null)
n_z1=$(echo "$output" | grep -c "^z = 1;$")
if [ "$n_z1" -eq 1 ]; then
    pass "forward_y_positive_only: z=1 が解集合に含まれる ($n_z1 件)"
else
    fail "forward_y_positive_only: z=1 の解が見つからない (got $n_z1)" "$output"
fi
# 全解数: y=814..8140 で z=0 が 7326 件 + z=1 が 1 件 = 7327 件
n_total=$(echo "$output" | grep -c "^----------$")
if [ "$n_total" -eq 7327 ]; then
    pass "forward_y_positive_only: 全解 7327 件"
else
    fail "forward_y_positive_only: 全解 7327 件 期待、got $n_total"
fi

echo
echo "Results: $PASSED passed, $FAILED failed"
exit $FAILED
