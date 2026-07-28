# MiniZinc Challenge ベンチマーク

## 概要

MiniZinc Challenge の問題を使用した sabori_csp のベンチマーク環境。

## ディレクトリ構成

```
benchmarks/minizinc_challenge/
├── mznc2010_probs/ .. mznc2026_probs/   # MiniZinc Challenge 各年度の問題セット
├── squashfs-root/            # 旧 MiniZinc バンドル（AppImage展開・2.9.3）。使わない
├── lib_benchmark.py          # bench_*.py 共通ライブラリ（minizinc と .msc をここで解決）
├── bench_compare.py          # 年度別全問題ベンチマーク
└── README.md
```

**minizinc は snap 版 `/snap/bin/minizinc`（2.10.0）を使う。** `squashfs-root/` は 2.9.3 の
古い展開物で、比較用ソルバーも snap 側のほうが揃っている（後述）。ディレクトリ自体は
まだ残しているが、参照しないこと。

Sabori 本体の mznlib は `build/share/minizinc/sabori_csp/` にある。canonical なソースは
`share/minizinc/sabori_csp/` で、**毎ビルド クリーンコピー**される（`cmake/sync_mznlib.cmake`）。
`.mzn` の追加・削除も `cmake --build build` だけで追従するので reconfigure は不要。

## 重要: ベンチマーク実行方法

### 必ず minizinc 経由で実行すること

**理由**: `redefinitions.mzn` により、sabori がサポートしていない制約（gecode固有制約、set制約など）が標準の分解に置き換えられる。直接 `fzn_sabori` を実行すると未サポート制約エラーになる。

### ソルバー設定は絶対パスの .msc で指定すること

`--solver sabori_csp` のような**名前指定はしない**。使うのは
`build/share/minizinc/solvers/sabori_csp.msc` のみ。

```bash
cd /path/to/sabori_csp/benchmarks/minizinc_challenge
MSC=$(pwd)/../../build/share/minizinc/solvers/sabori_csp.msc

# Sabori で実行（最適化問題は -i 必須。理由は後述）
/snap/bin/minizinc --solver "$MSC" -i --time-limit 30000 \
    mznc<year>_probs/<problem>/<problem>.mzn \
    mznc<year>_probs/<problem>/<data>.dzn

# 比較用: cp-sat / chuffed 等は snap 同梱の設定で使える
/snap/bin/minizinc --solver cp-sat -i --time-limit 30000 \
    mznc<year>_probs/<problem>/<problem>.mzn \
    mznc<year>_probs/<problem>/<data>.dzn
```

`bench_*.py` は `lib_benchmark.py` が同じ `.msc` を解決するのでそのまま使ってよい。

#### 経緯: 名前指定が危険だった理由

以前は旧バンドル側にも `squashfs-root/usr/share/minizinc/solvers/sabori.msc`
（`name: sabori_csp`）があり、その
`mznlib` は `squashfs-root/usr/share/minizinc/sabori_csp/` という**古いスナップショット**
だった。`fzn_tree_int.mzn` / `fzn_subcircuit.mzn` / `fzn_lex_*` / `fzn_increasing_*` /
`fzn_seq_precede_chain_int.mzn` / `fzn_value_precede*.mzn` を欠いており、名前解決すると
これらのネイティブ実装が**エラーを出さずに std 分解へ落ちて**いた。

バンドル側の `sabori.msc` は削除済み。同ディレクトリに残る `sabori_a.msc` /
`sabori_b.msc` / `sabori_nogac.msc` も同じ古い mznlib を指しているので使わないこと。

**ただし名前解決が失敗するとは限らない。** 現在 `--solver sabori_csp` は
`~/.minizinc/solvers/sabori_csp.msc`（→ `~/.local` の install ツリーへの symlink）に
解決される。これは壊れてはいないが **`cmake --install` した時点のスナップショット**で、
ブランチを切り替えても追従しない。ベンチでは必ず build 側 `.msc` を絶対パスで指定すること。

ネイティブ述語が効いているかは生成 FZN で確認できる:

```bash
/snap/bin/minizinc -c --solver "$MSC" <model.mzn> <data.dzn> --fzn /tmp/a.fzn
grep -c sabori_tree /tmp/a.fzn    # 0 なら mznlib が古い側を掴んでいる
```

### 最適化問題では `-i` を必ず付ける

`fzn_sabori` は `-a` なしだと最良解を内部に溜め込み終了時に出力する。minizinc の
`--time-limit` は SIGTERM でソルバーを殺すため、`-i` なしだと**解を見つけていても
`=====UNKNOWN=====` になる**。`-i` を付ければ改善解が逐次流れる。

### FlatZinc のみ生成（デバッグ用）

```bash
/snap/bin/minizinc -c --solver "$MSC" \
    mznc<year>_probs/<problem>/<problem>.mzn \
    mznc<year>_probs/<problem>/<data>.dzn \
    --fzn /tmp/output.fzn
```

## ソルバー設定

### build/share/minizinc/solvers/sabori_csp.msc

cmake が生成する。実体は次の通り（パスは絶対パスで埋め込まれる）:

```json
{
  "id": "io.github.tofjw.sabori_csp",
  "name": "sabori_csp",
  "mznlib": "/path/to/sabori_csp/build/share/minizinc/sabori_csp",
  "executable": "/path/to/sabori_csp/build/src/fzn/fzn_sabori",
  ...
}
```

### redefinitions.mzn

`build/share/minizinc/sabori_csp/redefinitions.mzn`（canonical は `share/minizinc/sabori_csp/`）:

```minizinc
% Sabori CSP redefinitions
include "nosets.mzn";
```

`nosets.mzn` をインクルードすることで:
- `set_in`, `set_in_reif` → bool配列に分解
- gecode固有制約 (`gecode_int_element` 等) を回避

## 利用可能なソルバー

```bash
/snap/bin/minizinc --solvers
```

snap 版には以下が同梱されている:

- `cp-sat` (OR Tools CP-SAT 9.15)
- `chuffed` (Chuffed 0.14.0 — LCG 比較用)
- `gecode` (Gecode 6.2.0 / 6.4.0)
- `highs` / `scip` / `coin-bc` / `gurobi` / `cplex` (MIP 比較用)

Sabori はこの一覧には出ない（snap に登録していないため）。上記の `.msc` 絶対パスで指定する。

## 注意事項

1. **ソルバーは .msc の絶対パスで指定** - `--solver sabori_csp` は使わない
2. **作業ディレクトリ** - `benchmarks/minizinc_challenge/` から実行すること
3. **並列数は最大4プロセス**

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
