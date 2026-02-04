# テスト方法

## ビルド

テストを含めてビルドするには:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## テスト実行

### 全テスト実行

```bash
ctest --test-dir build
```

詳細出力付き:

```bash
ctest --test-dir build --output-on-failure
```

### C++単体テスト

#### 全テスト
```bash
./build/tests/cpp/test_sabori_csp
```

#### タグ指定
```bash
# 制約テスト全体
./build/tests/cpp/test_sabori_csp "[constraint]"

# 特定の制約
./build/tests/cpp/test_sabori_csp "[int_eq]"
./build/tests/cpp/test_sabori_csp "[int_ne]"
./build/tests/cpp/test_sabori_csp "[int_lt]"
./build/tests/cpp/test_sabori_csp "[int_le]"
./build/tests/cpp/test_sabori_csp "[int_eq_reif]"
```

#### 複数タグ
```bash
# int_eq または int_ne
./build/tests/cpp/test_sabori_csp "[int_eq],[int_ne]"
```

#### テスト一覧表示
```bash
./build/tests/cpp/test_sabori_csp --list-tests
```

### FlatZincテスト

#### 単一ファイル実行
```bash
./build/src/fzn/fzn_sabori tests/fzn/constraints/int_eq/simple.fzn
```

#### 全解探索
```bash
./build/src/fzn/fzn_sabori -a tests/fzn/constraints/int_ne/simple.fzn
```

## テスト構成

```
tests/
├── cpp/                    # C++単体テスト (Catch2)
│   └── test_constraints.cpp
└── fzn/                    # FlatZinc統合テスト
    └── constraints/        # 制約ごとのテスト
        ├── int_eq/
        ├── int_lt/
        └── int_ne/
```

## タグ一覧

| タグ | 説明 |
|------|------|
| `[constraint]` | 全制約テスト |
| `[int_eq]` | int_eq制約 (x == y) |
| `[int_ne]` | int_ne制約 (x != y) |
| `[int_lt]` | int_lt制約 (x < y) |
| `[int_le]` | int_le制約 (x <= y) |
| `[int_eq_reif]` | int_eq_reif制約 ((x == y) <-> b) |

## MiniZinc経由のテスト

ビルド時に `build/share/minizinc/` に MiniZinc ソルバー設定ファイルが生成されます。

### 環境変数の設定

```bash
export MZN_SOLVER_PATH=/path/to/sabori_csp/build/share/minizinc/solvers
```

### ソルバーの確認

```bash
minizinc --solvers | grep sabori
```

出力例:
```
  sabori_csp 1.0.0 (io.github.tofjw.sabori_csp, cp)
```

### MiniZincモデルの実行

```bash
# 1解
minizinc --solver sabori_csp model.mzn

# 全解
minizinc --solver sabori_csp -a model.mzn

# FlatZinc出力の確認（デバッグ用）
minizinc --solver sabori_csp -c model.mzn
cat model.fzn
```

### テスト例

```bash
# alldifferent制約のテスト
cat > /tmp/test.mzn << 'EOF'
include "alldifferent.mzn";
var 1..3: x;
var 1..3: y;
var 1..3: z;
constraint alldifferent([x, y, z]);
solve satisfy;
output ["x=\(x), y=\(y), z=\(z)\n"];
EOF

MZN_SOLVER_PATH=$PWD/build/share/minizinc/solvers \
minizinc --solver sabori_csp -a /tmp/test.mzn
```

### ネイティブサポートされるグローバル制約

以下の制約は分解されずにネイティブ実装が使用されます:

| MiniZinc | FlatZinc出力 | sabori_csp内部 |
|----------|--------------|----------------|
| `alldifferent` | `fzn_all_different_int` | `AllDifferentConstraint` |
| `circuit` | `fzn_circuit` | `CircuitConstraint` |

### ファイル構成

```
share/minizinc/
├── solvers/
│   └── sabori_csp.msc.in    # テンプレート（ソース管理）
└── sabori_csp/
    ├── redefinitions.mzn    # リダイレクト定義の読み込み
    ├── fzn_all_different_int.mzn
    └── fzn_circuit.mzn

build/share/minizinc/        # ビルド時に生成
├── solvers/
│   └── sabori_csp.msc       # 絶対パス埋め込み済み
└── sabori_csp/
    └── *.mzn                # コピー
```
