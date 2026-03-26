"""N-Queens with the high-level API.

Compare with 02_nqueens.py (core API) — diagonal constraints
collapse from 10 lines to 2 lines thanks to operator overloading.
"""

import sys
from sabori_csp import CpModel, CpSolver, SolveStatus, all_different


def solve_nqueens(n: int, find_all: bool = False) -> None:
    m = CpModel()
    queens = [m.int_var(0, n - 1, f"q{i}") for i in range(n)]

    # All columns different
    m.add(all_different(queens))

    # All "row + col" diagonals different — no manual aux vars needed
    m.add(all_different([queens[i] + i for i in range(n)]))

    # All "row - col" diagonals different
    m.add(all_different([queens[i] - i for i in range(n)]))

    solver = CpSolver()

    if find_all:
        count = solver.solve_all(m, lambda _: True)
        print(f"{n}-Queens: {count} solutions")
    else:
        status = solver.solve(m)
        if status == SolveStatus.FEASIBLE:
            placement = [solver.value(queens[i]) for i in range(n)]
            print(f"{n}-Queens: {placement}")
            for row in range(n):
                line = ". " * placement[row] + "Q " + ". " * (n - 1 - placement[row])
                print("  " + line)
        else:
            print(f"{n}-Queens: no solution")

    stats = solver.stats
    print(f"  Failures: {stats.fail_count}, Restarts: {stats.restart_count}")


if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    all_flag = "--all" in sys.argv
    solve_nqueens(n, find_all=all_flag)
