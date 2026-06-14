# 実測判定: conflict learning vs propagator queue (2026-06-11)

[cpsat-gap-analysis-2026-06-10.md](cpsat-gap-analysis-2026-06-10.md) の残施策のうち、
どちらが有望かを本実装前に「オラクル実験」で判定した記録。

## 方法

事前に判定ルールを固定した上で2つの実験を実施。

- **実験1 (参照ソルバー・オラクル)**: 壁問題8問を Gecode 6.3（学習なしCP）と
  Chuffed 0.13.2（学習ありCP/LCG、sabori+学習の将来像に最も近い）で 30秒実行。
  「Chuffed のみ解ける」= 学習が決定打、「Gecode でも解ける」= 伝播/探索品質で届く
- **実験2 (延長タイムアウト・プローブ)**: 同問題を sabori で 300秒実行。
  ギャップ倍率（解/証明に要した時間 ÷ 30秒）が ~3倍以内なら定数倍改善
  （propagator queue の理論上限 ~2倍）で届く距離、10倍超なら学習のみ
- スクリプト: `benchmarks/minizinc_challenge/exp_oracle.sh` / `exp_gap.sh`

## 結果

| 問題 | Gecode 30s | Chuffed 30s | sabori 300s | ギャップ | 判定 |
|------|-----------|-------------|-------------|---------|------|
| fillomino [5x5_1] | 解なし | **SOL 0.4s** | 解なし@300s | >10× | 学習のみ |
| solbat (UNSAT) | 解なし | **証明 13.5s** | 解なし@300s | >10× | 学習のみ |
| amaze | 解なし | SOL 18.6s | 解なし@300s | >10× | 学習のみ |
| sudoku_opt | 解なし | 証明 15.3s | **証明 83.9s** | 2.8× | 定数倍でも可 |
| elitserien handball1 | 証明 19.0s | 証明 4.3s | **証明 92.8s** | 3.1× | 定数倍でも可 |
| oocsp_racks (UNSAT) | 解なし | 証明 1.4s | **証明 29.1s** | ≈1.0× | ほぼ届く |
| pentominoes | SOL 9.0s | SOL 9.2s | SOL 29.2s | ≈1.0× | ほぼ届く |
| p1f-pjs-2021 [12] | SOL 18.1s | SOL 18.3s | (実行モード差により参考外) | — | — |

## 結論

**conflict learning が本命**（事前判定ルールに合致）:
- 「学習ありのみ解ける」5/8 問。うち3問（fillomino/solbat/amaze）は sabori 300秒でも解なし
  = ギャップ10倍超で、定数倍改善（queue）では構造的に届かない
- Chuffed の fillomino 0.4s / oocsp UNSAT 1.4s は枝刈り能力の次元差の直接証拠

**進め方: propagator queue → conflict learning の順で同一路線として実装**:
- LCG は「伝播→理由記録→衝突解析」パイプラインが前提で、イベント即時実行の
  現アーキテクチャには載らない。queue はその必須インフラ
- queue 単体でも 1〜3×ギャップ群（sudoku_opt / elitserien / oocsp / pentominoes の4問）の
  回収が期待でき、中間マイルストーンとしてベンチ検証可能
- 二択ではなく「途中駅（queue）と終点（learning）」

## 副次的発見

`sabori_csp.msc` の `stdFlags: ["-a"]` により、**minizinc 経由（ベンチ）と fzn 直接実行で
実行モードが異なる**。境界線上の問題（p1f-pjs[12] 等）では解の有無が変わる。
決定論比較の手順に「-a の有無を揃える」を追加すること。

## 関連

- 実装時のゲート: 1〜3×群の4問をピンポイントに、queue 導入で bench_alldiff /
  bench_circuit / bench_diffn 全て非劣化を確認（registry 級の変更なので全ファミリ必須）
- propagator queue はリファクタリング計画 v2 のフェーズ5-6 と整合
  （[refactoring-plan-2026-06.md](refactoring-plan-2026-06.md)）
