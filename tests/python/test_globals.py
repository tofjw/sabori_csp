"""Tests for global constraint helpers."""
import pytest
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../../python"))

from sabori_csp import (
    CpModel,
    CpSolver,
    SolveStatus,
    all_different,
    all_different_except_0,
    circuit,
    count,
    cumulative,
    element,
    inverse,
    maximum,
    minimum,
    nvalue,
    table,
)


@pytest.fixture
def ms():
    return CpModel(), CpSolver()


class TestAllDifferent:
    def test_basic(self, ms):
        m, s = ms
        vs = [m.int_var(1, 4, f"v{i}") for i in range(4)]
        m.add(all_different(vs))
        s.solve(m)
        vals = [s.value(v) for v in vs]
        assert len(set(vals)) == 4

    def test_infeasible(self, ms):
        m, s = ms
        vs = [m.int_var(1, 3, f"v{i}") for i in range(4)]
        m.add(all_different(vs))
        assert s.solve(m) == SolveStatus.INFEASIBLE


class TestAllDifferentExcept0:
    def test_basic(self, ms):
        m, s = ms
        vs = [m.int_var(0, 3, f"v{i}") for i in range(4)]
        m.add(all_different_except_0(vs))
        m.add(vs[0] == 0)
        m.add(vs[1] == 0)
        s.solve(m)
        vals = [s.value(v) for v in vs]
        assert vals[0] == 0 and vals[1] == 0
        non_zero = [v for v in vals if v != 0]
        assert len(non_zero) == len(set(non_zero))


class TestCircuit:
    def test_3_nodes(self, ms):
        m, s = ms
        # 0-based successor representation
        vs = [m.int_var(0, 2, f"n{i}") for i in range(3)]
        m.add(circuit(vs))
        s.solve(m)
        vals = [s.value(v) for v in vs]
        # Verify it forms a single cycle
        visited = set()
        cur = 0
        for _ in range(3):
            assert cur not in visited
            visited.add(cur)
            cur = vals[cur]
        assert cur == 0  # back to start


class TestElement:
    def test_const_array(self, ms):
        m, s = ms
        idx = m.int_var(0, 3, "idx")
        result = m.int_var(0, 100, "result")
        m.add(element(idx, [10, 20, 30, 40], result))
        m.add(idx == 2)
        s.solve(m)
        assert s.value(result) == 30

    def test_var_array(self, ms):
        m, s = ms
        a = m.int_var(10, 10, "a")
        b = m.int_var(20, 20, "b")
        c = m.int_var(30, 30, "c")
        idx = m.int_var(0, 2, "idx")
        result = m.int_var(0, 100, "result")
        m.add(element(idx, [a, b, c], result))
        m.add(idx == 1)
        s.solve(m)
        assert s.value(result) == 20


class TestTable:
    def test_basic(self, ms):
        m, s = ms
        x = m.int_var(1, 3, "x")
        y = m.int_var(1, 3, "y")
        m.add(table([x, y], [[1, 2], [2, 3], [3, 1]]))
        solutions = []
        s.solve_all(m, lambda sol: (solutions.append((sol["x"], sol["y"])), True)[-1])
        assert set(solutions) == {(1, 2), (2, 3), (3, 1)}


class TestInverse:
    def test_basic(self, ms):
        m, s = ms
        # inverse uses 1-based indexing (FlatZinc convention)
        f = [m.int_var(1, 3, f"f{i}") for i in range(3)]
        g = [m.int_var(1, 3, f"g{i}") for i in range(3)]
        m.add(inverse(f, g))
        m.add(all_different(f))
        s.solve(m)
        fv = [s.value(v) for v in f]
        gv = [s.value(v) for v in g]
        # f[i] = j ⟹ g[j-1] = i+1 (1-based)
        for i in range(3):
            assert gv[fv[i] - 1] == i + 1


class TestMaximum:
    def test_basic(self, ms):
        m, s = ms
        vs = [m.int_var(1, 10, f"v{i}") for i in range(3)]
        mx = m.int_var(1, 10, "mx")
        m.add(maximum(vs) == mx)
        m.add(vs[0] == 3)
        m.add(vs[1] == 7)
        m.add(vs[2] == 5)
        s.solve(m)
        assert s.value(mx) == 7


class TestMinimum:
    def test_basic(self, ms):
        m, s = ms
        vs = [m.int_var(1, 10, f"v{i}") for i in range(3)]
        mn = m.int_var(1, 10, "mn")
        m.add(minimum(vs) == mn)
        m.add(vs[0] == 3)
        m.add(vs[1] == 7)
        m.add(vs[2] == 5)
        s.solve(m)
        assert s.value(mn) == 3


class TestCount:
    def test_basic(self, ms):
        m, s = ms
        vs = [m.int_var(1, 3, f"v{i}") for i in range(5)]
        n = m.int_var(0, 5, "n")
        m.add(count(vs, 1) == n)
        m.add(n == 3)
        s.solve(m)
        vals = [s.value(v) for v in vs]
        assert vals.count(1) == 3


class TestNValue:
    def test_basic(self, ms):
        m, s = ms
        vs = [m.int_var(1, 3, f"v{i}") for i in range(4)]
        n = m.int_var(1, 3, "n")
        m.add(nvalue(vs) == n)
        m.add(n == 2)
        s.solve(m)
        vals = [s.value(v) for v in vs]
        assert len(set(vals)) == 2


class TestCumulative:
    def test_basic(self, ms):
        m, s = ms
        starts = [m.int_var(0, 10, f"s{i}") for i in range(3)]
        durs = [m.int_var(2, 2, f"d{i}") for i in range(3)]
        demands = [m.int_var(1, 1, f"r{i}") for i in range(3)]
        cap = m.constant(2)
        m.add(cumulative(starts, durs, demands, cap))
        # All 3 tasks with dur=2, demand=1, capacity=2
        # At most 2 tasks at any time
        s.solve(m)
        sv = [s.value(st) for st in starts]
        # Verify resource constraint
        for t in range(max(sv) + 2):
            active = sum(1 for i in range(3) if sv[i] <= t < sv[i] + 2)
            assert active <= 2

    def test_int_args(self, ms):
        """Test that plain int arguments for durations/demands/capacity work."""
        m, s = ms
        starts = [m.int_var(0, 10, f"s{i}") for i in range(2)]
        m.add(cumulative(starts, [3, 3], [1, 1], 1))
        s.solve(m)
        s0, s1 = s.value(starts[0]), s.value(starts[1])
        # With capacity=1 and dur=3, tasks cannot overlap
        assert s0 + 3 <= s1 or s1 + 3 <= s0
