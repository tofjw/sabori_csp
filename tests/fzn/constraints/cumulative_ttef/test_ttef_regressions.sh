#!/bin/bash
# TTEFPropagator regression tests
# (1) 2026-05-07: forward_pass / backward_pass で j を動かす際のエネルギー予算が
#     slack のみで計算されていた。実際は slack + j_mandatory_in_LR が正しい。
#     aircraft-disassembly で false UNSAT (=最適誤宣言) を引き起こしていた。

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
SOLVER="$PROJECT_ROOT/build/src/fzn/fzn_sabori"

FAILED=0
PASSED=0
pass() { echo "PASS: $1"; PASSED=$((PASSED + 1)); }
fail() { echo "FAIL: $1"; [ -n "$2" ] && echo "  $2"; FAILED=$((FAILED + 1)); }

# --- Test 1: j_mandatory budget bug ---
# Buggy 版: TTEF が sA >= 6 を導出し、sA <= 5 制約と矛盾して false UNSAT。
# 修正後  : sA は [2, 5] の範囲で SAT。
output=$("$SOLVER" "$SCRIPT_DIR/j_mandatory_budget.fzn" 2>/dev/null)

if echo "$output" | grep -q "UNSATISFIABLE"; then
    fail "j_mandatory_budget: false UNSAT (TTEF が j_mandatory を予算に戻していない)" "$output"
elif echo "$output" | grep -q "^sA = "; then
    sa_value=$(echo "$output" | grep "^sA = " | sed 's/sA = //; s/;//')
    if [ "$sa_value" -ge 2 ] && [ "$sa_value" -le 5 ]; then
        pass "j_mandatory_budget: sA=$sa_value ∈ [2, 5]"
    else
        fail "j_mandatory_budget: sA=$sa_value が範囲外 ([2, 5] 期待)" "$output"
    fi
else
    fail "j_mandatory_budget: 解が見つからない" "$output"
fi

echo
echo "Results: $PASSED passed, $FAILED failed"
exit $FAILED
