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
