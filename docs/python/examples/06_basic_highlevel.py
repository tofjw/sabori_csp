"""Basic usage with the high-level API.

Compare with 01_basic.py (core API) to see the difference.
"""

from sabori_csp import CpModel, CpSolver, SolveStatus

# --- Example 1: Simple inequality ---
print("=== x != y ===")
m = CpModel()
x = m.int_var(1, 3, "x")
y = m.int_var(1, 3, "y")
m.add(x != y)

solver = CpSolver()
status = solver.solve(m)
print(f"x = {solver.value(x)}, y = {solver.value(y)}")

# --- Example 2: Ordered variables ---
print("\n=== a <= b <= c, a != c ===")
m = CpModel()
a = m.int_var(1, 5, "a")
b = m.int_var(1, 5, "b")
c = m.int_var(1, 5, "c")
m.add(a <= b)
m.add(b <= c)
m.add(a != c)

solver = CpSolver()
solver.solve(m)
print(f"a = {solver.value(a)}, b = {solver.value(b)}, c = {solver.value(c)}")

# --- Example 3: Find all solutions ---
print("\n=== All solutions: x + y = 5, 1 <= x,y <= 4 ===")
m = CpModel()
x = m.int_var(1, 4, "x")
y = m.int_var(1, 4, "y")
m.add(x + y == 5)

solutions = []
solver = CpSolver()
count = solver.solve_all(m, lambda s: (solutions.append(dict(s)), True)[-1])
for s in solutions:
    print(s)

# --- Example 4: Using int_var_from_domain ---
print("\n=== Domains with no overlap ===")
m = CpModel()
x = m.int_var_from_domain([1, 3, 5, 7], "x")
y = m.int_var_from_domain([2, 4, 6, 8], "y")
m.add(x == y)

solver = CpSolver()
status = solver.solve(m)
print(f"Solution: {status.name}")  # INFEASIBLE — no overlap

# --- Example 5: Weighted sum with optimization ---
print("\n=== Minimize 2x + 3y, subject to x + y >= 10 ===")
m = CpModel()
x = m.int_var(0, 20, "x")
y = m.int_var(0, 20, "y")
m.add(x + y >= 10)
m.minimize(2 * x + 3 * y)

solver = CpSolver()
status = solver.solve(m)
print(f"x = {solver.value(x)}, y = {solver.value(y)}")
print(f"Objective = {solver.value(2 * x + 3 * y)}, status = {status.name}")
