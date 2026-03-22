"""Sudoku solver using AllDifferent constraints."""

from sabori_csp.core import Model, Solver, AllDifferentConstraint


def solve_sudoku(grid: list[list[int]]) -> list[list[int]] | None:
    """Solve a 9x9 Sudoku. 0 = empty cell."""
    model = Model()

    # Create variables: fixed cells get constant, empty cells get domain 1..9
    cells = []
    for r in range(9):
        row = []
        for c in range(9):
            if grid[r][c] != 0:
                v = model.create_variable(f"c{r}{c}", grid[r][c])
            else:
                v = model.create_variable(f"c{r}{c}", 1, 9)
            row.append(v)
        cells.append(row)

    # Row constraints
    for r in range(9):
        model.add_constraint(AllDifferentConstraint(cells[r]))

    # Column constraints
    for c in range(9):
        col = [cells[r][c] for r in range(9)]
        model.add_constraint(AllDifferentConstraint(col))

    # 3x3 box constraints
    for br in range(3):
        for bc in range(3):
            box = [cells[br * 3 + r][bc * 3 + c]
                   for r in range(3) for c in range(3)]
            model.add_constraint(AllDifferentConstraint(box))

    solver = Solver()
    sol = solver.solve(model)

    if sol is None:
        return None

    return [[sol[f"c{r}{c}"] for c in range(9)] for r in range(9)]


if __name__ == "__main__":
    # Example puzzle (0 = empty)
    puzzle = [
        [5, 3, 0, 0, 7, 0, 0, 0, 0],
        [6, 0, 0, 1, 9, 5, 0, 0, 0],
        [0, 9, 8, 0, 0, 0, 0, 6, 0],
        [8, 0, 0, 0, 6, 0, 0, 0, 3],
        [4, 0, 0, 8, 0, 3, 0, 0, 1],
        [7, 0, 0, 0, 2, 0, 0, 0, 6],
        [0, 6, 0, 0, 0, 0, 2, 8, 0],
        [0, 0, 0, 4, 1, 9, 0, 0, 5],
        [0, 0, 0, 0, 8, 0, 0, 7, 9],
    ]

    print("Puzzle:")
    for row in puzzle:
        print("  " + " ".join(str(x) if x else "." for x in row))

    result = solve_sudoku(puzzle)
    if result:
        print("\nSolution:")
        for row in result:
            print("  " + " ".join(str(x) for x in row))
    else:
        print("\nNo solution")
