# CP-SAT との勝敗分析と改善方針 (2026-06-10)

データ: `benchmarks/minizinc_challenge/20260610_fable/`（MiniZinc Challenge 2012〜2025、全279問、30秒タイムアウト）
ビルド: alldiff bounds(Z) + diffn 冗長 cumulative + circuit SCC 導入後

## 総合成績

**Sabori 110勝 / CP-SAT 116勝 / Tie 37**（残りは両者 UNKNOWN/TIMEOUT 等の判定なし16）

ステータス組合せ (Sabori, CP-SAT):

| 組合せ | 件数 |
|---|---|
| OPTIMAL / OPTIMAL | 96 |
| SOL / SOL | 90 |
| SOL / OPTIMAL | 33 |
| SOL / TIMEOUT | 16 |
| TIMEOUT / TIMEOUT | 11 |
| TIMEOUT / OPTIMAL | 9 |
| OPTIMAL / SOL | 8 |
| TIMEOUT / SOL | 7 |
| UNKNOWN / UNKNOWN | 4 |
| UNSAT / UNSAT | 3 |
| TIMEOUT / UNSAT | 2 |

## 負け116件の内訳

| パターン | 件数 | 意味 |
|---|---|---|
| OPTIMAL vs OPTIMAL（速度負け） | 44 | 両者証明、CP-SAT が速い。時間比の中央値 **6.4倍**、25件は5倍以上 |
| SOL vs OPTIMAL | 33 | 解は出るが最適性を証明できない |
| SOL vs SOL（obj負け） | 19 | 解の質で負け |
| TIMEOUT vs OPTIMAL/SOL/UNSAT | 18 | 30秒で解が一つも出ない |
| UNSAT vs UNSAT（速度負け） | 2 | UNSAT 証明が遅い |

### 敗因① reified/Boolean 密度の高いモデル（最大クラスタ）

繰り返し負けている問題の FlatZinc 構成:

- **elitserien**（×4 全敗、TIMEOUT）: int_eq_reif 704 + int_lin_eq 575 + array_var_int_element 393 + int_lin_ne_reif 240
- **fillomino**（TIMEOUT、CP-SAT 0.3s）: int_eq_reif 493 + bool2int 425 + array_bool_and 88
- **solbat**（UNSAT を証明できず、CP-SAT 0.9s）: **bool_clause 3,042** + int_eq_reif 1,515 + array_bool_and 1,260 + int_ne_reif 961
- oocsp_racks（UNSAT 証明できず）も同系

実質 SAT 問題で、CP-SAT の clause learning が圧勝する領域。UNSAT 証明負けも同根。

### 敗因② 最適性証明力の不足（33件 + 速度負け44件の大半）

SOL→OPTIMAL に至れないファミリは jp-encoding / zephyrus / diameterc-mst / gfd-schedule（各2）ほか広く分布。
本質は conflict learning の質: 現状は decision trail ベースの nogood のみで、伝播グラフからの説明生成（1UIP 相当）がない。

OPTIMAL vs OPTIMAL 速度負けの極端な例: nfc 120倍、multi-knapsack 60倍、hitori 41倍、hrc 36倍、freepizza 35倍。

### 敗因③ alldifferent の伝播力不足（sudoku が象徴）

- **sudoku_opt / sudoku_fixed (p20)**: 構成は **fzn_all_different_int 75本 + int_lin_eq 1本のみ**。CP-SAT 1〜2秒に対し Sabori TIMEOUT
- 全面 GAC は過去ベンチで overhead 負けして無効化中だが、この結果は「適応的 GAC」の再評価を正当化する

### 敗因④ 解の質（SOL vs SOL 19件）

- steelmillslab: 353 vs 70 / 50 vs 37（大差）
- cargo 2018: 110,466 vs 38,433
- fox-geese-corn（MAX）: 87 vs 144、20 vs 48
- ほか cvrp / largescheduling / lot-sizing / ptv / yumi-static / traveling-tppv 等は小差

30秒の restart 反復で良解近傍を集中的に掘る仕組み（LNS）がない。

### 勝ちパターン（参考）

spot5 / gbac（各3）、project-planning / ship-schedule / solbat / vrp / black-hole / cargo / celar / nmseq / proteindesign12 / amaze / road-cons / stochastic-vrp / traveling-tppv（各2）。
VRP・スケジューリング・packing 系の探索主体の問題で、activity RL + 2026-06-10 の propagator 強化（alldiff bounds(Z) / diffn 冗長 cumulative / circuit SCC）が効く領域。

## 改善の方向性（優先順）

### 1. Boolean/reified レイヤの強化 ❌ 実測で棄却 (2026-06-11)
- gprof 再プロファイルの結果、仮説が不成立:
  - bool_clause は既に watched-literal 実装済みで、elitserien / fillomino どちらでも上位に現れない
  - elitserien の実ボトルネックは適応 GAC（63%）→ ホットパス最適化で対処済み（エコー集約 + ハイブリッド方式、work-log/2026-06-11.md）
  - fillomino の実ボトルネックは **array_var_int_element（42%、on_set_max → propagate_via_queue 172M回）** → 次の最有力候補
- BoolDomain 自体はリファクタ計画フェーズ5の項目として存続（伝播スループット最適化として）。「reif 密集問題の敗因」への直撃打は施策②（conflict learning）に変更

### 2. Conflict learning の質向上 — 1件あたりのリターン最大
- decision-trail nogood → 制約が矛盾理由を説明する 1UIP 風学習へ
- 敗因②（33件）と UNSAT 証明に直撃
- 大工事のため、頻出制約（int_lin / alldifferent / bool_clause）だけ説明を実装する段階導入が現実的

### 3. LNS（Large Neighborhood Search）の導入 — 定番で確実
- restart 時に best solution の一部を固定して近傍探索
- 敗因④（19件）と②の一部に効く。既存の restart 基盤 + `set_hint_solution` に乗せられる

### 4. alldifferent の適応的 GAC — 小さく確実 ✅ 実施済み (2026-06-10)
- 「変数数 4〜32 かつ 値域スパン ≤ 32」のときだけ Régin GAC（実装済み・無効化中だった）を有効化
- **結果**: bench_alldiff で Timeouts 4→1。解なし→解ありが3件（sudoku_opt は CP-SAT の最適値と同じ obj=-3 に到達、sudoku_fixed、elitserien handball2）。リグレッションなし
- 注意: sudoku_p20 は 25×25 なので閾値 24 では弾かれる（32 に設定）。詳細は work-log/2026-06-10.md

### 推奨着手順: 4 → 1 → 3 → 2

4 は既存コードのゲート調整で即試せて sudoku で効果検証が明確。1 はリファクタリング計画と重なり一石二鳥。2 は最大リターンだが、1 で Boolean 基盤を整えた後が安全。

## 検証方法のメモ

- 各施策のゲート: 該当ベンチ（bench_alldiff.py 等）+ 全体 `bench_compare.py` で Wins 同等以上・Timeouts 同等以下
- ピンポイント検証: sudoku_p20（施策4）、elitserien handball1（施策1）、solbat UNSAT（施策2）、steelmillslab（施策3）
- マージナル差はノイズで反転するため、際どい判定は fzn 直接実行で決定論比較
