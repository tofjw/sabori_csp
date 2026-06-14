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
- [ ] **ReifConstraintBase**: enforce_b1 / enforce_b0 / infer_b の3フックを派生が実装する形に。reif は false UNSAT 事故リスクがあるため、**各制約の移行ごとにランダム全解数照合テストを追加**（〜450行削減）
- [ ] comparison.cpp のドメイン交差・双方向伝播ヘルパ抽出（〜200行削減）
- [ ] **presolve/propagate 二重実装の統一**: ドメイン更新を抽象化（直接 or enqueue を切り替える DomainWriter）。diffn / disjunctive から開始
- [ ] **コールバック署名整理**: `var_idx`/`internal_var_idx` の引き回しを一本化。全制約に波及するため**フェーズ最後**に、シグネチャ変更でコンパイラに非互換を検出させる形で実施

**完了条件**: ゴールデン一致 + ctest green + ベンチ3本 + `bench_compare.py` 総合非劣化。
**削減見込み**: 1,000 行強。

## フェーズ5: Domain 表現と性能（2週間、高リスク・ベンチゲート厳格）

旧計画 Phase 6 の継承 + フェーズ0 の再プロファイル結果で優先順位を確定。現時点の候補:

- [ ] `BoundsDomain::removed_set_` の `unordered_set` → オフセット付きビットマップ/ソート済み vector（ドメイン幅で自動選択）
- [ ] BoolDomain（min=0,max=1 の2bit 表現）の追加
- [ ] Domain/SparseDomain/BoundsDomain のフラグ分岐 → 明確な内部表現切り替え（variant or 関数テーブル）に整理し、`contains`/`remove_*` のロジック複製を解消
- [ ] `remove_value()` の境界更新 O(n) → 境界削除時のみの遅延スキャン（再計測で要否判断）
- [ ] `rewind_dirty_constraints()` の virtual dispatch 削減（再計測で要否判断）

**完了条件**: 項目ごとに ctest green + ベンチ3本 + bench_compare 非劣化 + gprof で対象ホットスポットの改善確認。
**リスク**: Domain はソルバーの心臓部。1表現 = 1コミット、ランダム照合テスト併設。

## フェーズ6: solver.cpp 再分解（2週間）

フェーズ3〜5 で薄くなった後に実施。再肥大化（1,308→1,810行）の原因である後付け機能を分離する。

- [ ] `search_with_restart` / `search_with_restart_optimize` の共通骨格抽出（restart 制御・cycle 管理・mode_reward 更新の二重実装解消）
- [ ] mode_reward / mix_p 抽選を RestartController（または新 ModeRewardPolicy）へ移管
- [ ] gradient ヒューリスティクスを optimize 専用 Strategy へ分離
- [ ] 統計記録の残りを ConstraintStats レイヤへ
- [ ] find_all + restart の解列挙設計レビュー（solution nogood 依存。2026-06-10 に root 確定変数 watch の無限ループバグを修正済み。1リテラル縮退時の unit nogood 化の非対称も解消）
- [ ] 目標: solver.cpp を探索ループ + フレーム管理のみの **900 行以下**へ。RestartController / ModeRewardPolicy の単体テスト追加

**完了条件**: ゴールデン一致 + 全ベンチ非劣化。

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
