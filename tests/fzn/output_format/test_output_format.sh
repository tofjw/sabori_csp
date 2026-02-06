#!/bin/bash
# Test FlatZinc output format compliance
# Specifically tests the ========== marker behavior

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
FZN_SABORI="${SCRIPT_DIR}/../../../build/src/fzn/fzn_sabori"

PASS=0
FAIL=0

# Helper function
check_output() {
    local name="$1"
    local has_complete="$2"  # "yes" or "no"
    local output="$3"

    if [ "$has_complete" = "yes" ]; then
        if echo "$output" | grep -q "^==========$"; then
            echo "PASS: $name (has ==========)"
            PASS=$((PASS+1))
        else
            echo "FAIL: $name (expected ========== but not found)"
            FAIL=$((FAIL+1))
        fi
    else
        if echo "$output" | grep -q "^==========$"; then
            echo "FAIL: $name (unexpected ==========)"
            FAIL=$((FAIL+1))
        else
            echo "PASS: $name (no ==========)"
            PASS=$((PASS+1))
        fi
    fi
}

# Test 1: satisfy single solution - no ==========
cat > /tmp/test_sat.fzn << 'EOF'
var 1..3: x :: output_var;
solve satisfy;
EOF
output=$("$FZN_SABORI" /tmp/test_sat.fzn 2>&1)
check_output "satisfy (single solution)" "no" "$output"

# Test 2: satisfy -a (all solutions) - has ==========
output=$("$FZN_SABORI" -a /tmp/test_sat.fzn 2>&1)
check_output "satisfy -a (all solutions)" "yes" "$output"

# Test 3: satisfy -a with only one solution - has ==========
cat > /tmp/test_single.fzn << 'EOF'
var 1..1: x :: output_var;
solve satisfy;
EOF
output=$("$FZN_SABORI" -a /tmp/test_single.fzn 2>&1)
check_output "satisfy -a (single solution exists)" "yes" "$output"

# Test 4: minimize - has ==========
cat > /tmp/test_min.fzn << 'EOF'
var 1..3: x :: output_var;
solve minimize x;
EOF
output=$("$FZN_SABORI" /tmp/test_min.fzn 2>&1)
check_output "minimize" "yes" "$output"

# Test 5: maximize - has ==========
cat > /tmp/test_max.fzn << 'EOF'
var 1..3: x :: output_var;
solve maximize x;
EOF
output=$("$FZN_SABORI" /tmp/test_max.fzn 2>&1)
check_output "maximize" "yes" "$output"

# Test 6: unsatisfiable - no ========== (has =====UNSATISFIABLE=====)
cat > /tmp/test_unsat.fzn << 'EOF'
var 1..1: x;
var 2..2: y;
constraint int_eq(x, y);
solve satisfy;
EOF
output=$("$FZN_SABORI" /tmp/test_unsat.fzn 2>&1)
if echo "$output" | grep -q "=====UNSATISFIABLE====="; then
    echo "PASS: unsatisfiable (has =====UNSATISFIABLE=====)"
    PASS=$((PASS+1))
else
    echo "FAIL: unsatisfiable (expected =====UNSATISFIABLE=====)"
    FAIL=$((FAIL+1))
fi

# Cleanup
rm -f /tmp/test_sat.fzn /tmp/test_single.fzn /tmp/test_min.fzn /tmp/test_max.fzn /tmp/test_unsat.fzn

echo ""
echo "Results: $PASS passed, $FAIL failed"

if [ $FAIL -eq 0 ]; then
    exit 0
else
    exit 1
fi
