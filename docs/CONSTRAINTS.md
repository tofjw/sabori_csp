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

### グローバル制約 (global)

| 制約名 | クラス | 説明 |
|--------|--------|------|
| `all_different` | `AllDifferentConstraint` | 全ての変数が異なる値を取る |
| `int_lin_eq` | `IntLinEqConstraint` | Σ(coeffs[i] * vars[i]) == target |
| `int_lin_le` | `IntLinLeConstraint` | Σ(coeffs[i] * vars[i]) <= bound |
| `circuit` | `CircuitConstraint` | 変数がハミルトン閉路を形成する |
| `int_element` | `IntElementConstraint` | array[index] = result を維持する |

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
