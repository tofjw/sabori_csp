# presolve ギャップ分析 (2026-07-09)

探索空間縮小・変数削減テクニックの漏れを、実装棚卸し + 標準カタログ
(MIP presolve / SAT preprocessing / CP) との突き合わせで洗い出した。

## 現状の装備（棚卸し結果）

| 層 | あるもの |
|---|---|
| ビルド時 (src/fzn) | bool2int エイリアス統合 (model.cpp:78)、par 定数の遅延実体化 (model.cpp:127)、エイリアス解決、線形の重複変数除去、defined_var マーキング |
| ModelSimplifier (main.cpp:255, `-E` で無効) | **2変数 int_lin_eq (\|c\|=1) の代入消去のみ**。代入先は線形族7種 (eq/le/ne/eq_reif/ne_reif/le_reif/le_imp) 限定。「連鎖防止」(y_vars 再利用禁止) で chain は交互リンクしか消えない。defining constraint は残す = 消去は実質「探索対象からの除外」 |
| Solver::presolve (solver.cpp:329) | 全制約 presolve() の固定点反復（ドメインフィルタ）、OneHotChannelAggregator、watch list 再構築 |
| 探索準備 | decision/defined/unconstrained 3分割 (variable_selector.cpp:11)、activity 初期化 |

## 漏れ（優先度順）

### 優先1: root probing / failed literal
b∈{0,1} を仮置き伝播して片側 fail なら確定、両側生存なら共通剪定 +
含意/等価 (b1↔b2) を収穫。現装備で最大の空白。bottomup probe =
「obj 限定 destructive probing」の一般化として実装余地あり。
**G4（充足解未達: implication chain の掘り間違い）への直接処方**で、
presolve 系で唯一「探索の質」を変えうる。予算制御は probe 系ノウハウ流用。
※ 物量の静的計測は不能（探索的手法）— 実装判断は G4 問題での直接 A/B で。

### 優先2: 重複 reif の CSE
同一条件の int_eq_reif(x,c,b1) / (x,c,b2) の b 統合 pass がない。
**OneHotChannelAggregator は「重複を含むグループを集約しない」ため、
重複が集約まで阻害している**（二重の損失）。ハッシュ1パスで安い。

### 優先3: 等価マージの本格化 (union-find 正準化)
IntEq presolve はドメイン交差のみで変数・制約とも残存。ModelSimplifier
も defining constraint が残る。union-find で正準変数に全制約種を書き換えれば
変数が真に消え、「連鎖防止」制限も解消。

### 優先4: 線形衛生系 (MIP presolve 定番)
- GCD チェック (gcd(係数) ∤ rhs → 即 UNSAT / 値フィルタ)
- 係数タイトニング + rhs 床処理 (≤ 行の CG-lite)
- 重複行・支配行の除去
- dual fixing (片符号出現 ∧ obj/output 無関係 → 境界固定)。bool の
  pure literal も同型

### 優先5: root entailment 一掃
presolve 後に恒真化した制約の除去 (watch list 縮小)。search 中の
entailment フラグの root 版。

### 見送り
対称性検出 (モデル側で破壊済みが大半 + 機構が重い) / SAC (probing の
劣化上位互換でコスト過大) / SAT 流 BVE・blocked clause (bool 構造が
clause 純でない) / view 機構 (アーキ変更対効果が constant-factor) /
連結成分分解 (VIG 基盤で実装容易だが root 分解できる MZC モデルは稀)

## 期待値の見立て

大負け (G1/G2/G3) は探索・証明支配なので presolve 追加はほぼ定数倍。
例外は probing (G4 の質的改善候補) と reif CSE (集約解禁の波及)。
実装判断は物量計測 (presolve_volume_probe.py, 下記) → 効果ゲートの
IntEqImp 方式で。

## 物量計測結果 (2026-07-09, .fzn_cache_one_hot 274問)

presolve_volume_probe.py (静的近似、定数配列解決込み)。
corpus 総計: vars 193万 / cons 783万。

| metric | total | %vars | 問題数>0 | 備考 |
|---|---|---|---|---|
| dup_reif (同一条件 reif の b 重複) | 4,113 | 0.21% | 37 | **局所集中**: code-gen20 1860 (2.4%), stripboard 252/241 (**12%**), community-det 282 (**11%**), network_50_cstr 333 (4.5%), bnn-planner 185 |
| dup_row (完全重複制約) | 46,110 | — | 23 | gt-sort(G5) 42k を除くと ~4k。harmony 1344 = **全制約の 16%**、code-gen20 1862 |
| eq2_pot (union-find 消去上限) | 23,222 | 1.20% | 97 | |
| eq2_ach (現 ModelSimplifier 相当) | 19,927 | 1.03% | 97 | **現行が上限の 86% を既に達成** |
| eq2_half (片側 \|c\|=1) | 4,382 | 0.23% | 43 | |
| int_eq 素ペア | 36 | 0% | 1 | |
| dualfix (片符号変数) | 59 | 0% | 2 | |
| purelit (片極性 bool) | 29 | 0% | 2 | |

## 優先度決定 (計測に基づく)

1. **採用候補A: 制約 hash-consing pass (dup_row 除去 + dup_reif の b 統合)**
   — 1パスで両方カバー。corpus 全体では小さいが、対象問題では 11〜16% の
   局所物量 (stripboard/community-detection/harmony) + network_50_cstr (G1)
   と code-generator に集中 + one-hot 集約の解禁ボーナス。実装が安い
2. **採用候補B: root probing** — 静的計測不能 (探索的)。G4 問題
   (amaze/prize-collecting) での直接 A/B で判断。実装は bottomup probe の
   一般化で中規模
3. **不採用 (物量なし、IntEqImp 前例と同じ判定)**: dual fixing / pure
   literal / 素 int_eq マージ / union-find 等価マージ本格化 (現行 simplifier
   が上限の 86% を達成済み、差分 0.17%) / 線形衛生系の残り (重複行は A に吸収)

**教訓**: minizinc コンパイラが既に CSE・pure literal 級の掃除をしてくる
ため、「MIP presolve の定番」の多くは FZN 入力では物量が残っていない。
残るのは (a) コンパイラが跨げない重複 (同一条件の reif が別経路で生成される
ケース) と (b) 探索的手法 (probing) のみ。

## 候補A 実装と効果測定 (2026-07-09, 257427e)

FZN ビルド層 (fzn/model.cpp to_model Phase 0.6) に hash-consing を実装。
完全重複行の除去 + 同一条件 *_reif の b エイリアス統合 (_imp は片方向で
b1=b2 が帰結しないため対象外)。SABORI_DEDUP=0 で無効化、既定有効。

**発火量 (probe 予測と一致)**: community-detection 282+282、network 333、
stripboard 252、harmony 1344行、code-generator20 1862行、bnn 214+185

**効果 A/B (30s×2seed, 8対象+2対照): net −1/20 = wash**
- community-detection +2 / stripboard22 −2 / 他はほぼ不変
- **one-hot 解禁ボーナスは実測なし** (対象問題の重複 reif は int_lin_le_reif
  系で one-hot の対象外だった)
- 判定: outcome 中立の衛生 (モデル縮小・伝播削減・健全性リスクなし) として
  既定 ON 採用

**副産物: ポインタ順非決定性の再発見と修正移植 (e64d480)**
初回 A/B で zephyrus が「dedup 発火ゼロ (モデル同一) なのに on/off で
再現的に別軌道」という怪現象 → 真犯人は LinearConstraintBase::
aggregate_terms の `unordered_map<Variable*,...>` 反復 (ヒープレイアウト
依存)。dedup パスの追加アロケーションがレイアウトをずらして発現した。
**2026-07-02 の修正 9a06ccd は feature/mp にしか無く、本ブランチ系列には
未適用だった** → cherry-pick (golden 期待値の更新込み)。移植後は
zephyrus/gfd で on/off が bit 一致し決定性回復。
教訓: (1) 決定性修正は全アクティブブランチに配る (2) 「no-op のはずの
変更で軌道が変わる」はヒープレイアウト感度のシグナル。
