"""Job scheduling with the high-level API.

Compare with 03_scheduling.py (core API) — makespan constraints
become simple inequalities instead of manual auxiliary variables.
"""

from sabori_csp import CpModel, CpSolver, SolveStatus, disjunctive


def solve_scheduling() -> None:
    """Schedule 5 jobs on a single machine, minimizing makespan."""
    durations = [3, 2, 5, 4, 1]
    n = len(durations)
    horizon = sum(durations)

    m = CpModel()

    # Start time variables
    starts = [m.int_var(0, horizon - durations[i], f"start_{i}") for i in range(n)]

    # No two jobs overlap
    m.add(disjunctive(starts, durations))

    # Makespan: must be >= every job's end time
    makespan = m.int_var(0, horizon, "makespan")
    for i in range(n):
        m.add(starts[i] + durations[i] <= makespan)

    m.minimize(makespan)

    solver = CpSolver()
    status = solver.solve(m)

    if status == SolveStatus.OPTIMAL:
        print(f"Optimal makespan: {solver.value(makespan)}")
        for i in range(n):
            s = solver.value(starts[i])
            print(f"  Job {i}: start={s}, duration={durations[i]}, end={s + durations[i]}")
    else:
        print("No solution")

    stats = solver.stats
    print(f"  Failures: {stats.fail_count}")


if __name__ == "__main__":
    solve_scheduling()
