"""Job scheduling with DisjunctiveConstraint and optimization."""

from sabori_csp.core import (
    Model, Solver,
    DisjunctiveConstraint, IntLinEqConstraint, IntLeConstraint,
)


def solve_scheduling() -> None:
    """Schedule 5 jobs on a single machine, minimizing makespan."""
    durations = [3, 2, 5, 4, 1]  # Job durations
    n = len(durations)
    horizon = sum(durations)

    model = Model()

    # Start time variables
    starts = [model.create_variable(f"start_{i}", 0, horizon - durations[i])
              for i in range(n)]

    # Duration variables (fixed)
    dur_vars = [model.create_variable(f"dur_{i}", durations[i])
                for i in range(n)]

    # No two jobs overlap (disjunctive constraint)
    model.add_constraint(DisjunctiveConstraint(starts, dur_vars))

    # Makespan variable: makespan >= start_i + duration_i for all i
    makespan = model.create_variable("makespan", 0, horizon)
    for i in range(n):
        # start_i + dur_i <= makespan  =>  start_i + dur_i - makespan <= 0
        end_i = model.create_variable(f"end_{i}", durations[i], horizon)
        model.add_constraint(
            IntLinEqConstraint([1, 1, -1], [starts[i], dur_vars[i], end_i], 0)
        )
        model.add_constraint(IntLeConstraint(end_i, makespan))

    # Minimize makespan
    solver = Solver()
    best = solver.solve_optimize(
        model, makespan.id(), True,
        on_improve=lambda sol: (print(f"  Improved: makespan = {sol['makespan']}"), True)[-1]
    )

    if best:
        print(f"\nOptimal makespan: {best['makespan']}")
        for i in range(n):
            s = best[f"start_{i}"]
            print(f"  Job {i}: start={s}, duration={durations[i]}, end={s + durations[i]}")
    else:
        print("No solution")

    stats = solver.stats()
    print(f"  Failures: {stats.fail_count}")


if __name__ == "__main__":
    solve_scheduling()
