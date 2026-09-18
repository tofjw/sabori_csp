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

## 2. アイデアと検証結果

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

### (d) 短いプローブの結果を特徴にして戦略を決める（algorithm selection）— **検証済み・不成立**

**2026-09-19 に案2（既存 probe を特徴源にする）を実測し、不成立と判定した。**
連鎖して案1・案3 も「どのアームが勝つかを当てる」形では成立しない。結果は §2(d-result) に。

以下は検討時の記述（記録として残す）。「ちょっとだけ探索してから、その状態を特徴量に加えて
探索方法を決める」型（SATzilla 系の probing features、instance-specific configuration）の
導入余地について。

**前提: この repo では「予測して選ぶ」型が4回外している。**

| 試み | 結果 |
|---|---|
| LCG の出し分け | 撤退。proof-burst 特徴でも予測不能、always-L の静的 full に確定 |
| phase hint の activity 重み付け | 全滅。失敗率は変数間でほぼ一定（sd 0.01–0.09）で判別力なし |
| 分岐方向の先験 | coin-flip hotness が「どのモデルが敏感か」を特定できずクローズ |
| リスタートの adaptive tighten | stale stats で不発、出荷挙動が always-widen と同じに |

共通の失敗様式は**選択器の設計ではなく特徴側**——安い signal に判別力が無かった。
一方で「予測せず、走らせて進捗を測って資源を寄せる」形（`ModeRewardPolicy` の mix_p、
`RoundRobinRing`）は当たっている。導入するならこの器に薄く足す形にする。

**案1（最小・推奨）: `RoundRobinRing` の報酬事前分布をプローブで初期化する。**
いまは `reward_(n_, 1.0)` の一様初期化。最初の数リスタートをプローブ期間として全メンバーに
均等に回し、その進捗で事前分布を作る。**外しても `kFloor` と EMA が洗い流す**ので
downside が構造的に小さい。過去4件が「予測を信じて一方に倒した」形だったのに対し、
これは予測を初期値にしか使わない。

**案2（今の main だけで試せる）: 既にある probe を特徴源にする。**
`SABORI_PROBE_ROOT`（failed-literal 検出）と bottomup probe は**すでに「ちょっと探索する」を
実装していて、結果を枝刈りにしか使っていない**。プローブ中の failed literal 率・伝播量・
目的の動きは追加の探索なしで取れる。

**案3（オフライン学習）: ラダーの順序だけ決める。**
14年コーパスと `bench_ab_perf.py` / `lib_benchmark.py` の基盤がある。presolve 時点の静的特徴
（制約種ヒストグラム、`-c` の modularity、モデル規模、目的の構造）で「ラダーの先頭に何を置くか」
だけ決める。全戦略を選ぶのでなく順序を決めるだけなので、外しても後続スレッドで回収される。

**着手順**: 案2 →（mp 移植後に）案1 → 案3。案2 で「プローブ中に何が測れて、それがアーム間の
勝敗とどれだけ相関するか」を先に確かめる。**判別力が無ければ案1も案3も成立しない**ので、
過去4件の教訓に素直な順序はこれ。

**測定ゲート（先に決めておく）**: セレクタの価値は VBS/オラクル推定で過大評価される
（単スレ greedy VBS で有望に見えた defined bool 昇格が実並列 A/B で符号ごと反転した実例が
[[promote-def-bool-arm]]）。**「最良の静的単一構成」と「round-robin/バンディット」の両方を
超えて初めて採用**とする。

### (d-result) 案2 の実測結果（2026-09-19）— 不成立

ハーネス: `benchmarks/minizinc_challenge/probe_feature_study.py`（収集）と
`probe_feature_analyze.py`（3ゲート判定）。生データ `/tmp/probe_study.json`。

条件: `.fzn_cache_abperf` の 77 問を 3 秒プローブし、**プローブ中に証明が終わる 24 問を除外**
（選択の余地がないため）。残り 53 問 × 6 アーム × 3 シード × 30 秒 = 954 run。
アームはすべて現 main の env フラグ: base / `SABORI_BOTTOMUP=1` / `SABORI_PROBE_ROOT=1` /
`SABORI_PROMOTE_IMPACT=1` / `SABORI_CLAUSE_WITNESS=1` / `SABORI_BISECT_DIR=low`。

#### Gate 1: 余地は名目 28%、ただし静的に差し替えるべきアームは無い

| arm | 総当たり勝ち点 | 単独最良 | base との直接対決 | ステータス階層 |
|---|---|---|---|---|
| probe_root | 157 | 10 | 26勝 23負 110分 | 改善3 / 悪化3 |
| bisect_low | 140 | 9 | 18勝 16負 125分 | 改善1 / 悪化4 |
| bottomup | 131 | 8 | 24勝 30負 105分 | 改善2 / 悪化13 |
| base | 124 | 4 | — | — |
| promote_impact | 109 | 11 | 21勝 43負 95分 | 改善5 / 悪化8 |
| clause_witness | 106 | 0 | 2勝 12負 145分 | 改善1 / 悪化3 |

VBS は最良の単一アームに 159 セル中 44 セル（27.7%）で勝つ。

**総当たり勝ち点の順位は読み間違えやすい**。probe_root が base の上に来るのは弱いアーム
（clause_witness / promote_impact）から点を稼いでいるためで、**base との一騎打ちでは
26勝23負でほぼ互角**。既定を差し替えるべきアームはこの 6 本には無い。
（`SABORI_PROBE_ROOT` p2k の既定ON昇格判定は依然として未実施のまま。この 53 問は
「プローブで終わらない難しめの問題」に偏っているので、これを昇格の根拠にはできない。）

#### Gate 2: 勝者はインスタンスの性質ではなくシードで決まる ← ここで折れた

| | 問題数 |
|---|---|
| 全シードで勝者が一致 | **2** |
| シードで勝者が入れ替わる | **26** |
| そもそもアーム間に差がない | 25 |

```
2022__gfd-schedule:     [bottomup] → [probe_root] → [base]
2022__ma-path-finding:  [promote_impact] → [probe_root] → [probe_root]
2022__traveling-tppv:   [bottomup] → [base 他] → [promote_impact]
```

Gate 1 の 28% は**インスタンスの属性ではなくシードごとの当たり外れ**だった。
予測対象そのものが存在しないので、特徴量を改善しても届かない。

#### Gate 3: どの特徴も有意でない

| 対象 | 最良特徴の AUC | 並べ替え検定(2000回, 全特徴の最大\|AUC-0.5\|) |
|---|---|---|
| bottomup | bisect 0.794 | p=0.182 有意でない |
| probe_root | ng_domain 0.735 | p=0.257 有意でない |
| promote_impact | ng_per_fail 0.242 | p=0.265 有意でない |
| bisect_low | bisect 0.791 | p=0.451 有意でない |
| ANY_NONBASE | probe_improve 0.675 | p=0.425 有意でない |
| clause_witness | base に勝つ問題が 0 件 | 判定不能 |

生 AUC は 0.79 まで出るが、特徴量が約 20 個ある状態でラベルをシャッフルすると帰無分布の
中央値が 0.20〜0.28 に来る。**この水準はラベルを無作為に振っても普通に出る**。

#### 判定と、過去の失敗との違い

案2 は不成立。案1（プローブで報酬事前分布を作る）と案3（静的特徴でラダー順序を決める）も、
「どのアームが勝つかを当てる」目標に対しては同じ理由で成立しない。

過去4件（LCG 出し分け / phase hint 重み付け / 分岐方向の先験 / restart adaptive）は
「特徴に判別力が無かった」で止まっていたが、**今回は Gate 2 で理由まで特定できた**——
予測すべき目標がインスタンスの属性として存在していない。これは特徴量エンジニアリングでは
解けない種類の壁で、**この系統に今後コストを割く根拠が無くなった**と考えてよい。

限界: 陽性が 4〜9 件と少なく、Gate 3 単独では「効果が無いと証明した」とは言えない（検出力不足）。
ただし Gate 2 が独立に効いているので結論は動かない。

#### 副産物: ポートフォリオを支持する証拠になった

「勝者がシードで入れ替わる」は裏返せば、**並べて走らせて実測で資源を寄せる方式なら
取りに行ける分散が 28% ある**ということ。予測器には届かないがバンディット配分なら拾える形で、
§2(a)(b) の方向を支持する。**ここが `RoundRobinRing` の存在理由**であり、mp 移植の
優先度を上げる材料になる。

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
