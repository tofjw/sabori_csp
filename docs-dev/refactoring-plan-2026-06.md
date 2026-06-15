# sabori_csp 全面リファクタリング計画 v2 (2026-06)

作成: 2026-06-10
状態: ドラフト（フェーズ0着手前）
前史: [refactoring-plan.md](refactoring-plan.md)（2026-02 のリアーキテクチャ。Phase 0〜5, 7 完了、Phase 6 = Domain 表現のみ未着手）

## 前回計画との関係

2026-02 計画で **解消済み**（本計画では扱わない）:
- Variable/VarData 二重 API（presolve/propagation の API 分離 + アサーション）
- 制約登録の if-else チェーン（ConstraintRegistry 化）
- shared_ptr 過剰使用（生ポインタ/unique_ptr 化）
- Model 側 Trail の統一
- `domain().values()` のヒープ確保・`pending_updates_` の deque・`is_instantiated()` 非 inline（2026-06-10 検証で解消済みを確認。2026-02-06 プロファイルメモは一部失効）

**残った/その後発生した問題**（本計画の対象）:
1. 旧 Phase 6（Domain 表現: `unordered_set` 除去・BoolDomain）が未着手のまま
2. solver.cpp が 1,308 行 → **1,810 行に再肥大化**（community analysis / mode_reward RL / gradient / find_all 追加分が無計画に堆積。verbose 統計分岐の複製がファイルの3〜4割）
3. 制約層のコピペ重複は前回スコープ外で手つかず（約18,000行中 1,400〜1,800 行が削減可能）
4. テスト資産の形骸化（fzn ペアの 48/54 フォルダが ctest 未登録）
5. リポジトリ衛生（in-source build 残骸 214 ファイルが git 追跡中、等）

## 現状診断（2026-06-10 調査）

### リポジトリ衛生
- in-source build 残骸 **214 ファイルが git 追跡中**（`tests/cpp/CMakeFiles` 181件、`src/**/CMakeFiles`、`Makefile`×5、`cmake_install.cmake`×5）
- `src2/`（古い in-source build の失敗物 828KB）、`Testing/`、`*~`/`#*#`/`.bak`/`.cpp-new` 約30個が放置
- `benchmarks/minizinc_challenge/` 直下に実験フォルダ 237 個 + ベンチ HTML が無秩序に蓄積（2.1GB、未管理）
- CLAUDE.md が存在しない `tests/fzn/run_tests.py` を参照。TESTING.md に Python テスト手順なし

### テスト
- `tests/fzn/constraints/` 54 フォルダ・約190ペア中、**ctest から実行されるのは 6 フォルダのみ**（残り約165ペアは飾り）
- .expected が「特定の1解」を期待する形式で、ヒューリスティクス変更に脆い（diffn の3ペアは既に現行バイナリと不整合）
- `tests/cpp/test_global_constraints.cpp` が 4,749 行・128 TEST_CASE の単一ファイル
- bench_*.py が **31 本**あり、`cleanup_stale_processes`/`run_solver`/`judge_winner`/`generate_html` 等がコピペ重複（重複率 50〜90%）

### 制約層の重複（src/core/constraints 約18,000行）
- **int_lin_* 系 7 ファイル(3,117行)**: potential 管理（`current_fixed_sum_`/`min_rem_potential_`/`max_rem_potential_`）・差分更新・制約内部 trail がほぼ同一実装（〜800行重複）
- **reified ボイラープレート**: b確定分岐 / b推論 / presolve 骨格が7ファイルで重複（〜650行）
- **制約内部 trail パターン**（save_point チェック + push + rewind_to）が6ファイル以上で複製 ※Model 側 Trail は統一済みだが制約内部状態の trail は各自実装のまま
- **sparse set プール**が all_different / all_different_except_0 / circuit で三重実装。`pool_sparse_` は `unordered_map`
- **presolve(直接操作)/propagate(enqueue) の二重実装**（diffn `propagate_pairwise` / `_direct` 等）
- comparison.cpp のドメイン交差・双方向伝播が3箇所以上で複製（〜290行）
- `global.hpp` が 2,420 行の単一ヘッダ（26クラス）
- 「現在未使用」の AllDifferentGAC が常時コンパイル対象

### コア層
- solver.cpp: `search_with_restart` と `search_with_restart_optimize` に restart 制御・mode_reward 更新が二重実装。SetMin/SetMax/RemoveValue の各ディスパッチに verbose/非 verbose の複製ブロック
- Domain/SparseDomain/BoundsDomain: 継承でも variant でもないフラグ分岐で、`contains`/`remove_*`/走査のロジック複製。`removed_set_` が `unordered_set`（旧 Phase 6 残件）
- コールバック署名が `var_idx` と `internal_var_idx` を両方引き回す
- `remove_value()` 境界更新の線形スキャン（要再計測）

---

## 全体方針（鉄則）

1. フェーズは「**安全網 → 衛生 → 機械的整理 → 構造変更 → 性能**」の順。挙動に触る前に検証手段を整える
2. **1コミット = ビルド + 全テスト green**。バルク sed 置換は禁止（ビルド破壊実績あり）。1ファイルずつ変更 → ビルド → テスト
3. **挙動に影響しうる変更はベンチマークゲート必須**: `bench_alldiff.py` / `bench_diffn.py` / `bench_circuit.py` で Wins 同等以上・Timeouts 同等以下。マージナル差はノイズなので、際どい場合は同一 fzn の直接実行で決定論比較
4. **純リファクタの検証はゴールデンマスター**: 代表 fzn の `-a` 全解出力 + 探索統計の完全一致
5. false UNSAT / false OPTIMAL が最悪の事故。伝播ロジックに触る変更には**ランダム全解数のブルートフォース照合テスト**を併設（alldiff bounds(Z) / circuit SCC で実績ある方式）
6. ベンチ実行前に `pkill -x fzn_sabori; pkill -x fzn-cp-sat; pkill -x minizinc`（`pkill -f` 禁止）。並列4まで
7. 各フェーズ末に work-log へ記録。フェーズ途中で停止しても資産が残る構成にする

---

## フェーズ0: 検証基盤（0.5〜1日）

**目的**: 以降の全フェーズの合否を機械的に判定できる状態を作る。

- [ ] **ゴールデンマスター** `tests/golden/run_golden.sh`:
  - 代表 fzn 約20本（制約カテゴリ別 + tsp / accap 等）の `-a` 全解出力と `-v` 統計（ノード数・伝播数）を記録し、前後比較できるようにする
- [ ] **再プロファイリング**: gprof でホットスポットの現状を取り直す（2026-02-06 データは一部失効済み。クリーンリビルド + 内蔵タイムアウトフラグ使用）。結果を docs-dev に記録し、フェーズ5の優先順位を確定する
- [ ] ベンチ3本 + `bench_compare.py`（全問題セット）の baseline を `docs-dev/benchmarks-baseline-20260610.md` に固定記録
- [ ] ctest 実行時間の現状記録

**完了条件**: run_golden.sh が現行バイナリで green、プロファイル結果文書化。

## フェーズ1: リポジトリ衛生（0.5日、挙動リスクゼロ）

- [ ] git 追跡中のビルド成果物 214 件を `git rm --cached`
- [ ] `.gitignore` 拡充: `**/CMakeFiles/`, `src/**/Makefile`, `cmake_install.cmake`, `src2/`, `Testing/`, `.claude/`, `.codex`, `*.bak`, `*.cpp-new`, `benchmarks/minizinc_challenge/20*/`, `benchmarks/minizinc_challenge/mznc*_probs/`, `benchmarks/minizinc_challenge/*.html`
- [ ] CMakeLists.txt 冒頭に in-source build 拒否ガード
- [ ] `src2/` 物理削除。`*~`/`#*#`/`.bak`/`.cpp-new` 全削除
- [ ] ベンチ実験フォルダ 237 個の整理方針を決める（`archive/` 移動 or 削除。**要ユーザー判断**）
- [ ] ドキュメント整合: CLAUDE.md のテストコマンド修正、TESTING.md に Python テスト手順 + fzn ペアの実行状況を明記
- [ ] TODO.md の完了項目を work-log へ移す

**完了条件**: 追跡ジャンク0。クリーンクローン + out-of-source build が一発で通る。

## フェーズ2: テスト安全網の拡充（2〜4日）

**進捗 (2026-06-14)**: golden master が先行構築され ctest 登録済み（`golden_master`, ラベル
`golden`, ~180 fzn の `-a -s` 出力照合）。Python テストも ctest 統合済み（`python_bindings`）。
→ fzn の**リファクタ不変性網は完成**したため、堅牢ペアランナー(#1/#2)は **deferred**（下記理由）。

- [x] Python テストの ctest 統合（`BUILD_PYTHON_BINDINGS` 時、`Python3_EXECUTABLE`+`PYTHONPATH`）→ commit 71d1124
- [x] golden master を ctest 登録（`FZN_SABORI` で実バイナリ注入）→ commit 112256c
- [ ] `test_global_constraints.cpp`(4,749行) を制約カテゴリ別 5〜6 ファイルに分割（機械的移動のみ）
- [ ] **ベンチ共通ライブラリ** `lib_benchmark.py` 抽出、主要4本(alldiff/diffn/circuit/compare)先行移行
- [ ] CLAUDE.md / TESTING.md のテストコマンドを最終形に更新（一部済: run_tests.py 誤参照修正・golden/python 追記）

### deferred: 堅牢ペアランナー（`run_pairs.py`）— なぜ将来なお必要か
golden は「**挙動が変わったか**」を見る不変性網であり、純リファクタ(Phase 3-6)には十分。
だが以下は golden では担保できず、堅牢ランナーが必要になる:
1. **正しさの担保**: golden は現状出力をそのまま凍結する＝**潜在バグも「正」として固定**する。
   `.expected`(183, 手書き) や充足検証は「意図する正解」を符号化でき、潜在バグを検出しうる。
2. **ヒューリスティクス変更への耐性**: 変数選択・restart 等を変えると golden は全面赤になり
   無力（どれが真のリグレッションか分からない）。status/目的値/充足検証なら「正しさ」で判定でき、
   探索チューニング時のリグレッション検出に使える。
3. **非リファクタ変更**（新制約・伝播強化）の受け入れテストとして、特定解一致でなく充足性で見たい。

→ **着手の引き金**: Phase 3-6 完了後にヒューリスティクス/伝播の改良フェーズへ移る時、
   または golden が脆すぎて回帰判定に使えない場面が出た時。設計は計画初版の3形式
   （ステータスのみ / 最適化 obj+`==========` / satisfy 充足検証）を踏襲。`.expected`(183) は
   その時に「廃止 or 堅牢形式へ移行」を決める（現状は golden と重複のため保留）。

**完了条件（改）**: ctest が C++ + fzn(golden) + Python を含めて green。最大テストファイル 1,500 行未満。

## フェーズ3: 機械的な重複削減・死コード削除（1週間、挙動不変）

- [x] **TrailManager<State> テンプレート**: 制約内部 trail（save_point チェック/push/rewind）を共通化。`ConstraintTrail<State>`（`include/sabori_csp/constraint_trail.hpp`）として int_lin_* 7制約 + all_different 系2制約に適用（2026-06-15, commit b803462/65eb553）。circuit は trail が union-find と密結合のため見送り
- [x] **SparseSetPool クラス**: all_different / except_0 のプール実装を `SparseSetPool`（`include/sabori_csp/sparse_set_pool.hpp`）に統一。`pool_sparse_` の `unordered_map` は offset 付きフラット配列（広域は map フォールバック）に自動選択化（2026-06-15, commit 5256c19）。circuit は元からフラット配列（unordered_map 不使用）のため対象外
- [x] **global.hpp (2,420行) 分割**: alldifferent / linear / reified / scheduling / element / graph 等のヘッダへ（commit 0ae11c6）
- [x] **solver.cpp の verbose/統計複製の排除**: コールバック呼び出し + 統計記録をヘルパへ一本化（commit 35ccc92）
- [x] **死コード処分**: 呼び出し元のない private メソッド3件を削除（remove_from_pool ×2 /
  check_feasibility, 2026-06-15 commit 9ccab04）。
  **AllDifferentGAC は削除しない**（feature/lcg で自動切換え(adaptive GAC dispatch)を検討中のため保持。2026-06-14 ユーザー判断）。
  bump_activity の `#if 0` ブロック群は activity ヒューリスティクスの A/B 実験アーカイブ
  （日付ラベル付き・active なチューニング領域）のため停滞死コードではなく保持。

**完了条件**: ゴールデンマスター完全一致 + ctest green + ベンチ3本ゲート（統計ヘルパは hot path のため）。
**削減見込み**: 600〜800 行。

## フェーズ4: 制約層の構造化（2〜3週間、中リスク）

- [~] **LinearConstraintBase**: 基底クラスを新設し、コンストラクタの線形項集約（係数合算・
  係数0除外・unique_vars 構築）と係数列 coeffs_ を `aggregate_terms` に共通化（2026-06-15,
  commit 0bde8a4, 7制約・~80行削減, golden green）。
  **potential 管理(min_rem/max_rem/fixed_sum)の差分更新は基底化しない**: eq=min+max /
  le=min のみ / ne=fixed_sum+unfixed_count と形状が分岐し、かつ on_instantiate/on_set_min/max
  という hot path に介入するため。集約のみで ROI を確保し、伝播セマンティクスは派生に残置。
- [~] **ReifConstraintBase**: reif は false UNSAT 事故リスクがあるため、移行前に
  ランダム全解数照合テストを併設（commit 6957db3, `tests/cpp/test_lin_reif_brute.cpp`,
  eq/ne × b-free/fixed × ±係数, 806 assertion）。
  - [x] **IntLinEqNeReifBase**: eq_reif/ne_reif を述語極性 `negated_` で統一（2026-06-15,
    commit 12aad80, 純net ~470行削減, golden byte一致 + brute 806 green）。b 推論/矛盾検出を
    `reconcile_b()` に集約。le_reif と異なり線形側の刈り込みは無い形状なので統一が綺麗に収まる。
  - [x] **le_reif/le_imp**: 全クラス統一は見送り（le_imp は le_reif の b=1 半分で状態形状が
    異なる: min のみ/max_rem 無し・推論は対偶のみ。統一すると未使用 max_rem 追跡と毎コールバックの
    フラグ分岐を hot path に持ち込むため「hot path に限界的介入をしない」原則に反する）。
    代わりに**共通の刈り込みカーネル `LinearConstraintBase::prune_sum_le` を抽出**し、
    int_lin_le / le_reif / le_imp の3重複（sum<=bound の境界絞り loop）を一本化（2026-06-15,
    commit fe736eb, net -48行, brute 1614 green + golden byte一致）。gt 方向（le_reif のみ）は
    重複が無いため対象外。le 用 brute net は commit 8d555b0 で先行併設。
- [x] comparison.cpp のドメイン交差・双方向伝播ヘルパ抽出（presolve + propagation 両側完了）
  - [x] **presolve ヘルパ抽出**: ドメイン交差（x==y）を `intersect_eq`（IntEq / IntEqReif b=1 /
    IntEqImp b=1 の逐語3複製）、境界対絞り込み（lo<=hi / strict）を `enforce_le`（IntLt / IntLe /
    IntLeReif b=1=x<=y・b=0=y<x strict）に一本化（2026-06-15, commit 続き, net -69行, golden byte一致 +
    scalar comparison brute net 2910 assertion）。安全網先行: `tests/cpp/test_cmp_reif_brute.cpp`。
  - [x] **propagation 側**（2026-06-15 完了）: x==y の逐語複製のみ一本化。
    `eq_fix_mutual(model,x,y)→bool`（相互固定: IntEq / IntEqReif b=1 の on_instantiate 2箇所）+
    `eq_propagate_bound(model,changed,x,y,bound,is_min)`（bounds 相互伝播: IntEq / IntEqReif b=1 /
    IntEqImp b=1 の on_set_min/max 計6箇所）。le/lt は offset 差・new_min vs var_min 再読の差で逐語でなく、
    IntEqImp の on_instantiate も else-if 構造が異なるため対象外（presolve ほど逐語的でない方針通り、
    逐語複製のみ集約）。brute net 2924 assertion + golden 182 byte一致。
- [~] **presolve/propagate 二重実装の統一**: ドメイン更新を抽象化（直接 or enqueue を切り替える DomainWriter）。
  - [x] **diffn**: `propagate_pairwise`(enqueue) と `propagate_pairwise_direct`(presolve 直接)を
    accessor ポリシー(`EnqueueAccess`/`DirectAccess`)付きテンプレート `diffn_pairwise<Acc>` に統一
    （2026-06-15, commit 続き, net -23行）。読み出し元（var_data_ / Domain 直接）も accessor に
    含め各経路の参照源を厳密保存（protein 教訓）。テンプレートなので伝播 hot path に仮想呼び出し無し。
    安全網: `tests/cpp/test_diffn_brute.cpp`（strict/nonstrict・0面積免除・強制分離）。golden byte 一致。
  - [x] **disjunctive `set_bits`**: `set_bits`(trail あり) / `set_bits_direct`(presolve, trail なし)の
    timeline bit-mask 幾何を `apply_set_bits(timeline, start, len, trail)` に統一（2026-06-15, net -4行）。
    trail(w) を各語書き込み直前に呼ぶポリシーで、direct は no-op・伝播版は旧値退避ラムダ。
    安全網: `tests/cpp/test_disjunctive_brute.cpp`。golden byte 一致。
  - [-] **disjunctive `update_compulsory_part` は統合しない**（確定判断）: `_direct` 版は CP=∅ 前提・
    trail 無しの**presolve 特化アルゴリズム**で full 版（incremental CP 拡張 + trail）とロジックが
    別物。読み書き方式だけの差ではないため DomainWriter 対象外。
- [x] **コールバック署名整理**: `var_idx`/`internal_var_idx` の引き回しを一本化（4イベント
  コールバックから冗長な `var_idx` を除去し、必要な制約は `var_id(internal_var_idx)` で O(1) 導出）。
  （2026-06-15, commit 4312dda, 48ファイル・注入47箇所, golden 182 byte一致 + ctest 222/222 + python 100）。
  - base 仮想4 + 定義（constraint.hpp/.cpp）、135 override、solver dispatch 7箇所、明示的 base/parent
    呼び出しを更新。`internal_var_idx` を唯一の真実とし、dispatch が不整合ペアを渡す余地を構造的に排除。
    var_idx を使わない hot-path 線形/scheduling コールバックは引数1つ減。
  - **安全網**: 派生 135 宣言は全て既に `override` 付き（当初 grep アーティファクトで「未付与」と誤認 →
    paren マッチで再検証し判明）。基底署名変更がコンパイラで全 override 検証され、grep 完全性でも残存
    旧シグネチャ 0 を確認。
  - **テスト修正の要点**: `test_constraints.cpp` は比較制約に「実グローバル id + dummy internal=0」の
    **不整合ペア**を渡していた（旧コードは var_idx しか見ないので通っていた）。リファクタで internal が
    真実になるため正しい内部 index へ補正（x→0/y→1/m·b→2）。リテラル index を渡す graph/linear/alldiff は
    第3引数を落とすだけ（internal 値保存で挙動不変）。
  - net +39行（注入47 が param 削減を相殺）。目的は行数でなく重複/不整合源の除去。

**完了条件**: ゴールデン一致 + ctest green + ベンチ3本 + `bench_compare.py` 総合非劣化。
**削減見込み**: 1,000 行強。
**進捗 (2026-06-15)**: 実体的 dedup 完了（reif/le/comparison/diffn/disjunctive, ~610行削減・全 golden
byte一致）。brute 安全網を新規4本追加（lin_reif / cmp_reif / diffn / disjunctive, 計 ~2920+ assertion）。
**署名整理も完了**（commit 4312dda, golden 182 byte一致）→ **Phase 4 全完了**。次は Phase 5 または Phase 6。

## フェーズ5: Domain 表現と性能（**設計衛生のみ実施・2026-06-16 クローズ**）

旧計画 Phase 6 の継承。**着手時の再判断**（フェーズ0 の再プロファイル + ユーザー方針）で
「設計衛生の純リファクタのみ実施、性能最適化項目は別イニシアチブへ分離」と確定（2026-06-16）。

### 実施（純リファクタ・golden 182 byte一致）
- [x] **死コード 5 ファイル削除**（commit c2dfedf）: `SparseDomain`/`BoundsDomain`
  （sparse_domain.{hpp,cpp} / bounds_domain.{hpp,cpp}）+ それらからしか include されない
  `var_data.hpp`。実態はライブ実装が `Domain` 1クラス（内部 `bounds_only_` 分岐）のみで、
  これらは全ブランチの全履歴で一度も CMakeLists に登録されたことがない放棄された Domain 分割の試み。
  計画が想定した「Domain/SparseDomain/BoundsDomain のフラグ分岐」は**実在しなかった**。
- [x] **`removed_set_` メンバシップ判定の DRY 化**（commit ac8527b）: bounds-only の
  `removed_set_.find(v)==end()` パターン6箇所を private inline `is_removed()` に一本化。

### 見送り（確定・理由付き）
- [-] **`removed_set_` の `unordered_set`→ビットマップ化**: bounds-only はレンジ>10000 専用で
  上限ガードが無く、巨大レンジ（例 0..2³¹）でビットマップ即確保はメモリ激増（〜268MB/変数）。
  遅延確保の unordered_set の方が最悪ケース安全。計画の「ドメイン幅で自動選択」はフラグ分岐を
  一段増やし衛生目的（分岐削減）に逆行。かつ **profile-20260614 で flat profile 上位に不在＝性能 ROI 低**。
- [-] **BoolDomain / Domain 内部表現の variant 化 / remove_value O(n) / rewind virtual dispatch**:
  いずれも純リファクタでなく性能最適化（挙動変更リスク + 厳格ベンチゲート + gprof 検証が必要）。
  profile-20260614 の結論「性能 ROI は Domain 表現でなく propagator 本体（IntLinEq 差分 / TTEF）と
  核ループ定数倍（process_queue / set_min-max）」より、**Domain 表現刷新を最優先とする根拠は現データに無い**。
  → **別イニシアチブ（性能最適化フェーズ）へ分離**。着手時は再プロファイルで対象を確定すること。

**結論**: Phase 5 の純リファクタ（設計衛生）完了。リファクタリング計画 v2（Phase 0〜6）は**全クローズ**。
残る性能最適化は計画の枠外の独立タスクとして再起票する。

## フェーズ6: solver.cpp 再分解（2週間）

フェーズ3〜5 で薄くなった後に実施。再肥大化（1,308→1,810行）の原因である後付け機能を分離する。

**進捗 (2026-06-15)**: ヘルパ抽出（二重実装解消 + optimize スリム化）に続き、状態クラスタの**クラス化**
を実施（全 commit golden byte一致）。`search_with_restart_optimize` は ~460→~200 行、
`search_with_restart` は ~200→~150 行。solver.cpp 全体は 1,784→1,650 行。

- [x] `search_with_restart` / `search_with_restart_optimize` の共通骨格抽出（restart 制御・cycle 管理・mode_reward 更新の二重実装解消）— 実用上の限界点まで完了
  - [x] **mode_reward EMA 更新 + mix_p 再抽選**（byte一致 32行重複）→ ヘルパ→ ModeRewardPolicy へ移管
  - [x] **restart 共通簿記**（restart_count++ / community report / select_best_assignment / bloom /
    restart pivot / NoGood GC+rebuild / activity 減衰）→ `apply_restart_bookkeeping(model)`。経路差の
    temporal_activity_ リセット・gradient リセットは呼出側に残置
  - [x] **restart 末尾**（mode 再抽選 + shuffle + init_tracking + unassigned_trail クリア）→
    `resample_and_reshuffle(model)`。非対称（temporal リセット/gradient disable）は呼出側に残しフラグ回避
  - [x] **timeout エピローグ**（community report + verbose + sync_nogood_stats）→ `finish_search_on_timeout()`
  - [x] **内側 SAT 処理を focused helper に委譲する対称構造**: optimize=`run_improvement_probe`、
    通常 find_all=`handle_find_all_solution`（`FindAllAction` enum）。両ループが薄く並行に
    （search_with_restart ~141→~85 行）
  - [-] cycle begin/end 構造 / UNKNOWN backtrack プロローグの共通化は見送り（optimize は domain_count 追跡 +
    cycle_interrupted、UNKNOWN は break/return・戻り値・current_decision_ が分岐し、シグナル enum 無しでは
    綺麗に収まらない。2箇所のみで分岐本体も別物のため callback/template 間接化は over-engineering）
- [x] **mode_reward / mix_p 抽選を ModeRewardPolicy クラスへ移管**（`mode_reward_policy.hpp`, header-only）。
  状態(reward/p_idx/mix_p/improvement/max_depth)+抽選ロジックをカプセル化。Solver からは
  `mix_p()` / `note_improvement()` / `observe_depth()` / `update_and_resample(rng)` / `set_fixed()`。
- [x] **gradient ヒューリスティクスを GradientStrategy クラスへ分離**（`gradient_strategy.{hpp,cpp}`）。
  状態(gradient/hint var-dir-ref/eligible/prev solution)+`rebuild_eligible`/`compute`。値順序付けの
  読み出しと散在 reset 5箇所を accessor(`hint_active_for`/`direction`/`ref_val`/`consume_hint`/
  `disable_hint`/`clear`)に集約。
- [x] **improvement probe を分離**: `run_improvement_probe(model, callback, root_point)` が `ProbeAction`
  enum（Continue / BreakInnerLoop / ReturnOptimal）を返す（handle_ascent の AscentAction と同パターン）
- [x] **単体テスト追加**: `test_mode_reward_policy.cpp`（5 TC, グリッド値域/決定性/改善バイアス）+
  `test_restart_controller.cpp`（7 TC, cycle/grow-shrink/cap-floor）。ctest 234/234。
- [x] **統計記録を ConstraintStats レイヤへ集約**: verbose 統計記録（type別 + instance別の
  call/fail/fail_depth/reduction）+ 失敗時 bump_activity の3経路重複（propagate_instantiate /
  process_queue invoke_cb / batch propagator）を member template `record_constraint_call(model,
  constraint_idx, bump_var_idx, call)` に一本化。inline template で hot path はインライン化。
  hot path のため golden byte一致 + A/B 計測（tsp/magic_square 30x, 非劣化）で二重検証。
- [x] find_all + restart の解列挙設計レビュー（2026-06-16 完了 → [find-all-enumeration-review-2026-06-16.md](find-all-enumeration-review-2026-06-16.md)）。
  結論: restart+NoGood 列挙は**完全・重複なし・終了する**。独立オラクル照合
  `tests/cpp/test_find_all_consistency.cpp`（restart on/off の2エンジンが同一解集合 + 重複ゼロ +
  解析的解数一致、commit 8eaa808）を新設し、golden の自己照合では担保できなかった「独立オラクル」を補完。
  唯一の未解決点はメモリ/伝播コストの O(#解) スケール（正しさでなく性能トレードオフ）で、対処
  （find_all 時 DFS 既定化）は golden 再録 + 解順変更を伴うため**ユーザー判断待ちの将来項目**に分離。
- [x] 目標: solver.cpp を探索ループ + フレーム管理のみの **900 行以下**へ（2026-06-16 達成: 1,600 → **414 行**）。
  関数定義を3 TU へ機械的分配（commit fda9b35）: `solver_search.cpp`(606, restart 探索ループ系) /
  `solver_frame.cpp`(372, 明示スタックのフレーム管理) / `solver_propagate.cpp`(253, 伝播エンジン)。
  file-local static / 匿名 namespace が無く全関数が Solver::/Literal:: メンバのため、宣言(solver.hpp)を
  共有したまま定義のみ分割。golden 182 byte一致 + ctest 234/234 + python 100。

**完了条件**: ゴールデン一致 + 全ベンチ非劣化。**現状**: ヘルパ抽出 4 + クラス化 2 + 単体テスト 2 +
統計層集約 1 + **ファイル分割 1**(commit fda9b35) commit、全 golden byte一致（軌道不変＝ベンチ不要）。
RestartController / ModeRewardPolicy / GradientStrategy の3クラスに整理完了 + solver.cpp 414 行（900行目標達成）。
**残: find_all 解列挙設計レビューのみ**（これは純リファクタでなく設計判断を伴うため別途）。

---

## スケジュールと依存

```
フェーズ0 (0.5-1日) → 1 (0.5日) → 2 (2-4日)
   → 3 (1週) → 4 (2-3週) → 5 (2週) → 6 (2週)
```

- 0→1→2 は順次必須。3以降は2の安全網が前提
- 4 と 5 は項目単位で入れ替え可能。6 は最後（3〜5で薄くしてから解体）
- 合計目安: **集中作業で 7〜9 週間**。各フェーズ末で停止可能

## 期待効果まとめ

| 領域 | 効果 |
|------|------|
| リポジトリ | 追跡ジャンク 214 件解消、2.1GB の未管理データに管理方針 |
| テスト | 実行されないテスト 0 に（+165ペア有効化）、脆い .expected の根治 |
| コード量 | 制約層 + solver で約 2,000 行削減（全体の約7%）、global.hpp 分割でビルド時間短縮 |
| 性能 | Domain 表現刷新（再プロファイルに基づく）。ベンチゲートで非劣化保証 |
| 保守性 | 新制約追加時の定型コード（trail/reif/linear）がテンプレートで完結 |
