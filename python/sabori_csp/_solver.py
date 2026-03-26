"""CpSolver: high-level solver interface."""
from __future__ import annotations

from enum import IntEnum
from typing import TYPE_CHECKING, Callable, Union

from sabori_csp import core
from sabori_csp._expressions import IntVar, LinearExpr

if TYPE_CHECKING:
    from sabori_csp._model import CpModel


class SolveStatus(IntEnum):
    """Result of a solve call."""

    FEASIBLE = 1
    OPTIMAL = 2
    INFEASIBLE = 3


class CpSolver:
    """High-level solver that wraps the C++ Solver.

    Example::

        solver = CpSolver()
        status = solver.solve(model)
        if status == SolveStatus.FEASIBLE:
            print(solver.value(x))
    """

    def __init__(self) -> None:
        self._solver = core.Solver()
        self._solution: dict[str, int] | None = None

    def solve(self, model: CpModel) -> SolveStatus:
        """Solve the model.

        Returns SolveStatus.FEASIBLE or OPTIMAL if a solution is found,
        SolveStatus.INFEASIBLE otherwise.
        """
        if model._objective is not None:
            obj_var, minimize = model._objective
            sol = self._solver.solve_optimize(
                model._model, obj_var.index, minimize
            )
            if sol is not None:
                self._solution = sol
                return SolveStatus.OPTIMAL
            return SolveStatus.INFEASIBLE
        else:
            sol = self._solver.solve(model._model)
            if sol is not None:
                self._solution = sol
                return SolveStatus.FEASIBLE
            return SolveStatus.INFEASIBLE

    def value(self, var_or_expr: Union[IntVar, LinearExpr]) -> int:
        """Get the value of a variable or expression in the current solution."""
        if self._solution is None:
            raise RuntimeError("No solution found. Call solve() first.")
        if isinstance(var_or_expr, IntVar):
            return self._solution[var_or_expr.name]
        if isinstance(var_or_expr, LinearExpr):
            result = var_or_expr._const
            for var, coeff in var_or_expr._terms.items():
                result += coeff * self._solution[var.name]
            return result
        raise TypeError(f"Expected IntVar or LinearExpr, got {type(var_or_expr)}")

    def solve_all(
        self,
        model: CpModel,
        callback: Callable[[dict[str, int]], bool],
    ) -> int:
        """Find all solutions, calling callback for each one.

        The callback receives a dict mapping variable names to values.
        Return True from callback to continue search, False to stop.
        Returns the total number of solutions found.
        """
        return self._solver.solve_all(model._model, callback)

    @property
    def stats(self) -> core.SolverStats:
        """Access solver statistics."""
        return self._solver.stats()

    # --- Configuration passthrough ---

    def set_nogood_learning(self, enabled: bool) -> None:
        self._solver.set_nogood_learning(enabled)

    def set_restart_enabled(self, enabled: bool) -> None:
        self._solver.set_restart_enabled(enabled)

    def set_activity_selection(self, enabled: bool) -> None:
        self._solver.set_activity_selection(enabled)

    def set_activity_first(self, enabled: bool) -> None:
        self._solver.set_activity_first(enabled)

    def set_bisection_threshold(self, threshold: int) -> None:
        self._solver.set_bisection_threshold(threshold)

    def set_verbose(self, enabled: bool) -> None:
        self._solver.set_verbose(enabled)

    def set_community_analysis(self, enabled: bool) -> None:
        self._solver.set_community_analysis(enabled)

    def stop(self) -> None:
        """Signal the solver to stop (thread-safe)."""
        self._solver.stop()

    def reset_stop(self) -> None:
        """Clear the stop signal."""
        self._solver.reset_stop()
