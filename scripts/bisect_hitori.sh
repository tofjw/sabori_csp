#!/bin/bash
# hitori リグレッション bisect スクリプト
# 57beed7 (good) から HEAD までの各コミットで fzn_sabori を実行し、
# 部分解が得られるか確認する。

set -e

GOOD_COMMIT="57beed785ff54698dc1f12ba66805d092a07e4ff"
BAD_COMMIT="HEAD"
FZN_FILE="benchmarks/minizinc_challenge_2025/mznc2025_probs/hitori/hitori.fzn"
TIMEOUT=120
BUILD_DIR="build_bisect"

# リポジトリのルートに移動
cd "$(git rev-parse --show-toplevel)"

# コミット一覧を取得（古い順）
commits=("$GOOD_COMMIT")
while IFS= read -r line; do
    commits+=("$line")
done < <(git rev-list --reverse "${GOOD_COMMIT}..${BAD_COMMIT}")

echo "=== hitori リグレッション bisect ==="
echo "テスト対象コミット数: ${#commits[@]}"
echo "FZNファイル: $FZN_FILE"
echo "タイムアウト: ${TIMEOUT}s"
echo ""

# 現在のブランチを保存
original_branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || git rev-parse HEAD)

cleanup() {
    echo ""
    echo "=== クリーンアップ: 元のブランチに戻します ==="
    git checkout "$original_branch" 2>/dev/null || git checkout -
}
trap cleanup EXIT

for commit in "${commits[@]}"; do
    short=$(git rev-parse --short "$commit")
    msg=$(git log --oneline -1 "$commit")
    echo "=== テスト: $msg ==="

    # チェックアウト
    git checkout "$commit" --quiet 2>/dev/null

    # ビルド
    echo "  ビルド中..."
    cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-O2" > /dev/null 2>&1
    cmake --build "$BUILD_DIR" -j$(nproc) > /dev/null 2>&1

    if [ ! -x "$BUILD_DIR/src/fzn/fzn_sabori" ]; then
        echo "  ❌ ビルド失敗"
        echo ""
        continue
    fi

    # 実行
    echo "  実行中 (timeout=${TIMEOUT}s)..."
    output=$("./$BUILD_DIR/src/fzn/fzn_sabori" -a -t "$TIMEOUT" -s -v "$FZN_FILE" 2>&1) || true

    # 部分解の判定: "----------" が出力に含まれていれば解が得られている
    solution_count=$(echo "$output" | grep -c "^----------$" || true)

    if [ "$solution_count" -gt 0 ]; then
        echo "  ✅ GOOD: ${solution_count} 個の解が得られた"
    else
        echo "  ❌ BAD: 解なし"
        # 出力の最後の数行を表示
        echo "  出力 (末尾5行):"
        echo "$output" | tail -5 | sed 's/^/    /'
    fi
    echo ""
done

echo "=== bisect 完了 ==="
