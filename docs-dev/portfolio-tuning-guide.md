# マルチスレッド・ポートフォリオ チューニング手順

`make_portfolio_configs`（`src/core/parallel_solver.cpp`）の多様化ラダーを
再チューニングするための手順書。初版 2026-07-03。

## いつ再チューニングが必要か

ラダーの根拠は「特定バイナリでの各構成の探索挙動」なので、**単一スレッドの探索
軌道が変わる変更**を入れたら失効する。具体的なトリガー:

- 変数選択・activity・restart 等のヒューリスティクス変更
- propagator の伝播タイミング/強度の変更（枝刈りが変わる = 軌道が変わる）
- 探索順に影響する決定性修正（例: 2026-07-02 の int_lin_base ポインタ順→初出順）
- 新しい多様化軸（WorkerConfig の新フィールド）の追加

コメントだけの変更や、探索順に影響しない最適化（同一軌道で速くなるだけ）は不要。

## 設計原則（2026-07-03 の教訓）

1. **軸とシードを直交させて測る。** 過去の失敗2パターン:
   - 軸ごとに別シードを割り当てる → 軸の Δ にシード運が混入（旧
     bench_portfolio_diversity.py。旧1位 conflict Δ+0.062 の実態はシード効果）
   - 全構成シード固定 → シード多様性の価値がゼロ評価（旧 bench_restart_sat.py）
2. **軸の真の寄与 = 同シード差分** `mean_s[score(axis,s) − score(base,s)]`。
   2026-07-03 計測では全 ablation 軸が平均マイナスで、純シード変種が opt の
   第一選択だった。軸は「特定インスタンスでの大勝ヘッジ」としてのみ配置する。
3. **worker0 = base 固定**（単一スレッドより悪くならない保証）。変えない。
4. **単一スレッド VBS 予測は実並列の下界。** objective bound 共有の超加法性がある
   （実測: elitser14 で -j2 obj=6 < base単独 7 / worker1単独 9）。
   採否の最終判定は必ず実並列 S(k) で行う。
5. **新旧比較は統一レンジで再正規化。** score の best/worst 正規化は run 内
   クローズドなので、別 run の値を直接比較してはいけない。

## 手順

前提: Release ビルド済み（`cmake --build build`）、ベンチ実行前に
`pkill -x fzn_sabori; pkill -x minizinc`（`-f` は自己killするので禁止）。
各スクリプトは冒頭で自前 cleanup を呼ぶ。並列は最大4プロセス。

### Step 0: fzn キャッシュの準備（初回 or 問題セット変更時のみ）

- opt セット: `bench_portfolio_diversity.py` を一度実行すると
  `/tmp/portfolio_div/*.fzn` にキャッシュされる（16問、SAT/OPT混合）
- SAT セット: `bench_restart_sat.py` を一度実行すると
  `/tmp/restart_sat/*.fzn` にキャッシュされる（costas/word-equations 等 11問）

/tmp なので再起動で消える。消えていたら上記を再実行してフラット化から。

### Step 1: 現状ラダーの S(k) 実測（ベースライン）

```bash
cd benchmarks/minizinc_challenge
python3 bench_scaling.py          # k=1,2,4,8 実並列、~10-15分
mv /tmp/bench_scaling/results.json /tmp/bench_scaling/results_oldladder.json
```

読み方:
- `S_all` の ΔS が k の増加でどこで平らになるか（flatten 点）を見る
- k=1 は決定的なので1回、k>1 は3反復平均。目的値レース系（celar/tppv）は
  ±0.3 程度ぶれるので、反復平均でも ΔS < 0.05 はノイズ内と扱う
- `proved`（最適証明数）が k で伸びない場合は協調（bound 共有以上）の課題で、
  ラダーでは解決しない

### Step 2: 軸×シード分離グリッド

```bash
python3 bench_axis_seed_grid.py             # opt セット、~15-25分
python3 bench_axis_seed_grid.py --set sat   # SAT セット、~20-30分
# 再分析のみ: --analyze を付ける（JSON キャッシュから）
```

読み方（それぞれ opt / sat 別に出る）:
- **軸の真の寄与**（同シード差分）: プラスの軸だけが「平均的に効く」軸。
  per-seed Δ が符号バラバラの軸はインスタンス/シード特異的（=ヘッジ枠候補）
- **純シード変種のみの VBS(k)**: 「軸なしでシードを増やすだけ」の基準線。
  軸構成はこれに勝てて初めてスロットを得る資格がある
- **貪欲 VBS ラダー**: worker0=base@S0 固定で限界ゲイン順に選んだ列。
  Δ=+0.000 が続き始めたら、その先は単一スレッド VBS では区別できない
  （実並列では bound 共有分の上積みがあるので無価値ではない）
- 特定インスタンスの解禁がどの構成かは、`-j` の二分（例: -j5/6/7）で解禁
  ワーカーを特定 → そのワーカーの (軸, シード) を env で単体再現して
  「軸のみ / シードのみ / 両方」に分解して確認する

### Step 3: ラダー更新

`src/core/parallel_solver.cpp` の `make_portfolio_configs` の switch を編集。

- 構成: 「シード優先・軸は疎に」— 純シードスロット（case で何もしない）と
  証拠のある軸を交互配置。7スロット（k = (i-1)%7、n>8 は循環）
- 貪欲ラダーで Δ>0 だった軸を前へ、平均プラスまたは特異的大勝ちの軸を
  ヘッジとして後ろへ、平均が強い負の軸は排除
- 根拠（スクリプト名・日付・主要数値）をコメントに残す
- worker0 と単一スレッド経路は不変なので golden には影響しない

```bash
cmake --build build && ctest --test-dir build   # 261テスト green を確認
```

### Step 4: 新ラダーの S(k) 実測と統一比較

```bash
python3 bench_scaling.py    # 新ラダーで再計測（results.json に上書き）
```

新旧を**統一レンジで再正規化**して比較する（インラインスクリプト例）:

```python
import json
old = json.loads(open('/tmp/bench_scaling/results_oldladder.json').read())
new = json.loads(open('/tmp/bench_scaling/results.json').read())
kinds = new["instances"]; ks = new["ks"]
ranges = {}
for i, kind in kinds.items():
    objs = [r["obj"] for d in (old, new) for k in ks
            for r in d["results"][i][str(k)]
            if r["obj"] is not None and r["status"] in ("OPT", "FEAS")]
    ranges[i] = ((0,0) if not objs else
                 (min(objs), max(objs)) if kind == "minimize" else
                 (max(objs), min(objs)))
def score(kind, r, best, worst):
    if kind == "satisfy":
        return 1.0 if r["status"] in ("SAT", "UNSAT") else 0.0
    if r["obj"] is None or r["status"] in ("UNSAT", "NOSOL", "HANG"):
        return 0.0
    base = (1.0 if best == worst else
            (worst - r["obj"]) / (worst - best) if kind == "minimize" else
            (r["obj"] - worst) / (best - worst))
    return min(1.0, base * 0.97 + (0.03 if r["optimal"] else 0.0))
def S(d, k):
    vals = []
    for i in kinds:
        rs = d["results"][i][str(k)]
        vals.append(sum(score(kinds[i], r, *ranges[i]) for r in rs) / len(rs))
    return sum(vals) / len(vals)
for k in ks:
    o, n = S(old, k), S(new, k)
    print(f"k={k}: old={o:.3f} new={n:.3f} Δ={n-o:+.3f}")
```

**採用基準**: k=1 が新旧一致（健全性チェック。単一スレッド経路を触っていない
証明）、かつ全 k で新 ≥ 旧（ノイズ幅考慮）、かつ低 k（2,4）の傾きが改善。
満たさなければリバート。

### Step 5: 記録

- 本ファイルの「チューニング履歴」に追記
- 大きな知見はメモリ（portfolio-ladder-seed-first 等）も更新
- work-log に計測値を残す

## 既知の落とし穴

- **WSL2 のクロックジャンプ**: 長時間ベンチ中に elapsed が負になったり
  timeout 超えの値が出ることがある。スコアは status/目的値ベースにして
  時間は参考値と扱う
- **SAT セットの識別力**: satisfy 問題は「解けた/解けない」が2値なので
  識別インスタンスが少ない（2026-07 時点で実質 costas と solbat）。
  SAT ラダーの結論は opt より弱い証拠に基づくことを自覚する
- **12s timeout への過学習**: 本番（MZC は 5-20分）とは時間スケールが違う。
  ラダーの相対順は移りやすいが、絶対値は参考程度
- costas18 は flatten timeout で fzn キャッシュが無い（スキップされる）

## チューニング履歴

| 日付 | トリガー | 結果 |
|------|---------|------|
| 2026-06-30 | 初版ラダー（bench_portfolio_diversity / bench_restart_sat） | 軸のみ7スロット。後に confound 判明 |
| 2026-07-03 | 決定性修正(int_lin_base)で旧データ失効 + confound 発見 | 「シード優先・軸は疎に」へ再構成。S(k)=0.712/0.799/0.876/0.885（旧 0.712/0.763/0.738/0.870）、solbat 解禁 k=8→k=2 |
