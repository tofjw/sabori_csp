"""N-Queens problem using AllDifferent constraints."""

import sys
from sabori_csp.core import Model, Solver, AllDifferentConstraint, IntLinEqConstraint

def solve_nqueens(n: int, find_all: bool = False) -> None:
    model = Model()

    # queens[i] = column of queen in row i (1-indexed)
    queens = [model.create_variable(f"q{i}", 1, n) for i in range(n)]

    # All columns different
    model.add_constraint(AllDifferentConstraint(queens))

    # All "row + col" diagonals different
    diag1 = [model.create_variable(f"d1_{i}", 1 + i, n + i) for i in range(n)]
    for i in range(n):
        # d1_i = queens[i] + i  =>  queens[i] - d1_i = -i
        model.add_constraint(IntLinEqConstraint([1, -1], [queens[i], diag1[i]], -i))
    model.add_constraint(AllDifferentConstraint(diag1))

    # All "row - col" diagonals different
    diag2 = [model.create_variable(f"d2_{i}", 1 - i, n - i) for i in range(n)]
    for i in range(n):
        # d2_i = queens[i] - i  =>  queens[i] - d2_i = i
        model.add_constraint(IntLinEqConstraint([1, -1], [queens[i], diag2[i]], i))
    model.add_constraint(AllDifferentConstraint(diag2))

    solver = Solver()

    if find_all:
        count = solver.solve_all(model, lambda _: True)
        print(f"{n}-Queens: {count} solutions")
    else:
        sol = solver.solve(model)
        if sol:
            placement = [sol[f"q{i}"] for i in range(n)]
            print(f"{n}-Queens: {placement}")
            # Print board
            for row in range(n):
                col = placement[row]
                print("  " + ". " * (col - 1) + "Q " + ". " * (n - col))
        else:
            print(f"{n}-Queens: no solution")

    stats = solver.stats()
    print(f"  Failures: {stats.fail_count}, Restarts: {stats.restart_count}")


if __name__ == "__main__":
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 8
    all_flag = "--all" in sys.argv
    solve_nqueens(n, find_all=all_flag)
