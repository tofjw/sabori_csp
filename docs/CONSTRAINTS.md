# サポート制約一覧

## 制約グループ

### 比較制約 (comparison)

| 制約名 | クラス | 説明 |
|--------|--------|------|
| `int_eq` | `IntEqConstraint` | x == y |
| `int_eq_reif` | `IntEqReifConstraint` | (x == y) <-> b |
| `int_ne` | `IntNeConstraint` | x != y |
| `int_lt` | `IntLtConstraint` | x < y |
| `int_le` | `IntLeConstraint` | x <= y |
| `int_le_reif` | `IntLeReifConstraint` | (x <= y) <-> b |
| `int_max` | `IntMaxConstraint` | max(x, y) = m |

### 算術制約 (arithmetic)

| 制約名 | クラス | 説明 |
|--------|--------|------|
| `int_times` | `IntTimesConstraint` | x * y = z |

#### int_times 制約

乗算制約 `x * y = z`。

**引数:**
- `x`: 被乗数
- `y`: 乗数
- `z`: 積

**伝播ロジック:**
- bounds propagation: z の範囲を x * y の可能な範囲で制限
- x または y が 0 の場合: z = 0
- 2変数確定時: 残りの1変数を計算（割り切れる場合のみ）

**例:**
```cpp
auto x = make_var("x", 1, 5);
auto y = make_var("y", 1, 5);
auto z = make_var("z", 1, 25);
IntTimesConstraint c(x, y, z);

// x=3, y=4 の場合 → z=12
```

### Bool 制約 (int 制約のエイリアス)

Bool 変数は 0-1 整数変数として扱われます。以下の bool 制約は対応する int 制約のエイリアスです。

| 制約名 | エイリアス先 | 説明 |
|--------|-------------|------|
| `bool2int` | `IntEqConstraint` | b <-> i (bool を int に変換) |
| `bool_eq` | `IntEqConstraint` | a == b |
| `bool_ne` | `IntNeConstraint` | a != b |
| `bool_lt` | `IntLtConstraint` | a < b |
| `bool_le` | `IntLeConstraint` | a <= b (a implies b) |
| `bool_eq_reif` | `IntEqReifConstraint` | (a == b) <-> r |
| `bool_le_reif` | `IntLeReifConstraint` | (a <= b) <-> r |
| `bool_lin_eq` | `IntLinEqConstraint` | Σ(coeffs[i] * vars[i]) == sum |
| `bool_lin_le` | `IntLinLeConstraint` | Σ(coeffs[i] * vars[i]) <= bound |

### 論理制約 (logical)

| 制約名 | クラス | 説明 |
|--------|--------|------|
| `array_bool_and` | `ArrayBoolAndConstraint` | r = b1 ∧ b2 ∧ ... ∧ bn |
| `array_bool_or` | `ArrayBoolOrConstraint` | r = b1 ∨ b2 ∨ ... ∨ bn |
| `bool_clause` | `BoolClauseConstraint` | SAT節: ∨pos[i] ∨ ∨¬neg[j] |

#### array_bool_and / array_bool_or 制約

配列の bool 変数に対する論理演算制約。2-watched literal (2WL) を使用して効率的に伝播を行う。

**伝播ロジック (array_bool_and):**
- r = 1 のとき: 全ての bi = 1 に確定
- bi = 0 が1つでも確定したとき: r = 0 に確定
- r = 0 で残り1つの未確定 bi があり、他が全て 1 のとき: その bi = 0 に確定

**2-watched literal の仕組み:**
- r = 0 のとき、「0 になりうる変数」を2つ監視
- 監視変数の1つが 1 に確定したら、別の候補に監視を移動
- 移動先がなく、もう一方も 1 に確定していたら矛盾
- 移動先がなく、もう一方が未確定なら、それを 0 に確定

**例:**
```cpp
// r = b1 ∧ b2 ∧ b3
auto b1 = make_var("b1", 0, 1);
auto b2 = make_var("b2", 0, 1);
auto b3 = make_var("b3", 0, 1);
auto r = make_var("r", 0, 1);
ArrayBoolAndConstraint c({b1, b2, b3}, r);

// r = 1 を設定すると、全ての bi = 1 が導かれる
// bi のいずれかが 0 になると、r = 0 が導かれる
```

#### bool_clause 制約

SAT節を表す制約。`∨(pos[i]) ∨ ∨(¬neg[j])` の形式。

**引数:**
- `pos`: 正リテラル（これらのいずれかが 1 なら節は充足）
- `neg`: 負リテラル（これらのいずれかが 0 なら節は充足）

**充足条件:**
- いずれかの `pos[i] = 1` または いずれかの `neg[j] = 0`
- 空の節（両方とも空）は常に矛盾（UNSAT）

**2-watched literal の仕組み:**
- 節を充足しうるリテラルを2つ監視
- 監視リテラルが充足不能になったら別の候補に移動
- 残り1つの未確定リテラルのみなら unit propagation を実行
- 両方の監視リテラルが充足不能になったら矛盾

**例:**
```cpp
// (p1 ∨ p2 ∨ ¬n1): pos={p1, p2}, neg={n1}
auto p1 = make_var("p1", 0, 1);
auto p2 = make_var("p2", 0, 1);
auto n1 = make_var("n1", 0, 1);
BoolClauseConstraint c({p1, p2}, {n1});

// p1=0, p2=0, n1=1 → 矛盾
// p1=0, p2=0, n1=? → n1=0 に確定 (unit propagation)
```

### グローバル制約 (global)

| 制約名 | クラス | 説明 |
|--------|--------|------|
| `all_different` | `AllDifferentConstraint` | 全ての変数が異なる値を取る |
| `int_lin_eq` | `IntLinEqConstraint` | Σ(coeffs[i] * vars[i]) == target |
| `int_lin_eq_reif` | `IntLinEqReifConstraint` | Σ(coeffs[i] * vars[i]) == target <-> b |
| `int_lin_le` | `IntLinLeConstraint` | Σ(coeffs[i] * vars[i]) <= bound |
| `int_lin_le_imp` | `IntLinLeImpConstraint` | b = 1 -> Σ(coeffs[i] * vars[i]) <= bound |
| `int_lin_le_reif` | `IntLinLeReifConstraint` | Σ(coeffs[i] * vars[i]) <= bound <-> b |
| `int_lin_ne` | `IntLinNeConstraint` | Σ(coeffs[i] * vars[i]) != target |
| `int_lin_ne_reif` | `IntLinNeReifConstraint` | Σ(coeffs[i] * vars[i]) != target <-> b |
| `circuit` | `CircuitConstraint` | 変数がハミルトン閉路を形成する |
| `int_element` | `IntElementConstraint` | array[index] = result を維持する |
| `array_int_maximum` | `ArrayIntMaximumConstraint` | m = max(x[0], ..., x[n-1]) |
| `array_int_minimum` | `ArrayIntMinimumConstraint` | m = min(x[0], ..., x[n-1]) |

#### circuit 制約

変数 `x[0], x[1], ..., x[n-1]` がハミルトン閉路を形成する制約。
`x[i] = j` は「ノード i の次はノード j」を意味する。

**特徴:**
- Union-Find スタイルでパスを管理し、サブサーキット（部分閉路）を早期検出
- AllDifferent の性質を内包（各ノードの入次数は最大1）
- Sparse Set による値プール管理で効率的な重複チェック

**例:** n=3 の場合、有効な解は以下の2つ:
- `x = [1, 2, 0]` → 0 → 1 → 2 → 0
- `x = [2, 0, 1]` → 0 → 2 → 1 → 0

#### int_element 制約

配列要素アクセス `array[index] = result` を維持する制約。

**引数:**
- `index_var`: インデックス変数（デフォルトは 1-based、MiniZinc 仕様）
- `array`: 定数整数の配列
- `result_var`: 結果変数
- `zero_based`: true なら 0-based インデックス（デフォルト: false）

**特徴:**
- 状態を持たないため、バックトラック時の巻き戻し処理が不要
- CSR（逆引き）データ構造で効率的に値からインデックスを検索
- Monotonic Wrapper（prefix/suffix の min/max）を保持（将来の bounds propagation 用）

**例:**
```cpp
// array = {10, 20, 30} (1-based: array[1]=10, array[2]=20, array[3]=30)
auto index = make_var("index", 1, 3);
auto result = make_var("result", 10, 30);
std::vector<Domain::value_type> array = {10, 20, 30};
IntElementConstraint c(index, array, result);

// 有効な解: (1,10), (2,20), (3,30)
```

#### int_lin_le_imp 制約

半具象化（含意）版の線形不等式制約。`b = 1 -> Σ(coeffs[i] * vars[i]) <= bound`

**引数:**
- `coeffs`: 係数の配列
- `vars`: 変数の配列
- `bound`: 上限値
- `b`: 含意変数（bool）

**特徴:**
- `b = 1` の場合: 線形不等式 `sum <= bound` を強制
- `b = 0` の場合: 制約は無条件で充足（vacuously true）
- `_reif` との違い: `_reif` は双方向（`P <-> b`）だが、`_imp` は単方向（`b -> P`）
- `b` が未確定でも、線形変数の状態は追跡される（後で `b = 1` になる可能性があるため）

**例:**
```cpp
// b = 1 -> 2*x + 3*y <= 10
auto x = make_var("x", 1, 5);
auto y = make_var("y", 1, 5);
auto b = make_var("b", 0, 1);
IntLinLeImpConstraint c({2, 3}, {x, y}, 10, b);

// b = 0 の場合: x, y は自由に選べる
// b = 1 の場合: 2*x + 3*y <= 10 を満たす必要がある
```

#### int_lin_le_reif 制約

完全具象化版の線形不等式制約。`Σ(coeffs[i] * vars[i]) <= bound <-> b`

**引数:**
- `coeffs`: 係数の配列
- `vars`: 変数の配列
- `bound`: 上限値
- `b`: 具象化変数（bool）

**特徴:**
- `b = 1` の場合: 線形不等式 `sum <= bound` を強制
- `b = 0` の場合: 線形不等式 `sum > bound` を強制
- `b` が未確定でも、線形制約が常に成立する場合は `b = 1` に推論
- `b` が未確定でも、線形制約が常に不成立の場合は `b = 0` に推論
- `_imp` との違い: `_imp` は単方向（`b -> P`）だが、`_reif` は双方向（`P <-> b`）

**例:**
```cpp
// 2*x + 3*y <= 10 <-> b
auto x = make_var("x", 1, 5);
auto y = make_var("y", 1, 5);
auto b = make_var("b", 0, 1);
IntLinLeReifConstraint c({2, 3}, {x, y}, 10, b);

// x, y が小さい値に制限されると b = 1 に推論される
// x = y = 5 の場合: sum = 25 > 10 なので b = 0 に推論される
```

#### int_max 制約 (2変数版)

2変数の最大値制約。`max(x, y) = m`

**引数:**
- `x`: 第1引数
- `y`: 第2引数
- `m`: 最大値を格納する変数

**伝播ロジック:**
- m の下限: max(x.min(), y.min())
- m の上限: max(x.max(), y.max())
- x, y の上限: m の上限で制限
- x または y が確定して m.max と等しい場合: m を確定

**例:**
```cpp
// m = max(x, y)
auto x = make_var("x", 1, 5);
auto y = make_var("y", 3, 7);
auto m = make_var("m", 1, 10);
IntMaxConstraint c(x, y, m);

// 伝播後: m の範囲は [3, 7] に絞られる
// (下限 = max(1,3) = 3, 上限 = max(5,7) = 7)
```

#### array_int_maximum / array_int_minimum 制約

配列の最大値・最小値を取る制約。`m = max(x[0], ..., x[n-1])` または `m = min(x[0], ..., x[n-1])`。

**引数:**
- `m`: 最大値/最小値を格納する変数
- `x`: 整数変数の配列

**伝播ロジック (array_int_maximum):**
- m の下限: 配列要素の下限の最大値 (`max(x[i].min())`)
- m の上限: 配列要素の上限の最大値 (`max(x[i].max())`)
- 各 x[i] の上限: m の上限で制限
- x[i] が確定して m.max と等しい場合: m を確定

**伝播ロジック (array_int_minimum):**
- m の下限: 配列要素の下限の最小値 (`min(x[i].min())`)
- m の上限: 配列要素の上限の最小値 (`min(x[i].max())`)
- 各 x[i] の下限: m の下限で制限
- x[i] が確定して m.min と等しい場合: m を確定

**例:**
```cpp
// m = max(x1, x2, x3)
auto m = make_var("m", 1, 10);
auto x1 = make_var("x1", 1, 5);
auto x2 = make_var("x2", 3, 7);
auto x3 = make_var("x3", 2, 4);
ArrayIntMaximumConstraint c(m, {x1, x2, x3});

// 伝播後: m の範囲は [3, 7] に絞られる
// (下限 = max(1,3,2) = 3, 上限 = max(5,7,4) = 7)
```

## 制約の追加方法

### 1. 適切なグループを選択

制約は機能ごとにグループ分けされています：

| グループ | ファイル | 内容 |
|----------|----------|------|
| comparison | `constraints/comparison.{hpp,cpp}` | 比較制約 (eq, ne, lt, le, gt, ge) |
| arithmetic | `constraints/arithmetic.{hpp,cpp}` | 算術制約 (plus, times, div, mod) |
| global | `constraints/global.{hpp,cpp}` | グローバル制約 (alldifferent, element) |
| logical | `constraints/logical.{hpp,cpp}` | 論理制約 (and, or, xor, not) |

新しいグループが必要な場合は、新規ファイルを作成します。

### 2. ヘッダファイルにクラス宣言を追加

`include/sabori_csp/constraints/<group>.hpp`:

```cpp
/**
 * @brief int_xxx制約: <説明>
 */
class IntXxxConstraint : public Constraint {
public:
    IntXxxConstraint(VariablePtr x, VariablePtr y);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool propagate() override;

private:
    VariablePtr x_;
    VariablePtr y_;
};
```

### 3. 実装ファイルにメソッドを実装

`src/core/constraints/<group>.cpp`:

```cpp
// IntXxxConstraint implementation

IntXxxConstraint::IntXxxConstraint(VariablePtr x, VariablePtr y)
    : x_(std::move(x)), y_(std::move(y)) {}

std::string IntXxxConstraint::name() const {
    return "int_xxx";
}

std::vector<VariablePtr> IntXxxConstraint::variables() const {
    return {x_, y_};
}

std::optional<bool> IntXxxConstraint::is_satisfied() const {
    if (x_->is_assigned() && y_->is_assigned()) {
        // 満足判定のロジック
        return /* true/false */;
    }
    return std::nullopt;
}

bool IntXxxConstraint::propagate() {
    // 伝播ロジック
    // ドメインが空になったらfalseを返す
    return !x_->domain().empty() && !y_->domain().empty();
}
```

### 4. 新しいグループの場合

#### 4.1 ヘッダファイルを作成

`include/sabori_csp/constraints/<newgroup>.hpp`

#### 4.2 実装ファイルを作成

`src/core/constraints/<newgroup>.cpp`

#### 4.3 constraint.hpp にインクルードを追加

```cpp
// 各制約グループのヘッダをインクルード
#include "sabori_csp/constraints/comparison.hpp"
#include "sabori_csp/constraints/<newgroup>.hpp"  // 追加
```

#### 4.4 CMakeLists.txt に追加

`src/core/CMakeLists.txt`:

```cmake
add_library(sabori_csp_core
  ...
  # Constraints
  constraints/comparison.cpp
  constraints/<newgroup>.cpp  # 追加
)
```

### 5. FlatZincパーサへの登録

`src/fzn/fzn_parser.cpp` で制約名とクラスのマッピングを追加します。

### 6. テストを追加

`tests/fzn/constraints/<constraint_name>/` ディレクトリに `.fzn` と `.expected` ファイルのペアを作成します。

```
tests/fzn/constraints/int_xxx/
├── simple.fzn      # テスト用FlatZincファイル
└── simple.expected # 期待される出力
```
