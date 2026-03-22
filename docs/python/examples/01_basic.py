"""Basic usage: variables, constraints, and solving."""

from sabori_csp.core import (
    Model, Solver, Domain,
    IntEqConstraint, IntNeConstraint, IntLeConstraint,
)

# --- Example 1: Simple inequality ---
print("=== x != y ===")
m = Model()
x = m.create_variable("x", 1, 3)
y = m.create_variable("y", 1, 3)
m.add_constraint(IntNeConstraint(x, y))

sol = Solver().solve(m)
print(sol)  # e.g. {'x': 3, 'y': 1}

# --- Example 2: Ordered variables ---
print("\n=== a <= b <= c ===")
m = Model()
a = m.create_variable("a", 1, 5)
b = m.create_variable("b", 1, 5)
c = m.create_variable("c", 1, 5)
m.add_constraint(IntLeConstraint(a, b))
m.add_constraint(IntLeConstraint(b, c))
m.add_constraint(IntNeConstraint(a, c))  # a < c (since a <= b <= c and a != c)

sol = Solver().solve(m)
print(sol)

# --- Example 3: Find all solutions ---
print("\n=== All solutions: x + y = 5, 1 <= x,y <= 4 ===")
from sabori_csp.core import IntLinEqConstraint

m = Model()
x = m.create_variable("x", 1, 4)
y = m.create_variable("y", 1, 4)
m.add_constraint(IntLinEqConstraint([1, 1], [x, y], 5))

solutions = []
Solver().solve_all(m, lambda s: (solutions.append(s), True)[-1])
for s in solutions:
    print(s)

# --- Example 4: Using Domain ---
print("\n=== Domain with holes ===")
m = Model()
x = m.create_variable("x", Domain([1, 3, 5, 7]))
y = m.create_variable("y", Domain([2, 4, 6, 8]))
m.add_constraint(IntEqConstraint(x, y))

sol = Solver().solve(m)
print(f"Solution: {sol}")  # None — no overlap
