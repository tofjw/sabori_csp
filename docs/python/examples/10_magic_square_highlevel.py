"""Magic square with the high-level API.

Compare with 05_magic_square.py (core API) — sum constraints
become simple `sum(row) == magic_sum` instead of IntLinEqConstraint.
"""

import sys
from sabori_csp import CpModel, CpSolver, SolveStatus, all_different


def solve_magic_square(n: int) -> None:
    magic_sum = n * (n * n + 1) // 2
    print(f"{n}x{n} magic square, target sum = {magic_sum}")

    m = CpModel()

    # Variables: cells[r][c] in {1, ..., n*n}
    cells = [[m.int_var(1, n * n, f"c{r}_{c}") for c in range(n)] for r in range(n)]

    # All values different
    m.add(all_different([cells[r][c] for r in range(n) for c in range(n)]))

    # Row sums
    for r in range(n):
        m.add(sum(cells[r]) == magic_sum)

    # Column sums
    for c in range(n):
        m.add(sum(cells[r][c] for r in range(n)) == magic_sum)

    # Main diagonal
    m.add(sum(cells[i][i] for i in range(n)) == magic_sum)

    # Anti-diagonal
    m.add(sum(cells[i][n - 1 - i] for i in range(n)) == magic_sum)

    solver = CpSolver()
    status = solver.solve(m)

    if status == SolveStatus.FEASIBLE:
        for r in range(n):
            vals = [solver.value(cells[r][c]) for c in range(n)]
            print("  " + " ".join(f"{v:3d}" for v in vals))
    else:
        print("No solution")

    stats = solver.stats
    print(f"Failures: {stats.fail_count}, Restarts: {stats.restart_count}")


if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 3
    solve_magic_square(n)
