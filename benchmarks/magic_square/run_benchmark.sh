#!/bin/bash
# Magic Square Benchmark Runner for sabori_csp
#
# Usage: ./run_benchmark.sh [small|medium|large|all] [--count-all]

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
if [ ! -f "$SCRIPT_DIR/magic_square_small.fzn" ]; then
    echo "Generating FlatZinc files..."
    python3 "$SCRIPT_DIR/generate_magic_square.py"
    echo ""
fi

# Parse arguments
SIZE="${1:-all}"
COUNT_ALL="${2:-}"

# Function to run benchmark
run_benchmark() {
    local name=$1
    local fzn_file="$SCRIPT_DIR/magic_square_${name}.fzn"

    if [ ! -f "$fzn_file" ]; then
        echo "Skipping $name: file not found"
        return
    fi

    echo "=== Magic Square $name ==="
    echo "File: $fzn_file"

    # Run with timeout and measure time
    local start_time=$(date +%s.%N)

    if [ "$COUNT_ALL" = "--count-all" ]; then
        # Count all solutions
        if timeout 120 "$SOLVER" -a "$fzn_file" > /tmp/magic_result.txt 2>&1; then
            local end_time=$(date +%s.%N)
            local elapsed=$(echo "$end_time - $start_time" | bc)

            # Count solutions
            local num_solutions=$(grep -c "^----------$" /tmp/magic_result.txt || echo "0")

            echo "Status: SAT"
            echo "Solutions found: $num_solutions"
            echo "Time: ${elapsed}s"
        else
            local end_time=$(date +%s.%N)
            local elapsed=$(echo "$end_time - $start_time" | bc)
            echo "Status: TIMEOUT or ERROR"
            echo "Time: ${elapsed}s"
        fi
    else
        # Find first solution
        if timeout 60 "$SOLVER" "$fzn_file" > /tmp/magic_result.txt 2>&1; then
            local end_time=$(date +%s.%N)
            local elapsed=$(echo "$end_time - $start_time" | bc)

            echo "Status: SAT"
            echo "Time: ${elapsed}s"
            echo "Solution:"

            # Extract and display grid
            # Get the order n from the filename
            local n
            case "$name" in
                small) n=3 ;;
                medium) n=4 ;;
                large) n=5 ;;
            esac

            for i in $(seq 0 $((n-1))); do
                local row=""
                for j in $(seq 0 $((n-1))); do
                    local val=$(grep "x_${i}_${j} = " /tmp/magic_result.txt | sed 's/.*= //' | sed 's/;$//')
                    row="$row $(printf '%3s' $val)"
                done
                echo "  $row"
            done
        else
            local end_time=$(date +%s.%N)
            local elapsed=$(echo "$end_time - $start_time" | bc)

            if grep -q "UNSATISFIABLE" /tmp/magic_result.txt 2>/dev/null; then
                echo "Status: UNSAT"
            else
                echo "Status: TIMEOUT or ERROR"
            fi
            echo "Time: ${elapsed}s"
        fi
    fi

    echo ""
}

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
        echo "Usage: $0 [small|medium|large|all] [--count-all]"
        echo ""
        echo "Options:"
        echo "  --count-all  Count all solutions (may take longer)"
        exit 1
        ;;
esac

echo "=== Summary ==="
echo "Solver: $SOLVER"
echo "Benchmarks completed."
