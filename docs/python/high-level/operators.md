# Operator Reference

This page documents all Python operators supported by `IntVar` and `LinearExpr`.

## Arithmetic operators

All arithmetic operators return `LinearExpr` (a lazy symbolic expression), except for non-linear operations which return specialized expression nodes.

### Linear (returns `LinearExpr`)

| Expression | Description | Notes |
|------------|-------------|-------|
| `x + y` | Addition | `IntVar`, `LinearExpr`, or `int` on either side |
| `x - y` | Subtraction | |
| `3 * x` | Scalar multiplication | Integer on either side |
| `x * 3` | Scalar multiplication | |
| `-x` | Negation | |
| `sum([x, y, z])` | Built-in sum | Works via `__radd__` with `int` start value |

`LinearExpr` merges like terms automatically:

```python
expr = 2 * x + x    # LinearExpr({x: 3})
expr = x - x        # LinearExpr({}, const=0) — terms cancel
```

### Non-linear

| Expression | Returns | C++ constraint |
|------------|---------|---------------|
| `x * y` | `TimesExpr` | `IntTimesConstraint` |
| `x // y` | `DivExpr` | `IntDivConstraint` |
| `x % y` | `ModExpr` | `IntModConstraint` |
| `abs(x)` | `AbsExpr` | `IntAbsConstraint` |

Non-linear expressions must be used in a comparison before passing to `model.add()`:

```python
m.add(x * y == z)    # OK
m.add(abs(x) <= 5)   # OK
m.add(x * y)         # TypeError — not a constraint
```

## Comparison operators

All comparison operators return constraint objects that are passed to `model.add()`.

### On `IntVar` and `LinearExpr`

| Expression | Constraint type | Internal form |
|------------|----------------|---------------|
| `x == y` | `BoundedExpr(EQ)` | x - y = 0 |
| `x != y` | `BoundedExpr(NE)` | x - y ≠ 0 |
| `x <= y` | `BoundedExpr(LE)` | x - y ≤ 0 |
| `x < y` | `BoundedExpr(LE)` | x - y + 1 ≤ 0 |
| `x >= y` | `BoundedExpr(LE)` | y - x ≤ 0 |
| `x > y` | `BoundedExpr(LE)` | y - x + 1 ≤ 0 |

Strict inequalities (`<`, `>`) are normalized using the +1 trick for integer domains.

Either side can be an `IntVar`, `LinearExpr`, or plain `int`:

```python
m.add(x == 5)
m.add(2 * x + y <= 10)
m.add(3 >= x)
```

### On non-linear expressions

| Expression | Returns |
|------------|---------|
| `x * y == z` | `NonLinearBoundedExpr` |
| `abs(x) <= 5` | `NonLinearBoundedExpr` |
| `x // y != 0` | `NonLinearBoundedExpr` |

All six comparison operators (`==`, `!=`, `<=`, `<`, `>=`, `>`) are supported.

## Constraint lowering

When `model.add()` receives a constraint expression, it selects the most efficient C++ constraint:

| Pattern | C++ constraint used |
|---------|-------------------|
| `x == y` (two vars, unit coefficients) | `IntEqConstraint` |
| `x != y` | `IntNeConstraint` |
| `x <= y` | `IntLeConstraint` |
| `x == 5` (single var vs constant) | `IntEqConstraint` with constant variable |
| `2*x + 3*y == 10` (general linear) | `IntLinEqConstraint` |
| `2*x + 3*y <= 10` | `IntLinLeConstraint` |
| `2*x + 3*y != 10` | `IntLinNeConstraint` |
| `x * y == z` | `IntTimesConstraint` |
| `abs(x) == y` | `IntAbsConstraint` |
| `x // y == z` | `IntDivConstraint` |
| `x % y == z` | `IntModConstraint` |

For non-EQ comparisons on non-linear expressions (e.g., `x * y <= z`), an auxiliary variable is created automatically:

```python
m.add(x * y <= z)
# Internally: create aux, post IntTimesConstraint(x, y, aux), post aux <= z
```

## Important notes

### `__eq__` returns a constraint, not a boolean

```python
x == y     # returns BoundedExpr, NOT True/False
x is y     # use 'is' for identity checks
hash(x)    # works — hash is id(x)
{x: 1}     # IntVar can be used as dict key
```

### `bool()` raises TypeError

```python
if x:      # TypeError — prevents accidental misuse
    ...

if x > 0:  # Also TypeError — BoundedExpr is not a boolean
    ...
```

This is intentional. Use `model.add()` to post constraints, and `solver.value()` to check results after solving.
