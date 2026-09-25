"""CpSolver: high-level solver interface."""
from __future__ import annotations

import threading
from enum import IntEnum
from typing import TYPE_CHECKING, Callable, Optional, Union

from sabori_csp import core
from sabori_csp._expressions import IntVar, LinearExpr

if TYPE_CHECKING:
    from sabori_csp._model import CpModel


class SolveStatus(IntEnum):
    """Result of a solve call."""

    FEASIBLE = 1
    OPTIMAL = 2
    INFEASIBLE = 3
    TIMEOUT = 4


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
        # ParallelSolver 経路で stop() を届けるために保持する（solve 中のみ有効）。
        self._parallel: Optional[core.ParallelSolver] = None
        # 並列経路の base WorkerConfig に反映する設定トグル。
        self._nogood_learning = True
        self._restart_enabled = True
        self._bisection_threshold = 8
        self._verbose = False
        self._verbose_worker = 0
        self._seed: Optional[int] = None  # None = ソルバ既定シードを使う
        self._last_result: Optional[core.ParallelResult] = None

    def solve(
        self,
        model: CpModel,
        *,
        num_workers: int = 1,
        time_limit: Optional[float] = None,
        handle_sigint: bool = False,
    ) -> SolveStatus:
        """Solve the model.

        Args:
            model: The model to solve.
            num_workers: Number of portfolio worker threads (>= 1). ``1`` (the
                default) uses the single-threaded solver, preserving previous
                behavior. ``> 1`` runs a diversified parallel portfolio.
            time_limit: Optional wall-clock limit in seconds. When set, a
                watchdog stops the search on expiry (returns the best solution
                found so far, or ``SolveStatus.TIMEOUT`` if none).
            handle_sigint: When ``True``, install a temporary ``SIGINT`` handler
                for the duration of the solve so that pressing Ctrl-C stops the
                search gracefully and returns the best solution found so far
                (instead of the interrupt being deferred until solve returns).
                The previous handler is restored afterward, and a *second*
                Ctrl-C propagates normally. Only effective when called from the
                main thread; ignored (with a warning) otherwise.

        Returns:
            ``OPTIMAL`` (proved optimal), ``FEASIBLE`` (solution found but not
            proved optimal / satisfaction problem), ``INFEASIBLE`` (proved no
            solution), or ``TIMEOUT`` (stopped with no solution / by Ctrl-C).
        """
        if num_workers < 1:
            raise ValueError("num_workers must be >= 1")

        # 単一スレッド・時間制限なしは従来経路（完全後方互換）。
        if num_workers == 1 and time_limit is None:
            run = lambda: self._solve_single(model)  # noqa: E731
        else:
            run = lambda: self._solve_parallel(model, num_workers, time_limit)  # noqa: E731

        if handle_sigint:
            return self._run_with_sigint(run)
        return run()

    def _run_with_sigint(self, run: Callable[[], SolveStatus]) -> SolveStatus:
        """Run ``run()`` on a worker thread while a SIGINT handler calls stop().

        The core solve() releases the GIL, so a Python-level SIGINT handler would
        not fire while the main thread is blocked inside it. Running the solve on
        a background thread keeps the main thread free to process the signal (via
        the periodic ``join`` below), letting the handler call ``self.stop()``.
        """
        import signal
        import warnings

        if threading.current_thread() is not threading.main_thread():
            warnings.warn(
                "handle_sigint=True is only supported on the main thread; "
                "ignoring and solving inline.",
                stacklevel=2,
            )
            return run()

        box: dict[str, object] = {}

        def worker() -> None:
            try:
                box["result"] = run()
            except BaseException as exc:  # noqa: BLE001 - re-raised on main thread
                box["error"] = exc

        prev = signal.getsignal(signal.SIGINT)

        def handler(signum, frame):  # noqa: ANN001, ARG001
            # 2 度目の Ctrl-C は通常動作（既定ハンドラ）に戻してから停止要求。
            signal.signal(signal.SIGINT, prev if prev is not None else signal.SIG_DFL)
            self.stop()

        thread = threading.Thread(target=worker, name="sabori-solve")
        signal.signal(signal.SIGINT, handler)
        try:
            thread.start()
            while thread.is_alive():
                # タイムアウト付き join で主スレッドが定期的に bytecode に戻り、
                # 保留中の SIGINT（→ handler → stop()）を処理できる。
                thread.join(0.05)
        finally:
            signal.signal(signal.SIGINT, prev if prev is not None else signal.SIG_DFL)

        if "error" in box:
            raise box["error"]  # type: ignore[misc]
        return box["result"]  # type: ignore[return-value]

    def _solve_single(self, model: CpModel) -> SolveStatus:
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

    def _solve_parallel(
        self,
        model: CpModel,
        num_workers: int,
        time_limit: Optional[float],
    ) -> SolveStatus:
        is_optimize = model._objective is not None

        base = core.WorkerConfig()
        base.nogood_learning = self._nogood_learning
        base.restart_enabled = self._restart_enabled
        base.bisection_threshold = self._bisection_threshold
        if self._seed is not None:
            base.seed = self._seed  # worker0 + 派生ワーカーのシードが base.seed 起点に
        configs = core.make_portfolio_configs(num_workers, is_optimize, base)

        ps = core.ParallelSolver(num_workers, configs)
        # 並列では全ワーカーの verbose を出すと交錯するため、1 ワーカーに限定する。
        if self._verbose:
            ps.set_verbose(True, self._verbose_worker)
        self._parallel = ps
        timer: Optional[threading.Timer] = None
        if time_limit is not None:
            timer = threading.Timer(time_limit, ps.stop)
            timer.daemon = True
            timer.start()
        try:
            if is_optimize:
                obj_var, minimize = model._objective
                result = ps.solve_optimize(model._model, obj_var.index, minimize)
            else:
                result = ps.solve(model._model)
        finally:
            if timer is not None:
                timer.cancel()
            self._parallel = None

        self._last_result = result
        self._solution = result.solution
        has_sol = result.solution is not None

        if has_sol:
            if is_optimize and result.proved_optimal:
                return SolveStatus.OPTIMAL
            return SolveStatus.FEASIBLE
        if result.status == core.SearchResult.UNSAT:
            return SolveStatus.INFEASIBLE
        return SolveStatus.TIMEOUT

    def solution(self) -> dict[str, int]:
        """Get the full solution as a dict mapping variable names to values."""
        if self._solution is None:
            raise RuntimeError("No solution found. Call solve() first.")
        return self._solution

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
        self._nogood_learning = enabled
        self._solver.set_nogood_learning(enabled)

    def set_restart_enabled(self, enabled: bool) -> None:
        self._restart_enabled = enabled
        self._solver.set_restart_enabled(enabled)

    def set_activity_selection(self, enabled: bool) -> None:
        # 注: 並列 (num_workers > 1) 経路には反映されない（WorkerConfig に相当項目なし）。
        self._solver.set_activity_selection(enabled)

    def set_activity_first(self, enabled: bool) -> None:
        # 注: 並列経路には反映されない（多様化は make_portfolio_configs が担う）。
        self._solver.set_activity_first(enabled)

    def set_bisection_threshold(self, threshold: int) -> None:
        self._bisection_threshold = threshold
        self._solver.set_bisection_threshold(threshold)

    def set_seed(self, seed: int) -> None:
        """Set the RNG seed for reproducible search.

        Applies to both the single-threaded and parallel paths. On the parallel
        path this becomes the base seed; worker 0 uses it directly and the other
        workers derive distinct seeds from it, so the whole portfolio shifts
        reproducibly with ``seed``.
        """
        self._seed = seed
        self._solver.set_seed(seed)

    def set_verbose(self, enabled: bool, worker_idx: int = 0) -> None:
        """Enable search-progress logging (printed to stderr).

        On the parallel path (``num_workers > 1``) output is limited to a single
        worker (``worker_idx``, default 0) to avoid interleaved lines.
        """
        self._verbose = enabled
        self._verbose_worker = worker_idx
        self._solver.set_verbose(enabled)

    def set_community_analysis(self, enabled: bool) -> None:
        # 注: 並列経路には反映されない。
        self._solver.set_community_analysis(enabled)

    def stop(self) -> None:
        """Signal the solver to stop (thread-safe)."""
        parallel = self._parallel
        if parallel is not None:
            parallel.stop()
        self._solver.stop()

    def reset_stop(self) -> None:
        """Clear the stop signal."""
        self._solver.reset_stop()
