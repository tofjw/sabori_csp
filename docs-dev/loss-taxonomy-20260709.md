# CP-SAT 大負け問題の機構別分類 (2026-07-09)

「証明支配」と一括りにしてきた負けを、20260707_constraints3 の実データ
（14年・CP-SAT 勝ち 106 問中、時間差のみの 60 問を除く 46 問）から
機構別に分解した。判定材料: 解の到達度（同 obj / obj 差 / 解なし）、
目的関数の構造（FZN の obj 定義・ドメイン）、CP-SAT 側の証明時間。

## G1: ペナルティ和・下界崩壊型（7問、最重症）

zephyrus15 (×1247), gfd-schedule18/22 (×39〜49), arithmetic-target22 (×117),
roster-shifts-bool23 (×5.9), collaborative-construction20 (解なし),
network_50_cstr24 (解なし), code-generator19/23 (×1.5〜1.9, 境界)

- **指紋**: 目的関数 = 多項ペナルティ和。静的 LB と最適値の乖離が数桁
  （zephyrus: dom 5..100000 / 最適 12、arithmetic-target: 1..73268 / 最適 5、
  collaborative: 0..490 / 最適 9）。reified 網が違反 bool を定義。
  CP-SAT は数秒で OPTIMAL = 和への強い下界 (LP/PB)
- **機構**: (a) 良解への誘導が無い（解の質で負け）(b) 和の LB = 項 min の和 ≈ 0
  で B&B 枝刈り無力（証明で負け）。**証明以前に (a) で負けているのが
  「学習支配」との違い**
- **処方箋**: (a) は内製で攻められる（本ドキュメント末尾のヒューリスティクス案）。
  (b) は LP 領域 = 外部ポートフォリオ

## G2: 微小整数最適の UNSAT 証明型（7問、真の学習支配コア）

elitserien14/16/18 (obj*=2〜4), hrc17/19 (11, 6), skill-allocation20/25 (2),
sudoku_opt22 (−3), products-and-shelves25 (5), valve-network23, mondoku25

- **指紋**: **最適値に到達済み**（同 obj）。欠けるのは「obj ≤ k−1 は UNSAT」の
  深い反証 = 節学習の独壇場。CP-SAT 1〜10s
- **処方箋**: 射程外コア。外部 SAT ポートフォリオ枠。内製で追わない
  （[[solver-positioning-activity-vs-external]] 通り）

## G3: 数値 bound 証明型（5問、native で攻められる証明ギャップ）

rcpsp13 (77/157), mapping15 (793, int_max 目的), depot-placement16 (int_max),
liner-sf-repositioning14/19, largescheduling15/18 (軽微)

- **指紋**: 同 obj 到達済み。目的が makespan/max 型 or 大数値。証明に必要なのは
  節ではなく energetic/precedence 系の数値下界。depot は CP-SAT も 30s
- **処方箋**: TTEF 強化・precedence LB 等の古典 CP 技術。**G2 と混ぜないこと**
  （欠落部品が「節」でなく「数値推論」）
- **攻略可能性調査済み (2026-07-09、下記セクション)**: G3a (schedule-bound 型)
  は bottomup prover 構成で攻略可能（rcpsp を 118s で完全証明）、
  G3b (int_max 集約型) は階段が構造的に不成立で射程外

## G4: 充足解未達型（4問、フィージビリティ探索）

amaze12 (count/reif の経路構築), prize-collecting16 (CP-SAT は 1.1s で OPT),
collaborative-construction20, network_50_cstr24（後2者は G1 重複 = 重症形）

- **指紋**: 巨大 reified 網で最初の充足解に届かない。implication chain を
  辿れば決まる構造を activity 探索が掘り間違える
- **処方箋**: 値選択の極性 (phase)、並列シード多様性（mondoku 型崖と同処方）

## G5: 構造非対応（1問）

gt-sort（set 変数）。判定済み・射程外。

## G6: 漸進差帯（約12問、×1.05〜1.2、CP-SAT も未証明が多い）

cvrp, jp-encoding14/17, ptv, triangular19/22, cargo, crosswords,
fox-geese-corn19/24, opt-cryptanalysis18 (特殊: table×96 で ×2.46), tppv,
neighbours21, minimal-decision-sets, java-routing, graph-clear, magc23

- 単一機構なし。チューニング / ポートフォリオ / ジッタ帯

## 分類が変えること

- 従来「証明支配 → 全部外部行き」だった塊のうち、**G1(a) と G3 は内製で攻められる**
- 本当に手を出すべきでないのは G2 のみ
- G4 は探索の値選択・並列で拾える可能性

## G1(a) ヒューリスティクス検討 (2026-07-09)

### 既存機構の棚卸（なぜ今負けているか）
- **incumbent 誘導は実装済み**（current_best_assignment_ を enumerate 先頭 /
  bisect 方向ヒントに使用）→ G1 では逆効果の疑い: incumbent (obj=587) の
  近傍に張り付き、多数のペナルティを同時に 0 へ倒す大移動ができない
- **改善プローブは実装済み・既定 ON**（ub 側から残レンジの 5% カット、
  fail_limit=10）→ G1 で不発: 値選択に低ペナルティ方向の誘導がないため、
  fail 10 回では target 以下の解に到達できない
- gradient（擬似勾配値選択）は過去に ablation 済みで net 中立

### 提案（優先順）

**H-A: bottom-up optimistic restart（本命）**
- k 回に 1 回の restart を「obj ≤ lb + δ」の楽観境界付きで実行
  （restart の fail 予算がそのまま打ち切り）
- SAT → 一撃で準最適解。**証明付き UNSAT → root で obj_lb を引き上げ
  （健全な下界証明の蓄積 = G1(b) にも寄与）**。不明 → 次の楽観 restart で δ を拡大
- 核となる洞察: ペナルティ和では **tight bound 自体が最強の伝播ガイド**
  （Σ penalty ≤ K が K 個を除く全ペナルティを 0 に強制 → reified 網が
  連鎖して探索空間が近可行領域に絞られる）。CP-SAT が数秒で証明できるのは
  この「bound → 伝播」の連鎖が浅いため、と整合
- 実装: run_improvement_probe の対称拡張（lb 側版）。opt-in env から
- 検証: zephyrus/gfd/arithmetic-target/roster-shifts + G6 対照 + 標準ゲート

**H-B: ペナルティ・コーン零極性**
- obj の推移的 fan-in（objective cone）を静的に計算し、cone 上の変数の
  値試行順を「寄与 0」優先に（bool +係数 → 0 先行）。cone 変数に限り
  incumbent 誘導を override
- gradient の再来だが「cone 限定・静的極性・ランタイムコストほぼ 0」が相違点
- SABORI_PROMOTE_DEF_BOOL（opt-in 温存中）と併用で penalty bool を直接
  分岐対象にできる

**H-C: dive-on-penalties**
- restart 時に penalty:=0 を貪欲 dive → 矛盾直前で止めて通常探索
- H-B の動的版。MaxSAT の stratification に相当。H-A/B の結果を見てから

### 着手判断
H-A から。理由: (1) G1 の指紋（LB↔最適の数桁乖離）に直結 (2) 実装が既存
probe の対称形で小さい (3) UNSAT 側が健全な lb 証明として蓄積し証明ギャップ
にも効く (4) 値選択系は gradient/promote の前例から単体 net±0 リスクが高い

## G3 攻略可能性調査 (2026-07-09)

bottomup probe（G1 用に実装した SABORI_BOTTOMUP）の「prover 構成」
（budget 10M / cutoff 無効 = 壁の1段を跨ぐまで諦めない）を G3 5問に適用。
単スレ・FZN 直接実行、objective に output_var 注釈。

### 実測

| 問題 | 目的の型 | 階段の挙動 | 結果 |
|---|---|---|---|
| rcpsp13 | makespan (cumulative×4 + precedence×329) | lb 45→60 全段 step_fails=0、67=3k、75=491k、最終段76=1M〜10M fails | **OPTIMAL 完全証明 118s**（CP-SAT 12s の約10倍だが閉じる） |
| largescheduling15 | 大数値 makespan | lb 20078→151149（incumbent の 63%）まで全段 step_fails=0 | 閉じないが b20k で obj 微改善 (239442→235079) |
| mapping15 | int_max 集約 | lb 0→511 無料、そこで壁。probe jackpot が最適解 793 は即発見 | 300s でも lb=511/793 から不動 |
| depot-placement16 | int_max 集約 | lb→8191 登坂（41→544→234k fails と漸増）、そこで壁 | 300s でも lb=8191/13995 |
| liner-sf19 | 大数値線形 | 毎段 ~20k fails × obj スケール 4M | 階段が構造的に遅い。arm 微負 |

### 判定: G3 は2亜型に割れる

- **G3a（スケジュール bound 型: rcpsp, largescheduling）= 攻略可能**。
  obj≤K が cumulative/precedence に直結して強く伝播し、lb 階段の大部分が
  無料（TTEF+precedence が即答）。壁は最終数段のみで有限。
  **G1 では jackpot（SAT 側）が武器、G3a では UNSAT 側の階段が本体**という対照。
- **G3b（int_max 集約型: mapping, depot）= この装備では不可**。
  obj=max(e_i) の「obj>K」証明は全 e_i≤K の一括フィージビリティ反証で
  漸進化できない。階段は「どれかの e_i の lb が target 超え」の間だけ無料。
  節学習の領分（depot は CP-SAT ですら 30s）→ 外部ポートフォリオ枠へ。

### 含意

1. **30s 単スレ窓ではスコア不変**: prover 構成は 30s だと解探索を飢えさせる
   （rcpsp が UNKNOWN 化）。効くのは長時間 or feature/mp で、既定 b2k とは別に
   **prover ワーカー（budget 10M / cutoff off）を1本**立てる形
2. probe の enqueue_set_min による root lb はワーカー内限り →
   **lb 共有（現状 ub=best_obj のみ）が feature/mp 統合ポイント**
3. budget は壁の1段を跨げるかの閾値そのもの（rcpsp: 1M 不可 / 10M 可）。
   prover 亜種は「大きいほど良い」でグリッド既定 2000 とは別物
4. TTEF 強化（energetic reasoning 等）は G3a の壁段を直接削る候補
   （118s → CP-SAT の 12s に近づける余地）。G3b には効かない
