# 制約実装ガイド

このドキュメントは sabori_csp で新しい制約を実装する際のノウハウをまとめたものです。

## 1. 制約の基本構造

### 1.1 必須メソッド

```cpp
class MyConstraint : public Constraint {
public:
    MyConstraint(VariablePtr x, VariablePtr y);

    std::string name() const override;           // 制約名（FlatZinc名と一致させる）
    std::vector<VariablePtr> variables() const override;  // 関連変数リスト
    std::optional<bool> is_satisfied() const override;    // 充足判定
    bool propagate() override;                   // 初期伝播

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, Domain::value_type value,
                        Domain::value_type prev_min,
                        Domain::value_type prev_max) override;
    bool on_final_instantiate() override;        // 全変数確定時の最終チェック

protected:
    void check_initial_consistency() override;   // 初期矛盾チェック
};
```

### 1.2 状態を持つ制約の追加メソッド

AllDifferent, Circuit, IntLinEq など内部状態を持つ制約は以下も実装：

```cpp
void rewind_to(int save_point);              // バックトラック時の状態復元
bool on_last_uninstantiated(Model& model, int save_point,
                            size_t last_var_internal_idx);  // 残り1変数時の処理
```

## 2. 重要な原則

### 2.1 enqueue メソッドを使ったドメイン変更（最重要）

**on_instantiate() 内でドメインを変更する場合は、必ず Model の `enqueue_*` メソッドを使用する。**

```cpp
// NG: 直接ドメイン変更（バックトラック時に復元されない）
b_->domain().assign(1);
y_->domain().remove(x_val);

// NG: 即座にドメイン変更（他の制約に伝播しない）
model.instantiate(save_point, b_idx, 1);
model.remove_value(save_point, y_idx, x_val);

// OK: enqueue でキューに追加（Trail 記録 + 他制約への伝播）
model.enqueue_instantiate(b_idx, 1);
model.enqueue_remove_value(y_idx, x_val);
model.enqueue_set_min(y_idx, x_val);
model.enqueue_set_max(x_idx, y_val);
```

#### なぜ enqueue が必要か

1. **Trail への記録**: enqueue されたドメイン変更は `Solver::process_queue()` で処理される際に Trail に記録される
2. **他の制約への伝播**: 変数が新たに確定すると、その変数に関連する全ての制約の `on_instantiate()` が呼ばれる

#### 直接 model.* を呼んだ場合の問題

```cpp
// 問題のあるコード
bool IntEqConstraint::on_instantiate(Model& model, int save_point, ...) {
    if (x_->is_assigned()) {
        model.instantiate(save_point, y_idx, x_val);  // 直接変更
    }
    return true;
}
```

例: `int_eq(a, b)` と `int_eq(b, c)` が両方あるとき
1. `a = 0` が確定 → `IntEqConstraint(a, b)` の `on_instantiate()` が呼ばれる
2. `model.instantiate(b_idx, 0)` で `b = 0` が確定
3. **問題**: `IntEqConstraint(b, c)` の `on_instantiate()` は呼ばれない
4. 結果: `c` のドメインは更新されず、不完全な伝播になる

#### enqueue を使った場合の正しいフロー

```cpp
// 正しいコード
bool IntEqConstraint::on_instantiate(Model& model, int save_point, ...) {
    if (x_->is_assigned()) {
        model.enqueue_instantiate(y_idx, x_val);  // キューに追加
    }
    return true;
}
```

1. `a = 0` が確定 → `IntEqConstraint(a, b)` の `on_instantiate()` が呼ばれる
2. `model.enqueue_instantiate(b_idx, 0)` でキューに追加
3. `on_instantiate()` が返った後、`process_queue()` が `b = 0` を処理
4. `b` が新たに確定したので、`b` に関連する全制約の `on_instantiate()` が呼ばれる
5. `IntEqConstraint(b, c)` の `on_instantiate()` が呼ばれ、`c = 0` がキューに追加
6. 連鎖的に全ての関連変数が更新される

#### 利用可能な enqueue メソッド

| メソッド | 用途 |
|---------|------|
| `model.enqueue_instantiate(var_idx, value)` | 変数を特定の値に確定 |
| `model.enqueue_set_min(var_idx, new_min)` | 下限を設定 |
| `model.enqueue_set_max(var_idx, new_max)` | 上限を設定 |
| `model.enqueue_remove_value(var_idx, value)` | 特定の値を除去 |

#### 変数のモデル内インデックス取得

Variable は `id()` メソッドでモデル内のインデックスを O(1) で取得できます：

```cpp
// on_instantiate 内で変数のモデル内インデックスを取得
model.enqueue_instantiate(b_->id(), 1);
model.enqueue_set_min(x_->id(), new_min);
model.enqueue_remove_value(y_->id(), val);
```

**注意**: `var->id()` は Model が `create_variable()` で変数を作成した際に自動的に設定されます。FlatZinc パーサ経由で作成された変数は常に有効な ID を持ちます。

### 2.2 propagate() と on_instantiate() の違い

| メソッド | 呼ばれるタイミング | Trail | 用途 |
|---------|------------------|-------|------|
| `propagate()` | 初期伝播時（探索開始前） | 不要 | 初期ドメイン削減 |
| `on_instantiate()` | 変数確定時（探索中） | **必須** | 増分伝播 |

```cpp
bool MyConstraint::propagate() {
    // 初期伝播: Trail なしでドメイン直接操作 OK
    x_->domain().remove(val);
    return !x_->domain().empty();
}

bool MyConstraint::on_instantiate(Model& model, int save_point, ...) {
    // 増分伝播: enqueue でキューに追加（process_queue で Trail 記録 + 伝播）
    model.enqueue_remove_value(x_idx, val);
    return true;
}
```

### 2.3 check_initial_consistency()

コンストラクタ末尾で呼び出し、初期状態で矛盾があれば `set_initially_inconsistent(true)` を設定：

```cpp
void MyConstraint::check_initial_consistency() {
    // 例: x <= y で x.min > y.max なら矛盾
    auto x_min = x_->domain().min();
    auto y_max = y_->domain().max();

    if (x_min && y_max && *x_min > *y_max) {
        set_initially_inconsistent(true);
    }
}
```

## 3. Reified 制約の実装パターン

`(P) <-> b` 形式の制約（P が成り立つ ⟺ b = 1）

### 3.1 伝播ロジック

```cpp
bool MyReifConstraint::on_instantiate(Model& model, int save_point, ...) {
    // 1. b が確定している場合
    if (b_->is_assigned()) {
        if (b_->assigned_value().value() == 1) {
            // P を強制（x, y のドメインを絞る）
            // 例: model.enqueue_set_max(x_->id(), y_max);
        } else {
            // ¬P を強制（x, y のドメインを絞る）
            // 例: model.enqueue_set_min(x_->id(), y_min + 1);
        }
    }

    // 2. x, y の bounds から b を決定できるか
    if (!b_->is_assigned()) {
        if (/* P が必ず真 */) {
            model.enqueue_instantiate(b_->id(), 1);
        } else if (/* P が必ず偽 */) {
            model.enqueue_instantiate(b_->id(), 0);
        }
    }

    // 3. x, y が両方確定したら b を決定
    if (x_->is_assigned() && y_->is_assigned() && !b_->is_assigned()) {
        bool p_holds = /* P の真偽を計算 */;
        model.enqueue_instantiate(b_->id(), p_holds ? 1 : 0);
    }

    return !x_->domain().empty() && !y_->domain().empty() && !b_->domain().empty();
}
```

### 3.2 例: int_le_reif (x <= y) <-> b

- b = 1 のとき: x の上限を y.max に、y の下限を x.min に
- b = 0 のとき: x > y を強制（x の下限を y.min + 1 に、y の上限を x.max - 1 に）
- x.max <= y.min なら b = 1 が確定
- x.min > y.max なら b = 0 が確定

## 4. 状態を持つ制約の実装パターン

### 4.1 Trail の設計

```cpp
class AllDifferentConstraint : public Constraint {
private:
    // 状態
    std::vector<Domain::value_type> pool_values_;
    size_t pool_n_;

    // Trail: (save_point, 復元情報)
    std::vector<std::pair<int, std::pair<size_t, size_t>>> pool_trail_;
};
```

### 4.2 状態変更時の Trail 保存

```cpp
bool AllDifferentConstraint::on_instantiate(Model& model, int save_point, ...) {
    // 同じ save_point で既に保存済みならスキップ
    if (pool_trail_.empty() || pool_trail_.back().first != save_point) {
        pool_trail_.push_back({save_point, {pool_n_, unfixed_count_}});
    }

    // 状態を変更
    --pool_n_;
    --unfixed_count_;

    return true;
}
```

### 4.3 rewind_to() の実装

```cpp
void AllDifferentConstraint::rewind_to(int save_point) {
    while (!pool_trail_.empty() && pool_trail_.back().first > save_point) {
        auto& [level, state] = pool_trail_.back();
        pool_n_ = state.first;
        unfixed_count_ = state.second;
        pool_trail_.pop_back();
    }
}
```

### 4.4 Solver への登録

`src/core/solver.cpp` の `backtrack()` に追加：

```cpp
void Solver::backtrack(Model& model, int save_point) {
    model.rewind_to(save_point);

    for (const auto& constraint : model.constraints()) {
        if (auto* my = dynamic_cast<MyConstraint*>(constraint.get())) {
            my->rewind_to(save_point);
        }
        // 他の制約型も同様...
    }
}
```

## 5. FlatZinc パーサへの登録

`src/fzn/model.cpp` に追加：

```cpp
} else if (decl.name == "my_constraint") {
    if (decl.args.size() != 2) {
        throw std::runtime_error("my_constraint requires 2 arguments");
    }
    auto x = get_var(decl.args[0]);
    auto y = get_var(decl.args[1]);
    constraint = std::make_shared<MyConstraint>(x, y);
}
```

## 6. テストの追加

### 6.1 C++ 単体テスト

`tests/cpp/test_constraints.cpp` または `tests/cpp/test_global_constraints.cpp` に追加：

```cpp
TEST_CASE("MyConstraint name", "[constraint][my_constraint]") {
    auto x = std::make_shared<Variable>("x", Domain(1, 5));
    auto y = std::make_shared<Variable>("y", Domain(1, 5));
    MyConstraint c(x, y);
    REQUIRE(c.name() == "my_constraint");
}

TEST_CASE("MyConstraint is_satisfied", "[constraint][my_constraint]") {
    // 各ケースをテスト
}

TEST_CASE("MyConstraint on_final_instantiate", "[constraint][my_constraint]") {
    // Solver 統合テスト
    Model model;
    // ...
    Solver solver;
    auto sol = solver.solve(model);
    REQUIRE(sol.has_value());
}
```

### 6.2 FlatZinc 統合テスト

`tests/fzn/constraints/my_constraint/` ディレクトリを作成：

```
tests/fzn/constraints/my_constraint/
├── simple.fzn
└── simple.expected
```

**simple.fzn:**
```
var 1..5: x :: output_var;
var 1..5: y :: output_var;
constraint my_constraint(x, y);
solve satisfy;
```

**simple.expected:**
```
x = 1;
y = 1;
----------
```

**注意:** `:: output_var` アノテーションがないと、解が見つかっても値が出力されない。

## 7. チェックリスト

新しい制約を実装する際のチェックリスト：

- [ ] Constraint 基底クラスを継承
- [ ] コンストラクタで `check_initial_consistency()` を呼び出し
- [ ] `on_instantiate()` では **`enqueue_*` メソッド**でドメイン変更（直接 `model.*` を使わない）
- [ ] 状態を持つ制約は `rewind_to()` を実装
- [ ] `solver.cpp` の `backtrack()` に rewind_to 呼び出しを追加
- [ ] FlatZinc パーサに登録
- [ ] C++ 単体テスト追加
- [ ] FlatZinc 統合テスト追加（`:: output_var` 忘れずに）
- [ ] 複数制約が連鎖する FlatZinc テストを追加（例: `int_eq(a,b)` + `int_eq(b,c)`）
- [ ] `docs/CONSTRAINTS.md` に追記

## 8. よくある問題と解決策

| 症状 | 原因 | 解決策 |
|-----|------|-------|
| FlatZinc で UNSAT になるが単体テストは通る | on_instantiate で直接ドメイン変更 | enqueue_* メソッドに変更 |
| 複数制約の連鎖が伝播しない | model.* を直接呼んでいる | enqueue_* メソッドに変更 |
| 無限ループになる | process_queue で既確定変数を再伝播 | Solver 側で was_instantiated チェック |
| バックトラック後に状態がおかしい | rewind_to 未実装 or Trail 未保存 | Trail を正しく保存・復元 |
| 解が出力されない | output_var アノテーション忘れ | FlatZinc ファイルに追加 |
| 初期状態で矛盾を検出できない | check_initial_consistency 未実装 | 実装を追加 |

### 8.1 伝播が連鎖しない問題の詳細

**症状**: `int_eq(a, b)` と `int_eq(b, c)` がある状態で `a = 0` を確定しても `c` が更新されない

**原因**: `on_instantiate()` 内で `model.instantiate()` を直接呼ぶと、`b` は更新されるが `b` に関連する他の制約の `on_instantiate()` は呼ばれない

**解決策**: `model.enqueue_instantiate()` を使う

```cpp
// NG
model.instantiate(save_point, b_idx, value);

// OK
model.enqueue_instantiate(b_idx, value);
```

`enqueue_*` を使うと、`Solver::process_queue()` がキューを処理し、変数が新たに確定するたびに関連制約の `propagate()` を呼び出す。これにより連鎖的な伝播が正しく行われる。
