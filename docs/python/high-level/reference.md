# High-level API Reference

All classes and functions are available via `from sabori_csp import ...`.

---

## CpModel

The model holds variables, constraints, and an optional objective.

```python
from sabori_csp import CpModel

m = CpModel()
```

### Variable creation

| Method | Returns | Description |
|--------|---------|-------------|
| `m.int_var(lb, ub, name="")` | `IntVar` | Integer variable with domain [lb, ub] |
| `m.int_var_from_domain(values, name="")` | `IntVar` | Integer variable with explicit value set |
| `m.bool_var(name="")` | `IntVar` | Boolean variable (domain {0, 1}) |
| `m.constant(val)` | `IntVar` | Constant variable (cached; same value returns same object) |

If `name` is omitted, an auto-generated name is used (`v0`, `v1`, ...).

### Constraint posting

```python
m.add(constraint) -> None
```

Accepts:
- **Comparison expressions** — results of `==`, `!=`, `<=`, `<`, `>=`, `>` on `IntVar` / `LinearExpr`
- **Non-linear comparisons** — results of comparisons on `x * y`, `abs(x)`, `x // y`, `x % y`
- **Global constraints** — results of `all_different()`, `circuit()`, etc.

### Objective

```python
m.minimize(obj)   # obj: IntVar, LinearExpr, or int
m.maximize(obj)
```

The objective expression is automatically realized as an auxiliary variable if needed.

### Advanced

| Property | Type | Description |
|----------|------|-------------|
| `m.raw_model` | `core.Model` | Underlying C++ Model for direct access |

---

## IntVar

A decision variable. Created by `CpModel` — never instantiated directly.

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `v.name` | `str` | Variable name |
| `v.index` | `int` | Variable index in the model |

### Supported operators

See [Operator Reference](operators.md) for the full table.

```python
x + y       x - y       x * 3       3 * x       -x
x * y       x // y      x % y       abs(x)
x == y      x != y      x <= y      x < y       x >= y      x > y
sum([x, y, z])
```

### Notes

- `==` and `!=` return constraint objects, not booleans. `IntVar` is still hashable (`hash` = `id`), so it can be used as a dict key or in sets.
- `bool(x)` raises `TypeError` to prevent accidental misuse in `if` statements.

---

## LinearExpr

A linear combination of variables: `sum(coeff_i * var_i) + constant`.

Created implicitly by arithmetic on `IntVar`:

```python
expr = 2 * x + 3 * y - z + 5  # LinearExpr with terms {x:2, y:3, z:-1}, const=5
```

### Operators

Same arithmetic and comparison operators as `IntVar`. Scalar multiplication only (`LinearExpr * int`).

### Evaluation

After solving, evaluate an expression with `solver.value(expr)`:

```python
solver.value(2 * x + 3 * y)  # returns the integer value
```

---

## CpSolver

```python
from sabori_csp import CpSolver, SolveStatus

solver = CpSolver()
```

### Solving

| Method | Returns | Description |
|--------|---------|-------------|
| `solver.solve(model)` | `SolveStatus` | Find one solution (or optimal if objective is set) |
| `solver.solve_all(model, callback)` | `int` | Find all solutions; callback receives `dict[str, int]`, returns `True` to continue |

### Solution access

| Method | Returns | Description |
|--------|---------|-------------|
| `solver.value(var)` | `int` | Value of an `IntVar` in the current solution |
| `solver.value(expr)` | `int` | Evaluated value of a `LinearExpr` |

Raises `RuntimeError` if called before a successful `solve()`.

### Configuration

| Method | Default | Description |
|--------|---------|-------------|
| `set_nogood_learning(enabled)` | `True` | Clause learning from failures |
| `set_restart_enabled(enabled)` | `True` | Geometric restarts |
| `set_activity_selection(enabled)` | `True` | VSIDS-like variable selection |
| `set_activity_first(enabled)` | `False` | Prioritize activity over domain size |
| `set_bisection_threshold(n)` | `8` | Binary split for domains > n; 0 = disable |
| `set_verbose(enabled)` | `False` | Print search progress |
| `set_community_analysis(enabled)` | `False` | Constraint graph community detection |

### Control

| Method | Description |
|--------|-------------|
| `solver.stop()` | Signal solver to stop (thread-safe) |
| `solver.reset_stop()` | Clear the stop signal |

### Statistics

```python
solver.stats  # core.SolverStats (read-only)
```

See [Core API Reference](../core/reference.md#solverstats) for the full list of fields.

---

## SolveStatus

```python
from sabori_csp import SolveStatus

SolveStatus.FEASIBLE    # 1 — solution found (satisfaction problem)
SolveStatus.OPTIMAL     # 2 — optimal solution found (optimization problem)
SolveStatus.INFEASIBLE  # 3 — no solution exists
```

`SolveStatus` is an `IntEnum`, so it can be compared with integers and used in boolean contexts.

---

## Global Constraint Functions

All imported from `sabori_csp`:

```python
from sabori_csp import all_different, circuit, element, ...
```

### Direct constraints

These return objects that are passed to `model.add()`.

| Function | Semantics |
|----------|-----------|
| `all_different(vars)` | All values distinct |
| `all_different_except_0(vars)` | All non-zero values distinct |
| `circuit(vars)` | Hamiltonian circuit (0-based successor) |
| `element(index, array, result)` | `result = array[index]` (0-based). Array can be `list[int]` or `list[IntVar]` |
| `table(vars, tuples)` | Variables match one of the given tuples |
| `inverse(f, invf)` | `f` and `invf` are inverse permutations (1-based) |
| `cumulative(starts, durations, demands, capacity)` | Cumulative resource constraint |
| `disjunctive(starts, durations)` | No-overlap 1D scheduling |
| `diffn(x, y, dx, dy)` | Non-overlapping rectangles in 2D |
| `regular(vars, num_states, num_symbols, transitions, initial, accepting)` | DFA-based regular language constraint |

For `cumulative`, `disjunctive`, and `diffn`, durations/demands/capacity can be plain `int` values — they are automatically wrapped as constant variables.

### Expression-like constraints

These return expression objects that support comparison operators. Use them with `model.add(expr == var)`.

| Function | Semantics | Usage |
|----------|-----------|-------|
| `maximum(vars)` | Maximum of variables | `m.add(maximum(xs) == m_var)` |
| `minimum(vars)` | Minimum of variables | `m.add(minimum(xs) == m_var)` |
| `count(vars, value)` | Count of `value` in variables | `m.add(count(xs, 1) == n)` |
| `nvalue(vars)` | Number of distinct values | `m.add(nvalue(xs) == k)` |

These also support inequality operators (`<=`, `>=`, `<`, `>`):

```python
m.add(maximum(xs) <= 100)
m.add(count(xs, 0) >= 2)
```

### Accepting expressions in `all_different`

`all_different` and `all_different_except_0` accept linear expressions, not just variables. Auxiliary variables are created automatically:

```python
queens = [m.int_var(0, n - 1, f"q{i}") for i in range(n)]
m.add(all_different([queens[i] + i for i in range(n)]))  # diagonal constraint
```
