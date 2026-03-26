# High-level API Quickstart

The high-level API lets you model constraint problems using natural Python syntax — arithmetic operators, comparison operators, and `sum()` all work on decision variables.

## Basic workflow

```python
from sabori_csp import CpModel, CpSolver, SolveStatus

# 1. Create a model
m = CpModel()

# 2. Create variables
x = m.int_var(1, 10, "x")
y = m.int_var(1, 10, "y")

# 3. Add constraints using operators
m.add(x + y == 15)
m.add(x != y)

# 4. Solve
solver = CpSolver()
status = solver.solve(m)

if status == SolveStatus.FEASIBLE:
    print(f"x = {solver.value(x)}, y = {solver.value(y)}")
```

## Creating variables

```python
x = m.int_var(1, 10, "x")                 # domain {1, 2, ..., 10}
y = m.int_var_from_domain([2, 5, 8], "y") # domain {2, 5, 8}
b = m.bool_var("b")                        # domain {0, 1}
```

If you omit the name, one is generated automatically:

```python
v = m.int_var(0, 100)  # named "v0", "v1", ...
```

## Posting constraints

All constraints go through `model.add()`. You never need to import constraint classes — just use Python operators.

```python
m.add(x + y == 10)          # linear equality
m.add(x != y)               # binary inequality
m.add(2 * x + 3 * y <= 20)  # weighted linear inequality
m.add(x >= 3)               # bound
```

## Using `sum()`

Python's built-in `sum()` works on lists of variables:

```python
xs = [m.int_var(0, 9, f"x{i}") for i in range(5)]
m.add(sum(xs) == 25)
```

## Global constraints

Import helpers from the package for global constraints:

```python
from sabori_csp import all_different, circuit, element, cumulative

m.add(all_different(xs))
m.add(circuit(successors))
m.add(element(index, [10, 20, 30], result))
```

Some globals return expression-like objects that support comparison operators:

```python
from sabori_csp import maximum, minimum, count, nvalue

max_var = m.int_var(0, 100, "max")
m.add(maximum(xs) == max_var)
m.add(count(xs, 1) == 3)     # exactly 3 ones
m.add(nvalue(xs) == 4)       # exactly 4 distinct values
```

## Non-linear constraints

Operator overloading handles multiplication, division, modulo, and absolute value:

```python
z = m.int_var(0, 100, "z")
m.add(x * y == z)       # product
m.add(x // y == 3)      # integer division
m.add(x % y == 2)       # modulo
m.add(abs(x) == y)      # absolute value
```

## Optimization

```python
m.minimize(x + y)        # minimize a linear expression
# or
m.maximize(2 * x + 3 * y)
```

```python
solver = CpSolver()
status = solver.solve(m)

if status == SolveStatus.OPTIMAL:
    print(f"Optimal: {solver.value(x) + solver.value(y)}")
```

## Finding all solutions

```python
solutions = []
count = solver.solve_all(m, lambda sol: (solutions.append(dict(sol)), True)[-1])
print(f"Found {count} solutions")
```

## Evaluating expressions

`solver.value()` accepts both variables and linear expressions:

```python
obj = 2 * x + 3 * y
solver.solve(m)
print(solver.value(obj))  # evaluates the expression using the solution
```

## Solver configuration

```python
solver = CpSolver()
solver.set_nogood_learning(True)     # clause learning (default: True)
solver.set_restart_enabled(True)     # restarts (default: True)
solver.set_activity_selection(True)  # VSIDS-like heuristic (default: True)
solver.set_bisection_threshold(8)    # binary split threshold (default: 8)
solver.set_verbose(True)             # print search progress
```

## Complete example: N-Queens

```python
from sabori_csp import CpModel, CpSolver, SolveStatus, all_different

n = 8
m = CpModel()
queens = [m.int_var(0, n - 1, f"q{i}") for i in range(n)]

m.add(all_different(queens))
m.add(all_different([queens[i] + i for i in range(n)]))
m.add(all_different([queens[i] - i for i in range(n)]))

solver = CpSolver()
status = solver.solve(m)

if status == SolveStatus.FEASIBLE:
    for i in range(n):
        row = ["." if j != solver.value(queens[i]) else "Q" for j in range(n)]
        print(" ".join(row))
```

## Accessing the core API

For advanced use cases (reified constraints, solver hints, constraint statistics), access the underlying C++ objects:

```python
m.raw_model   # the core.Model instance
```

See the [Core API Reference](../core/reference.md) for details.

## Next steps

- [API Reference](reference.md) — Full method signatures for CpModel, CpSolver, IntVar
- [Operator Reference](operators.md) — Complete operator table
- [Core API Quickstart](../core/quickstart.md) — When you need low-level control
