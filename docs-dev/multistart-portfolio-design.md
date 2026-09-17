# マルチスタート × マルチスレッドの設計メモ

2026-09-17 時点。`feature/mp` / `feature/mp-multistart` を現在の main に載せ直すときの
方針と、そのとき決めたい設計判断を残す。**このメモは未実装のアイデアを含む**（実測済みの
事実と、まだ測っていない案を明示的に分けてある）。

## 1. 現状（`feature/mp-multistart` に実装済み・計測済み）

### 仕組み

`ParallelSolver(num_threads, configs, instances_per_thread)` が
**num_threads × instances_per_thread の2次元グリッド**を張る。

- **スレッド間**: 本物の並列。presolve を1回だけ実行し、`Model::clone()` でワーカー数ぶん
  複製して独立に探索する。協調は「純ポートフォリオ + bound 共有」で、nogood/clause は共有しない
- **スロット間**（`RoundRobinRing`, `parallel_solver.hpp`）: OS スレッドは n 本立てるが
  **常にちょうど1本だけアクティブ**。リスタートのたびに次へ CPU を譲るので、実質1スレッド分の
  CPU で n 種類の戦略を時分割する。各インスタンスは NG/activity を独立に持ち続け、
  使い捨てにしない（UNSAT・最適性の証明能力を落とさないため）
- ターンの受け渡しは EMA 報酬比例の抽選。報酬信号は `RestartController::end_cycle` と同じ
  「NoGood 枝刈りが進み、かつ深さが伸びたか」、式と定数は mix_p バンディット
  （`mode_reward_policy.hpp`）から流用（`kDecay=0.5` / `kFloor=0.1`）。`kFloor` で全メンバーに
  最低確率を残す
- 入口は `SABORI_MULTISTART_N`（既定1）。`-j` なしでも `ParallelSolver` 経路に入る

### 多様化軸

両次元とも `apply_diversification_axis()` の**同じ7ケース**を使う。スレッド間は
`make_portfolio_configs` のラダー、スロット間は `make_instance_config` が
`(slot_idx-1) % 7` で適用し、シード導出定数だけ分けている（`0x86545D77u` / `2654435761u`）。

7ケースの内訳（最適化時）: 純シード×4 / `conflict_learning` 反転 / `nogood_learning=false` /
`fixed_mixp=0`（mrv）。SAT 時は `probe_enabled=false` と `gradient_enabled=false` が入る。

### 測定で分かっていること（2026-07-25）

- 使い捨て（直列 discard）は明確に net negative。ラウンドロビン化で改善、バンディット化で
  n=4 の Δ が −0.099 → +0.010、n=2 は +0.036
- **価値は平均時間ではなく尾の救済に出る**。`solbat` は n=1 で NOSOL、n=4 で 4.28 秒
  （20.05 秒から）。一方で平均実行時間は 9.32 → 11.32 秒と悪化（イージーな問題での CPU 分割税）
- ラダー case0 の手動チューニングの効果は +0.034 程度で、分解すると `gradient_enabled=false`
  単体が最大（+0.033）、`restart_scale` はほぼ無効果（rs2 は −0.054）
- Optuna は手動チューニングを上回れなかった（大サンプルで Δ+0.000 vs 手動 +0.034）
- 総括: 「使っても明確には損しない」までは到達、「使うと明確に得する」証拠はまだ無い

## 2. 未実装・未計測のアイデア

### (a) 2次元で軸表を分ける

いまは両次元が同じ7ケース表を共有しているので、4スレッド×3スロットだと
「同じ軸ケース・シードだけ違う」升目がグリッド上に並ぶ。7ケース中4つが純シードなので
実害は小さいが、**グリッド全体のカバレッジを誰も管理していない**。

役割で分けるのが素直:

- **スレッド間（CPU を独占）= 構造的に違う戦略**。`conflict_learning` on/off、
  `nogood_learning=false`、probe / bottomup、LNS。UNSAT・最適性証明のように
  連続時間が要るものはここに置く
- **スロット間（時分割、リスタート単位で明け渡す）= 初期分散が大きく切り替えコストが低いもの**。
  シード、`SABORI_BISECT_DIR`、`restart_scale`、phase hint 系

根拠: スロットはリスタートのたびに CPU を手放すので、長時間の連続計算を要する戦略には向かない。
測定でも multistart の価値は初解の尾の救済に出ている。

### (b) スロットの適応的ファンアウト

分割税はイージーな問題で効く（平均 9.32 → 11.32 秒）。なので

> 初解が出るまでは n=1 で走り、一定 conflict を超えても初解が無いときだけ n を増やす

とすれば「平均は落とさず尾だけ救う」という測定結果の形に合う。**未実装・未計測**。

### (c) 評価指標を尾に寄せる

S(k) の平均だけでなく **NOSOL率・最悪ケース**を併記する。07-25 の TODO として挙がったまま
未実装。multistart の採否をこの指標で判定できないと、平均で薄まって毎回「ほぼ互角」になる。

## 3. 前提になる整理（`env-flags-inventory.md` の未消化分）

新しいアームはどれも env 経由で**プロセスグローバル**なので、per-worker アームにできない。
WorkerConfig への移設が前提になる。優先順:

| アーム | 状態 |
|---|---|
| `SABORI_BISECT_DIR=low` | resume メモが「次の一手」と名指し。ラダー軸の第一候補 |
| `SABORI_PROBE_ROOT` p2k | フラグ棚卸しで「生きている玉」。既定ON昇格判定（14年ゲート+複数シード）は未実施 |
| `SABORI_BOTTOMUP` | グリッドで既定値確定済み（b2k / cutoff=8 / iso=off） |
| `SABORI_CLAUSE_WITNESS` 1WL | 14年ベンチで portfolio-arm 候補まで格上げ |
| LNS (`feature/lns`) | 既定 OFF arm（強制既定だと net −35〜−53）。main 未統合 |

## 4. 移植計画（マージではなく新ブランチ）

### なぜマージでなく移植か

merge-base は 2026-06-29 で、main はそれ以降 **247 ファイル・19,527 行**動いている。
両側が触ったファイルは 31 本。ただし内訳を見ると、判断は「どちらが安いか」ではなく
「どちらが安全か」になる。

| 種類 | 規模 | コスト |
|---|---|---|
| 新規ファイル（`parallel_solver.hpp/cpp`、`test_parallel_clone.cpp`、`test_parallel.py`） | 1,263 行 | どちらの道でもそのまま持ってくるだけ。衝突ゼロ |
| フック追加（`solver.hpp` +213 / `solver.cpp` +227 / `main.cpp` +191 / `solver_search.cpp` +69 / model +95） | 約 800 行の追加ハンク | main 側が `solver_search.cpp` +527、`solver_frame.cpp` +175 動いた場所に載る |
| 各制約の `clone()` | ヘッダ 15 本 | **どちらの道でも必須**（下記） |

`Constraint::clone()` は**純粋仮想**で、「未実装の具象サブクラスはコンパイルエラーになり
取りこぼしを防げる」という設計になっている。main には mp 以降に 7 クラス
（Subcircuit / Tree / BinPackingLoad / GlobalCardinality / LexLessEq / ValuePrecede /
ClauseWitness）が増えており、どれも `clone()` を持たない。**マージしてもビルドは通らず、
結局この 7 本は手で書く**。

つまり両者の差は「800 行のフックハンクを git に 3-way マージさせるか、自分で現在の文脈に
置き直すか」だけ。2026-09-17 に、main 側が大きく書き換わったファイルで auto-merge が
通ったのに中身が壊れていた事故を踏んでいる（[[conflict-resolution-and-staging-rules]]）。
その帯域では手で置き直す方が安全と判断する。

### 手順

1. main から新ブランチを切る
2. **新規4ファイルは丸ごと持ってくる**（`git checkout feature/mp-multistart -- <path>`）。
   `RoundRobinRing` のバンディット・ラダー・`apply_diversification_axis` は計測で詰めた
   成果物なので**作り直さない**
3. clone 基盤（`constraint.hpp` の純粋仮想 + `SABORI_CSP_CLONE_IMPL`、`Model::clone`）を移植し、
   コンパイラに未実装クラスを列挙させる。7 本の新規制約はここで追加
4. フック点（`Solver::apply_worker_config`、`restart_yield_hook_`、CLI の `-j` / `-C` / `-V` /
   `SABORI_MULTISTART_N`）を現在の `solver_search.cpp` / `solver_frame.cpp` を見ながら手で置く
5. env アームを WorkerConfig へ移設（§3 の順）
6. 2次元の軸表を分離（§2a）
7. 軸×シードのグリッドで**再チューニング**

### 再チューニングが必須な理由

[[portfolio-ladder-seed-first]] の教訓が「1スレッド処理方式を変えたらラダーは要再チューニング」。
2026-09-17 に perf 群（entailment dispatch / hash-consing / table / Variable order 修正）を
main に入れて単スレッド軌道が変わったので、mp のラダー値（2026-06〜07 に別の単スレッド挙動の
上で決めたもの）はそのままでは使えない。

## 5. 積み残し（`feature/mp-multistart` の TODO、2026-07-25 時点）

- ラダー case0/1/2 の並び順（`gradient_enabled=false` → base → `conflict_learning=true`）を
  合意したが `parallel_solver.cpp` への反映が未実施
- バンディットの `kDecay` / `kFloor` は mix_p の値を流用しただけで round-robin 文脈では未チューニング
- `bench_axis_seed_grid.py` の過去の公開済み計測は、`time.time()` のクロックジャンプバグの
  影響を遡って修正できていない

## 関連

- ブランチ: `feature/mp`(12) / `feature/mp-multistart`(13, tip `051dbba`)
- `docs-dev/env-flags-inventory.md`（フラグ整理方針）
- `docs-dev/work-log/2026-07-25.md`（マルチスタートの計測記録）
- `benchmarks/minizinc_challenge/bench_multistart_grid.py` / `tune_ladder_case.py` /
  `bench_axis_seed_grid.py`（いずれも未追跡）
