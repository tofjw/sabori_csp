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
ctest --test-dir build                              # 全テスト
./build/tests/cpp/test_sabori_csp "[constraint]"   # C++タグ指定
pytest tests/fzn/run_tests.py -v -k alldifferent   # FlatZinc特定制約
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
  2. **実施内容**
     - 箇条書き
  3. **設計判断・理由**
  4. **気づいた課題・TODO**
  5. **関連ファイル・コミット**（あれば）

- ログを書く前に「この内容でログを残しますか？」と一回確認を取ってください
- 日付は必ず環境変数の今日の日付を使ってください（ファイル名に惑わされない）

## 関連ドキュメント

- [サポート制約一覧](docs/CONSTRAINTS.md)
- [制約実装ガイド](docs/constraint-implementation-guide.md)
- [テスト方法](docs/TESTING.md)
- [FlatZinc仕様](https://docs.minizinc.dev/en/2.9.5/fzn-spec.html)
- [MiniZinc Challenge ベンチマーク](benchmarks/minizinc_challenge_2025/README.md)

## ベンチマーク実行（重要）

MiniZinc 問題のベンチマークは **必ず minizinc 経由** で実行すること。

```bash
cd benchmarks/minizinc_challenge_2025
./squashfs-root/usr/bin/minizinc --solver "Sabori CSP" \
    mznc2025_probs/<problem>/<problem>.mzn \
    mznc2025_probs/<problem>/<data>.dzn
```

**理由**: `redefinitions.mzn` により未サポート制約（gecode固有、set制約等）が標準分解に置き換えられる。直接 `fzn_sabori` を実行すると未サポート制約エラーになる。

詳細は [benchmarks/minizinc_challenge_2025/README.md](benchmarks/minizinc_challenge_2025/README.md) を参照。

## Profiling
When profiling C++ code, use gprof (not perf) as perf is unavailable in WSL2. Always do a clean rebuild (`make clean && make`) before profiling to avoid stale binary/gmon.out issues. Use the solver's built-in timeout flag instead of SIGTERM to ensure gmon.out is written.

## Code Modification Rules section
After bulk sed/replacement operations across C++ files, always do a full build before moving on. Bulk replacements frequently break code in constructors and methods that lack the expected context (e.g., missing Model& parameter). Prefer incremental file-by-file changes over big-bang refactors.

## Testing section
Always run all tests (`make test` or equivalent) after any code change and report the result. Never consider a task complete until tests pass.
