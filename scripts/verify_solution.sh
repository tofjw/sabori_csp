#!/bin/bash
# sabori_csp の解を Gecode で検証するスクリプト
# 使い方: ./scripts/verify_solution.sh problem.fzn

set -e

if [ $# -lt 1 ]; then
    echo "Usage: $0 <problem.fzn>"
    exit 1
fi

FZN_FILE="$1"
VERIFY_FILE="/tmp/verify_$$.fzn"
CONSTRAINTS_FILE="/tmp/constraints_$$.fzn"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
FZN_SABORI="$PROJECT_DIR/build/src/fzn/fzn_sabori"

if [ ! -f "$FZN_FILE" ]; then
    echo "Error: File not found: $FZN_FILE"
    exit 1
fi

if [ ! -x "$FZN_SABORI" ]; then
    echo "Error: fzn_sabori not found. Run: cmake --build build --target fzn_sabori"
    exit 1
fi

# 1. sabori_csp で解を得る
echo "=== Running sabori_csp ==="
SOLUTION=$("$FZN_SABORI" "$FZN_FILE" 2>&1)
echo "$SOLUTION"

# UNSAT チェック
if echo "$SOLUTION" | grep -qF "UNSATISFIABLE"; then
    echo ""
    echo "=== sabori_csp returned UNSATISFIABLE ==="
    echo "=== Checking with Gecode ==="
    GECODE_RESULT=$(fzn-gecode "$FZN_FILE" 2>&1 | head -10)
    echo "$GECODE_RESULT"

    if echo "$GECODE_RESULT" | grep -qF "UNSATISFIABLE"; then
        echo ""
        echo "RESULT: Both solvers agree - problem is UNSATISFIABLE"
        exit 0
    else
        echo ""
        echo "RESULT: BUG DETECTED - Gecode finds a solution but sabori_csp says UNSAT"
        exit 1
    fi
fi

# 解がない場合（エラー等）
if ! printf '%s\n' "$SOLUTION" | grep -qF -- "----------"; then
    echo "Error: No solution output from sabori_csp"
    exit 1
fi

# 2. 解を制約として生成
> "$CONSTRAINTS_FILE"

# 各行を処理（変数名 = 値; の形式のみ抽出）
printf '%s\n' "$SOLUTION" | grep -E '^[a-zA-Z_][a-zA-Z0-9_]* = .*; *$' | while read -r line; do
    # "var = value;" 形式をパース
    var_name=$(echo "$line" | sed 's/ = .*//')
    value=$(echo "$line" | sed 's/.* = //; s/;$//')

    # true/false の場合は bool_eq
    if [ "$value" = "true" ] || [ "$value" = "false" ]; then
        echo "constraint bool_eq($var_name, $value);" >> "$CONSTRAINTS_FILE"
    # 整数値の場合は int_eq（数字とオプションの先頭マイナスのみ）
    elif printf '%s' "$value" | grep -qE '^-?[0-9]+$'; then
        echo "constraint int_eq($var_name, $value);" >> "$CONSTRAINTS_FILE"
    fi
    # それ以外（配列など）はスキップ
done

# 3. 検証用 FZN を作成（solve 行の前に制約を挿入）
SOLVE_LINE=$(grep -E '^solve ' "$FZN_FILE")
grep -v -E '^solve ' "$FZN_FILE" > "$VERIFY_FILE"
cat "$CONSTRAINTS_FILE" >> "$VERIFY_FILE"
echo "$SOLVE_LINE" >> "$VERIFY_FILE"

# 追加された制約を表示
echo ""
echo "=== Added constraints for verification ==="
cat "$CONSTRAINTS_FILE" | head -10
CONSTRAINT_COUNT=$(wc -l < "$CONSTRAINTS_FILE")
echo "(Total: $CONSTRAINT_COUNT constraints)"

# 4. Gecode で検証
echo ""
echo "=== Verifying with Gecode ==="
GECODE_RESULT=$(fzn-gecode "$VERIFY_FILE" 2>&1)

if echo "$GECODE_RESULT" | grep -qF "UNSATISFIABLE"; then
    echo "VERIFICATION FAILED: sabori_csp の解は不正です"
    echo ""
    echo "Gecode output:"
    echo "$GECODE_RESULT"
    rm -f "$VERIFY_FILE" "$CONSTRAINTS_FILE"
    exit 1
else
    echo "VERIFICATION PASSED: sabori_csp の解は正しいです"
    rm -f "$VERIFY_FILE" "$CONSTRAINTS_FILE"
    exit 0
fi
