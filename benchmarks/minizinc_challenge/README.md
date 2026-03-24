# MiniZinc Challenge ベンチマーク

## 概要

MiniZinc Challenge の問題を使用した sabori_csp のベンチマーク環境。

## ディレクトリ構成

```
benchmarks/minizinc_challenge/
├── mznc2025_probs/           # MiniZinc Challenge 2025 問題セット
├── squashfs-root/            # MiniZinc バンドル（AppImage展開）
│   └── usr/share/minizinc/
│       ├── solvers/
│       │   └── sabori.msc    # Sabori ソルバー設定
│       └── sabori_csp/
│           └── redefinitions.mzn  # Sabori 用制約定義
├── bench_compare.py             # 単一問題実行スクリプト
└── README.md
```

## 重要: ベンチマーク実行方法

### 必ず minizinc 経由で実行すること

**理由**: `redefinitions.mzn` により、sabori がサポートしていない制約（gecode固有制約、set制約など）が標準の分解に置き換えられる。

### 正しい実行方法

```bash
cd /path/to/sabori_csp/benchmarks/minizinc_challenge_2025

# Sabori で実行
./squashfs-root/usr/bin/minizinc --solver sabori_csp \
    mznc2025_probs/<problem>/<problem>.mzn \
    mznc2025_probs/<problem>/<data>.dzn

# 比較用: CP-SAT で実行
./squashfs-root/usr/bin/minizinc --solver cp-sat \
    mznc2025_probs/<problem>/<problem>.mzn \
    mznc2025_probs/<problem>/<data>.dzn

# タイムアウト付き
timeout 30 ./squashfs-root/usr/bin/minizinc --solver sabori_csp ...
```

### FlatZinc のみ生成（デバッグ用）

```bash
./squashfs-root/usr/bin/minizinc --solver sabori_csp -c \
    mznc2025_probs/<problem>/<problem>.mzn \
    mznc2025_probs/<problem>/<data>.dzn \
    -o /tmp/output.fzn
```

## ソルバー設定

### sabori.msc

`squashfs-root/usr/share/minizinc/solvers/sabori.msc`:

```json
{
  "id": "org.sabori.sabori_csp",
  "name": sabori_csp,
  "mznlib": "../sabori_csp",
  "executable": "/path/to/sabori_csp/build/src/fzn/fzn_sabori",
  ...
}
```

### redefinitions.mzn

`squashfs-root/usr/share/minizinc/sabori_csp/redefinitions.mzn`:

```minizinc
% Sabori CSP redefinitions
include "nosets.mzn";
```

`nosets.mzn` をインクルードすることで:
- `set_in`, `set_in_reif` → bool配列に分解
- gecode固有制約 (`gecode_int_element` 等) を回避

## 利用可能なソルバー

```bash
./squashfs-root/usr/bin/minizinc --solvers
```

- `Sabori CSP` (org.sabori.sabori_csp)
- `cp-sat` (OR Tools CP-SAT)
- `gecode` (Gecode) ※ライブラリ問題で動作しない場合あり

## 注意事項

1. **ソルバー名は sabori_csp** - `--solver sabori` ではなく `--solver sabori_csp`
2. **作業ディレクトリ** - `benchmarks/minizinc_challenge/` から実行すること

## ベンチマーク実行条件

### 実行環境
- **並列数**: 最大4プロセス
- **実行方法**: minizinc 経由（redefinitions.mzn 適用のため）
- **タイムアウト**: 30秒
- **比較対象**: Sabori CSP vs CP-SAT

### 結果表示項目
| 項目 | 説明 |
|------|------|
| 問題名 | MiniZinc Challenge の問題名 |
| インスタンス名 | データファイル名（.dzn / .json） |
| 問題タイプ | SAT（充足）/ MIN（最小化）/ MAX（最大化） |
| 結果ステータス | OPTIMAL / SOL / TIMEOUT / ERROR / UNSAT |
| 目的関数値 | 最適化問題の場合、最後に見つかった解の値 |
| 解の数 | 途中解の場合、見つかった解の数 |

### 結果ステータスの意味
| ステータス | 意味 |
|-----------|------|
| OPTIMAL | 最適解を証明 |
| SOL | 解は見つかったが最適性未証明（タイムアウト） |
| TIMEOUT | 解が見つからずタイムアウト |
| UNSAT | 充足不能 |
| ERROR | パースエラーまたは未サポート制約 |

## ベンチマークスクリプト

### bench_compare.py - 年度別全問題ベンチマーク

指定年度の全問題で Sabori と CP-SAT を比較し、HTML レポートを生成する。

```bash
cd benchmarks/minizinc_challenge

# 単年度
python3 bench_compare.py 2024

# 複数年度
python3 bench_compare.py 2022 2023 2024 2025
```

各問題の最小インスタンスを自動選択し、30秒タイムアウト・最大4並列で実行。結果は `sabori_benchmark_<年度>_<日時>.html` に出力される。
