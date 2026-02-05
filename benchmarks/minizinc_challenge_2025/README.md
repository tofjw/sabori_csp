# MiniZinc Challenge 2025 ベンチマーク

## 概要

MiniZinc Challenge 2025 の問題を使用した sabori_csp のベンチマーク環境。

## ディレクトリ構成

```
benchmarks/minizinc_challenge_2025/
├── mznc2025_probs/           # MiniZinc Challenge 2025 問題セット
├── squashfs-root/            # MiniZinc バンドル（AppImage展開）
│   └── usr/share/minizinc/
│       ├── solvers/
│       │   └── sabori.msc    # Sabori ソルバー設定
│       └── sabori_csp/
│           └── redefinitions.mzn  # Sabori 用制約定義
└── README.md
```

## 重要: ベンチマーク実行方法

### 必ず minizinc 経由で実行すること

**理由**: `redefinitions.mzn` により、sabori がサポートしていない制約（gecode固有制約、set制約など）が標準の分解に置き換えられる。

### 正しい実行方法

```bash
cd /home/tofjw/develop/cp/sabori_csp/benchmarks/minizinc_challenge_2025

# Sabori で実行
./squashfs-root/usr/bin/minizinc --solver "Sabori CSP" \
    mznc2025_probs/<problem>/<problem>.mzn \
    mznc2025_probs/<problem>/<data>.dzn

# 比較用: CP-SAT で実行
./squashfs-root/usr/bin/minizinc --solver cp-sat \
    mznc2025_probs/<problem>/<problem>.mzn \
    mznc2025_probs/<problem>/<data>.dzn

# タイムアウト付き
timeout 30 ./squashfs-root/usr/bin/minizinc --solver "Sabori CSP" ...
```

### FlatZinc のみ生成（デバッグ用）

```bash
./squashfs-root/usr/bin/minizinc --solver "Sabori CSP" -c \
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
  "name": "Sabori CSP",
  "mznlib": "../sabori_csp",
  "executable": "/home/tofjw/develop/cp/sabori_csp/build/src/fzn/fzn_sabori",
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

1. **直接 fzn_sabori を実行しない** - redefinitions.mzn が適用されず、未サポート制約でエラーになる
2. **ソルバー名は "Sabori CSP"** - `--solver sabori` ではなく `--solver "Sabori CSP"`
3. **作業ディレクトリ** - `benchmarks/minizinc_challenge_2025/` から実行すること
