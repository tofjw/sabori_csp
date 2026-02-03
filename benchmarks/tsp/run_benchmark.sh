#!/bin/bash
# TSP Benchmark Runner for sabori_csp
#
# Usage: ./run_benchmark.sh [small|medium|large|all]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SOLVER="$PROJECT_ROOT/build/src/fzn/fzn_sabori"

# Check if solver exists
if [ ! -f "$SOLVER" ]; then
    echo "Error: Solver not found at $SOLVER"
    echo "Please build the project first: cmake --build build"
    exit 1
fi

# Generate FlatZinc files if not exist
if [ ! -f "$SCRIPT_DIR/tsp_small.fzn" ]; then
    echo "Generating FlatZinc files..."
    python3 "$SCRIPT_DIR/generate_tsp.py"
    echo ""
fi

# Function to run benchmark
run_benchmark() {
    local name=$1
    local fzn_file="$SCRIPT_DIR/tsp_${name}.fzn"

    if [ ! -f "$fzn_file" ]; then
        echo "Skipping $name: file not found"
        return
    fi

    echo "=== TSP $name ==="
    echo "File: $fzn_file"

    # Run with timeout and measure time
    local start_time=$(date +%s.%N)
    local result

    if timeout 60 "$SOLVER" "$fzn_file" > /tmp/tsp_result.txt 2>&1; then
        local end_time=$(date +%s.%N)
        local elapsed=$(echo "$end_time - $start_time" | bc)

        # Extract solution info
        local total_cost=$(grep "total_cost" /tmp/tsp_result.txt | head -1 | sed 's/.*= //' | sed 's/;$//')

        echo "Status: SAT"
        echo "Total cost: $total_cost"
        echo "Time: ${elapsed}s"

        # Show tour
        echo "Tour: "
        grep "next_" /tmp/tsp_result.txt | head -10
    else
        local end_time=$(date +%s.%N)
        local elapsed=$(echo "$end_time - $start_time" | bc)

        if grep -q "UNSATISFIABLE" /tmp/tsp_result.txt 2>/dev/null; then
            echo "Status: UNSAT"
        else
            echo "Status: TIMEOUT or ERROR"
        fi
        echo "Time: ${elapsed}s"
    fi

    echo ""
}

# Parse arguments
SIZE="${1:-all}"

case "$SIZE" in
    small|medium|large)
        run_benchmark "$SIZE"
        ;;
    all)
        run_benchmark "small"
        run_benchmark "medium"
        run_benchmark "large"
        ;;
    *)
        echo "Usage: $0 [small|medium|large|all]"
        exit 1
        ;;
esac

echo "=== Summary ==="
echo "Solver: $SOLVER"
echo "Benchmarks completed."
