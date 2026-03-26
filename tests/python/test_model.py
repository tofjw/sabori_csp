"""Tests for CpModel constraint lowering and end-to-end solving."""
import pytest
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../../python"))

from sabori_csp import CpModel, CpSolver, SolveStatus, all_different


@pytest.fixture
def ms():
    """Return (model, solver) pair."""
    return CpModel(), CpSolver()


class TestVariableCreation:
    def test_int_var(self):
        m = CpModel()
        x = m.int_var(1, 10, "x")
        assert x.name == "x"

    def test_int_var_auto_name(self):
        m = CpModel()
        v0 = m.int_var(1, 10)
        v1 = m.int_var(1, 10)
        assert v0.name != v1.name

    def test_bool_var(self):
        m = CpModel()
        b = m.bool_var("b")
        assert b.name == "b"

    def test_int_var_from_domain(self):
        m = CpModel()
        v = m.int_var_from_domain([1, 3, 5, 7], "odd")
        assert v.name == "odd"

    def test_constant(self):
        m = CpModel()
        c1 = m.constant(42)
        c2 = m.constant(42)
        assert c1 is c2  # cached


class TestBinaryConstraints:
    def test_eq(self, ms):
        m, s = ms
        x = m.int_var(1, 5, "x")
        y = m.int_var(1, 5, "y")
        m.add(x == y)
        status = s.solve(m)
        assert status == SolveStatus.FEASIBLE
        assert s.value(x) == s.value(y)

    def test_ne(self, ms):
        m, s = ms
        x = m.int_var(1, 2, "x")
        y = m.int_var(1, 2, "y")
        m.add(x != y)
        status = s.solve(m)
        assert status == SolveStatus.FEASIBLE
        assert s.value(x) != s.value(y)

    def test_le(self, ms):
        m, s = ms
        x = m.int_var(1, 10, "x")
        y = m.int_var(1, 10, "y")
        m.add(x <= y)
        m.add(x == 7)
        status = s.solve(m)
        assert status == SolveStatus.FEASIBLE
        assert s.value(y) >= 7

    def test_lt(self, ms):
        m, s = ms
        x = m.int_var(1, 10, "x")
        y = m.int_var(1, 10, "y")
        m.add(x < y)
        m.add(y == 3)
        status = s.solve(m)
        assert s.value(x) < 3

    def test_ge(self, ms):
        m, s = ms
        x = m.int_var(1, 10, "x")
        y = m.int_var(1, 10, "y")
        m.add(x >= y)
        m.add(y == 5)
        status = s.solve(m)
        assert s.value(x) >= 5

    def test_gt(self, ms):
        m, s = ms
        x = m.int_var(1, 10, "x")
        y = m.int_var(1, 10, "y")
        m.add(x > y)
        m.add(y == 5)
        status = s.solve(m)
        assert s.value(x) > 5

    def test_eq_const(self, ms):
        m, s = ms
        x = m.int_var(1, 10, "x")
        m.add(x == 5)
        s.solve(m)
        assert s.value(x) == 5


class TestLinearConstraints:
    def test_sum_eq(self, ms):
        m, s = ms
        x = m.int_var(1, 10, "x")
        y = m.int_var(1, 10, "y")
        m.add(x + y == 10)
        s.solve(m)
        assert s.value(x) + s.value(y) == 10

    def test_weighted_sum(self, ms):
        m, s = ms
        x = m.int_var(1, 5, "x")
        y = m.int_var(1, 5, "y")
        m.add(2 * x + 3 * y == 13)
        s.solve(m)
        assert 2 * s.value(x) + 3 * s.value(y) == 13

    def test_sum_le(self, ms):
        m, s = ms
        x = m.int_var(1, 10, "x")
        y = m.int_var(1, 10, "y")
        m.add(x + y <= 5)
        s.solve(m)
        assert s.value(x) + s.value(y) <= 5

    def test_sum_ne(self, ms):
        m, s = ms
        x = m.int_var(5, 5, "x")
        y = m.int_var(5, 5, "y")
        m.add(x + y != 10)
        status = s.solve(m)
        assert status == SolveStatus.INFEASIBLE

    def test_builtin_sum(self, ms):
        m, s = ms
        xs = [m.int_var(1, 5, f"x{i}") for i in range(3)]
        m.add(sum(xs) == 6)
        m.add(all_different(xs))
        s.solve(m)
        assert sum(s.value(x) for x in xs) == 6


class TestNonLinear:
    def test_times(self, ms):
        m, s = ms
        x = m.int_var(1, 10, "x")
        y = m.int_var(1, 10, "y")
        z = m.int_var(1, 100, "z")
        m.add(x * y == z)
        m.add(x == 3)
        m.add(y == 4)
        s.solve(m)
        assert s.value(z) == 12

    def test_abs(self, ms):
        m, s = ms
        x = m.int_var(-10, 10, "x")
        y = m.int_var(0, 10, "y")
        m.add(abs(x) == y)
        m.add(x == -7)
        s.solve(m)
        assert s.value(y) == 7

    def test_div(self, ms):
        m, s = ms
        x = m.int_var(1, 20, "x")
        y = m.int_var(1, 10, "y")
        z = m.int_var(0, 20, "z")
        m.add(x // y == z)
        m.add(x == 17)
        m.add(y == 5)
        s.solve(m)
        assert s.value(z) == 3

    def test_mod(self, ms):
        m, s = ms
        x = m.int_var(1, 20, "x")
        y = m.int_var(1, 10, "y")
        z = m.int_var(0, 10, "z")
        m.add(x % y == z)
        m.add(x == 17)
        m.add(y == 5)
        s.solve(m)
        assert s.value(z) == 2


class TestOptimization:
    def test_minimize(self, ms):
        m, s = ms
        x = m.int_var(1, 10, "x")
        y = m.int_var(1, 10, "y")
        m.add(x + y >= 5)
        m.minimize(x + y)
        status = s.solve(m)
        assert status == SolveStatus.OPTIMAL
        assert s.value(x) + s.value(y) == 5

    def test_maximize(self, ms):
        m, s = ms
        x = m.int_var(1, 10, "x")
        y = m.int_var(1, 10, "y")
        m.add(x + y <= 15)
        m.maximize(x + y)
        status = s.solve(m)
        assert status == SolveStatus.OPTIMAL
        assert s.value(x) + s.value(y) == 15

    def test_minimize_single_var(self, ms):
        m, s = ms
        x = m.int_var(3, 10, "x")
        m.minimize(x)
        status = s.solve(m)
        assert status == SolveStatus.OPTIMAL
        assert s.value(x) == 3

    def test_infeasible(self, ms):
        m, s = ms
        x = m.int_var(1, 5, "x")
        m.add(x >= 10)
        status = s.solve(m)
        assert status == SolveStatus.INFEASIBLE


class TestSolveAll:
    def test_count_solutions(self, ms):
        m, s = ms
        x = m.int_var(1, 3, "x")
        y = m.int_var(1, 3, "y")
        m.add(x != y)
        solutions = []
        count = s.solve_all(m, lambda sol: (solutions.append(dict(sol)), True)[-1])
        assert count == 6  # 3*3 - 3 = 6


class TestNQueens:
    def test_4queens(self):
        n = 4
        m = CpModel()
        queens = [m.int_var(0, n - 1, f"q{i}") for i in range(n)]
        m.add(all_different(queens))
        m.add(all_different([queens[i] + i for i in range(n)]))
        m.add(all_different([queens[i] - i for i in range(n)]))

        s = CpSolver()
        status = s.solve(m)
        assert status == SolveStatus.FEASIBLE
        vals = [s.value(q) for q in queens]
        # Verify no conflicts
        for i in range(n):
            for j in range(i + 1, n):
                assert vals[i] != vals[j]
                assert abs(vals[i] - vals[j]) != abs(i - j)

    def test_8queens_count(self):
        n = 8
        m = CpModel()
        queens = [m.int_var(0, n - 1, f"q{i}") for i in range(n)]
        m.add(all_different(queens))
        m.add(all_different([queens[i] + i for i in range(n)]))
        m.add(all_different([queens[i] - i for i in range(n)]))

        s = CpSolver()
        count = s.solve_all(m, lambda _: True)
        assert count == 92
