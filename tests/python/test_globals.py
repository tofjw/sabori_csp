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
    bin_packing_load,
    circuit,
    count,
    cumulative,
    element,
    global_cardinality,
    increasing,
    inverse,
    lex_less,
    lex_lesseq,
    maximum,
    minimum,
    nvalue,
    subcircuit,
    table,
    value_precede,
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
    def test_0based(self, ms):
        """Default offset=0: f[i]=j ⟺ g[j]=i, values in {0,1,2}."""
        m, s = ms
        f = [m.int_var(0, 2, f"f{i}") for i in range(3)]
        g = [m.int_var(0, 2, f"g{i}") for i in range(3)]
        m.add(inverse(f, g))
        m.add(all_different(f))
        s.solve(m)
        fv = [s.value(v) for v in f]
        gv = [s.value(v) for v in g]
        for i in range(3):
            assert gv[fv[i]] == i

    def test_1based(self, ms):
        """offset=1 (FlatZinc): f[i]=j ⟺ g[j-1]=i+1, values in {1,2,3}."""
        m, s = ms
        f = [m.int_var(1, 3, f"f{i}") for i in range(3)]
        g = [m.int_var(1, 3, f"g{i}") for i in range(3)]
        m.add(inverse(f, g, offset=1))
        m.add(all_different(f))
        s.solve(m)
        fv = [s.value(v) for v in f]
        gv = [s.value(v) for v in g]
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

    def test_var_target(self, ms):
        m, s = ms
        vs = [m.int_var(1, 3, f"v{i}") for i in range(5)]
        y = m.int_var(1, 3, "y")
        n = m.int_var(0, 5, "n")
        m.add(count(vs, y) == n)
        m.add(n == 4)
        m.add(vs[0] == 2)
        s.solve(m)
        vals = [s.value(v) for v in vs]
        assert vals.count(s.value(y)) == 4


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


class TestSubcircuit:
    def test_full_circuit(self, ms):
        m, s = ms
        vs = [m.int_var(0, 3, f"v{i}") for i in range(4)]
        m.add(subcircuit(vs))
        # Forbid self-loops so all nodes are in the circuit
        for i, v in enumerate(vs):
            m.add(v != i)
        s.solve(m)
        vals = [s.value(v) for v in vs]
        # Follow successors: must return to node 0 after exactly 4 steps
        node, seen = 0, set()
        for _ in range(4):
            assert node not in seen
            seen.add(node)
            node = vals[node]
        assert node == 0

    def test_partial_circuit(self, ms):
        m, s = ms
        vs = [m.int_var(0, 3, f"v{i}") for i in range(4)]
        m.add(subcircuit(vs))
        m.add(vs[3] == 3)  # node 3 out of the circuit
        m.add(vs[0] == 1)  # node 0 in the circuit
        s.solve(m)
        vals = [s.value(v) for v in vs]
        assert vals[3] == 3
        # Nodes in the circuit form a single cycle
        node, seen = 0, set()
        while node not in seen:
            seen.add(node)
            node = vals[node]
        assert node == 0
        for i in range(4):
            if i not in seen:
                assert vals[i] == i


class TestIncreasing:
    def test_non_strict(self, ms):
        m, s = ms
        vs = [m.int_var(1, 5, f"v{i}") for i in range(4)]
        m.add(increasing(vs))
        s.solve(m)
        vals = [s.value(v) for v in vs]
        assert all(vals[i] <= vals[i + 1] for i in range(3))

    def test_strict(self, ms):
        m, s = ms
        vs = [m.int_var(1, 4, f"v{i}") for i in range(4)]
        m.add(increasing(vs, strict=True))
        s.solve(m)
        vals = [s.value(v) for v in vs]
        assert vals == [1, 2, 3, 4]  # only feasible assignment

    def test_strict_infeasible(self, ms):
        m, s = ms
        vs = [m.int_var(1, 3, f"v{i}") for i in range(4)]
        m.add(increasing(vs, strict=True))
        assert s.solve(m) == SolveStatus.INFEASIBLE


class TestLex:
    def test_lesseq_allows_equal(self, ms):
        m, s = ms
        xs = [m.int_var(1, 3, f"x{i}") for i in range(2)]
        ys = [m.int_var(1, 3, f"y{i}") for i in range(2)]
        m.add(lex_lesseq(xs, ys))
        for x, y in zip(xs, ys):
            m.add(x == y)
        assert s.solve(m) == SolveStatus.FEASIBLE

    def test_less_forbids_equal(self, ms):
        m, s = ms
        xs = [m.int_var(1, 3, f"x{i}") for i in range(2)]
        ys = [m.int_var(1, 3, f"y{i}") for i in range(2)]
        m.add(lex_less(xs, ys))
        for x, y in zip(xs, ys):
            m.add(x == y)
        assert s.solve(m) == SolveStatus.INFEASIBLE

    def test_less_orders(self, ms):
        m, s = ms
        xs = [m.int_var(1, 3, f"x{i}") for i in range(2)]
        ys = [m.int_var(1, 3, f"y{i}") for i in range(2)]
        m.add(lex_less(xs, ys))
        s.solve(m)
        xv = [s.value(v) for v in xs]
        yv = [s.value(v) for v in ys]
        assert xv < yv  # Python list comparison is lexicographic


class TestValuePrecede:
    def test_basic(self, ms):
        m, s = ms
        vs = [m.int_var(1, 3, f"v{i}") for i in range(4)]
        m.add(value_precede(1, 2, vs))
        m.add(vs[0] == 2)  # 2 at index 0 → no room for a preceding 1
        assert s.solve(m) == SolveStatus.INFEASIBLE

    def test_forces_witness(self, ms):
        m, s = ms
        vs = [m.int_var(1, 3, f"v{i}") for i in range(4)]
        m.add(value_precede(1, 2, vs))
        m.add(vs[1] == 2)
        s.solve(m)
        assert s.value(vs[0]) == 1  # only possible witness for s=1


class TestGlobalCardinality:
    def test_basic(self, ms):
        m, s = ms
        vs = [m.int_var(1, 3, f"v{i}") for i in range(5)]
        c1 = m.int_var(0, 5, "c1")
        c2 = m.int_var(0, 5, "c2")
        m.add(global_cardinality(vs, [1, 2], [c1, c2]))
        m.add(c1 == 2)
        m.add(c2 == 3)
        s.solve(m)
        vals = [s.value(v) for v in vs]
        assert vals.count(1) == 2
        assert vals.count(2) == 3

    def test_length_mismatch(self, ms):
        m, _ = ms
        vs = [m.int_var(1, 3, f"v{i}") for i in range(3)]
        c1 = m.int_var(0, 3, "c1")
        with pytest.raises(ValueError):
            m.add(global_cardinality(vs, [1, 2], [c1]))


class TestBinPackingLoad:
    def test_basic(self, ms):
        m, s = ms
        bins = [m.int_var(0, 1, f"b{i}") for i in range(3)]
        loads = [m.int_var(0, 10, f"l{b}") for b in range(2)]
        weights = [3, 4, 5]
        m.add(bin_packing_load(loads, bins, weights))
        m.add(loads[0] == loads[1] + 2)  # 7 vs 5
        s.solve(m)
        bv = [s.value(b) for b in bins]
        lv = [s.value(ld) for ld in loads]
        for b in range(2):
            assert lv[b] == sum(w for i, w in enumerate(weights) if bv[i] == b)
        assert lv == [7, 5]

    def test_length_mismatch(self, ms):
        m, _ = ms
        bins = [m.int_var(0, 1, f"b{i}") for i in range(2)]
        loads = [m.int_var(0, 10, f"l{b}") for b in range(2)]
        with pytest.raises(ValueError):
            m.add(bin_packing_load(loads, bins, [3]))
