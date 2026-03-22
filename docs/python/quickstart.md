# Quickstart

## Basic workflow

Every program follows the same pattern: create a **Model**, add **Variables** and **Constraints**, then **solve**.

```python
from sabori_csp.core import Model, Solver, IntNeConstraint

# 1. Create a model
model = Model()

# 2. Add variables
x = model.create_variable("x", 1, 5)  # domain {1, 2, 3, 4, 5}
y = model.create_variable("y", 1, 5)

# 3. Add constraints
model.add_constraint(IntNeConstraint(x, y))  # x != y

# 4. Solve
solver = Solver()
solution = solver.solve(model)

if solution:
    print(solution)  # {'x': 5, 'y': 1}
else:
    print("No solution")
```

## Creating variables

```python
# Range domain: {1, 2, ..., 10}
x = model.create_variable("x", 1, 10)

# Specific values: {2, 5, 8}
y = model.create_variable("y", [2, 5, 8])

# Fixed constant
c = model.create_variable("c", 42)

# From a Domain object
from sabori_csp.core import Domain
d = Domain(1, 100)
z = model.create_variable("z", d)
```

## Finding all solutions

```python
from sabori_csp.core import Model, Solver, AllDifferentConstraint

model = Model()
xs = [model.create_variable(f"x{i}", 1, 3) for i in range(3)]
model.add_constraint(AllDifferentConstraint(xs))

solutions = []
solver = Solver()
count = solver.solve_all(model, lambda sol: (solutions.append(sol), True)[-1])
print(f"Found {count} solutions")
for sol in solutions:
    print(sol)
```

## Optimization

```python
from sabori_csp.core import Model, Solver, IntLinLeConstraint

model = Model()
x = model.create_variable("x", 0, 10)
y = model.create_variable("y", 0, 10)

# x + y <= 15
model.add_constraint(IntLinLeConstraint([1, 1], [x, y], 15))

# Objective variable: z = 2x + 3y
z = model.create_variable("z", 0, 50)
model.add_constraint(
    IntLinEqConstraint([2, 3, -1], [x, y, z], 0)
)

solver = Solver()
# Maximize z (obj_var_idx = z.id(), minimize = False)
best = solver.solve_optimize(model, z.id(), False)
if best:
    print(f"Optimal: z = {best['z']}")
```

## Solver configuration

```python
solver = Solver()
solver.set_nogood_learning(True)      # Enable clause learning (default)
solver.set_restart_enabled(True)      # Enable restarts (default)
solver.set_activity_selection(True)   # Activity-based variable selection (default)
solver.set_bisection_threshold(8)     # Binary split for domains > 8 (default)
solver.set_verbose(True)              # Print search statistics
```

## Inspecting results

```python
solver = Solver()
solution = solver.solve(model)

# Access statistics
stats = solver.stats()
print(f"Failures: {stats.fail_count}")
print(f"Restarts: {stats.restart_count}")
print(f"Nogoods:  {stats.nogood_count}")

# Inspect variables after solving
for var in model.variables():
    print(f"{var.name()} = {var.assigned_value()}")
```

## Next steps

- [API Reference](reference.md) for the full list of constraints and methods
- [Examples](examples/) for complete programs (N-Queens, scheduling, etc.)
