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

#### ctest 経由（統合テスト）
FlatZinc 統合テストは `tests/CMakeLists.txt` で `add_test` 登録され、ctest から実行される。
```bash
ctest --test-dir build -L fzn          # FlatZinc ラベルのテストのみ
ctest --test-dir build -R int_div      # 名前で絞り込み
```

### Python テスト

pybind11 バインディングのテスト。`tests/python/` に配置。

拡張 `_sabori_csp` は `python/sabori_csp/` にビルドされるため、`PYTHONPATH=python` を指定する。
```bash
PYTHONPATH=python pytest tests/python/                   # 全 Python テスト
PYTHONPATH=python pytest tests/python/test_globals.py    # 特定ファイル
```

事前に `cmake --build build` で `_sabori_csp` 拡張をビルドしておくこと。

### ゴールデンマスター（リファクタ検証）

純リファクタで挙動が変わっていないことを機械判定する。代表 fzn の `-a -s` 出力
（全解 + 決定論的統計、timing は除外）の指紋を `tests/golden/expected/` に記録し、前後比較する。

```bash
bash tests/golden/run_golden.sh check   # 期待値と一致するか検証（既定。不一致なら exit 1）
bash tests/golden/run_golden.sh record  # 現行バイナリで期待値を再生成（意図的な変更後のみ）
bash tests/golden/run_golden.sh list    # 対象 fzn コーパスを再構築
```

注意: `record` は「現状の挙動を正」として固定する。バグ修正・仕様変更で出力が変わった場合のみ、
差分を目視確認してから再生成すること。

ctest にも `golden_master`（ラベル `golden`）として登録済み:
```bash
ctest --test-dir build -L golden
```

## テスト構成

```
tests/
├── cpp/                    # C++単体テスト (Catch2)
├── python/                 # Python バインディングテスト (pytest)
├── golden/                 # ゴールデンマスター (run_golden.sh + corpus + expected/)
└── fzn/                    # FlatZinc統合テスト
    ├── constraints/        # 制約ごとのテスト (.fzn + .expected)
    ├── output_format/      # 出力形式テスト
    └── problems/           # 問題テスト (tsp 等)
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

ビルド時に `build/share/minizinc/` に MiniZinc ソルバー設定ファイル（`.msc`）と
mznlib が生成されます。**開発・ベンチマーク中はこの build 側を使うこと**
（配布・常用は `cmake --install` で入れたツリー側。使い分けは後述の「ファイル構成」参照）。

**`~/.minizinc/` へのコピーは作らないこと。** build 側の `.msc` は絶対パスなので
コピーしても mznlib は読まれず、古いコピーが名前解決で本物を隠すだけになります。

### ソルバー設定の指定

`.msc` を絶対パスで指定するのが確実です。

```bash
MSC=/path/to/sabori_csp/build/share/minizinc/solvers/sabori_csp.msc
```

`--solver sabori_csp` と名前で指定することもできますが、その場合は
`MZN_SOLVER_PATH` を build 側に向けておく必要があります。

```bash
export MZN_SOLVER_PATH=/path/to/sabori_csp/build/share/minizinc/solvers
```

名前解決は「どの `.msc` を掴んだか」が見えず、別の mznlib を指す同名設定があると
ネイティブ実装が**エラーなく std 分解へ落ちます**（実例と対処は
[ベンチマーク README](../benchmarks/minizinc_challenge/README.md) 参照）。

### ソルバーの確認

```bash
minizinc --solvers | grep sabori
```

出力例:
```
  sabori_csp 1.0.0 (io.github.tofjw.sabori_csp, cp)
```

`id` が `io.github.tofjw.sabori_csp` であることを確認してください。

### MiniZincモデルの実行

```bash
# 1解
minizinc --solver "$MSC" model.mzn

# 全解
minizinc --solver "$MSC" -a model.mzn

# 最適化問題（-i を付けないと、タイムアウト時に解を見つけていても UNKNOWN になる）
minizinc --solver "$MSC" -i --time-limit 30000 model.mzn

# FlatZinc出力の確認（デバッグ用）
minizinc --solver "$MSC" -c model.mzn --fzn model.fzn
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

minizinc --solver $PWD/build/share/minizinc/solvers/sabori_csp.msc -a /tmp/test.mzn
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
│   └── sabori_csp.msc.in    # .msc のテンプレート（JSON の正本はここ 1 箇所）
└── sabori_csp/              # mznlib の正本
    ├── redefinitions.mzn    # リダイレクト定義の読み込み
    ├── fzn_all_different_int.mzn
    └── fzn_circuit.mzn

build/share/minizinc/        # ビルド時に生成（開発用）
├── solvers/
│   └── sabori_csp.msc       # build ツリーを絶対パスで指す
└── sabori_csp/
    └── *.mzn                # 毎ビルド clean copy（差分コピーではない）

<prefix>/                    # cmake --install で配置
├── bin/fzn_sabori
└── share/minizinc/
    ├── solvers/sabori_csp.msc   # 相対パス（../../../bin/..., ../sabori_csp）
    └── sabori_csp/*.mzn
```

`.msc` は build ツリー用とインストール用の 2 つが生成される。どちらも
`share/minizinc/solvers/sabori_csp.msc.in` を `configure_file` したもので、
差し替わるのは `@SABORI_MSC_EXECUTABLE@` / `@SABORI_MSC_MZNLIB@` の 2 箇所だけ
（`src/fzn/CMakeLists.txt`）。JSON のフィールドを増やすときはテンプレートを直す。

build ツリー用は絶対パスなので `~/.minizinc/` にコピーしても意味がない
（mznlib 側は読まれず、古いコピーが名前解決で本物を隠すだけ）。開発中は
`MZN_SOLVER_PATH` か `--solver <.msc の絶対パス>` を使う。配布・常用には
`cmake --install build --prefix ~/.local` で入れたツリー側を使う。こちらの `.msc` は
相対パスなので、ツリーごと移動しても prefix を変えても動く（単体でのコピーは不可、
シンボリックリンクは可）。
