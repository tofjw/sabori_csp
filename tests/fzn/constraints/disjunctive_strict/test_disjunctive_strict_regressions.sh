#!/bin/bash
# DisjunctiveConstraint (strict) regression tests
# (1) 2026-05-08: strict mode で d=0 タスクが他タスクの (sj, sj+dj) に
#     入る配置を許してしまい、最適化問題で false optimum を返していた。
#     原因は on_final_instantiate / on_instantiate / presolve / prepare_propagation
#     のすべてで d=0 ペアが検査されていなかったこと。

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
SOLVER="$PROJECT_ROOT/build/src/fzn/fzn_sabori"

FAILED=0
PASSED=0
pass() { echo "PASS: $1"; PASSED=$((PASSED + 1)); }
fail() { echo "FAIL: $1"; [ -n "$2" ] && echo "  $2"; FAILED=$((FAILED + 1)); }

# --- Test 1: zero-d task strictly inside another task is rejected ---
# s1=5 (d=0), s2=4 (d=2) -> point 5 in (4, 6) -> must be UNSAT under strict
output=$("$SOLVER" "$SCRIPT_DIR/zero_dur_inside.fzn" 2>/dev/null)
if echo "$output" | grep -q "UNSATISFIABLE"; then
    pass "zero_dur_inside: UNSAT (strict が d=0 を内部配置として却下)"
else
    fail "zero_dur_inside: UNSAT 期待だが SAT が返った" "$output"
fi

# --- Test 2: zero-d task at the start of another task is allowed ---
# s1=4 (d=0), s2=4 (d=2) -> point 4 == start -> SAT
output=$("$SOLVER" "$SCRIPT_DIR/zero_dur_at_start.fzn" 2>/dev/null)
if echo "$output" | grep -q "UNSATISFIABLE"; then
    fail "zero_dur_at_start: SAT 期待だが UNSAT (境界での配置を誤って却下)" "$output"
elif echo "$output" | grep -q "^s1 = 4;" && echo "$output" | grep -q "^s2 = 4;"; then
    pass "zero_dur_at_start: SAT (start 境界配置を許容)"
else
    fail "zero_dur_at_start: 期待解 (s1=4, s2=4) と異なる" "$output"
fi

# --- Test 3: zero-d task at the (exclusive) end of another task is allowed ---
# s1=6 (d=0), s2=4 (d=2) -> point 6 == s2+d2 -> SAT
output=$("$SOLVER" "$SCRIPT_DIR/zero_dur_at_end.fzn" 2>/dev/null)
if echo "$output" | grep -q "UNSATISFIABLE"; then
    fail "zero_dur_at_end: SAT 期待だが UNSAT (end 境界配置を誤って却下)" "$output"
elif echo "$output" | grep -q "^s1 = 6;" && echo "$output" | grep -q "^s2 = 4;"; then
    pass "zero_dur_at_end: SAT (end 境界配置を許容)"
else
    fail "zero_dur_at_end: 期待解 (s1=6, s2=4) と異なる" "$output"
fi

# --- Test 4: optimization regression (original bug repro, optimal=7) ---
# Buggy 版は obj=6 を返した。Gecode/Chuffed と一致する正解は obj=7。
output=$("$SOLVER" -t 30 "$SCRIPT_DIR/zero_dur_minimize.fzn" 2>/dev/null)
obj_value=$(echo "$output" | grep "^obj = " | tail -1 | sed 's/obj = //; s/;//')
if [ "$obj_value" = "7" ]; then
    pass "zero_dur_minimize: obj=7 (Gecode/Chuffed と一致)"
elif [ "$obj_value" = "6" ]; then
    fail "zero_dur_minimize: obj=6 (元のバグが再発)" "$output"
else
    fail "zero_dur_minimize: obj=7 期待、got obj=$obj_value" "$output"
fi

# --- Test 5: non-strict variant still allows zero-d inside (sanity) ---
# 修正が strict だけに影響することを確認するため non-strict 版で SAT を期待。
output=$("$SOLVER" "$SCRIPT_DIR/zero_dur_nonstrict_unaffected.fzn" 2>/dev/null)
if echo "$output" | grep -q "UNSATISFIABLE"; then
    fail "zero_dur_nonstrict_unaffected: 非 strict 版で SAT 期待だが UNSAT" "$output"
else
    pass "zero_dur_nonstrict_unaffected: 非 strict 版は内部配置を引き続き許容"
fi

echo
echo "Results: $PASSED passed, $FAILED failed"
exit $FAILED
