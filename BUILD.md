# ビルド手順

## 必要な環境

- C++17対応コンパイラ
- CMake 3.16以上
- Bison / Flex（パーサ生成用）

## ビルド

```bash
# 設定（Releaseビルド）
cmake -B build -DCMAKE_BUILD_TYPE=Release

# ビルド実行
cmake --build build
```

Debugビルドの場合：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## クリーンアップ

ビルドディレクトリを削除：

```bash
rm -rf build
```

ソースディレクトリで誤って `cmake .` を実行した場合：

```bash
rm -f DartConfiguration.tcl CMakeCache.txt cmake_install.cmake Makefile
rm -rf CMakeFiles
```

## プロファイルビルド（gprof）

```bash
cmake -B build_profile -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-pg"
cmake --build build_profile
```

実行後にカレントディレクトリに `gmon.out` が生成される：

```bash
./build_profile/src/fzn/fzn_sabori problem.fzn
gprof build_profile/src/fzn/fzn_sabori gmon.out
```

## ビルド成果物

| ファイル | 説明 |
|----------|------|
| `build/src/fzn/fzn_sabori` | FlatZincソルバー実行ファイル |
| `build/src/core/libsabori_csp_core.a` | コアライブラリ |
| `build/python/_sabori_csp*.so` | Pythonバインディング |
