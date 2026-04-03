"""Tests for sabori_csp.claude_helper module."""
import pytest

from sabori_csp import CpModel, CpSolver, SolveStatus, all_different
from sabori_csp.claude_helper import (
    diagnose,
    format_grid,
    format_schedule,
    format_table,
    parse_verbose,
    solve_with_timeout,
)


# ---------------------------------------------------------------------------
# format_grid
# ---------------------------------------------------------------------------


class TestFormatGrid:
    def test_3x3_grid(self):
        sol = {f"c_{r}_{c}": r * 3 + c + 1 for r in range(3) for c in range(3)}
        out = format_grid(sol, "c_{r}_{c}", 3, 3)
        assert "1" in out
        assert "9" in out
        lines = out.strip().splitlines()
        # header sep + 3 rows + 2 inner seps + footer sep = 7
        assert len(lines) == 7

    def test_sudoku_block_separators(self):
        # 4x4 grid with 2x2 blocks
        sol = {f"c_{r}_{c}": (r * 4 + c) % 4 + 1 for r in range(4) for c in range(4)}
        out = format_grid(sol, "c_{r}_{c}", 4, 4, block_rows=2, block_cols=2)
        # Should have thick separators with '='
        assert "=" in out

    def test_value_map(self):
        sol = {"c_0_0": 1, "c_0_1": 0}
        vmap = {1: "Q", 0: "."}
        out = format_grid(sol, "c_{r}_{c}", 1, 2, value_map=vmap)
        assert "Q" in out
        assert "." in out

    def test_missing_var(self):
        sol = {"c_0_0": 5}  # c_0_1 missing
        out = format_grid(sol, "c_{r}_{c}", 1, 2)
        assert "?" in out


# ---------------------------------------------------------------------------
# format_schedule
# ---------------------------------------------------------------------------


class TestFormatSchedule:
    def test_basic_schedule(self):
        sol = {"s_A": 0, "s_B": 4, "s_C": 8}
        tasks = [
            {"name": "Task A", "start_var": "s_A", "duration": 4},
            {"name": "Task B", "start_var": "s_B", "duration": 4},
            {"name": "Task C", "start_var": "s_C", "duration": 4},
        ]
        out = format_schedule(sol, tasks)
        assert "Task A" in out
        assert "Task B" in out
        assert "Task C" in out
        assert "=" in out  # bar characters
        assert "[0, 4)" in out

    def test_duration_as_variable(self):
        sol = {"s_0": 0, "d_0": 5}
        tasks = [{"name": "T", "start_var": "s_0", "duration": "d_0"}]
        out = format_schedule(sol, tasks)
        assert "[0, 5)" in out

    def test_empty_tasks(self):
        out = format_schedule({}, [])
        assert "no tasks" in out


# ---------------------------------------------------------------------------
# format_table
# ---------------------------------------------------------------------------


class TestFormatTable:
    def test_basic_table(self):
        sol = {"x": 1, "y": 2, "z": 3}
        out = format_table(sol, var_names=["x", "y", "z"])
        assert "x" in out
        assert "3" in out
        lines = out.strip().splitlines()
        # sep + header + sep + 3 rows + sep = 7
        assert len(lines) == 7

    def test_auto_sort(self):
        sol = {"bb": 2, "aa": 1}
        out = format_table(sol)
        lines = out.strip().splitlines()
        # 'aa' should come before 'bb'
        aa_line = [i for i, l in enumerate(lines) if "aa" in l and "|" in l][0]
        bb_line = [i for i, l in enumerate(lines) if "bb" in l and "|" in l][0]
        assert aa_line < bb_line

    def test_empty(self):
        out = format_table({})
        assert "no variables" in out

    def test_custom_headers(self):
        sol = {"x": 42}
        out = format_table(sol, headers=("Name", "Result"))
        assert "Name" in out
        assert "Result" in out


# ---------------------------------------------------------------------------
# parse_verbose
# ---------------------------------------------------------------------------


class TestParseVerbose:
    SAMPLE_VERBOSE = """\
% [verbose] presolve start: 27 constraints, 81 variables (avg 3.0 constraints/var, max 4)
% [verbose] presolve done
% [verbose] vars: total=81 decision=51 defined=30
% [verbose] all_different: vars=9 (unfixed=6) vals=9 ratio=0.67
% [verbose] search vars: decision=51 defined=30
% [verbose] search_with_restart start
% [verbose] restart #1 cl=100 outer=50 fails=150 max_depth=12 nogoods=42 prune=10
% [verbose] cycle end: prune_delta=10 depth_grew=true new_outer=75
% [verbose] restart #2 cl=100 outer=75 fails=300 max_depth=15 nogoods=85 prune=25
% [verbose] cycle end: prune_delta=15 depth_grew=false new_outer=75
"""

    def test_presolve(self):
        data = parse_verbose(self.SAMPLE_VERBOSE)
        assert data["presolve"]["constraints"] == 27
        assert data["presolve"]["variables"] == 81
        assert data["presolve"]["failed"] is False

    def test_vars(self):
        data = parse_verbose(self.SAMPLE_VERBOSE)
        assert data["vars"]["total"] == 81
        assert data["vars"]["decision"] == 51
        assert data["vars"]["defined"] == 30

    def test_search_vars(self):
        data = parse_verbose(self.SAMPLE_VERBOSE)
        assert data["search_vars"]["decision"] == 51

    def test_all_different(self):
        data = parse_verbose(self.SAMPLE_VERBOSE)
        assert len(data["all_different"]) == 1
        assert data["all_different"][0]["unfixed"] == 6

    def test_restarts(self):
        data = parse_verbose(self.SAMPLE_VERBOSE)
        assert len(data["restarts"]) == 2
        assert data["restarts"][0]["fails"] == 150
        assert data["restarts"][1]["nogoods"] == 85

    def test_cycles(self):
        data = parse_verbose(self.SAMPLE_VERBOSE)
        assert len(data["cycles"]) == 2
        assert data["cycles"][0]["prune_delta"] == 10
        assert data["cycles"][0]["depth_grew"] is True

    def test_presolve_failed(self):
        text = "% [verbose] presolve start: 5 constraints, 10 variables (avg 2.0 constraints/var, max 3)\n% [verbose] presolve failed\n"
        data = parse_verbose(text)
        assert data["outcome"] == "infeasible"
        assert data["presolve"]["failed"] is True

    def test_timeout(self):
        text = "% [verbose] search stopped (timeout)\n"
        data = parse_verbose(text)
        assert data["outcome"] == "timeout"

    def test_optimal(self):
        text = "% [verbose] new best objective: 42\n% [verbose] optimal (search exhausted)\n"
        data = parse_verbose(text)
        assert data["outcome"] == "optimal"
        assert data["optimal_reason"] == "search exhausted"
        assert data["objectives"] == [42]

    def test_bump_activity(self):
        text = "% [verbose] bump_activity enabled (modularity=0.5)\n"
        data = parse_verbose(text)
        assert data["bump_activity"] is True

        text2 = "% [verbose] bump_activity disabled (modularity=0.1)\n"
        data2 = parse_verbose(text2)
        assert data2["bump_activity"] is False


# ---------------------------------------------------------------------------
# diagnose
# ---------------------------------------------------------------------------


class TestDiagnose:
    def test_presolve_failed(self):
        data = {"presolve": {"failed": True}, "restarts": [], "all_different": [], "outcome": "infeasible"}
        hints = diagnose(data)
        assert any("contradictory" in h.lower() or "infeasib" in h.lower() for h in hints)

    def test_many_restarts(self):
        restarts = [{"number": i, "fails": i * 100, "nogoods": i * 10, "prune": i * 5, "max_depth": 10, "conflict_limit": 100, "outer": 50} for i in range(1, 52)]
        data = {"presolve": {}, "restarts": restarts, "all_different": [], "outcome": "timeout"}
        hints = diagnose(data)
        assert any("restart" in h.lower() for h in hints)

    def test_healthy(self):
        data = {"presolve": {}, "restarts": [], "all_different": [], "outcome": "unknown"}
        hints = diagnose(data)
        assert any("no issues" in h.lower() for h in hints)

    def test_tight_alldiff(self):
        data = {"presolve": {}, "restarts": [], "all_different": [{"vars": 9, "unfixed": 9, "vals": 9, "ratio": 1.0}], "outcome": "unknown"}
        hints = diagnose(data)
        assert any("tight" in h.lower() for h in hints)

    def test_loose_alldiff(self):
        data = {"presolve": {}, "restarts": [], "all_different": [{"vars": 3, "unfixed": 3, "vals": 100, "ratio": 0.03}], "outcome": "unknown"}
        hints = diagnose(data)
        assert any("loose" in h.lower() for h in hints)


# ---------------------------------------------------------------------------
# solve_with_timeout (integration)
# ---------------------------------------------------------------------------


class TestSolveWithTimeout:
    def test_simple_solve(self):
        m = CpModel()
        x = m.int_var(1, 5, "x")
        y = m.int_var(1, 5, "y")
        m.add(x + y == 6)
        m.add(x != y)

        result = solve_with_timeout(m, timeout_sec=5.0)
        assert result["status"] in ("FEASIBLE", "OPTIMAL")
        assert result["values"] is not None
        assert result["values"]["x"] + result["values"]["y"] == 6

    def test_infeasible(self):
        m = CpModel()
        x = m.int_var(1, 3, "x")
        y = m.int_var(1, 3, "y")
        m.add(x == y)
        m.add(x != y)

        result = solve_with_timeout(m, timeout_sec=5.0)
        assert result["status"] == "INFEASIBLE"
        assert result["values"] is None
