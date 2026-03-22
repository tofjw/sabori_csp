# API Reference

All classes are available via `from sabori_csp.core import ...`.

## Core Classes

### Domain

Integer domain backed by a sparse set.

```python
Domain()                          # Empty domain
Domain(min: int, max: int)        # Range [min, max]
Domain(values: list[int])         # Explicit values

d.empty() -> bool
d.size() -> int
d.min() -> int | None
d.max() -> int | None
d.contains(value: int) -> bool
d.values() -> list[int]
d.is_singleton() -> bool
```

### Variable

A CSP variable. Created by `Model.create_variable()` — not instantiated directly.

```python
v.id() -> int                     # Index in model
v.name() -> str
v.min() -> int                    # Domain minimum
v.max() -> int                    # Domain maximum
v.is_assigned() -> bool           # True if domain is singleton
v.assigned_value() -> int | None
v.domain() -> Domain              # Current domain (reference)
```

### Model

Container for variables and constraints.

```python
Model()

# Variable creation (returns Variable reference, owned by Model)
m.create_variable(name: str, min: int, max: int) -> Variable
m.create_variable(name: str, value: int) -> Variable
m.create_variable(name: str, domain: Domain) -> Variable
m.create_variable(name: str, values: list[int]) -> Variable

# Constraint management
m.add_constraint(constraint: Constraint) -> None

# Access
m.variables() -> list[Variable]
m.variable(id: int) -> Variable
m.variable(name: str) -> Variable

# Hints for the solver
m.set_defined_var(var_idx: int) -> None    # Mark as auxiliary
m.set_no_bisect(var_idx: int) -> None      # Disable binary split
```

### Solver

Search engine with nogood learning, restarts, and activity-based heuristics.

```python
Solver()

# Search (all release the GIL during execution)
s.solve(model: Model) -> dict[str, int] | None
s.solve_all(model: Model, callback: Callable[[dict], bool]) -> int
s.solve_optimize(model: Model, obj_var_idx: int, minimize: bool,
                 on_improve: Callable[[dict], bool] | None = None) -> dict[str, int] | None

# Statistics
s.stats() -> SolverStats

# Configuration
s.set_nogood_learning(enabled: bool) -> None    # default: True
s.set_restart_enabled(enabled: bool) -> None    # default: True
s.set_activity_selection(enabled: bool) -> None  # default: True
s.set_activity_first(enabled: bool) -> None      # default: False
s.set_bisection_threshold(threshold: int) -> None  # default: 8, 0=disable
s.set_verbose(enabled: bool) -> None             # default: False
s.set_community_analysis(enabled: bool) -> None  # default: False

# Control
s.stop() -> None          # Interrupt search (signal-safe)
s.reset_stop() -> None
s.is_stopped() -> bool
```

**Return values:**
- `solve()` returns `dict[str, int]` mapping variable names to values, or `None` if UNSAT.
- `solve_all()` calls `callback(solution)` for each solution. Return `True` to continue, `False` to stop. Returns the total count.
- `solve_optimize()` returns the best solution found, or `None` if UNSAT.

### SolverStats

Read-only statistics. Access via `solver.stats()`.

```python
stats.fail_count: int              # Total failures
stats.restart_count: int           # Total restarts
stats.nogood_count: int            # Total nogoods learned
stats.nogood_prune_count: int      # Branches pruned by nogoods
stats.nogood_domain_count: int     # Domain reductions from nogoods
stats.nogood_instantiate_count: int
stats.nogood_check_count: int
stats.nogoods_size: int            # Total literals across nogoods
stats.unit_nogoods_size: int       # Unit nogoods count
stats.max_depth: int               # Max search tree depth
stats.depth_sum: int               # Sum of leaf depths
stats.depth_count: int             # Number of leaves
stats.bisect_count: int            # Binary split decisions
stats.enumerate_count: int         # Enumeration decisions
```

### Constraint (base class)

```python
c.name() -> str              # Constraint type name
c.id() -> int                # Constraint index in model
c.var_ids() -> list[int]     # Indices of involved variables
```

---

## Constraint Classes

### Comparison

| Class | Semantics |
|-------|-----------|
| `IntEqConstraint(x, y)` | x = y |
| `IntNeConstraint(x, y)` | x ≠ y |
| `IntLtConstraint(x, y)` | x < y |
| `IntLeConstraint(x, y)` | x ≤ y |
| `IntMaxConstraint(x, y, m)` | m = max(x, y) |
| `IntMinConstraint(x, y, m)` | m = min(x, y) |

### Reified Comparison

| Class | Semantics |
|-------|-----------|
| `IntEqReifConstraint(x, y, b)` | b ⟺ (x = y) |
| `IntNeReifConstraint(x, y, b)` | b ⟺ (x ≠ y) |
| `IntLeReifConstraint(x, y, b)` | b ⟺ (x ≤ y) |
| `IntEqImpConstraint(x, y, b)` | b ⟹ (x = y) |

### Arithmetic

| Class | Semantics |
|-------|-----------|
| `IntTimesConstraint(x, y, z)` | z = x × y |
| `IntAbsConstraint(x, y)` | y = \|x\| |
| `IntModConstraint(x, y, z)` | z = x mod y |
| `IntDivConstraint(x, y, z)` | z = x ÷ y (integer) |

### Linear

`coeffs: list[int]`, `vars: list[Variable]`, `target/bound: int`

| Class | Semantics |
|-------|-----------|
| `IntLinEqConstraint(coeffs, vars, target)` | Σ(c_i × x_i) = target |
| `IntLinLeConstraint(coeffs, vars, bound)` | Σ(c_i × x_i) ≤ bound |
| `IntLinNeConstraint(coeffs, vars, target)` | Σ(c_i × x_i) ≠ target |

### Reified Linear

All take an additional `b: Variable` (bool).

| Class | Semantics |
|-------|-----------|
| `IntLinEqReifConstraint(coeffs, vars, target, b)` | b ⟺ (Σ = target) |
| `IntLinNeReifConstraint(coeffs, vars, target, b)` | b ⟺ (Σ ≠ target) |
| `IntLinLeReifConstraint(coeffs, vars, bound, b)` | b ⟺ (Σ ≤ bound) |
| `IntLinLeImpConstraint(coeffs, vars, bound, b)` | b ⟹ (Σ ≤ bound) |

### Logical

| Class | Semantics |
|-------|-----------|
| `ArrayBoolAndConstraint(vars, r)` | r ⟺ ∀ vars[i] |
| `ArrayBoolOrConstraint(vars, r)` | r ⟺ ∃ vars[i] |
| `ArrayBoolXorConstraint(vars)` | odd parity |
| `BoolClauseConstraint(pos, neg)` | ∨ pos[i] ∨ ¬neg[j] |
| `BoolNotConstraint(a, b)` | b = ¬a |
| `BoolXorConstraint(a, b, c)` | c ⟺ (a ⊕ b) |

### Global

| Class | Semantics |
|-------|-----------|
| `AllDifferentConstraint(vars)` | All values distinct |
| `AllDifferentGACConstraint(vars)` | All different with GAC filtering |
| `AllDifferentExcept0Constraint(vars)` | All different, ignoring 0s |
| `CircuitConstraint(vars)` | Hamiltonian circuit |
| `TableConstraint(vars, flat_tuples)` | Allowed tuples (flat) |
| `CountEqConstraint(x_vars, target, count_var)` | count_var = #{x = target} |
| `CountEqVarTargetConstraint(x_vars, y_var, count_var)` | count_var = #{x = y_var} |
| `NValueConstraint(n_var, x_vars)` | n_var = #{distinct values} |
| `DisjunctiveConstraint(starts, durations, strict=True)` | No-overlap 1D scheduling |
| `DiffnConstraint(x, y, dx, dy, strict=True)` | Non-overlapping rectangles |
| `CumulativeConstraint(starts, durations, requirements, capacity)` | Cumulative resource |
| `InverseConstraint(f, invf)` | invf[f[i]] = i |

#### Element constraints

```python
IntElementConstraint(index, array: list[int], result, zero_based=False)
IntElementMonotonicConstraint(index, array, result, monotonicity, zero_based=False)
ArrayVarIntElementConstraint(index, array: list[Variable], result, zero_based=False)
ArrayIntMaximumConstraint(m, vars)   # m = max(vars)
ArrayIntMinimumConstraint(m, vars)   # m = min(vars)
```

#### Regular constraint

```python
RegularConstraint(
    vars: list[Variable],       # Input sequence
    num_states: int,            # Q (states 1..Q)
    num_symbols: int,           # S (symbols 1..S)
    transition_flat: list[int], # Q×S flat, 1-indexed, 0=reject
    initial_state: int,
    accepting_states: list[int]
)
```

### Enums

```python
from sabori_csp.core import Monotonicity

Monotonicity.NON_DECREASING
Monotonicity.NON_INCREASING
```
