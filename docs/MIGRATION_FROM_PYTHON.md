# Python実装からC++への移植ガイド

このドキュメントは `tmp/` ディレクトリにあるPython実装を、現在のC++コードベースに移植するための情報をまとめたものです。

## 1. 概要

### Python実装の場所
- `tmp/var.py` - IntVar クラス
- `tmp/solver.py` - Solver クラス、NoGood クラス
- `tmp/constraints/constraint.py` - Constraint 基底クラス
- `tmp/constraints/all_different.py` - AllDifferent 制約
- `tmp/constraints/lin_eq.py` - LinEq 制約
- `tmp/constraints/arithmetic.py` - Minus 制約（未完成）

### 現在のC++実装
- `include/sabori_csp/domain.hpp` - std::set ベースのドメイン
- `include/sabori_csp/variable.hpp` - 変数クラス
- `include/sabori_csp/solver.hpp` - 単純バックトラッキング
- `include/sabori_csp/constraint.hpp` - 制約基底クラス
- `include/sabori_csp/constraints/comparison.hpp` - 比較制約

---

## 2. IntVar (Python) → Domain/Variable (C++)

### 2.1 Sparse Set ベースのドメイン管理

Python実装では、ドメインを Sparse Set として管理し、O(1) での値の削除・存在確認を実現しています。

```python
# Python: tmp/var.py
class IntVar:
    def __init__(self, **kwargs):
        self.id = self._get_next_id()

        # Sparse Set 構造
        self._values = list(range(self._min, self._max + 1))  # Dense 配列
        self._n = len(self._values)  # 有効な値の数
        self._sparse = {v: i for i, v in enumerate(self._values)}  # 値→インデックス

        # 分散型 Trail
        self._trail = []  # [(stack_level, old_min, old_max, old_n), ...]
```

**C++への移植ポイント:**
- `std::vector<int64_t> values_` - Dense 配列
- `std::unordered_map<int64_t, size_t> sparse_` - 逆引きマップ
- `size_t n_` - 有効な値の数
- `std::vector<std::tuple<int, int64_t, int64_t, size_t>> trail_` - 分散Trail

### 2.2 分散型 Trail

各変数が自分の変更履歴を持ち、バックトラック時に自己完結的に復元できます。

```python
def _save_trail(self, decision):
    """同じレベルでの複数回変更は最初の1回のみ保存"""
    if not self._trail or self._trail[-1][0] != decision:
        self._trail.append((decision, self._min, self._max, self._n))

def rewind_to(self, save_point):
    """save_point より深いレベルの変更を巻き戻す"""
    while self._trail and self._trail[-1][0] > save_point:
        _, old_min, old_max, old_n = self._trail.pop()
        self._min = old_min
        self._max = old_max
        self._n = old_n
```

### 2.3 ドメイン操作

```python
def set_min(self, stack_level, new_min):
    """下限を更新し、それ未満の値を削除"""
    if new_min <= self._min: return True
    if new_min > self._max: return False

    self._save_trail(stack_level)

    # new_min 未満の値を Sparse Set から除外
    i = 0
    while i < self._n:
        val = self._values[i]
        if val < new_min:
            self._swap(i, self._n - 1)
            self._n -= 1
        else:
            i += 1
    # ...

def remove_value(self, stack_level, v):
    """特定の値を削除"""
    idx = self._sparse.get(v)
    if idx is None or idx >= self._n: return True

    self._save_trail(stack_level)
    self._swap(idx, self._n - 1)
    self._n -= 1
    return True

def instantiate(self, decision, v):
    """変数を値 v に固定"""
    idx = self._sparse.get(v)
    if idx is None or idx >= self._n: return False

    self._save_trail(decision)
    self._swap(idx, 0)
    self._n = 1
    self._min = v
    self._max = v
    return True

def _swap(self, i, j):
    """Sparse Set のスワップ操作"""
    v_i, v_j = self._values[i], self._values[j]
    self._values[i], self._values[j] = v_j, v_i
    self._sparse[v_i], self._sparse[v_j] = j, i
```

### 2.4 差分取得

```python
def get_delta(self, last_timestamp_n):
    """前回から削除された値のリストを返す"""
    return self._values[self._n : last_timestamp_n]
```

---

## 3. Constraint 基底クラス

### 3.1 2-Watched Literal (2WL)

制約が関与する変数のうち、未確定の2つだけを監視し、効率的な伝播を実現します。

```python
# Python: tmp/constraints/constraint.py
class Constraint:
    def __init__(self, vars, **kwargs):
        self.id = self._get_next_id()
        self._vars = vars

        # 2WL の初期設定
        self._w1 = -1
        self._w2 = -1
        for i in range(len(vars)):
            if not vars[i].is_instantiated:
                if self._w1 < 0:
                    self._w1 = i
                    self._w2 = (i + 1) % len(vars)
                else:
                    self._w2 = i
                    break

    def on_instantiate(self, solver, decision, var, value, prev_min, prev_max):
        """変数が確定した時に呼ばれる"""
        # 監視変数が確定したら、別の未確定変数に監視を移す
        if var.id == self._vars[self._w1].id:
            for idx in range(len(self.vars)):
                if idx == self._w1 or idx == self._w2:
                    continue
                if not self._vars[idx].is_instantiated:
                    self._w1 = idx
                    return True
            # 移せない場合は最終確認へ
            if not self._vars[self._w2].is_instantiated:
                return True
        # ... (w2 の場合も同様)

        return self.on_final_instantiate()

    def on_final_instantiate(self):
        """全変数確定時の最終チェック（サブクラスでオーバーライド）"""
        return True
```

---

## 4. AllDifferent 制約

### 4.1 Sparse Set プール

使用可能な値のプールを Sparse Set で管理し、鳩の巣原理による枝刈りを行います。

```python
# Python: tmp/constraints/all_different.py
class AllDifferent(Constraint):
    def __init__(self, vars, **kwargs):
        super().__init__(vars, **kwargs)

        # 全変数の値の和集合をプールとして管理
        all_vals = sorted(list(set(v for var in vars for v in var._values)))
        self._pool_values = all_vals
        self._pool_sparse = {v: i for i, v in enumerate(all_vals)}
        self._pool_n = len(all_vals)
        self._pool_trail = []  # (stack_level, old_n)

    def on_instantiate(self, solver, decision, var, value, prev_min, prev_max):
        if not super().on_instantiate(...):
            return False

        # プールから値を削除
        idx = self._pool_sparse.get(value)
        if idx is None or idx >= self._pool_n:
            return False  # 既に使われている

        self._pool_trail.append((decision, self._pool_n))
        # スワップして削除
        p2 = self._pool_n - 1
        v2 = self._pool_values[p2]
        self._pool_values[idx], self._pool_values[p2] = v2, value
        self._pool_sparse[value], self._pool_sparse[v2] = p2, idx
        self._pool_n -= 1

        # 鳩の巣原理チェック
        unfixed_vars = sum(1 for v in self.vars if not v.is_instantiated)
        if unfixed_vars > self._pool_n:
            return False

        # 最後の1変数は自動決定
        if unfixed_vars == 1 and self._pool_n == 1:
            remaining_value = self._pool_values[0]
            for v in self.vars:
                if not v.is_instantiated:
                    solver.enqueue_instantiate(v, remaining_value)
                    break

        return True

    def rewind_to(self, save_point):
        while self._pool_trail and self._pool_trail[-1][0] > save_point:
            _, old_n = self._pool_trail.pop()
            self._pool_n = old_n
```

---

## 5. LinEq 制約（線形等式）

### 5.1 差分更新による O(1) 伝播

```python
# Python: tmp/constraints/lin_eq.py
class LinEq(Constraint):
    def __init__(self, coeffs, vars, target_sum, **kwargs):
        # 同じ変数の係数を集約
        aggregated = {}
        for v, c in zip(vars, coeffs):
            aggregated[v] = aggregated.get(v, 0) + c

        self.vars = list(aggregated.keys())
        self.coeffs = list(aggregated.values())
        self.target_sum = target_sum

        # 差分更新用の状態
        self.current_fixed_sum = 0
        self.min_rem_potential = sum(c * min(v.values) for c, v in zip(coeffs, vars))
        self.max_rem_potential = sum(c * max(v.values) for c, v in zip(coeffs, vars))

        self._pool_trail = []  # (stack_level, (fix_sum, rem_min, rem_max))

    def _can_assign(self, var_idx, value, prev_min, prev_max):
        """Look-ahead: 値を割り当て可能か O(1) で判定"""
        c = self.coeffs[var_idx]

        # 上限チェック: 他が最小でも target を超えないか
        if self.current_fixed_sum + (self.min_rem_potential - c * prev_min) + c * value > self.target_sum:
            return False

        # 下限チェック: 他が最大でも target に届くか
        if self.current_fixed_sum + (self.max_rem_potential - c * prev_max) + c * value < self.target_sum:
            return False

        return True

    def on_instantiate(self, solver, decision, var, value, prev_min, prev_max):
        if not super().on_instantiate(...):
            return False

        if not self._can_assign(var_idx, value, prev_min, prev_max):
            return False

        c = self.coeffs[var_idx]

        # Trail に保存
        self._pool_trail.append((decision, (self.current_fixed_sum, self.min_rem_potential, self.max_rem_potential)))

        # 差分更新
        self.current_fixed_sum += c * value
        self.min_rem_potential -= c * prev_min
        self.max_rem_potential -= c * prev_max

        # 最後の1変数は自動決定
        unfixed_vars = [(v, self.coeffs[i]) for i, v in enumerate(self.vars) if not v.is_instantiated]
        if len(unfixed_vars) == 1:
            last_var, last_coeff = unfixed_vars[0]
            remaining = self.target_sum - self.current_fixed_sum
            if remaining % last_coeff == 0:
                required_value = remaining // last_coeff
                if last_var.is_in(required_value):
                    solver.enqueue_instantiate(last_var, required_value)
                else:
                    return False
            else:
                return False

        return True
```

---

## 6. Solver

### 6.1 NoGood 学習

失敗パターンを記録し、同じ失敗を繰り返さないようにします。

```python
# Python: tmp/solver.py
class NoGood:
    """失敗パターン: これらのリテラルが全て成立すると矛盾"""
    def __init__(self, literals):
        self.literals = literals  # list of (var, value)
        self.w1 = 0  # 監視リテラル1
        self.w2 = 1 if len(literals) > 1 else 0  # 監視リテラル2
```

### 6.2 Activity-based 変数選択

失敗に関与した変数の activity を上げ、優先的に選択します。

```python
class Solver:
    def __init__(self, vars, constraints):
        self._activity = [0] * len(vars)
        # ...

    def run_search(self, conflict_limit, depth=0):
        # Activity 最大の未割当変数を選択
        unassigned = [(i, v) for i, v in enumerate(vars) if not v.is_instantiated]
        index, var = max(unassigned, key=lambda iv: self._activity[iv[0]])
        # ...

        # 失敗時に activity を更新
        self._activity[index] += 1
        for var, val in assignments:
            self._activity[self.var_id_to_idx[var.id]] += 1.0 / len(assignments)
```

### 6.3 リスタート戦略

Luby-like なリスタート戦略と、良い部分解の再利用を行います。

```python
def solve(self, seed=None):
    inner_limit = 5   # 衝突回数の初期閾値
    outer_limit = 10  # outer ループの初期閾値

    while True:
        for _ in range(int(outer_limit)):
            result, solution = self.run_search(conflict_limit=int(inner_limit))
            if result == Result.SAT:
                return solution
            if result == Result.UNSAT:
                return None

            # リスタート
            self.backtrack(root_point)
            self._current_best_assignment = self._select_best_assignment()

            # Activity decay
            for i in range(len(self.vars)):
                self._activity[i] *= 0.99

            inner_limit *= 1.1

        outer_limit *= 1.001
        inner_limit = 5
```

### 6.4 良い部分解の保存と再利用（GA風クロスオーバー）

```python
def _add_best_assignment(self, num_instantiated, assignment):
    """深くまで探索できた部分解を保存"""
    self._best_assignments.append((num_instantiated, assignment))
    # 上限を超えたら activity が低いものを削除

def _select_best_assignment(self):
    """リスタート時に使用する割り当てを選択（GA風クロスオーバー）"""
    # Activity 上位2つの部分解を選択
    # 1点交叉で新しい割り当てを生成
    # ...
```

### 6.5 NoGood 伝播 (2-Watched Literal)

```python
def _propagate_ng(self, ng, triggered_key):
    """triggered_key の watch が成立したので、watch を移すか枝刈り"""
    # 未成立リテラルを探して watch を移す
    for i in range(len(lits)):
        if i == ng.w1 or i == ng.w2:
            continue
        var_i, val_i = lits[i]
        if not var_i.is_instantiated or var_i.value != val_i:
            # watch を移す
            # ...
            return True

    # 移せない場合
    other_var, other_val = lits[other_idx]
    if other_var.is_instantiated and other_var.value == other_val:
        return False  # 矛盾

    # ドメインから値を除去
    other_var.remove_value(self.current_decision, other_val)
    return True
```

---

## 7. 移植の優先順位

### Phase 1: ドメイン/変数の改善 ✅ 完了
1. ✅ `Domain` クラスを Sparse Set ベースに変更
   - `values_` (Dense配列), `sparse_` (逆引きmap), `n_` (有効サイズ)
   - O(1) での contains, remove, assign
2. ✅ **集中型 Trail** を `Model` に実装（SoA向け設計）
   - `VarTrailEntry`: var_idx, old_min, old_max, old_n
   - `save_var_state()`: 同一レベルでの重複保存防止
3. ✅ `Model::rewind_to()` メソッドの追加
   - SoA配列 (mins_, maxs_, sizes_) と Domain オブジェクト両方を復元
4. ✅ ドメイン操作メソッド（Trail付き）
   - `set_min()`, `set_max()`, `remove_value()`, `instantiate()`

### Phase 2: 制約基底クラスの改善 ✅ 完了
1. ✅ 2-Watched Literal (2WL) の実装
   - `w1_`, `w2_`: 監視変数のインデックス
   - `init_watches()`: 未確定の2変数を自動選択
   - `watch1()`, `watch2()`: 監視変数インデックス取得
   - `can_be_finalized()`: 全監視変数が確定しているか判定
2. ✅ `on_instantiate()` メソッドの追加
   - シグネチャ: `(Model&, int save_point, size_t var_idx, value, prev_min, prev_max)`
   - 監視変数が確定したら別の未確定変数に監視を移す
   - 全変数確定時は `on_final_instantiate()` を呼び出す
3. ✅ `on_final_instantiate()` メソッドの追加
   - 全変数確定時の最終チェック（サブクラスでオーバーライド）
   - デフォルトでは `is_satisfied()` を使用
4. ✅ 制約ID (`id()`) の自動付与
5. ✅ 比較制約 (IntEq, IntNe, IntLt, IntLe, IntEqReif) の更新

### Phase 3: 新しい制約の実装 ✅ 完了
1. ✅ `AllDifferentConstraint` (all_different)
   - Sparse Set プールで使用可能な値を管理
   - `on_instantiate()`: プールから値を削除、鳩の巣原理による枝刈り
   - `rewind_to()`: プールサイズを復元
   - Trail: `(save_point, old_pool_n)`
2. ✅ `IntLinEqConstraint` (int_lin_eq): Σ(c[i]*x[i]) == target
   - O(1) 差分更新による bounds consistency
   - Look-ahead チェック (`can_assign()`)
   - 係数の符号を考慮したポテンシャル計算
   - Trail: `(save_point, {fixed_sum, min_pot, max_pot})`
3. ✅ `IntLinLeConstraint` (int_lin_le): Σ(c[i]*x[i]) <= bound
   - IntLinEq と同様の差分更新方式

### Phase 4: ソルバーの改善 ✅ 完了
1. ✅ NoGood 学習（2-Watched Literal）
   - `NoGood` 構造体: literals, w1, w2
   - `add_nogood()`: 失敗パターンを記録
   - `propagate_nogood()`: 2WLによる効率的な伝播
   - `ng_watches_`: 監視リテラル用のマップ
2. ✅ Activity-based 変数選択
   - `activity_`: 各変数のアクティビティスコア
   - `select_variable()`: アクティビティ最大の未割当変数を選択
   - `decay_activities()`: アクティビティの減衰（0.99倍）
3. ✅ リスタート戦略
   - `search_with_restart()`: Luby-like リスタートループ
   - `initial_conflict_limit_`: 初期衝突制限（100）
   - `conflict_limit_multiplier_`: 制限の増加率（1.1倍）
4. ✅ 良い部分解の保存と再利用
   - `PartialAssignment`: 深くまで探索できた部分解
   - `save_partial_assignment()`: 良い部分解を保存
   - `select_best_assignment()`: GA風クロスオーバーで再利用

---

## 8. C++ 実装時の考慮事項

### メモリ管理
- `std::shared_ptr` の使用は現状維持
- Trail のメモリ効率に注意（頻繁な push/pop）

### 型の選択
- 値: `int64_t` (現状維持)
- インデックス: `size_t`
- スタックレベル: `int` または `size_t`

### パフォーマンス
- `std::unordered_map` vs `std::vector` (sparse 配列)
- インライン化の検討
- キャッシュ効率を考慮したデータ配置

### インターフェースの互換性
- FlatZinc パーサーとの整合性を維持
- 既存の比較制約との互換性

---

## 9. 集中Trail設計（SoA向け推奨設計）

Python実装では各変数・制約が分散して `rewind_to()` を持っていますが、
C++のSoA（Structure-of-Arrays）パターンを活かすには、**Modelに集中Trail**を持たせる方が効率的です。

### 9.1 分散Trail vs 集中Trail

| 項目 | 分散Trail (Python) | 集中Trail (推奨) |
|------|-------------------|-----------------|
| データ配置 | 各オブジェクトに分散 | Model に集約 |
| メモリアクセス | オブジェクトごとに飛ぶ | 連続メモリ |
| キャッシュ効率 | 悪い | 良い |
| バックトラック | 全オブジェクトをループ | Trail を逆スキャン |
| モジュラー性 | 高い（自己完結） | 低い（Model依存） |

### 9.2 推奨設計: ハイブリッドアプローチ

```cpp
// Trail エントリの種類
enum class TrailEntryType {
    VarDomain,      // 変数ドメインの変更
    ConstraintState // 制約状態の変更
};

// 変数ドメイン用 Trail エントリ
struct VarTrailEntry {
    size_t var_idx;
    int64_t old_min;
    int64_t old_max;
    size_t old_size;
    // Sparse Set の場合は old_n も必要
};

// 制約状態用 Trail エントリ（型消去またはvariant）
struct ConstraintTrailEntry {
    size_t constraint_idx;
    // 制約固有の状態（AllDifferentのpool_n、LinEqのfixed_sumなど）
    std::variant<
        size_t,                           // AllDifferent: pool_n
        std::tuple<int64_t, int64_t, int64_t>  // LinEq: (fixed_sum, min_pot, max_pot)
    > state;
};

class Model {
public:
    // SoA データ（変数ドメイン）
    std::vector<int64_t> mins_;
    std::vector<int64_t> maxs_;
    std::vector<size_t> sizes_;

    // Sparse Set 用追加データ
    std::vector<std::vector<int64_t>> values_;  // 各変数の Dense 配列
    std::vector<std::unordered_map<int64_t, size_t>> sparse_;  // 逆引き

    // 集中 Trail
    std::vector<std::pair<int, VarTrailEntry>> var_trail_;
    std::vector<std::pair<int, ConstraintTrailEntry>> constraint_trail_;

    // または統合 Trail
    // std::vector<std::tuple<int, TrailEntryType, std::variant<...>>> trail_;

    /// 変数ドメイン変更時に呼ばれる
    void save_var_state(int save_point, size_t var_idx) {
        // 同じレベルで既に保存済みならスキップ
        if (!var_trail_.empty() &&
            var_trail_.back().first == save_point &&
            var_trail_.back().second.var_idx == var_idx) {
            return;
        }
        var_trail_.push_back({save_point, {
            var_idx,
            mins_[var_idx],
            maxs_[var_idx],
            sizes_[var_idx]
        }});
    }

    /// 制約状態変更時に呼ばれる
    void save_constraint_state(int save_point, size_t constraint_idx, auto state) {
        constraint_trail_.push_back({save_point, {constraint_idx, state}});
    }

    /// バックトラック
    void rewind_to(int save_point) {
        // 変数ドメインの復元（連続メモリアクセス）
        while (!var_trail_.empty() && var_trail_.back().first > save_point) {
            auto& [level, entry] = var_trail_.back();
            mins_[entry.var_idx] = entry.old_min;
            maxs_[entry.var_idx] = entry.old_max;
            sizes_[entry.var_idx] = entry.old_size;
            var_trail_.pop_back();
        }

        // 制約状態の復元
        while (!constraint_trail_.empty() && constraint_trail_.back().first > save_point) {
            auto& [level, entry] = constraint_trail_.back();
            constraints_[entry.constraint_idx]->restore_state(entry.state);
            constraint_trail_.pop_back();
        }
    }
};
```

### 9.3 ドメイン操作のインターフェース

```cpp
class Model {
public:
    /// 変数の下限を更新
    bool set_min(int save_point, size_t var_idx, int64_t new_min) {
        if (new_min <= mins_[var_idx]) return true;
        if (new_min > maxs_[var_idx]) return false;

        save_var_state(save_point, var_idx);

        // Sparse Set から new_min 未満の値を除外
        auto& vals = values_[var_idx];
        auto& sp = sparse_[var_idx];
        size_t n = sizes_[var_idx];
        size_t i = 0;
        int64_t current_min = INT64_MAX;

        while (i < n) {
            if (vals[i] < new_min) {
                swap_value(var_idx, i, n - 1);
                --n;
            } else {
                current_min = std::min(current_min, vals[i]);
                ++i;
            }
        }

        if (n == 0) return false;

        sizes_[var_idx] = n;
        mins_[var_idx] = current_min;
        return true;
    }

    /// 特定の値を削除
    bool remove_value(int save_point, size_t var_idx, int64_t value) {
        auto& sp = sparse_[var_idx];
        auto it = sp.find(value);
        if (it == sp.end() || it->second >= sizes_[var_idx]) {
            return true;  // 既に無い
        }

        save_var_state(save_point, var_idx);

        size_t idx = it->second;
        swap_value(var_idx, idx, sizes_[var_idx] - 1);
        --sizes_[var_idx];

        // min/max の更新が必要な場合
        update_bounds(var_idx);
        return sizes_[var_idx] > 0;
    }

    /// 変数を特定の値に固定
    bool instantiate(int save_point, size_t var_idx, int64_t value) {
        auto& sp = sparse_[var_idx];
        auto it = sp.find(value);
        if (it == sp.end() || it->second >= sizes_[var_idx]) {
            return false;  // ドメインに無い
        }

        save_var_state(save_point, var_idx);

        size_t idx = it->second;
        swap_value(var_idx, idx, 0);
        sizes_[var_idx] = 1;
        mins_[var_idx] = value;
        maxs_[var_idx] = value;
        return true;
    }

private:
    void swap_value(size_t var_idx, size_t i, size_t j) {
        auto& vals = values_[var_idx];
        auto& sp = sparse_[var_idx];
        int64_t vi = vals[i], vj = vals[j];
        vals[i] = vj;
        vals[j] = vi;
        sp[vi] = j;
        sp[vj] = i;
    }
};
```

### 9.4 制約の状態管理

制約固有の状態は、制約クラス自身が保持しつつ、Modelに通知して Trail に記録します。

```cpp
class AllDifferentConstraint : public Constraint {
    // 内部状態
    std::vector<int64_t> pool_values_;
    std::unordered_map<int64_t, size_t> pool_sparse_;
    size_t pool_n_;

public:
    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, int64_t value,
                        int64_t prev_min, int64_t prev_max) override {
        // プールから値を削除する前に状態を保存
        model.save_constraint_state(save_point, this->id(), pool_n_);

        // ... プール操作 ...
    }

    void restore_state(const std::variant<...>& state) {
        pool_n_ = std::get<size_t>(state);
    }
};
```

### 9.5 Solver との統合

```cpp
class Solver {
    Model& model_;
    int current_decision_ = 0;

public:
    void backtrack(int save_point) {
        model_.rewind_to(save_point);
        // Solver 固有の状態（command_queue など）もクリア
        command_queue_.clear();
    }

    bool propagate(size_t var_idx, int64_t prev_min, int64_t prev_max) {
        // 制約への通知
        for (auto& c : model_.constraints_watching(var_idx)) {
            if (!c->on_instantiate(model_, current_decision_,
                                   var_idx, model_.value(var_idx),
                                   prev_min, prev_max)) {
                return false;
            }
        }
        return true;
    }
};
```

### 9.6 利点まとめ

1. **キャッシュ効率**: Trail が連続メモリなので、バックトラック時のスキャンが高速
2. **SoA との親和性**: ドメインデータは Model の SoA 配列で管理
3. **一括復元**: `rewind_to()` が Model の一箇所で完結
4. **柔軟性**: 制約固有の状態も統一的に管理可能

### 9.7 注意点

1. **制約の状態**: 制約固有の状態（AllDifferentのプールなど）は `std::variant` や型消去で統一
2. **Trail エントリサイズ**: 可変長データ（Sparse Set の完全なスナップショットなど）は避ける
3. **同レベル重複**: 同じ save_point で同じ変数/制約を複数回保存しないようチェック
