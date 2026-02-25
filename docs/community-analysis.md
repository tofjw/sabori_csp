# コミュニティ分析 (`-c` フラグ)

CSP問題のコミュニティ構造と探索の局所性を分析する機能。

```bash
fzn_sabori -c problem.fzn
# minizinc 経由
minizinc --solver "Sabori CSP" --fzn-flags "-c" problem.mzn data.dzn
```

## 出力の構成

2種類のレポートが stderr に出力される。

### 1. 静的レポート（presolve 後に1回）

```
% [community] VIG: 13606 vars, 292612 edges, avg_degree=43.0, max_degree=259
% [community] Communities: 562 (modularity Q=0.93)
% [community] Sizes: [4916, 227, 122, 118, ...]
% [community] Intra-edges: 276315 (94.4%), inter-edges: 16297 (5.6%)
```

| 項目 | 説明 |
|------|------|
| VIG | Variable Interaction Graph。同じ制約に参加する変数ペア間に辺を張ったグラフ |
| edges | 辺の数。重みは共有制約数 |
| avg_degree | 平均次数。1変数あたり平均何変数と制約を共有しているか |
| Communities | Label Propagation で検出されたコミュニティ数 |
| modularity Q | コミュニティ分割の品質（後述） |
| Sizes | コミュニティのサイズ（変数数）降順。20個まで表示 |
| Intra-edges | コミュニティ内の辺数と割合 |
| inter-edges | コミュニティ間の辺数と割合 |

### 2. 動的レポート（リスタート毎 + 最終）

```
% [locality] restart#5: decisions=1234 local=876(71.0%) cross=358(29.0%)
% [locality]   propagations=5678 local=4890(86.1%) cross=788(13.9%)
% [locality]   community_decisions: [0:312, 1:289, 2:267, ...]
```

| 項目 | 説明 |
|------|------|
| decisions | そのリスタート区間の判定回数 |
| local | 前回の判定と**同じコミュニティ**の変数を判定した回数 |
| cross | 前回の判定と**異なるコミュニティ**の変数を判定した回数 |
| propagations | ドメイン変更の回数 |
| propagations local | 判定変数と**同じコミュニティ**の変数が変更された回数 |
| propagations cross | 判定変数と**異なるコミュニティ**の変数が変更された回数 |
| community_decisions | コミュニティ別の判定回数（非ゼロのみ） |

## modularity Q の読み方

Q はコミュニティ分割がランダム分割と比べてどれだけ良いかを示す。

```
Q = (コミュニティ内辺の割合) - (ランダムグラフで期待されるコミュニティ内辺の割合)
```

| Q の範囲 | 解釈 |
|----------|------|
| Q > 0.3 | 明確なコミュニティ構造がある。問題が疎結合なサブ問題に分解できる |
| Q ≈ 0 | コミュニティ構造なし。全体が密に絡み合っている |
| Q < 0 | 検出失敗、または本質的に分解不可能な問題 |

## 典型的なパターンと解釈

### パターン1: 強い構造 + 巨大コミュニティ

```
% [community] Communities: 562 (modularity Q=0.93)
% [community] Sizes: [4916, 227, 122, ...]
% [locality] community_decisions: [1:5886]
```

- 問題全体は疎結合だが、**最大コミュニティが単独で難しい**
- 小コミュニティは伝播で自動的に解けている
- 探索のボトルネックは最大クラスター内部

### パターン2: 構造なし（密結合問題）

```
% [community] Communities: 939 (modularity Q=-0.03)
% [community] Sizes: [51, 51, 1, 1, 1, ...]
% [community] Intra-edges: 2550 (49.5%), inter-edges: 2601 (50.5%)
```

- ほぼ全変数が孤立コミュニティ → Label Propagation が収束しなかった
- 問題が本質的に分解不可能（CP-SAT でも苦戦するタイプ）
- locality メトリクスは参考にならない

### パターン3: 小クラスタへの張り付き

```
% [locality] restart#N: decisions=5000 local=4990(99.8%) cross=10(0.2%)
% [locality]   propagations=30000 local=28000(93.3%) cross=2000(6.7%)
% [locality]   community_decisions: [7:4950, 3:50]
```

- 特定の小コミュニティに判定が集中
- cross propagation も少ない → 他に波及しない無駄な探索の可能性
- activity decay が効いておらず、探索が局所に張り付いている疑い

### パターン4: バランスの取れた探索

```
% [locality] restart#N: decisions=2000 local=1200(60%) cross=800(40%)
% [locality]   propagations=15000 local=9000(60%) cross=6000(40%)
% [locality]   community_decisions: [0:400, 1:380, 2:350, 3:320, ...]
```

- 複数コミュニティに判定が分散
- cross propagation も適度 → コミュニティ間の制約も効率的に処理

## パフォーマンスへの影響

- `-c` なし: オーバーヘッドゼロ（`if` 分岐予測コストのみ）
- `-c` あり:
  - VIG 構築: O(Σ arity^2)、典型的な問題で数ms
  - Label Propagation: O(E × iterations)
  - 動的計測: `on_decision` O(1)、`on_propagation` O(1)
