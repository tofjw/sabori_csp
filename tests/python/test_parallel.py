"""Tests for parallel (portfolio) solving and time limits via CpSolver."""
import os
import sys
import time

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../../python"))

from sabori_csp import CpModel, CpSolver, SolveStatus, all_different
from sabori_csp import core


def _queens(n):
    """Build an n-queens model, returning (model, vars)."""
    m = CpModel()
    xs = [m.int_var(0, n - 1, f"q{i}") for i in range(n)]
    m.add(all_different(xs))
    m.add(all_different([xs[i] + i for i in range(n)]))
    m.add(all_different([xs[i] - i for i in range(n)]))
    return m, xs


def _is_valid_queens(vals):
    n = len(vals)
    return (
        len(set(vals)) == n
        and len({vals[i] + i for i in range(n)}) == n
        and len({vals[i] - i for i in range(n)}) == n
    )


class TestLowLevelPortfolio:
    def test_make_portfolio_configs_count_and_worker0(self):
        cfgs = core.make_portfolio_configs(4, is_optimize=False)
        assert len(cfgs) == 4
        # worker0 is the default (guarantees "no worse than single-thread")
        assert cfgs[0].seed == 12345678
        # workers 1+ get shifted seeds -> diversification
        assert len({c.seed for c in cfgs}) == 4

    def test_make_portfolio_configs_base_inherited(self):
        base = core.WorkerConfig()
        base.bisection_threshold = 3
        base.nogood_learning = False
        cfgs = core.make_portfolio_configs(3, is_optimize=True, base=base)
        assert cfgs[0].bisection_threshold == 3
        assert cfgs[0].nogood_learning is False


class TestParallelSatisfaction:
    def test_parallel_solves_queens(self):
        m, xs = _queens(8)
        s = CpSolver()
        status = s.solve(m, num_workers=4)
        assert status == SolveStatus.FEASIBLE
        vals = [s.value(x) for x in xs]
        assert _is_valid_queens(vals)

    def test_solution_and_value_accessible_in_parallel(self):
        m, xs = _queens(6)
        s = CpSolver()
        s.solve(m, num_workers=4)
        sol = s.solution()
        assert sol[xs[0].name] == s.value(xs[0])
        # LinearExpr evaluation over the parallel solution
        assert s.value(xs[0] + xs[1]) == s.value(xs[0]) + s.value(xs[1])

    def test_single_thread_default_unchanged(self):
        m, xs = _queens(8)
        s = CpSolver()
        # default num_workers=1, no time_limit -> legacy single-thread path
        status = s.solve(m)
        assert status == SolveStatus.FEASIBLE
        assert _is_valid_queens([s.value(x) for x in xs])


class TestParallelOptimization:
    def _bounded_model(self):
        m = CpModel()
        x = m.int_var(0, 10, "x")
        y = m.int_var(0, 10, "y")
        m.add(x + 2 * y <= 14)
        m.add(3 * x - y >= 0)
        m.maximize(x + y)
        return m, x, y

    def test_parallel_optimum_matches_single(self):
        m, x, y = self._bounded_model()
        s_par = CpSolver()
        st_par = s_par.solve(m, num_workers=4)
        s_seq = CpSolver()
        st_seq = s_seq.solve(m)
        assert st_par == SolveStatus.OPTIMAL
        assert st_seq == SolveStatus.OPTIMAL
        assert s_par.value(x + y) == s_seq.value(x + y)


class TestTimeLimit:
    def test_time_limit_returns_promptly(self):
        # A hard optimization that will not finish within a tiny budget.
        m, xs = _queens(30)
        m.maximize(sum(xs[i] * i for i in range(30)))
        s = CpSolver()
        t0 = time.time()
        status = s.solve(m, num_workers=4, time_limit=0.3)
        elapsed = time.time() - t0
        # watchdog must stop the search well before an unbounded run would
        assert elapsed < 5.0
        # any valid terminal status is acceptable; not INFEASIBLE (it is SAT)
        assert status in (
            SolveStatus.FEASIBLE,
            SolveStatus.OPTIMAL,
            SolveStatus.TIMEOUT,
        )

    def test_generous_time_limit_completes(self):
        m, xs = _queens(8)
        s = CpSolver()
        status = s.solve(m, num_workers=2, time_limit=30.0)
        assert status == SolveStatus.FEASIBLE
        assert _is_valid_queens([s.value(x) for x in xs])

    def test_time_limit_single_worker(self):
        # num_workers=1 with time_limit routes through the parallel(1) path.
        m, xs = _queens(8)
        s = CpSolver()
        status = s.solve(m, num_workers=1, time_limit=30.0)
        assert status == SolveStatus.FEASIBLE


class TestHandleSigint:
    def test_sigint_stops_search_and_returns_partial(self):
        import os
        import signal
        import threading

        m, xs = _queens(40)
        m.maximize(sum(xs[i] * i for i in range(40)))
        s = CpSolver()
        threading.Timer(0.5, lambda: os.kill(os.getpid(), signal.SIGINT)).start()
        t0 = time.time()
        status = s.solve(m, num_workers=4, handle_sigint=True)
        elapsed = time.time() - t0
        assert elapsed < 5.0  # Ctrl-C stopped the search promptly
        assert status in (
            SolveStatus.FEASIBLE,
            SolveStatus.OPTIMAL,
            SolveStatus.TIMEOUT,
        )
        # original handler restored
        assert signal.getsignal(signal.SIGINT) is signal.default_int_handler

    def test_no_interrupt_completes_and_restores_handler(self):
        import signal

        before = signal.getsignal(signal.SIGINT)
        m, xs = _queens(8)
        s = CpSolver()
        status = s.solve(m, num_workers=4, handle_sigint=True)
        assert status == SolveStatus.FEASIBLE
        assert _is_valid_queens([s.value(x) for x in xs])
        assert signal.getsignal(signal.SIGINT) is before

    def test_non_main_thread_falls_back_with_warning(self):
        import threading
        import warnings

        box = {}

        def work():
            m, xs = _queens(8)
            with warnings.catch_warnings(record=True) as caught:
                warnings.simplefilter("always")
                box["status"] = CpSolver().solve(m, handle_sigint=True)
                box["warned"] = any(
                    "main thread" in str(w.message) for w in caught
                )

        t = threading.Thread(target=work)
        t.start()
        t.join()
        assert box["status"] == SolveStatus.FEASIBLE
        assert box["warned"]


class TestSeed:
    def test_set_seed_accepted_and_solves(self):
        # set_seed must be accepted on both single and parallel paths and still
        # produce a valid solution. (Note: the solver is not fully reproducible
        # run-to-run even with a fixed seed, so we do not assert equality here.)
        for workers in (1, 4):
            m, xs = _queens(8)
            s = CpSolver()
            s.set_seed(12345)
            status = s.solve(m, num_workers=workers)
            assert status == SolveStatus.FEASIBLE
            assert _is_valid_queens([s.value(x) for x in xs])

    def test_worker_seeds_derive_from_base_seed(self):
        base = core.WorkerConfig()
        base.seed = 1000
        cfgs = core.make_portfolio_configs(3, is_optimize=True, base=base)
        assert cfgs[0].seed == 1000
        assert len({c.seed for c in cfgs}) == 3  # distinct per worker

    def test_default_worker_seeds_unchanged(self):
        # Default base.seed (12345678) must reproduce the historical formula
        # so CLI / existing behavior is untouched.
        cfgs = core.make_portfolio_configs(2, is_optimize=True)
        assert cfgs[0].seed == 12345678
        assert cfgs[1].seed == (12345678 + 2654435761) % (2**32)


class TestValidation:
    def test_num_workers_must_be_positive(self):
        m, _ = _queens(4)
        s = CpSolver()
        with pytest.raises(ValueError):
            s.solve(m, num_workers=0)
