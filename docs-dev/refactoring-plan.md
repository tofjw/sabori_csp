# sabori_csp 段階的リアーキテクチャ計画

> **Note (2026-06-10)**: 本計画は Phase 6 を除き完了。後続の全面リファクタリング計画は
> [refactoring-plan-2026-06.md](refactoring-plan-2026-06.md) を参照（Phase 6 はそちらのフェーズ5に統合）。

## Context

当初のアーキテクチャ問題と対応状況:

1. ~~**Variable/VarData 二重管理**~~ ✅ — debug assertion + propagation API 分離で解消（Phase 1）
2. ~~**Solver モノリス**~~ ✅ — NoGoodManager・VariableSelector・RestartController に分解、2,024行→1,308行（Phase 2）
3. ~~**制約登録の if-else チェーン**~~ ✅ — ConstraintRegistry で data-driven 化、model.cpp 1,370行→366行（Phase 3）
4. ~~**shared_ptr 過剰使用**~~ ✅ — VariablePtr→生ポインタ、ConstraintPtr→unique_ptr 済。制約内部の VariablePtr メンバも全除去完了（Phase 4）
5. ~~**Trail の二重管理**~~ ✅ — save_point キーで統一（Phase 5）
6. **Domain 表現** ⬜ — BoundsDomain の unordered_set 最適化・BoolDomain 未実装（Phase 6）

この計画は、テストを常に通しながら段階的に理想設計へ近づけるためのロードマップ。

## 実施内容

### Phase 0: 安全基盤の整備（前提条件） ✅ 完了

**目的**: リファクタリング全体を通じた回帰検出の仕組みを整える。

- [x] 既存テストの全パス確認: `ctest --test-dir build` — 152/152 パス
- [x] ベンチマーク baseline の記録（2種）:
  - `python bench_compare.py` — mznc2025 問題セット（Sabori vs CP-SAT、30秒タイムアウト、4並列）
  - `python bench_compare_2024.py` — mznc2024 問題セット（同条件）
- [x] 生成された HTML レポートを `baseline_before_refactoring_2025.html` / `baseline_before_refactoring_2024.html` にリネームして保存
- [x] ベンチマーク比較スクリプト `bench_compare.py`, `bench_compare_2024.py` 整備済み

---

### Phase 1: Variable/VarData 二重 API の解消（最優先） ✅ 完了

**目的**: propagation 中に `var->method()` を呼べないようにし、protein 型バグを設計レベルで排除する。

#### Step 1.1: デバッグアサーション追加 ✅
- `Variable::assign/remove/remove_below/remove_above` に presolve 以外での呼び出し禁止アサーション追加済み（`src/core/variable.cpp`）
- `Model` に `presolve_phase_` フラグ追加済み（`set_presolve_phase()` / `in_presolve_phase()`）

#### Step 1.2: propagation コールバック内の var-> 呼び出し一掃 ✅
- 全制約の propagation コールバックで `model.is_instantiated()` / `model.value()` / `model.enqueue_*()` を使用
- presolve 内のみ直接 Variable API（`var->assign()`, `var->remove()` 等）を使用
- 全制約ファイルで一貫したパターンを確認済み

#### Step 1.3: bump_activity の shared_ptr 参照除去 ✅
- `Constraint::var_ids_ref()` アクセサ追加済み（`constraint.hpp` line 72）
- Solver が `var_ids_ref()` 経由で使用済み

---

### Phase 2: Solver の分解（高優先） ✅ 完了

**目的**: 2,024行の Solver を責務別のモジュールに分解し、保守性を向上させる。

**実施結果**: solver.cpp は 2,024行 → 1,308行 に縮小。

#### Step 2.1: NoGoodManager 抽出 ✅
- `include/sabori_csp/nogood_manager.hpp` + `src/core/nogood_manager.cpp`（433行）
- NoGood 追加・削除・GC、2-Watched Literal 伝播、Bloom filter 管理をカプセル化

#### Step 2.2: VariableSelector 抽出 ✅
- `include/sabori_csp/variable_selector.hpp` + `src/core/variable_selector.cpp`（235行）
- var_order_ パーティション管理、MRV+Activity+NoGood Bloom overlap スコアリング

#### Step 2.3: RestartController 抽出 ✅
- `include/sabori_csp/restart_controller.hpp`（ヘッダオンリー、108行）
- inner/outer ループ管理、adaptive パラメータ調整
- Solver が `restart_ctrl_.method()` で使用（17箇所）

#### Step 2.4: init_search 抽出 ✅
- `Solver::init_search(Model&)` メソッド追加済み
- build_constraint_watch_list → presolve → community 分析の共通初期化

---

### Phase 3: 制約レジストリ化（中優先） ✅ 完了 (2026-02-27)

**目的**: fzn/model.cpp の45分岐 if-else を data-driven なレジストリに置換。

**実施結果**:
- `model.cpp` を 1370行 → 366行 に縮小（73%削減）
- 全45制約のファクトリ関数をレジストリに登録
- 新規制約の追加は `register_constraint("name", factory)` の1行で完結

**新規ファイル**:
| ファイル | 行数 | 内容 |
|---------|------|------|
| `src/fzn/fzn_build_context.hpp` | 75 | `FznBuildContext` 構造体（共有状態 + ヘルパー宣言） |
| `src/fzn/fzn_build_context.cpp` | 117 | `get_var`, `resolve_var_array`, `apply_substitutions` 等 |
| `src/fzn/constraint_registry.hpp` | 68 | `ConstraintRegistry` クラス + `ConstraintFactory` 型 |
| `src/fzn/constraint_registry.cpp` | 760 | 全45制約のファクトリ + `register_all_constraints()` |

**設計**:
```cpp
// FznBuildContext: to_model() 内のラムダをメンバ関数化
struct FznBuildContext {
    Model* model;
    std::map<std::string, VariablePtr>& var_map;
    // ... 共有状態への参照
    VariablePtr get_var(const ConstraintArg& arg);
    std::vector<std::string> resolve_var_array(const ConstraintArg& arg) const;
    bool apply_substitutions(...) const;
};

// ConstraintFactory: nullopt = 制約追加不要（ドメイン変更等で処理済み）
using ConstraintFactory = std::function<
    std::optional<ConstraintPtr>(const ConstraintDecl&, FznBuildContext&)>;

// to_model() 内の使用（~15行）:
ConstraintRegistry registry;
register_all_constraints(registry);
auto result = registry.create(decl.name, decl, ctx);
if (result.has_value() && *result) model->add_constraint(std::move(*result));
```

**検証**: 全152テストパス + accap0.fzn スモークテスト正常

---

### Phase 4: shared_ptr 除去（Phase 1 完了後） ✅ 完了 (2026-02-28)

**目的**: shared_ptr オーバーヘッドを排除。

#### Step 4.1〜4.3: VariablePtr / ConstraintPtr の型変更 ✅
- `VariablePtr` を `shared_ptr<Variable>` → `Variable*`（生ポインタ）に変更済み
- `ConstraintPtr` を `unique_ptr<Constraint>` に変更済み
- `Model` の Variable 保持は `vector<unique_ptr<Variable>>` に変更済み

#### Step 4.4: 制約内部の VariablePtr メンバ除去 ✅ (2026-02-28)

全制約クラスから VariablePtr メンバ変数を除去:

- **基底クラス**: `variables()` pure virtual 削除、`is_satisfied(const Model&)` を基底に汎用実装（`on_final_instantiate` 経由）
- **comparison** (9クラス): `x_`, `y_`, `b_`, `m_` 除去。ID メンバ (`x_id_` 等) のみ保持
- **arithmetic** (3クラス): `x_`, `y_`, `z_` 除去
- **logical** (3クラス): `vars_`, `r_`, `pos_`, `neg_`, `var_ptr_to_idx_` 除去。`can_satisfy(size_t)` 等の no-Model 版メソッドも削除
- **global** (20クラス): `vars_`, `index_var_`, `result_var_`, `m_`, `x_`, `b_`, `index_`, `array_`, `result_`, `var_ptr_to_idx_` 除去
  - DisjunctiveConstraint のタスクヘルパーに `const Model&` 引数追加
  - DiffnConstraint の `propagate_pairwise_direct` に `Model&` 引数追加

**presolve での Variable アクセス**: `model.variable(id)->` で Variable* を取得しドメイン直接操作（presolve では許可）

---

### Phase 5: Trail 統一（中優先、Phase 1 以降いつでも） ✅ 完了

**目的**: Trail を save_point キーで統一管理。backtrack 時の効率化。

**実施結果**:
- `var_trail_`、`constraint_trail_`、`dirty_constraint_trail_` の3本を save_point キーで統一
- `ConstraintTrailEntry` で全制約の rewind 状態を収容
- `Model::rewind_to(save_point)` で3本の Trail を一括巻き戻し

---

### Phase 6: Domain 表現の改善（低優先、いつでも開始可）

#### Step 6.1: BoundsDomain の unordered_set → flat hash / sorted vector
**対象**: `include/sabori_csp/bounds_domain.hpp`

#### Step 6.2: Bool 変数の最適化（2bit 表現）
- `BoolDomain` モードを Domain に追加（min=0, max=1 時に自動選択）

**リスク**: 低。局所的な最適化。
**検証**: 全テスト + Bool 変数を多用する問題のベンチマーク

---

### Phase 7: メモリ管理の改善（最低優先、Phase 4 完了後） ✅ 完了

**実施結果**:
- `ConstraintPtr` = `unique_ptr<Constraint>`（shared_ptr から変更済み）
- `Variable` 保持 = `vector<unique_ptr<Variable>>`（shared_ptr から変更済み）
- `constraint_ptrs_`（生ポインタキャッシュ）で繰り返し dereference を回避

---

## 依存関係

```
Phase 0 ✅ ─→ Phase 1 ✅ ─→ Phase 4 ✅ ─→ Phase 7 ✅
                 │
                 ├─→ Phase 2 ✅
                 │
                 └─→ Phase 5 ✅

Phase 3 ✅
Phase 6 ⬜（Phase 0 以降いつでも）
```

## 進捗サマリ

| Phase | 状態 | 内容 |
|-------|------|------|
| 0: 安全基盤 | ✅ 完了 | baseline 記録・テスト整備 |
| 1: Var/VarData 修正 | ✅ 完了 | debug assertion・propagation API 分離・var_ids_ref |
| 2: Solver 分解 | ✅ 完了 | NoGoodManager・VariableSelector・RestartController・init_search |
| 3: 制約レジストリ | ✅ 完了 | model.cpp 1370→366行、ConstraintRegistry + 45ファクトリ |
| 4: shared_ptr 除去 | ✅ 完了 | VariablePtr→生ポインタ、ConstraintPtr→unique_ptr、制約内部の VariablePtr メンバ全除去 |
| 5: Trail 統一 | ✅ 完了 | save_point キーで3 Trail を統一管理 |
| 6: Domain 改善 | ⬜ 未着手 | BoundsDomain の unordered_set 最適化・BoolDomain 未実装 |
| 7: メモリ管理 | ✅ 完了 | unique_ptr 化・constraint_ptrs_ キャッシュ |

**残作業**: Phase 6 の Domain 最適化のみ

## 鉄則

1. **1ファイルずつ変更 → ビルド → テスト**。バルク置換は禁止。
2. **各 Phase 完了時にベンチマーク**。性能劣化があれば即修正。
3. **各 Step は独立してコミット可能**。途中で中断しても壊れない。
4. **Phase の順序は依存関係以外は柔軟**。コンテスト準備と並行可能。
