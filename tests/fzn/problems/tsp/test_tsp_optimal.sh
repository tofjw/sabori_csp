#!/bin/bash
# TSP最適コスト検証テスト
# 最適コストが期待値と一致するか確認する

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
SOLVER="$PROJECT_ROOT/build/src/fzn/fzn_sabori"

# 期待される最適コスト
declare -A EXPECTED_COSTS
EXPECTED_COSTS["tsp_small"]=98
EXPECTED_COSTS["tsp_medium"]=219
EXPECTED_COSTS["tsp_large"]=220

FAILED=0
PASSED=0

for instance in tsp_small tsp_medium tsp_large; do
    fzn_file="$PROJECT_ROOT/benchmarks/tsp/${instance}.fzn"
    expected_cost="${EXPECTED_COSTS[$instance]}"

    if [ ! -f "$fzn_file" ]; then
        echo "SKIP: $instance (file not found)"
        continue
    fi

    # ソルバーを実行して total_cost を抽出
    output=$("$SOLVER" "$fzn_file" 2>&1)
    actual_cost=$(echo "$output" | grep "total_cost = " | sed 's/total_cost = \([0-9]*\);/\1/')

    if [ -z "$actual_cost" ]; then
        echo "FAIL: $instance (total_cost not found in output)"
        echo "  Output: $output"
        FAILED=$((FAILED + 1))
    elif [ "$actual_cost" -eq "$expected_cost" ]; then
        echo "PASS: $instance (optimal cost = $actual_cost)"
        PASSED=$((PASSED + 1))
    else
        echo "FAIL: $instance (expected $expected_cost, got $actual_cost)"
        FAILED=$((FAILED + 1))
    fi
done

echo ""
echo "Results: $PASSED passed, $FAILED failed"

if [ $FAILED -gt 0 ]; then
    exit 1
fi
exit 0
