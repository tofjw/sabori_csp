# Conflict Learning (LCG) 設計書

作成: 2026-06-11 / 改訂 v1.1: 2026-06-11(レビュー反映)
状態: 設計ドラフト(実装前)
改訂内容: ①発生源は PendingUpdate.source_cid を一次情報に(RAII ガード+assert で規律化)
②decision-approx をシンボリック化し O(1)/fallback に ③int_lin の説明は「推論時点の bounds」を
トレイル巻き戻しで再構成(現在 bounds の使用を明示的に禁止) ④explain/propagate の二重管理対策を新設
⑤推論トレイルから level フィールドを削除(境界配列方式) ⑥Ne 制限の効果リスクに計測チェックポイント設定
前提: propagator queue 第一段階 + バッチ化横展開 完了済み

## 1. ゴールと制約条件

**ゴール**: 伝播由来の説明(reason)から 1UIP 風の学習節を生成し、
fillomino / solbat / amaze 級(Chuffed のみが解ける群、sabori 300秒でも解なし)を解けるようにする。

**設計上の制約条件**(本書の全判断の基準):

- **C1: 低オーバーヘッド** — 探索が速いことが現在の勝ち領域(VRP/スケジューリング系 110勝)の源泉。
  learning off 時はゼロコスト、on 時も衝突が起きない限りほぼゼロコストであること
- **C2: 部分サポートで破綻しない** — 説明を実装した制約が一部だけでも健全に動き、
  説明ゼロの状態では**現行の decision-trail nogood と正確に同じもの**に退化すること
  (= 導入による後退がない)

## 2. 既存資産(再利用するもの)

| 資産 | 場所 | 役割 |
|------|------|------|
| `Literal {var, value, Eq/Leq/Geq}` + 否定 | solver.hpp | 学習節の原子。そのまま使う |
| NoGoodManager(eq watch + **bound watch** + GC + permanent + bloom) | nogood_manager.* | 学習節の保存・伝播。そのまま使う |
| decision-trail nogood (`learn_from_conflict`) | nogood_manager.cpp | フォールバック先(C2 の退化先) |
| 伝播キュー一元化(`pending_updates_` + `process_queue`) | model/solver | 全ドメイン変更が単一経路を通る = 理由記録の挿入点が1箇所 |
| バッチ実行点(`propagate_batch`) | constraint.hpp | 説明付き伝播の自然な挿入点 |
| 固定シード決定論 | solver.cpp | 等価性テスト・決定論比較が可能 |

## 3. アーキテクチャ概要

```
[伝播]                          [衝突時のみ]
enqueue_* ──→ process_queue ──→ 矛盾検出
   │             │                  │
   │   (L on時) 推論トレイル追記      │ 1UIP 分析(トレイルを逆走)
   │   {lit, reason_cid}             │   ├─ reason_cid あり → constraint->explain() [遅延]
   │             │                  │   └─ reason_cid なし → decision-approx 置換 ★C2
   └─ 発生源追跡 ─┘                  ▼
   (current_propagator_)        学習 nogood → NoGoodManager::add_nogood (既存2WL)
```

### 3.1 発生源追跡(オーバーヘッドの要)

**一次情報は `PendingUpdate.source_cid`(enqueue 時点でスナップショット)**。
処理時点の状態から発生源を推測することは決してしない(ネストで意味が壊れるため)。

```cpp
struct PendingUpdate { Type type; size_t var_idx; value_type value; uint32_t source_cid; };
```

`enqueue_*` の呼び出し側(全制約)に発生源引数を足すのは侵襲が大きいので、
書き込み値の供給は Model の `current_propagator_` 経由とするが、以下で規律化する:

```cpp
// RAII ガード: process_queue / propagate_batch 実行点 / 決定 / nogood 伝播が必ず使う
struct ScopedPropagator {
    Model& m; uint32_t prev;
    ScopedPropagator(Model& m, uint32_t cid) : m(m), prev(m.current_propagator_) { m.current_propagator_ = cid; }
    ~ScopedPropagator() { m.current_propagator_ = prev; }   // 例外・早期 return でも復元
};
```

- **スコープは「コールバック1回の呼び出し」単位**。コールバック内で enqueue された更新は
  すべてその時点の cid がスナップショットされるため、後続の別 propagator 起動
  (A→enqueue→B→enqueue)でも A の enqueue は A、B の enqueue は B と一意に決まる
- デバッグビルド assert: コールバック実行中 `current_propagator_ == その制約の model_idx`
  であること、および PendingUpdate 処理時に source_cid が有効範囲であることを常時検証
- 決定・nogood 伝播・presolve はセンチネル値(kSourceDecision / kSourceNoGood / kSourcePresolve)

コスト: enqueue 時に 4 バイトコピー1回 + RAII の代入2回/コールバック。分岐なし。

### 3.2 推論トレイル

```cpp
struct InferenceEntry {     // 16 bytes 目標, POD
    int64_t value;          // リテラルの値
    uint32_t var_idx;       // 変数
    uint32_t reason_cid : 24;  // 発生源 (制約 model_idx or センチネル)
    uint32_t lit_type   : 8;   // Eq / Leq / Geq
};
std::vector<InferenceEntry> inference_trail_;        // Solver 保持
std::vector<uint32_t>       level_start_;            // level → トレイル開始位置
```

- **level はエントリに持たない**(レビュー指摘)。`level_start_[k]` = レベル k 開始時の
  トレイル長(決定ごとに1要素 push、バックトラックで pop — トレイル切り詰めにも使う)。
  分析はトレイルを逆走するので、境界配列との突き合わせで level 判定は O(1) 償却
- 16B/推論 × ノード内推論数。バックトラックで level_start_ ごと切り詰めるため、
  常駐サイズは「現在のパス上の推論数」のみ(var trail と同オーダー)

- 記録点は `process_queue` の各 case で**ドメイン変更が成功した直後**(1箇所ずつ、計4箇所)
- `if (learning_enabled_)` の単一分岐 + push_back のみ ★C1
- RemoveValue は v1 ではトレイルに**記録しない**(後述 §5 のリテラル表現の制約)。
  ただしバイナリドメイン(0/1)の remove は `[x = 1-v]` の Eq として記録(bool 密集問題が主目標のため)
- バックトラック時は level_start_ を使って var trail と同時に切り詰め

### 3.3 説明インターフェース

```cpp
// Constraint に追加(デフォルト = 説明不可)
struct Explainer {
    // 「この制約が lit を推論した理由」を out に積む。
    // 規約: out の各リテラルは (a) 現在 true、(b) トレイル上で lit より前に成立、
    //       (c) lit 自身を含まない。守れない場合は false を返す(フォールバック)
    virtual bool explain(const Model&, const InferenceEntry& inf,
                         std::vector<Literal>& out) { return false; }

    // 「この制約が矛盾を検出した理由」(衝突節の種)
    virtual bool explain_failure(const Model&, std::vector<Literal>& out) { return false; }
};
```

- **遅延説明**(衝突時にのみ呼ばれる)。伝播ホットパスには一切コードが入らない ★C1
- 規約 (b) = acyclicity。デバッグビルドでは assert で検証(reason リテラルのトレイル位置 < lit の位置)

### 3.4 部分サポートの健全性: decision-approx フォールバック ★C2

衝突分析中、リテラル ℓ(レベル k で成立)の説明が得られない場合
(reason_cid がセンチネル / explain() が false / 制約が未対応):

```
reason(ℓ) := { レベル 1..k の決定リテラル }
```

これは常に健全(レベル k までの全推論は決定 d1..dk の論理的帰結)。

**実装はシンボリック**(レビュー指摘の O(level) 展開×回数を回避):
decision-approx(k1) ∪ decision-approx(k2) = decision-approx(max(k1,k2)) なので、
リテラル集合を実体化せず **単一の整数 `approx_level` を持つだけでよい**:

```
フォールバック発生:  approx_level = max(approx_level, level(ℓ)); N から ℓ を除去   // O(1)
分析終了時に1回だけ: N ∪= decision_trail_[0 .. approx_level)                      // O(level)×1
```

- フォールバック1件あたり **O(1)**。depth=500 で未説明リテラルが大量でも分析コストは増えない
- 1UIP 終了判定は「現レベルのリテラル数」で行うが、approx_level == 現レベル のときは
  現レベルの決定リテラル自体が N に入る扱い(カウントに +1)として整合させる
- この置換により:
  - **分析は常に完走する**(説明できないリテラルで止まらない)
  - **説明ゼロの極限では approx_level = 現レベル となり、学習節 = decision-trail nogood と一致**。
    最悪ケースが「今日の挙動」であり、説明の実装分だけ単調に節が鋭くなる

## 4. 衝突分析(nogood 形式の 1UIP)

学習対象は節ではなく nogood(「同時に成立してはならない事実集合」)で統一する
(既存 NoGoodManager の表現に合わせる。節 ↔ nogood は否定で等価)。

```
入力: 矛盾を検出した制約 C、現レベル L
N := explain_failure(C)  /  失敗時は decision-approx(L)
loop:
    cur := { ℓ ∈ N | level(ℓ) == L }
    if |cur| <= 1: break                 // 1UIP 到達
    ℓ := cur のうちトレイル位置が最後のもの
    R := explain(ℓ) / decision-approx(level(ℓ))   // ℓ の理由
    N := (N \ {ℓ}) ∪ R                   // 解決
    N を支配関係で簡約(同一変数の bounds は最強のみ残す、true な定数は除去)
出力: add_nogood(N)(既存の 2WL/bound watch/GC に乗る)
バックジャンプ: assertion level = N 中の第2位レベル … は Phase L3(v1 は通常バックトラック)
```

- 分析バッファは再利用(per-conflict のヒープ確保なし)
- N の最大サイズ防御: 上限(例: 256 リテラル)超過時は decision-approx に全面フォールバック
  (= 今日の nogood)して打ち切り。爆発防止 ★C1

## 5. リテラル表現の制約(v1)

- 学習節に入れるのは **Eq / Leq / Geq のみ**(既存 watch 機構がそのまま使える)
- `[x ≠ v]` は v1 では扱わない:
  - バイナリドメインでは `[x = 1-v]` に正規化(主目標の bool 密集問題はこれで足りる)
  - 多値ドメインの remove 推論は reason_cid のみ記録せずスキップ → その事実を理由に持つ推論は
    decision-approx に落ちる(健全・やや弱い)
  - Ne watch の追加は Phase L3 以降の拡張(NoGoodManager に remove/instantiate 連動の watch が必要)

**Ne 制限の効果リスク**(レビュー指摘): fillomino 系は ≠ 推論が多く、remove 由来の事実が
decision-approx に落ちる分、L2 の効果が見積もりより伸びない可能性がある。
緩和要因: fillomino の ≠ は int_eq_reif/int_ne_reif の b 変数(バイナリ = Eq 正規化可能)を
経由する割合が高い。**L2 のゲートで fillomino が動かなければ、Ne リテラル対応(L3 項目)を
前倒しする**という判断チェックポイントを置く。

## 6. 説明を実装する制約(優先順)

ターゲット問題の構成(solbat: bool_clause 3,042 + reif、fillomino: int_eq_reif 493 + lin)から:

| Phase | 制約 | 説明のコスト | 実装方式 |
|-------|------|-------------|---------|
| L1 | **bool_clause** | ゼロ(節自体が理由) | 遅延: unit 伝播の理由 = 「他の全リテラルの否定」。節は静的なので payload 不要 |
| L1 | NoGoodManager の nogood 伝播 | ゼロ(nogood 自体が理由) | 同上(学習節からの伝播も説明可能 = 解決が連鎖する) |
| L2 | **int_lin_le / int_lin_eq** | 中(本設計の最難所) | 遅延 + **推論時点の bounds を再構成**(§6.1)。現在 bounds の使用は禁止 |
| L2 | **int_eq_reif / int_ne_reif / int_le_reif 系** | 小 | 遅延: b の確定理由 = x,y の bounds / x,y の絞り込み理由 = b と相手の bounds |
| L3 | alldifferent(bounds(Z)/GAC)、element | 中〜大 | Hall 区間 / SCC からの説明。効果を見て判断 |

**重要**: どの Phase でも未対応制約はフォールバックで共存する(C2)。
L1 だけで solbat(bool_clause 支配)が、L2 で fillomino / elitserien(reif+lin 支配)が射程に入る。

### 6.1 int_lin の説明: 推論時点の bounds の再構成(レビュー指摘の最重要リスク)

**現在の bounds を理由に使ってはならない**。推論後に他項がさらに縮んでいる場合、
「未来の事実」を理由に引用することになり、acyclicity が壊れて分析が不正になる。

必要なのは「推論時点 T における各項の bounds」。eager コピー(推論毎に O(項数))は
C1 に反するため、**遅延再構成**方式を採る:

- 推論トレイルの index がそのまま大域タイムスタンプ T になる
  (learning on 時は var trail の bounds 変更と推論トレイルが 1:1 対応)
- 衝突分析で int_lin の推論(位置 T)を説明する際、**推論トレイルを末尾から T まで
  1回走査**し、当該制約の変数集合についてだけ「T 時点の bounds」スナップショットを構成
  (Leq/Geq エントリの逆適用。1衝突内の複数説明でスナップショットをキャッシュ・再利用)
- 理由リテラル [x_j ≥ v_j(T)] の成立位置は必ず T より前(T 時点で既に true だったため)、
  acyclicity は構成的に保証される
- コスト: 衝突時のみ、O(現レベルのトレイル長)。デバッグ assert(§9)で正当性を常時検証

**この再構成ユーティリティは L0 で先行試作し、マイクロベンチで衝突あたりのコストを
計測してから L2 に進む**(レビュー結論)。コストが許容外なら代替案
(int_lin のみ推論時に「他項 bounds の eager 圧縮コピー」= 係数順の固定長配列)に切り替える。

### 6.2 explain() / propagate() の二重管理対策(保守性)

「propagate を直して explain を直し忘れ → false UNSAT」が最大の長期負債(レビュー指摘)。対策:

1. **共有コア規約**: 説明は propagate と同じ計算・同じフィールドから導出する
   (例: int_lin の説明は propagate_bounds が使う potential/slack ヘルパを共用。
   独立に理由を再導出する実装はレビューで却下する)。constraint-implementation-guide に明文化
2. **制約ごとの `debug_check_explanation`**: デバッグビルドでは生成された全説明に対し
   「理由 ⊨ 推論」を制約固有の算術で直接検証(int_lin なら Σ bounds から推論 bound が出ることを再計算)。
   propagate と explain がズレた瞬間に assert で落ちる
3. **フォールバック率の統計**: -s 統計に制約種別ごとの explain 成功/フォールバック数を出す。
   リファクタで explain が silent に死んだ場合(false を返すだけ)も統計の変化で検出できる
4. **「説明を一時的に無効化してよい」運用**: propagate を大きく変える際は explain を
   false 固定にして出荷してよい(C2 によりフォールバックで健全なまま)。
   説明の修正を別コミットに分離できるため、二重修正の同時性を強制しない

## 7. オーバーヘッド予算と統制 ★C1

| 経路 | learning off | learning on(衝突なし) | 衝突時 |
|------|-------------|------------------------|--------|
| enqueue_* | +4B コピー | 同左 | — |
| process_queue 成功パス | 分岐1回 | +push_back(16B POD) | — |
| 伝播コールバック | 変更なし | 変更なし | — |
| 衝突 | 変更なし | — | 分析(トレイル長に比例) + 説明呼び出し |

- **off 時はビット同等の探索**(分岐1回と 4B コピーのみ)。`-N`(既存 nogood off フラグ)と同系の `-L` フラグ
- on 時の予算: 勝ち領域ファミリ(VRP/スケジューリング)のベンチ非劣化をゲートにする
- **適応キルスイッチ**: リスタート N 回ごとに学習節の prune 貢献(既存 prune_count)を確認し、
  decision nogood 比で増分ゼロが続けば推論トレイル記録を停止(決定論: カウントベース、時刻不使用)
- メモリ: 推論トレイル ≈ var trail と同オーダー(16B/推論、ノード毎に巻き戻し)

## 8. 段階導入計画と各ゲート

| Phase | 内容 | 完了条件 |
|-------|------|---------|
| **L0** | 発生源追跡 + 推論トレイル + 1UIP 分析(全制約フォールバック)。**先行試作: (a) シンボリック decision-approx の衝突あたりコスト計測 (b) §6.1 のトレイル再構成ユーティリティ + マイクロベンチ** | (1) off 時: 全ベンチで探索統計が既存と完全一致(決定論比較)。(2) on 時: 生成 nogood が decision nogood と一致することを assert で確認 = C2 の退化性の実証。(3) 試作 (a)(b) のコストが予算内(勝ち領域ベンチ非劣化)。ctest + ランダム全解数照合 |
| **L1** | bool_clause + nogood 伝播の説明、explain_failure | ピンポイント: solbat UNSAT(Chuffed 13.5s)、oocsp(同 1.4s)。ゲート: 3ファミリベンチ + 勝ち領域非劣化 + 標準手順(解なしTO/ステータス遷移/60秒検証) |
| **L2** | int_lin 系 + reif 系の説明 | ピンポイント: fillomino(Chuffed 0.4s)、elitserien_h1 証明(現17.5s、Chuffed 4.3s)、amaze。ゲート同上 + 全年度ベンチ |
| **L3** | assertive backjump、節最小化(再帰的 minimization)、Ne リテラル、alldiff/element 説明 | 効果測定の上で個別判断 |

各 Phase は「1Phase = 実装 → 決定論ピンポイント → ベンチゲート → 採用/取り下げ」のサイクル
(バッチ化横展開と同じ規律)。

## 9. 検証設計

1. **退化性テスト(C2 の要)**: L0 で「説明ゼロの学習節 ≡ decision nogood」をユニットテストで固定。
   以後この性質が partial coverage の安全網であることを回帰で保証
2. **acyclicity assert**(デバッグビルド): 全説明リテラルのトレイル位置 < 被説明リテラルの位置
3. **健全性 assert**(デバッグビルド): 説明リテラルが全て現在 true
4. **false-UNSAT ガード**: ランダム全解数のブルートフォース照合(alldiff/circuit で実績ある方式)を
   学習 on/off 両方で実行し解数一致を確認
5. **off 同等性**: `-L` off で代表 fzn の `-a` 出力 + 探索統計が変更前と完全一致

## 10. 主なリスクと対策

| リスク | 対策 |
|--------|------|
| 説明の acyclicity バグ → 不健全な節 → false UNSAT | assert 群 + ランダム照合 + Phase 毎の決定論ピンポイント |
| 勝ち領域の劣化(記録オーバーヘッド) | off 時ゼロ保証 + 適応キルスイッチ + 勝ち領域ベンチをゲートに含める |
| 節爆発(巨大 reason) | サイズ上限 + decision-approx 打ち切り。既存 GC/容量制限に乗せる |
| int_lin の遅延説明で古い bounds が必要 | 推論時に bounds トレイル位置を 1 int 記録(eager コピーはしない) |
| バックジャンプと既存 SearchFrame/restart の整合 | v1 は通常バックトラック(節は 2WL 伝播で効く)。backjump は L3 で分離 |

## 11. 本設計が満たすこと(要求との対応)

- **C1(低オーバーヘッド)**: 記録 = 分岐1+POD push、説明 = 衝突時のみの遅延呼び出し、
  off = ビット同等、適応キルスイッチ、勝ち領域ベンチがゲート
- **C2(部分サポート)**: decision-approx フォールバックにより任意のサポート率で健全。
  サポート 0% の極限 = 現行実装に正確に退化(単調改善のみが起こる構造)
