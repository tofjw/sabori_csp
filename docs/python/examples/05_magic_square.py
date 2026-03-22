"""Magic square: N×N grid with all rows, columns, and diagonals summing equally."""

import sys
from sabori_csp.core import (
    Model, Solver,
    AllDifferentConstraint, IntLinEqConstraint,
)


def solve_magic_square(n: int) -> None:
    magic_sum = n * (n * n + 1) // 2
    print(f"{n}x{n} magic square, target sum = {magic_sum}")

    model = Model()

    # Variables: cells[r][c] in {1, ..., n*n}
    cells = [[model.create_variable(f"c{r}_{c}", 1, n * n)
              for c in range(n)] for r in range(n)]

    # All values different
    model.add_constraint(AllDifferentConstraint(
        [cells[r][c] for r in range(n) for c in range(n)]
    ))

    ones = [1] * n

    # Row sums
    for r in range(n):
        model.add_constraint(IntLinEqConstraint(ones, cells[r], magic_sum))

    # Column sums
    for c in range(n):
        col = [cells[r][c] for r in range(n)]
        model.add_constraint(IntLinEqConstraint(ones, col, magic_sum))

    # Main diagonal
    diag = [cells[i][i] for i in range(n)]
    model.add_constraint(IntLinEqConstraint(ones, diag, magic_sum))

    # Anti-diagonal
    anti = [cells[i][n - 1 - i] for i in range(n)]
    model.add_constraint(IntLinEqConstraint(ones, anti, magic_sum))

    solver = Solver()
    sol = solver.solve(model)

    if sol:
        for r in range(n):
            vals = [sol[f"c{r}_{c}"] for c in range(n)]
            print("  " + " ".join(f"{v:3d}" for v in vals))
    else:
        print("No solution")

    stats = solver.stats()
    print(f"Failures: {stats.fail_count}, Restarts: {stats.restart_count}")


if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    solve_magic_square(n)
