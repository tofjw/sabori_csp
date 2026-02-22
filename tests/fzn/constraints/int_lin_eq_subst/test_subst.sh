#!/bin/bash
# int_lin_eq substitution tests
# Tests variable elimination from 2-variable int_lin_eq constraints

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
SOLVER="$PROJECT_ROOT/build/src/fzn/fzn_sabori"

FAILED=0
PASSED=0

pass() { echo "PASS: $1"; PASSED=$((PASSED + 1)); }
fail() { echo "FAIL: $1"; echo "  $2"; FAILED=$((FAILED + 1)); }

# --- Test 1: basic substitution produces a valid solution ---
output=$("$SOLVER" "$SCRIPT_DIR/basic.fzn" 2>/dev/null)
if echo "$output" | grep -q "^----------$"; then
    pass "basic: finds a solution"
else
    fail "basic: should find a solution" "$output"
fi

# --- Test 2: all solutions are correct after substitution ---
# z = 5-y, z != x => x+y != 5. x,y in 1..3 => 7 solutions
output=$("$SOLVER" -a "$SCRIPT_DIR/all_solutions.fzn" 2>/dev/null)
nsol=$(echo "$output" | grep -c "^----------$")
if [ "$nsol" -eq 7 ]; then
    pass "all_solutions: 7 solutions (got $nsol)"
else
    fail "all_solutions: expected 7 solutions, got $nsol" "$output"
fi

# --- Test 3: UNSAT from contradictory int_lin_eq ---
output=$("$SOLVER" "$SCRIPT_DIR/unsat_eq.fzn" 2>&1)
if echo "$output" | grep -q "UNSATISFIABLE\|UNSAT"; then
    pass "unsat_eq: detected UNSAT"
else
    fail "unsat_eq: should be UNSAT" "$output"
fi

# --- Test 4: UNSAT from int_lin_le after substitution ---
output=$("$SOLVER" "$SCRIPT_DIR/unsat_le.fzn" 2>&1)
if echo "$output" | grep -q "UNSATISFIABLE\|UNSAT"; then
    pass "unsat_le: detected UNSAT"
else
    fail "unsat_le: should be UNSAT" "$output"
fi

# --- Test 5: reif degenerates to b=true ---
output=$("$SOLVER" "$SCRIPT_DIR/reif_true.fzn" 2>/dev/null)
if echo "$output" | grep -qE "b = (1|true)"; then
    pass "reif_true: b = true"
else
    fail "reif_true: expected b = true" "$output"
fi

# --- Test 6: reif degenerates to b=false ---
output=$("$SOLVER" "$SCRIPT_DIR/reif_false.fzn" 2>/dev/null)
if echo "$output" | grep -qE "b = (0|false)"; then
    pass "reif_false: b = false"
else
    fail "reif_false: expected b = false" "$output"
fi

# --- Test 7: negative unit coefficient ---
output=$("$SOLVER" "$SCRIPT_DIR/neg_coeff.fzn" 2>/dev/null)
if echo "$output" | grep -q "^----------$"; then
    # Verify the solution satisfies original constraints
    x_val=$(echo "$output" | grep "^x = " | sed 's/x = \([0-9]*\);/\1/')
    y_val=$(echo "$output" | grep "^y = " | sed 's/y = \([0-9]*\);/\1/')
    z_val=$((2 * y_val - 6))
    sum=$((z_val + x_val))
    if [ "$sum" -le 10 ] && [ "$z_val" -ge 1 ] && [ "$z_val" -le 10 ]; then
        pass "neg_coeff: valid solution (x=$x_val, y=$y_val, z=$z_val)"
    else
        fail "neg_coeff: invalid solution (x=$x_val, y=$y_val, z=$z_val, z+x=$sum)" "$output"
    fi
else
    fail "neg_coeff: should find a solution" "$output"
fi

# --- Test 8: output var is protected from elimination ---
output=$("$SOLVER" -v "$SCRIPT_DIR/output_protected.fzn" 2>&1)
stderr=$(echo "$output" | grep "substitution")
stdout=$("$SOLVER" "$SCRIPT_DIR/output_protected.fzn" 2>/dev/null)
if echo "$stdout" | grep -q "^z = "; then
    pass "output_protected: z appears in output"
else
    fail "output_protected: z should appear in output (it's output_var)" "$stdout"
fi

echo ""
echo "Results: $PASSED passed, $FAILED failed"
[ $FAILED -eq 0 ] || exit 1
