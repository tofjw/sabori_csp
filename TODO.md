# TODO

## 直近の優先タスク (高)

- [x] **arithmetic-target で false-optimal を返す correctness バグ** (2026-05-01 発見・修正)
  - **真因**: `IntDivConstraint::propagate_bounds` (および `presolve`) の forward bound 計算で、`y_min < 0` のときのみ `update_z(y_min)` を呼んでいた。y が完全に正 (例: `[814, 8140]`) のとき、最小の y 端点が忘れられ、`x/y_max` だけで z の上限を計算してしまう
    - 例: x=814, y∈[814, 8140] のとき、正しい z 範囲は [0, 1] (=x/y_max..x/y_min) だが、誤って [0, 0] と計算
    - 結果として z=1 が解になる経路 (本問題の最適解 obj=5) を伝播段階で除外
  - **修正** (`src/core/constraints/arithmetic.cpp`):
    ```cpp
    // 修正前:
    if (y_min < 0) update_z(y_min);
    if (y_max > 0) update_z(y_max);
    // 修正後:
    if (y_min != 0) update_z(y_min);
    if (y_max != 0) update_z(y_max);
    ```
  - 再現で確認:
    - 元 FZN (`Testing/arithmetic_target_bug/arith_pinned.fzn`): default オプションで `tree_vals[1]=814, _objective=5, ==========` を返すようになった
    - フル MZN (`mznc2022_probs/arithmetic-target/814_with_1_2_4_6_6_7_8_9.json`): `_objective=5` で OPTIMAL 確定
    - cpp ユニットテスト 183/183 pass、TSP/int_lin_eq_subst の FZN 統合テスト pass
  - 既知の副作用:
    - `tests/fzn/constraints/int_div/backward_y.expected` は最初の解として `x=11, y=3, z=3` を期待しているが、修正後は `x=10, y=3, z=3` (どちらも有効解)。`-a` で全 7 解は変わらず正しい。`.expected` 自体は ctest から呼ばれていないため放置でよいが、必要なら更新

- [ ] **routing-flexible OPTIMAL 退行 (lin_le8)**
  - lin_le3/5/6/7 では OPTIMAL 化できていたが lin_le8 で SOL fail に退行
  - lin_le5 と lin_le8 の差分から bump 設定境界を特定
- [x] **n_bids カウンタのトレイル化 (one_hot_channel)** — 2026-06-16 計測の結果 **revert（不採用）**
  - 仮説: `bump_activity` の呼び出し頻度が高く、O(N) の n_bids 計算がホットパス → トレイル付きカウンタで定数倍削減
  - 検証: `benchmarks/minizinc_challenge/bench_one_hot_channel.py` を新設（`-v` で one_hot_channel 生成問題を自動検出 → before/after 2 バイナリ A/B）
  - 結果: 全 14 年・112 問で **健全性 112/112 OK**（探索木不変）だが性能効果なし。
    overall x1.000（faster 58 / slower 54）、完走>1s の 20 問では x0.991（after slower 13 / faster 6）と
    **むしろ微負**。「ホットパス」とされた O(N) ループは実際にはホットでなく（`holes_>0` 分岐が稀／
    各 channel の bool 数が小さく conflict 解析コストに埋もれる）、トレイル維持コストが上回った。
  - 対応: commit 0fcd895 の差分（trail_ / uninstantiated_b_count_ / rewind_to / on_instantiate 維持）を
    main 上で外科的に revert。ctest 248/248 pass。詳細は docs-dev/work-log/2026-06-16.md

## 将来の分析 (外部結果待ち)

- [ ] **MiniZinc Challenge 2026 結果が出たら、得意不得意を問題クラス別に分析** (2026-06-28 起票・エントリー済/結果待ち)
  - 第三者の公式環境での計測なので、自前ベンチ (30s wall-clock・単一シード・目的値判定) の
    run 揺れ/自己測定バイアスを外して「得意不得意の境界」を問題クラス単位で読める
  - 勝敗を問題クラスでマップし、記事の中心テーゼ「論理 vs 傾向」に紐づけて解釈:
    tendency が当たるクラス (設計/割当系) で互角 = 裏付け / 強学習が決定打のクラス
    (tight な組合せ・一部スケジューリング) で届かない = 射程ガード「測っていない」領域の外部確認
  - 注意: Challenge は objective スコア (フィールド相対) で順位がつくので、記事で実証した
    「直接対決 vs vs-フィールド」の指標乖離 (probe の例) が効く。順位だけでなく勝敗の中身を見る
  - 関連: `articles/search-algorithm-explained.md` §7・射程ガード、memory `mznc2026-third-party-analysis`

## 調査タスク (中)

- [ ] **fox-geese-corn (MAX) obj 退行**: 108→92 が lin_le6/7/8 で連発。MAX 方向の bump 量が依然強い可能性
- [ ] **ma-path-finding (MIN) obj 退行**: 112→177 が lin_le 系全般で継続。`logic` 単体でも 112 から SOL 化、回帰原因不明
- [ ] **oh7 残課題 4 件の構造分析**: solbat sb_12 / minimal-decision-sets / median-string / sdn-chain。solbat sb_12 と sb_13 の方向逆転がインスタンスサイズ依存か検証
- [ ] **connect (MIN) の改善余地**: lin_le6 (5.22M) / lin_le7 (4.04M) ほどには lin_le8 (13.5M) が伸びず。lin_le8 の安定性を保ちつつ取り込めるか

## 機能追加 (低)

### set 関連の制約の追加

**対象問題**:
- gt-sort (MiniZinc Challenge 2025)
- skill-allocation (MiniZinc Challenge 2025)

**必要な制約** (gecode FlatZinc から抽出):
- [ ] `set_card(set, int)` - 集合の基数（要素数）
- [x] `set_in(int, set)` - 要素が集合に含まれるか（実装済み: range/集合リテラルへ分解。2026-06-16 に
      `tests/fzn/constraints/set_in/` で SAT/集合リテラル/UNSAT をテスト追加）
- [ ] `set_union(set, set, set)` - 集合の和
- [x] `set_in_reif(int, set, bool)` - set_in の reified 版（実装済み: range→IntLeReif×2+And,
      集合→IntEqReif群+Or。2026-06-16 に `tests/fzn/constraints/set_in_reif/` で両極性/UNSAT をテスト追加）
- [ ] `array_var_set_element(int, array[int] of var set, var set)` - 集合配列の要素アクセス

**パーサー対応**:
- [x] 集合リテラル `{...}` の構文サポート（skill-allocation で必要）
  - 例: `constraint set_in_reif(X,{56,58},B)` — 動作確認済み（set_in/set_in_reif テストでカバー）

**備考**:
- 現在は redefinitions.mzn で整数制約に分解されているため、set 制約なしでも動作する
- ただし分解による性能低下あり（skill-allocation 最小インスタンスで10秒以内に解が出ない）
- ネイティブ set 制約を追加することで性能向上が期待できる

### その他

- [ ] **`int_ne_reif` 系への同種集約**: one_hot_channel と同じ構造 (定数値 reif の x への集約) で適用余地
- [ ] **ATSP ベンチで presolve 高速化の効果計測**: `IntLinEqConstraint::presolve` / `IntLinLeConstraint::presolve` の `remove_below`/`remove_above` 一括化の効果測定

## コードクリーンアップ (低) — dead code 調査結果 (2026-06-01)

調査範囲: `src/`, `include/`, `python/` (バックアップ `*~` と `#...#` を除外)
クロスリファレンス: 上記 + `tests/`, `parser.yy`, `lexer.ll`, `*.py`, `_bindings.cpp`

### A. 中身が空の関数 (no-op) — 削除不要

両方とも意図的なデフォルト no-op。

- `include/sabori_csp/constraint.hpp:267` `virtual void rewind_to(int) {}` — 基底クラスのデフォルト no-op (複数サブクラスでオーバーライド)
- `include/sabori_csp/sparse_domain.hpp:55` `void restore_trail(size_t) {} // no-op` — SparseDomain は trail データ不使用

### B. 使われていない関数 — 23 件

#### B-1. 完全 dead な内部ヘルパ (削除推奨)

- [x] `src/core/variable_selector.cpp:14` `select_weighted_by_activity_softmax(...)` — 削除済み (2026-06-01)。`<cmath>` include も併せて除去。192/192 テスト pass

#### B-2. ヘッダ宣言+cpp 定義の組で呼び出しゼロ (13 件)

- [ ] `src/core/nogood_manager.cpp:194` `NoGoodManager::add_unit_nogood`
- [ ] `src/core/constraints/global/int_lin_eq.cpp:312` `IntLinEqConstraint::check_feasibility`
- [ ] `src/core/community_analysis.cpp:392` `CommunityAnalysis::top_communities`
- [ ] `src/core/community_analysis.cpp:396` `CommunityAnalysis::community_vars`
- [ ] `src/core/model.cpp:541` `Model::constraint_trail_size`
- [ ] `src/core/model.cpp:565` `Model::sync_to_domains`
- [ ] `src/core/constraints/global/disjunctive.cpp:134` `DisjunctiveConstraint::count_free_bits`
- [ ] `src/core/solver.cpp:1453` `Solver::set_activity` — 要確認 (ウォームスタート系 API)
- [ ] `src/core/solver.cpp:1479` `Solver::get_activity_map` — 要確認
- [ ] `src/core/solver.cpp:1498` `Solver::set_hint_solution` — 要確認
- [ ] `src/core/constraint.cpp:31` `Constraint::set_var_ids`
- [ ] `src/fzn/model.cpp:49` `fzn::Model::set_var_upper_bound`
- [ ] `src/fzn/model.cpp:61` `fzn::Model::set_var_lower_bound`

#### B-3. ヘッダで inline 定義された未使用 getter/setter (9 件)

- [ ] `include/sabori_csp/constraints/global.hpp:2275` `b_ids()`
- [ ] `include/sabori_csp/variable_selector.hpp:111` `community_first_var()`
- [ ] `include/sabori_csp/variable_selector.hpp:112` `set_community_first_var()`
- [ ] `include/sabori_csp/fzn/model.hpp:145` `constraint_decls()`
- [ ] `include/sabori_csp/community_analysis.hpp:120` `is_collecting_stats()`
- [ ] `include/sabori_csp/one_hot_channel_aggregator.hpp:29` `set_min_group_size()`
- [ ] `include/sabori_csp/model.hpp:290` `unset_defined_var()`
- [ ] `include/sabori_csp/model.hpp:305` `set_randomize_value_order()`
- [ ] `include/sabori_csp/model_simplifier.hpp:54` `substitutions()`

#### B-4. 重複宣言 (要確認)

- [x] `parse_string` 完全削除 + `parse_file` 宣言を `fzn_parser.hpp` に一本化 (2026-06-01)
  - 削除: `include/sabori_csp/fzn/model.hpp` の `parse_file`/`parse_string` 宣言 (15行)
  - 削除: `src/fzn/fzn_parser.hpp` の `parse_string` 宣言、`yy_scan_string`/`yy_delete_buffer`/`YY_BUFFER_STATE`/`yy_buffer_state` 前方宣言
  - 削除: `src/fzn/fzn_parser.cpp` の `parse_string` 定義 (17行)
  - 192/192 テスト pass

### 削除順序の推奨

1. B-1 (匿名 namespace ヘルパ) — 影響範囲が単一 TU 内
2. B-3 (inline getter/setter) — 削除は機械的
3. B-2 のうち Solver 永続化系 (`set_activity` / `get_activity_map` / `set_hint_solution`) を除いたもの
4. Solver 永続化系は意図確認後に判断
5. B-4 は parse_string の用途を確認してから対応

## 外部報告 (完了)

- [x] MiniZinc 1249 flatten SEGV (peaceable_queens 等) → 報告済み・GitHub で修正済み
- [x] minizinc cwd 依存の `X_INTRODUCED_…` 連番ブレ → `bench_compare.py` 側で吸収 (cwd=BASE_DIR 固定)

## 完了済みアーカイブ

### 2026-06-16: テスト不足の解消（カバレッジ監査 + 死テスト対策）
- [x] **golden master に corpus 監査機構を追加**: `tests/golden/excluded.txt`（理由付き除外マニフェスト）
      + `run_golden.sh audit`（全 .fzn は corpus か excluded のどちらかに必ず属す）。Error 黙殺による
      死テスト化を恒久防止。除外は presolve UNSAT の `int_lin_eq_subst/unsat_{eq,le}` の2件のみ。
- [x] **死テスト復活**: `int_element_monotonic` 6件は inline 整数リテラル配列（parser 未対応）で
      Error→黙殺され一度も実行されていなかった。named array 形式へ書き換えて実テスト化。
- [x] **高リスク未テスト制約**: `int_one_hot_channel`（C++ ブルートフォース 310 assertion 新設）/
      `int_lt_reif`（FZN 4件）。
- [x] **UNSAT 欠落の解消**: alldifferent_except_0 / disjunctive_strict / strictly_increasing / int_div /
      array_var_int_element / array_bool_element / array_var_bool_element / bool_not / bool2int /
      bool_xor / array_bool_xor / bool_lt_reif / maximize / alldifferent / int_eq / int_ne / int_lt に追加。
- [x] **境界欠落の解消**: int_times（zero/neg_neg/reverse_zero）/ 新規 dir int_le, int_min, int_lin_le, circuit /
      array_int_maximum・minimum（single/negative）。
- [x] **registry 死角の解消**（Codex 監査の着眼を完遂）: set_in / set_in_reif / bool_lin_eq / bool_lin_le /
      inverse_offsets は fzn dir 皆無で dir ベース監査では不可視だった。各 SAT/UNSAT/境界を新規追加。
- [x] **死コード削除**: `connected.cpp`（クラス宣言なし・CMake未登録・未コンパイルの放棄 WIP）+ 対の
      `share/minizinc/fzn_connected.xmzn`。
- [x] `.gitignore` に `*.so`（pybind11 拡張のビルド成果物）を追加。
- 残: 低リスクの bool alias（bool_le/lt/ne, bool_eq_reif/imp, bool_le_reif）は make_int_* の単純 alias で
      伝播本体は検証済みのため未追加。set_card / set_union / array_var_set_element は未実装で別途。
- golden corpus 182 → 242（+60 fzn）。ctest 248/248、Python 100/100、golden 全 green。

### 2026-05-01: `is_initially_inconsistent_` の削除
- [x] フラグ・getter・setter・`check_initial_consistency()` を削除 (誰も読んでいなかったデッドステート)
- [x] 全 12 箇所の `set_initially_inconsistent(true)` 呼び出しを削除 (circuit / inverse / table / int_lin_eq / regular)
- [x] テストの `is_initially_inconsistent()` チェックを `prepare_propagation(model)` の戻り値検証に置換
- [x] **副次バグ修正**: `TableConstraint::prepare_propagation` で `arity=0 / num_tuples=0` の早期 return が無く SEGV していたバグを修正

### 2026-05-01: int_lin_le bump_activity チューニング
- [x] lin_le1〜lin_le8 を順次ベンチ比較、lin_le8 を採用 (コミット c099464)
- [x] init_activity を `|coef|/log(var_size)` で重み付け
- [x] bump_activity を再有効化、`|coef| × bound_shrinkage / var_size` で配分
- [x] is_easy_ フラグで自明制約をスキップ

### 2026-04-28: IntOneHotChannel 制約追加と bump_activity チューニング (oh3〜oh7)
- [x] `IntOneHotChannelConstraint` 新規追加 (定数値 `int_eq_reif` 群を集約)
- [x] `OneHotChannelAggregator` を Solver::presolve 内で発火
- [x] `find_value_index` を offset_ + contiguous_ で O(1)/O(log N) に最適化
- [x] holes_ 別の init_activity / bump_activity 配分 (exhaustive vs partial)
- [x] IntEqReifConstraint::bump_activity 追加 (y 定数時に非定数側 full bump)
- [x] `int_one_hot_channel.cpp` の診断 printf 削除済み
- [x] `init_activity` の `auto var_size` shadowing 解消済み
- [x] bench_compare.py の minizinc -t timeout 追加と cwd 固定

### 2026-04-03: bool_clause / arithmetic 系の調整
- [x] bool_clause の bump_activity カスタマイズは不要と判断（デフォルトで十分）

### 2026-03-31: bump_activity 改善
- [x] ArrayVarIntElement の bump_activity 改善 (コミット eb678cb)
- [x] IntLinEqConstraint::bump_activity 改善 (コミット f16bd35, dbfcc17)

### 2026-02-12: protein リグレッション解決
- [x] IntLinEq の propagation で SoA 配列と Domain の同期ラグ修正
- [x] propagate_lower/upper_bounds, prepare_propagation で `vars_[j]->is_assigned()` を使用

### 2026-02-06: 制約伝播リファクタリング
- [x] Phase 1: `presolve()` → `initialize()` リネーム
- [x] Phase 2: `propagate()` を `enqueue_xxx` 方式に変更
- [x] Phase 3: `initial_propagate()` 書き換え、`propagate_all()` 削除
- [x] Phase 4: 全テスト実行・修正
- [x] Phase 5: `sync_after_propagation()` 削除
- [x] FlatZinc 出力形式の修正（`true`/`false`、`==========` 仕様準拠）
- [x] FlatZinc 出力形式テスト追加

### 2026-02-06: メソッドリネーム + Phase 1 後の内部状態再構築
- [x] `Constraint::presolve()` → `prepare_propagation()` (内部構造の初期化)
- [x] `Constraint::initial_propagate()` → `presolve()` (探索前の初期伝播)
- [x] `Model::presolve()` → `prepare_propagation()`
- [x] `Solver::initial_propagate()` → `presolve()`
- [x] Phase 1 後に `model.prepare_propagation()` を追加（内部構造の再構築）
- [x] `solve()` / `solve_all()` の冗長な準備・矛盾チェックを `Solver::presolve()` に統合

### 2026-02-06: reif 制約 presolve バグ修正
- [x] `IntLinEqReifConstraint::presolve()` — キャッシュ値依存 → ドメインから毎回計算
- [x] `IntLinNeReifConstraint::presolve()` — 同上
- [x] `IntLinLeReifConstraint::presolve()` — 同上
- [x] 全制約の `presolve()` がキャッシュに依存していないことを確認済み

### 2026-02-06: Solver リファクタリング + バグ修正
- [x] `create_variable` 便利オーバーロード追加、呼び出し側簡潔化
- [x] `propagate` → `propagate_instantiate` リネーム
- [x] `process_queue` リファクタ（`need_propagate` フラグ廃止）
- [x] `propagation_queue_` 廃止、Model の `pending_updates_` に一本化
- [x] solver.cpp 内の `var->` アクセスを `model.*` SoA アクセスに置き換え
- [x] `enqueue_*` の O(n) 重複チェック削除
- [x] TSP 無限ループバグ修正（`int_lin_eq::propagate()` 内の SoA 不整合）

### 性能改善 (2026-02 以前)
- [x] SoA と Domain 直接操作の不整合解消
- [x] `IntLinEqConstraint::presolve()` 高速化 — `Domain::remove_below`/`remove_above` 一括除去メソッド追加、O(k×n) → O(n)
- [x] `IntLinLeConstraint::presolve()` にも同様の `remove_below`/`remove_above` 最適化を適用

### 2026-04-28: timeout / propagation バグ修正
- [x] `process_queue` を `PropagationResult { Ok, Conflict, Stopped }` 戻り値に変更し、SIGALRM 後の偽 UNSAT を解消 (コミット 1997ca4)
- [x] `BoolClauseConstraint::on_last_uninstantiated` を活性化、watch ベースで O(1) 化 (コミット 24be388)
