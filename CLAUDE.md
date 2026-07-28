# Project: sabori_csp


## Project Overview
This is a C++ constraint solver (CSP/FlatZinc). Key architecture: Model holds variables (SoA arrays for min/max), Domain manages value sets, Solver runs search with backtracking. Constraints implement propagate/presolve methods. Read docs/constraint_implementation_guide.md before implementing new constraints.

FlatZinc対応のCSPソルバー。C++コアライブラリ、FlatZincソルバー、Pythonバインディングを持つ。

## ディレクトリ構成
```
├── include/sabori_csp/     # 公開C++ヘッダー
├── src/
│   ├── core/               # コアライブラリ
│   └── fzn/                # fzn_sabori（Bison/Flex使用）
├── python/                 # pybind11バインディング
├── tests/
│   ├── cpp/                # Catch2単体テスト
│   ├── python/             # pytest
│   └── fzn/                # FlatZinc統合テスト
│       ├── constraints/    # 制約ごとのテスト（.fzn + .expected）
│       └── problems/       # テスト問題
└── docs/
    └── CONSTRAINTS.md      # サポート制約一覧
```

## ビルド成果物

| 名前 | 種類 | 説明 |
|------|------|------|
| `sabori_csp_core` | C++ライブラリ | コアロジック |
| `fzn_sabori` | 実行ファイル | FlatZincソルバー |
| `_sabori_csp` | Python拡張 | pybind11バインディング |

## コマンド
```bash
# ビルド
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 実行
./build/src/fzn/fzn_sabori problem.fzn
./build/src/fzn/fzn_sabori -a problem.fzn  # 全解探索

# テスト
ctest --test-dir build                              # 全テスト (C++ + FlatZinc)
./build/tests/cpp/test_sabori_csp "[constraint]"   # C++タグ指定
ctest --test-dir build -L fzn                       # FlatZinc 統合テストのみ
PYTHONPATH=python pytest tests/python/              # Python バインディング
bash tests/golden/run_golden.sh check               # ゴールデンマスター (リファクタ検証)
```

## 開発ルール

### C++
- C++17、名前空間 `sabori_csp`
- 公開APIは `include/sabori_csp/` に配置
- Doxygenコメント必須
- 命名: クラス `PascalCase`、関数・変数 `snake_case`

### パーサ (Bison/Flex)
- `src/fzn/parser.yy`, `src/fzn/lexer.ll`
- 生成ファイルはビルドディレクトリへ出力、コミットしない

### テスト
- C++: Catch2、タグで分類（`[constraint]`, `[solver]`, `[parser]`）
- FlatZinc: `.fzn` + `.expected` のペアで管理
- 新しい制約を追加したら `tests/fzn/constraints/` にもテスト追加

### Python
- Python 3.9以上、型ヒント必須、ruffでフォーマット

### 制約の実装
- **新しい制約を実装する前に、必ず [制約実装ガイド](docs/constraint-implementation-guide.md) を読むこと**
- ガイドに従って実装・テスト・ドキュメント更新を行う


# 開発ログ運用ルール
- 作業が一段落 OR 1日が終わるタイミングで、自動的にログを残すことを提案してください
- ログの保存先： docs-dev/work-log/YYYY-MM-DD.md
  （存在しなければ新規作成、存在すれば「## {今日の日付}」の見出しで追記）
- ログの内容フォーマット（必ずこの順番で）
  1. ## YYYY-MM-DD HH:MM 作業開始〜終了
  2. **作業ブランチ**: `<branch>`（セクション単位で必ず記録する。後述）
  3. **実施内容**
     - 箇条書き
  4. **設計判断・理由**
  5. **気づいた課題・TODO**
  6. **関連ファイル・コミット**（あれば）

- ログを書く前に「この内容でログを残しますか？」と一回確認を取ってください
- 日付は必ず環境変数の今日の日付を使ってください（ファイル名に惑わされない）

## ログは main に集約する（ブランチに置かない）

作業ログは**日付単位・全ブランチ横断**で、**main にのみ**コミットする。
1日のうちに複数のブランチを渡り歩くことが常態なので、作業ブランチにログを置くと
記録が散らばり、後から日単位で追えなくなる。

- 作業ブランチ上でログを書いた場合も、コミットは main で行う
  （`git reset --soft HEAD~1` してから `git checkout main` すればファイルは持ち越せる）
- 逆に、**ログ以外の成果物は main に置かない**。従来どおり作業ブランチにコミットする

## 作業ブランチを必ず記録する

**どのブランチでの作業か**を各セクションの冒頭に `**作業ブランチ**: \`<branch>\`` として書く。
末尾の「関連ファイル・コミット」にもブランチ名とコミットハッシュを再掲する。

- 1セクション内で複数ブランチにまたがる場合は併記する
  （例: `feature/int-times-sparse` / `fix/int-mod-const-result`）
- worktree を切って別ブランチで計測した場合はその旨も書く
- **ログ執筆時点のブランチと、最終的にコミットしたブランチが違うことがある**点に注意。
  「調査中は main、あとでブランチを切ってコミット」というパターンで実際に食い違いが発生した
  （2026-07-25 の count_eq セクション）。書くのは**成果をコミットしたブランチ**

記載漏れがあると事後の特定に `git reflog --date=iso` とファイル更新時刻の突き合わせが必要になり、
コミットされずに破棄された作業は復元できない（2026-07-22 の実例）。

## 関連ドキュメント

- [サポート制約一覧](docs/CONSTRAINTS.md)
- [制約実装ガイド](docs/constraint-implementation-guide.md)
- [テスト方法](docs/TESTING.md)
- [FlatZinc仕様](https://docs.minizinc.dev/en/2.9.5/fzn-spec.html)
- [MiniZinc Challenge ベンチマーク](benchmarks/minizinc_challenge/README.md)

## ベンチマーク実行（重要）

MiniZinc 問題のベンチマークは **必ず minizinc 経由**、**minizinc は snap 版 `/snap/bin/minizinc`**、
**ソルバー設定は `build/share/minizinc/solvers/sabori_csp.msc` を絶対パスで指定** して実行すること。

```bash
cd benchmarks/minizinc_challenge
MSC=$(pwd)/../../build/share/minizinc/solvers/sabori_csp.msc

# 最適化問題（-i 必須。理由は後述）
/snap/bin/minizinc --solver "$MSC" -i --time-limit 120000 \
    mznc<year>_probs/<problem>/<problem>.mzn \
    mznc<year>_probs/<problem>/<data>.dzn

# 比較用: cp-sat / chuffed / highs / scip 等は snap 同梱の設定で使える
/snap/bin/minizinc --solver cp-sat -i --time-limit 120000 ...
```

問題セットは `mznc2010_probs` 〜 `mznc2026_probs`。`bench_*.py` は `lib_benchmark.py` が
同じ minizinc と `.msc` を解決するのでそのまま使ってよい。

### `squashfs-root/usr/bin/minizinc` は使わない

AppImage 展開版（2.9.3）で snap 版（2.10.0）より古い。比較用ソルバーも snap 側のほうが
揃っている（cp-sat / chuffed に加え highs / scip / cbc / gurobi / cplex）。

### `--solver sabori_csp` と名前で指定してはいけない

`--solver` は絶対パスの `.msc` で渡す。名前解決はどの `.msc` を掴んだかが見えず、別の
mznlib を指す同名設定があるとネイティブ実装が**エラーなく std 分解に落ちる**。

実際 `squashfs-root/usr/share/minizinc/solvers/sabori.msc` が同じ `name: sabori_csp` を
持ち、その mznlib（`squashfs-root/usr/share/minizinc/sabori_csp/`）は
`fzn_tree_int.mzn` / `fzn_subcircuit.mzn` / `fzn_lex_*` / `fzn_increasing_*` /
`fzn_seq_precede_chain_int.mzn` / `fzn_value_precede*.mzn` を欠く古いスナップショットだった
（2026-07-26 に削除済み。同ディレクトリの `sabori_a.msc` / `sabori_b.msc` /
`sabori_nogac.msc` も同じ古い mznlib を指すので使わないこと）。

なお名前解決が必ず失敗するわけではない。現在 `--solver sabori_csp` は
`~/.minizinc/solvers/sabori_csp.msc`（`cmake --install` した `~/.local` ツリーへの
symlink）に解決される。壊れてはいないが**インストールした時点のスナップショット**で
ブランチ切替に追従しないため、ベンチには使わないこと。

正しさの確認方法 — 生成 FZN にネイティブ述語が出ているか見る:

```bash
/snap/bin/minizinc -c --solver "$MSC" ... --fzn /tmp/a.fzn
grep -c sabori_tree /tmp/a.fzn    # 0 なら mznlib が古い側を掴んでいる
```

canonical な mznlib は `share/minizinc/sabori_csp/`、ビルド時に `build/share/minizinc/sabori_csp/`
へ**毎ビルド クリーンコピー**される（`cmake/sync_mznlib.cmake`）。`.mzn` の追加・削除も
`cmake --build build` だけで追従するので reconfigure は不要。差分コピーではないため、
ブランチ切替で canonical から消えた `.mzn` が build 側に残ることもない。

### 最適化問題では `-i` を必ず付ける

`fzn_sabori` は `-a` なしだと最良解を内部に溜め込み終了時にまとめて出力する。minizinc の
`--time-limit` は SIGTERM で殺すため、`-i` なしだと**解を見つけていても `=====UNKNOWN=====`
になる**。`-i` を付ければ改善解が逐次流れる。

**minizinc 経由が必須な理由**: `redefinitions.mzn` により未サポート制約（gecode固有、set制約等）が標準分解に置き換えられる。直接 `fzn_sabori` を実行すると未サポート制約エラーになる。

詳細は [benchmarks/minizinc_challenge/README.md](benchmarks/minizinc_challenge/README.md) を参照。

## Profiling
When profiling C++ code, use gprof (not perf) as perf is unavailable in WSL2. Always do a clean rebuild (`make clean && make`) before profiling to avoid stale binary/gmon.out issues. Use the solver's built-in timeout flag instead of SIGTERM to ensure gmon.out is written.

## Code Modification Rules section
After bulk sed/replacement operations across C++ files, always do a full build before moving on. Bulk replacements frequently break code in constructors and methods that lack the expected context (e.g., missing Model& parameter). Prefer incremental file-by-file changes over big-bang refactors.

## Testing section
Always run all tests (`make test` or equivalent) after any code change and report the result. Never consider a task complete until tests pass.
