# hitori 問題デバッグ手順

## ファイル場所

```
benchmarks/minizinc_challenge_2025/mznc2025_probs/hitori/hitori.fzn
```

## sabori_csp で実行

```bash
# 通常実行
./build/src/fzn/fzn_sabori benchmarks/minizinc_challenge_2025/mznc2025_probs/hitori/hitori.fzn

# 全解探索
./build/src/fzn/fzn_sabori -a benchmarks/minizinc_challenge_2025/mznc2025_probs/hitori/hitori.fzn
```

現在の結果: `=====UNSATISFIABLE=====` （0.02秒で返る → 初期伝播で失敗している可能性大）

## Gecode で確認（正解を得る）

```bash
# MiniZinc 経由で Gecode を使用
minizinc --solver gecode -f benchmarks/minizinc_challenge_2025/mznc2025_probs/hitori/hitori.fzn

# 全解探索
minizinc --solver gecode -a -f benchmarks/minizinc_challenge_2025/mznc2025_probs/hitori/hitori.fzn
```

Gecode の結果例:
```
X_INTRODUCED_16_ = array1d(1..25,[true, false, false, false, false, false, true, false, false, true, false, false, false, false, false, false, true, false, true, false, false, false, true, false, false]);
```

## 問題の概要

- hitori.fzn は 5x5 の Hitori パズル
- Gecode では解が見つかるが、sabori_csp では UNSAT を返す
- 0.02秒で UNSAT → 初期伝播 (`initial_propagate`) または制約の `check_initial_consistency` で失敗している可能性

## 関連する制約（調査済み）

hitori.fzn で使用されている制約:
```
200 bool_clause
189 int_eq_reif
 87 array_bool_and
 56 bool_eq
 31 int_lin_eq
 25 bool_not
 25 array_var_int_element
 24 array_var_bool_element
 23 int_ne_reif
 23 int_lin_eq_reif
```

## 既に実施した修正

1. `array_var_int_element` の `rewind_to` バグ修正（`>= save_point` → `> save_point`）
2. `IntEqReifConstraint::on_remove_value` 実装追加
3. `IntNeReifConstraint::on_remove_value` 実装追加

## デバッグ用テストファイル

```bash
# 簡単なテスト（動作する）
./build/src/fzn/fzn_sabori /tmp/test_clause_ne.fzn

# hitori の部分的な再現テスト（これらは動作する）
./build/src/fzn/fzn_sabori /tmp/test_hitori_mini.fzn
./build/src/fzn/fzn_sabori /tmp/test_hitori_mini2.fzn
```

## ビルド方法

```bash
cmake --build build --target fzn_sabori
```

## テスト実行

```bash
# C++ テスト（全パス）
./build/tests/cpp/test_sabori_csp "[constraint]"

# 個別 FlatZinc テスト
./build/src/fzn/fzn_sabori tests/fzn/constraints/int_ne_reif/deduce_true.fzn
./build/src/fzn/fzn_sabori tests/fzn/constraints/bool_clause/simple.fzn
```

## fzn_sabori の出力を Gecode で検証

sabori_csp が出力した解が正しいかどうかを Gecode で検証する方法。

### 方法1: 解を制約として追加して検証

```bash
# 1. sabori_csp で解を得る
./build/src/fzn/fzn_sabori problem.fzn > solution.txt

# 2. 解の出力を FZN 制約に変換するスクリプト
# 例: "x = 5;" → "constraint int_eq(x, 5);"

# 3. 元の FZN に制約を追加して Gecode で実行
# SAT なら解は正しい、UNSAT なら sabori_csp のバグ
```

### 方法2: 手動で検証用 FZN を作成

```bash
# 1. sabori_csp の出力例
#    x = 1;
#    y = 2;

# 2. 検証用 FZN を作成（元の problem.fzn をコピーして編集）
cp problem.fzn verify.fzn

# 3. verify.fzn の変数宣言を固定値に変更
#    var 1..10: x;  →  var 1..1: x;
#    または制約を追加
#    constraint int_eq(x, 1);
#    constraint int_eq(y, 2);

# 4. Gecode で検証
minizinc --solver gecode -f verify.fzn
# SAT (解が出力される) なら正しい
# UNSAT なら sabori_csp が間違った解を出力している
```

### 方法3: fzn-gecode を直接使う

```bash
# fzn-gecode で FlatZinc を直接実行
fzn-gecode problem.fzn

# または minizinc 経由（-f は FlatZinc モード）
minizinc --solver gecode -f problem.fzn
```

### 方法4: 検証用 FZN ファイルを作成してから実行

```bash
# 1. sabori_csp の出力を取得
./build/src/fzn/fzn_sabori problem.fzn > solution.txt

# 2. 検証用ファイルを作成
cp problem.fzn verify.fzn

# 3. solution.txt の内容を制約として verify.fzn に追加
# 例: "x = 1;" を "constraint int_eq(x, 1);" に変換して追記
cat solution.txt | grep -E '^[a-zA-Z_]+ = ' | \
  sed 's/\([a-zA-Z_][a-zA-Z0-9_]*\) = \(.*\);/constraint int_eq(\1, \2);/' >> verify.fzn

# 4. Gecode で検証
fzn-gecode verify.fzn
# または
minizinc --solver gecode -f verify.fzn
```

### 検証例

```bash
# テスト問題で検証
echo "var 1..5: x :: output_var; solve satisfy;" > /tmp/simple.fzn
./build/src/fzn/fzn_sabori /tmp/simple.fzn
# 出力: x = 1;

# 検証用ファイルを作成
cp /tmp/simple.fzn /tmp/verify.fzn
echo "constraint int_eq(x, 1);" >> /tmp/verify.fzn

# Gecode で検証
fzn-gecode /tmp/verify.fzn
# SAT (解が出力される) なら OK
```

## sabori_csp の解を Gecode で検証するスクリプト

sabori_csp が出力した解が元の制約を満たすかどうかを確認する。

### 検証スクリプト (verify_solution.sh)

```bash
#!/bin/bash
# 使い方: ./verify_solution.sh problem.fzn

FZN_FILE="$1"
VERIFY_FILE="/tmp/verify_$$.fzn"

# 1. sabori_csp で解を得る
SOLUTION=$(./build/src/fzn/fzn_sabori "$FZN_FILE" 2>&1)

# UNSAT チェック
if echo "$SOLUTION" | grep -q "UNSATISFIABLE"; then
    echo "sabori_csp returned UNSATISFIABLE"
    echo "Checking with Gecode..."
    fzn-gecode "$FZN_FILE" | head -5
    exit 1
fi

# 2. 元の FZN をコピー
cp "$FZN_FILE" "$VERIFY_FILE"

# 3. 解を制約として追加
# "x = 1;" → "constraint int_eq(x, 1);"
# "arr = array1d(1..3,[1,2,3]);" → そのまま検証は難しいのでスキップ
echo "$SOLUTION" | grep -E '^[a-zA-Z_][a-zA-Z0-9_]* = [0-9-]+;$' | \
    sed 's/\([a-zA-Z_][a-zA-Z0-9_]*\) = \(.*\);/constraint int_eq(\1, \2);/' >> "$VERIFY_FILE"

# bool 変数の処理: "b = true;" → "constraint bool_eq(b, true);"
echo "$SOLUTION" | grep -E '^[a-zA-Z_][a-zA-Z0-9_]* = (true|false);$' | \
    sed 's/\([a-zA-Z_][a-zA-Z0-9_]*\) = \(.*\);/constraint bool_eq(\1, \2);/' >> "$VERIFY_FILE"

# 4. Gecode で検証
echo "=== Verifying with Gecode ==="
GECODE_RESULT=$(fzn-gecode "$VERIFY_FILE" 2>&1)

if echo "$GECODE_RESULT" | grep -q "UNSATISFIABLE"; then
    echo "VERIFICATION FAILED: sabori_csp の解は不正です"
    echo "sabori_csp output:"
    echo "$SOLUTION"
    rm -f "$VERIFY_FILE"
    exit 1
else
    echo "VERIFICATION PASSED: sabori_csp の解は正しいです"
    rm -f "$VERIFY_FILE"
    exit 0
fi
```

### 手動で検証する手順

```bash
# 1. sabori_csp を実行して解を保存
./build/src/fzn/fzn_sabori problem.fzn | tee solution.txt

# 2. 解の内容を確認
cat solution.txt
# 例:
# x = 5;
# y = 3;
# b = true;
# ----------

# 3. 検証用 FZN を作成
cp problem.fzn /tmp/verify.fzn

# 4. 整数変数の値を制約として追加
echo "constraint int_eq(x, 5);" >> /tmp/verify.fzn
echo "constraint int_eq(y, 3);" >> /tmp/verify.fzn

# 5. bool 変数の値を制約として追加
echo "constraint bool_eq(b, true);" >> /tmp/verify.fzn

# 6. Gecode で検証
fzn-gecode /tmp/verify.fzn

# 結果の解釈:
# - 解が出力される → sabori_csp の解は正しい
# - =====UNSATISFIABLE===== → sabori_csp の解は不正（バグ）
```

### 配列変数の検証

配列変数は検証が複雑。個別の要素として制約を追加する:

```bash
# sabori_csp の出力: arr = array1d(1..3,[10,20,30]);

# 検証用制約として追加:
echo "constraint int_eq(arr[1], 10);" >> /tmp/verify.fzn
echo "constraint int_eq(arr[2], 20);" >> /tmp/verify.fzn
echo "constraint int_eq(arr[3], 30);" >> /tmp/verify.fzn
```

## 次のデバッグステップ候補

1. `Solver::solve` の `initial_propagate` にデバッグ出力を追加
2. `check_initial_consistency` で false を返している制約を特定
3. 各制約の `propagate()` で false を返しているものを特定
