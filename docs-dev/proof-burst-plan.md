# Proof Burst 実験計画 (2026-06-14)

## 背景と狙い

このソルバーは LCG なしで好成績を出すよう探索側(improvement probe / best-assignment
再降下 / restart EMA バンディット / 制約別 init_activity)を作り込んである。
全年度ゲートで静的 full `-L` は **56/55-58**（ほぼ中立）。CDCL 流の節工学
(seen-bump / NG-bump 除去 / 節最小化)は全て探索平衡を乱して逆効果だった。

codex の診断: 学習 nogood は受動的でなく watch 伝播・activity bump・
`ng_usage_bloom_` のタイブレークに**能動干渉**する。よって「LCG を強化する」
でなく **「探索と喧嘩させない統合」= incumbent 停滞時のみ LCG を起動する
proof burst** が最有力。

LCG の正味価値は「探索が原理的に届かない証明」(fillomino/solbat UNSAT,
cg2023 OPTIMAL)に限定。これを**主探索の primal 性能を落とさずに**取り込むのが目標。

## 鍵となるコード事実

- ベースラインは `handle_failure` で prefix nogood を学習（`nogood_learning_`、既定 on）。
- LCG の 1UIP 節は `learn_at_conflict`(try_enumerate/try_bisect の衝突点)で
  **prefix nogood に上乗せ**して追加される。
- したがって **LCG の 1UIP パスだけをランタイム gate すれば、ベースライン挙動
  (prefix nogood のみ)を完全保持したまま burst 中だけ LCG を足せる**。
- 既存の状態: `optimize_no_incumbent_`(初解前 true)、`in_probe_`(probe 中)、
  `improvement_in_restart_`、`restart_max_depth_`、transient nogood 機構、
  `SABORI_LEARN_MODE` bit8(初解まで遅延)/bit64(probe 除外)/bit32(transient)。

## 設計

### 状態の分離

新フラグ `learn_active_`(ランタイム可変)を `learning_enabled_`(`-L`/auto で起動、
trail 記録 on)とは別に導入する。

- `learning_enabled_` = 記録の有無(trail recording)。
- `learn_active_` = 今 proof burst 中か(analyze_conflict → 1UIP 節追加 + bump)。

`learn_active_ == false` の通常探索では `learn_at_conflict` は記録のみで節を
追加しない(早期 return)→ **ベースラインとビット同等**。

記録(record_inference)は burst 開始リスタートの先頭で on にする
(リスタートで trail は root へ切り詰められるので、その周回の伝播で trail が
正しく再構築され、その周回の衝突を解析できる)。常時記録のオーバーヘッドを避ける。

### regime A: 最適化問題(incumbent あり)

停滞トリガ:
- `restarts_since_improvement_` をリスタート毎に +1、改善で 0 リセット。
- 閾値 `K`(初期案 30)を超えたら burst 入り。
- 追加トリガ候補: probe が連続 `P` 回 UNSAT/UNKNOWN(改善余地が薄いシグナル)。

burst 中(`learn_active_ = true`):
- record + analyze_conflict + 1UIP 節を **transient** で追加 + clause bump 許可。
- probe の外でのみ学習(bit64 相当を burst 中も維持)。
- 予算 = min(`B` リスタート, `T` 秒, `F` fails)。決定論のため fails/restarts 主、
  wall-time は保険。
- burst 中に改善が出たら即 burst 退出。

burst 退出時:
- 改善あり → transient 全破棄(旧 incumbent 近傍の節は新経路を塞ぐ。既存
  `remove_transient` を流用)、`restarts_since_improvement_ = 0`。
- 改善なし(予算切れ)→ transient のうち **短い説明済み節 / 目的関連節 / unit のみ**
  permanent 昇格、残り(fallback/demote/長節)は破棄。
- **activity 隔離**: burst 突入前に activity_ をスナップショット。改善なし退出時は
  復元 or ブレンド(主探索の activity を burst で汚さない)。改善あり退出時は
  burst が当てた経路なので保持。← 要 A/B(knob)。

### regime B: 満足問題 / UNSAT 系(incumbent なし)

probe/incumbent 機構が無いので「探索と喧嘩」の主因が小さい。
- 初期案: `R` リスタート無解で burst 入り(または最初から learn_active_)。
- fillomino が -L で UNSAT 1秒なので、満足問題は LCG を早期に効かせる方が良い
  可能性。regime B は「短い遅延後に learn_active_ 恒常 on」を既定にして A/B する。

### clause import policy(burst 退出時の永続化)

タグ付け: 各学習節に `explained`(fallback でない)/長さ/目的関連(目的変数 or
その線形係数に現れる変数を含む)を記録。
- permanent 昇格条件: `unit` OR (`explained` AND `len ≤ Lmax`(初期 12)) OR
  `目的関連 AND len ≤ Lobj`(初期 20)。
- それ以外は破棄。fallback 汚染(demote 由来)は持ち越さない。

## 実装フェーズ

1. **F1: learn_active_ 分離**(ベースライン保全)
   - `learn_active_` 追加。learn_at_conflict / process_queue の記録・bump を
     `learn_active_` で gate。`learning_enabled_ && !learn_active_` は記録のみ。
   - 検証: learn_active_ を恒常 false にして全スポット(protein/cg2023/solbat/
     roadcons)が **base と完全一致**(軌道中立)、ctest 202/202。

2. **F2: 停滞トリガ + burst 制御**(regime A)
   - `restarts_since_improvement_`、burst 突入/退出、予算 min(B,T,F)。
   - env knob: `SABORI_PROOFBURST=1`、`SABORI_PB_K`(停滞閾値)、`SABORI_PB_B`
     (burst 長)、`SABORI_PB_LMAX`。既定 off。
   - 検証: amaze/fillomino 健全(SOL/正しい UNSAT)、solbat UNSAT 維持、
     primal スポット(cargo2018/yumidyn 等)が base 近傍。

3. **F3: clause import policy + activity 隔離**
   - 節タグ付け(explained/len/obj-linked)、退出時昇格、activity snapshot/restore。
   - 検証: 同上 + 節昇格数・burst 回数の診断出力(`-s`)。

4. **F4: regime B**(満足/UNSAT の早期 LCG)

## 検証プロトコル(codex 反映)

- **leave-one-year-out** で閾値(K/B/Lmax)を選ぶ。第2インスタンス holdout は
  「易しいインスタンスへの過適合」で auto-LCG が失敗した(in-sample +15 →
  holdout -17)教訓を踏まえ、年単位の汎化を見る。
- **primal 中立性**の必須計測: 初解到達時間、目的の area(改善軌道)、
  OPTIMAL/SOL ステータス。proof-burst は「primal を落とさず証明勝ちを足す」が
  合格条件。証明勝ち(UNSAT/OPTIMAL の純増)を retain。
- 健全性: bench_learning_gate.py の SOL↔UNSAT 矛盾チェック常設(0 必須)、
  新規 UNSAT/OPTIMAL は CP-SAT で ground truth 確認。
- MiniZinc Challenge の scoring 注意: complete は証明を評価するが
  incomplete/area は最適性証明を無視し軌道を評価 → proof-burst が area を
  害さないことを確認。デフォルト化は primal 中立 + 証明勝ち retain を満たしてから。

## 期待結果と撤退条件

- 期待: 静的 full の負け(56/55-58)を base 近傍に戻しつつ、証明勝ち
  (fillomino/solbat/cg2023 等 ~7問)を retain → **純プラスのデフォルト候補**。
- 撤退: F1 で軌道中立が崩れる(実装バグ)/ F2 で primal が落ちる(burst が
  主探索を乱す)/ leave-one-year-out で汎化しない → proof-burst も棚上げし、
  `-L` は静的 full オプトインのまま据え置き。

## 段取り

F1 から着手。各フェーズで「軌道中立 → 健全性 → スポット → ゲート」の順に確認し、
F1/F2 が緑なら全年度ゲート(leave-one-year-out 込み)で採否判定。
