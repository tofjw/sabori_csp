---
name: solve-csp
description: 自然言語で記述された制約充足問題・最適化問題を sabori_csp で解く。数独、スケジューリング、割当問題、グラフ彩色、巡回セールスマン等。CSP、制約プログラミング、最適化、ソルバーに関する質問にも対応。
argument-hint: "[問題の説明（自然言語）]"
allowed-tools: Bash(python3*), Read, Write, Glob, Grep
---

# CSP問題を sabori_csp で解く

ユーザーが自然言語で記述した制約充足問題（CSP）または最適化問題を、sabori_csp の Python API でモデリングし、解いて結果を返す。

## 環境設定

Python スクリプト実行時は必ず以下の PYTHONPATH を設定する:

```bash
export PYTHONPATH="/home/tofjw/develop/cp/sabori_csp/python:/home/tofjw/develop/cp/sabori_csp/build/python:$PYTHONPATH"
```

## モデリング手順

### 1. 問題を理解し、変数・制約・目的関数を特定する

- 変数: 何を決定するか（各セルの値、各タスクの開始時刻、各ノードの色、等）
- 制約: 変数間の関係（異なる値、順序、容量制限、等）
- 目的関数: 最小化/最大化する量（あれば最適化問題、なければ充足問題）

### 2. Python スクリプトを書く

```python
import sys
sys.path.insert(0, "/home/tofjw/develop/cp/sabori_csp/python")
sys.path.insert(0, "/home/tofjw/develop/cp/sabori_csp/build/python")

from sabori_csp import CpModel, CpSolver, SolveStatus
from sabori_csp import all_different, element, table, circuit
from sabori_csp import cumulative, disjunctive, diffn, inverse
from sabori_csp import count, nvalue, maximum, minimum, regular
from sabori_csp.claude_helper import (
    format_grid, format_schedule, format_table,
    solve_with_timeout, parse_verbose, diagnose,
)

m = CpModel()
# ... 変数定義・制約追加 ...
solver = CpSolver()
status = solver.solve(m)
```

### 3. 使える制約一覧

| Python 関数 | 説明 | 使用例 |
|-------------|------|--------|
| `x == y`, `x != y`, `x < y`, `x <= y` | 比較 | `m.add(x != y)` |
| `x + y`, `2*x - y + 3` | 線形式 | `m.add(x + y <= 10)` |
| `x * y` | 乗算 | `m.add(x * y == z)` |
| `abs(x)` | 絶対値 | `m.add(abs(x) <= 5)` |
| `all_different(vars)` | 全て異なる | `m.add(all_different([x, y, z]))` |
| `element(idx, array, result)` | 配列参照 | `m.add(element(i, [10,20,30], v))` |
| `table(vars, tuples)` | 許可タプル | `m.add(table([x,y], [[1,2],[2,3]]))` |
| `circuit(vars)` | ハミルトン閉路 | `m.add(circuit(successors))` |
| `cumulative(s,d,r,cap)` | 累積資源制約 | `m.add(cumulative(starts, durs, demands, cap))` |
| `disjunctive(s,d)` | 排他スケジュール | `m.add(disjunctive(starts, durs))` |
| `diffn(x,y,dx,dy)` | 2D矩形非重複 | `m.add(diffn(xs, ys, ws, hs))` |
| `inverse(f, invf)` | 逆関数 | `m.add(inverse(assign, inv))` |
| `count(vars, val) == n` | 値の出現数 | `m.add(count([x,y,z], value=1) == n)` |
| `nvalue(vars) == n` | 異なる値の数 | `m.add(nvalue([x,y,z]) == n)` |
| `maximum(vars) == m` | 最大値 | `m.add(maximum([x,y,z]) == mx)` |
| `minimum(vars) == m` | 最小値 | `m.add(minimum([x,y,z]) == mn)` |
| `regular(vars, ...)` | 正規言語 | DFA で許可パターンを定義 |
| `m.minimize(expr)` | 最小化 | `m.minimize(x + y)` |
| `m.maximize(expr)` | 最大化 | `m.maximize(profit)` |

### 4. 可視化の選び方

問題タイプに応じて適切な `format_*` 関数を使う:

- **グリッド問題**（数独、ラテン方陣、N-Queens）→ `format_grid()`
  ```python
  print(format_grid(sol, "cell_{r}_{c}", 9, 9, block_rows=3, block_cols=3))
  ```

- **スケジューリング問題**（ジョブショップ、cumulative）→ `format_schedule()`
  ```python
  tasks = [{"name": "A", "start_var": "s_0", "duration": 4}, ...]
  print(format_schedule(sol, tasks))
  ```

- **汎用 / 割当問題** → `format_table()`
  ```python
  print(format_table(sol, var_names=["x", "y", "z"]))
  ```

## 診断（問題が解けない/遅い場合）

verbose モードで実行し、出力を解析する:

```python
result = solve_with_timeout(m, timeout_sec=30, verbose=True)
if result["diagnostics"]:
    for hint in result["diagnostics"]:
        print(f"  - {hint}")
```

または手動で:
```python
solver = CpSolver()
solver.set_verbose(True)
# ... stderr をキャプチャして ...
parsed = parse_verbose(stderr_text)
hints = diagnose(parsed)
```

### よくある問題と対策

| 症状 | 原因 | 対策 |
|------|------|------|
| presolve failed | 制約が矛盾 | 変数の範囲・制約を見直す |
| restart 回数が多い | 探索空間が広すぎる | 冗長制約を追加、ドメインを絞る |
| nogood prune = 0 | 学習が効いていない | グローバル制約（all_different等）を使う |
| timeout | 問題が難しい | タイムアウト延長、制約強化、対称性除去 |
| INFEASIBLE | 解なし | 制約を1つずつ外して原因を特定 |

## 注意事項

- 変数のインデックスは 0-based
- `int_var(lb, ub, name)` の lb, ub は包含的（inclusive）
- `circuit` 制約は successor 表現（変数 i の値が次のノード）
- `element` 制約のインデックスは 0-based
- 最適化問題では `solve()` が `SolveStatus.OPTIMAL` を返す

## $ARGUMENTS

上記の手順に従い、ユーザーの問題「$ARGUMENTS」をモデリング・解決・可視化する。
