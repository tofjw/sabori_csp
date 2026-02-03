# テスト方法

## ビルド

テストを含めてビルドするには:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## テスト実行

### 全テスト実行

```bash
ctest --test-dir build
```

詳細出力付き:

```bash
ctest --test-dir build --output-on-failure
```

### C++単体テスト

#### 全テスト
```bash
./build/tests/cpp/test_sabori_csp
```

#### タグ指定
```bash
# 制約テスト全体
./build/tests/cpp/test_sabori_csp "[constraint]"

# 特定の制約
./build/tests/cpp/test_sabori_csp "[int_eq]"
./build/tests/cpp/test_sabori_csp "[int_ne]"
./build/tests/cpp/test_sabori_csp "[int_lt]"
./build/tests/cpp/test_sabori_csp "[int_le]"
./build/tests/cpp/test_sabori_csp "[int_eq_reif]"
```

#### 複数タグ
```bash
# int_eq または int_ne
./build/tests/cpp/test_sabori_csp "[int_eq],[int_ne]"
```

#### テスト一覧表示
```bash
./build/tests/cpp/test_sabori_csp --list-tests
```

### FlatZincテスト

#### 単一ファイル実行
```bash
./build/src/fzn/fzn_sabori tests/fzn/constraints/int_eq/simple.fzn
```

#### 全解探索
```bash
./build/src/fzn/fzn_sabori -a tests/fzn/constraints/int_ne/simple.fzn
```

## テスト構成

```
tests/
├── cpp/                    # C++単体テスト (Catch2)
│   └── test_constraints.cpp
└── fzn/                    # FlatZinc統合テスト
    └── constraints/        # 制約ごとのテスト
        ├── int_eq/
        ├── int_lt/
        └── int_ne/
```

## タグ一覧

| タグ | 説明 |
|------|------|
| `[constraint]` | 全制約テスト |
| `[int_eq]` | int_eq制約 (x == y) |
| `[int_ne]` | int_ne制約 (x != y) |
| `[int_lt]` | int_lt制約 (x < y) |
| `[int_le]` | int_le制約 (x <= y) |
| `[int_eq_reif]` | int_eq_reif制約 ((x == y) <-> b) |
