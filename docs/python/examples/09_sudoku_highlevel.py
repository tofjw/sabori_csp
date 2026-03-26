"""Sudoku solver with the high-level API.

Compare with 04_sudoku.py (core API) — almost identical since
Sudoku is already a natural fit for all_different.
"""

from sabori_csp import CpModel, CpSolver, SolveStatus, all_different


def solve_sudoku(grid: list[list[int]]) -> list[list[int]] | None:
    """Solve a 9x9 Sudoku. 0 = empty cell."""
    m = CpModel()

    # Create variables: fixed cells become constants, empty cells get domain 1..9
    cells = []
    for r in range(9):
        row = []
        for c in range(9):
            if grid[r][c] != 0:
                row.append(m.int_var(grid[r][c], grid[r][c], f"c{r}{c}"))
            else:
                row.append(m.int_var(1, 9, f"c{r}{c}"))
        cells.append(row)

    # Row constraints
    for r in range(9):
        m.add(all_different(cells[r]))

    # Column constraints
    for c in range(9):
        m.add(all_different([cells[r][c] for r in range(9)]))

    # 3x3 box constraints
    for br in range(3):
        for bc in range(3):
            box = [cells[br * 3 + r][bc * 3 + c]
                   for r in range(3) for c in range(3)]
            m.add(all_different(box))

    solver = CpSolver()
    status = solver.solve(m)

    if status != SolveStatus.FEASIBLE:
        return None

    return [[solver.value(cells[r][c]) for c in range(9)] for r in range(9)]


if __name__ == "__main__":
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
