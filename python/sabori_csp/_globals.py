"""Global constraint helpers for the high-level API."""
from __future__ import annotations

from typing import Sequence, Union

from sabori_csp._expressions import (
    IntVar,
    _NonLinearExpr,
)


class _GlobalConstraint:
    """Base marker for global constraints, posted by CpModel.add()."""

    pass


# --- Direct constraints (model.add(all_different([x, y, z]))) ---


class _AllDifferent(_GlobalConstraint):
    def __init__(self, vars: Sequence[IntVar]) -> None:
        self.vars = list(vars)


class _AllDifferentExcept0(_GlobalConstraint):
    def __init__(self, vars: Sequence[IntVar]) -> None:
        self.vars = list(vars)


class _Circuit(_GlobalConstraint):
    def __init__(self, vars: Sequence[IntVar]) -> None:
        self.vars = list(vars)


class _Element(_GlobalConstraint):
    def __init__(
        self,
        index: IntVar,
        array: Sequence[Union[int, IntVar]],
        result: IntVar,
    ) -> None:
        self.index = index
        self.array = list(array)
        self.result = result


class _Table(_GlobalConstraint):
    def __init__(
        self, vars: Sequence[IntVar], tuples: Sequence[Sequence[int]]
    ) -> None:
        self.vars = list(vars)
        self.tuples = [list(t) for t in tuples]


class _Inverse(_GlobalConstraint):
    def __init__(
        self, f: Sequence[IntVar], invf: Sequence[IntVar], offset: int = 0
    ) -> None:
        self.f = list(f)
        self.invf = list(invf)
        self.offset = offset


class _Cumulative(_GlobalConstraint):
    def __init__(
        self,
        starts: Sequence[IntVar],
        durations: Sequence[Union[IntVar, int]],
        demands: Sequence[Union[IntVar, int]],
        capacity: Union[IntVar, int],
    ) -> None:
        self.starts = list(starts)
        self.durations = list(durations)
        self.demands = list(demands)
        self.capacity = capacity


class _Disjunctive(_GlobalConstraint):
    def __init__(
        self,
        starts: Sequence[IntVar],
        durations: Sequence[Union[IntVar, int]],
    ) -> None:
        self.starts = list(starts)
        self.durations = list(durations)


class _Diffn(_GlobalConstraint):
    def __init__(
        self,
        x: Sequence[IntVar],
        y: Sequence[IntVar],
        dx: Sequence[Union[IntVar, int]],
        dy: Sequence[Union[IntVar, int]],
    ) -> None:
        self.x = list(x)
        self.y = list(y)
        self.dx = list(dx)
        self.dy = list(dy)


class _Regular(_GlobalConstraint):
    def __init__(
        self,
        vars: Sequence[IntVar],
        num_states: int,
        num_symbols: int,
        transitions: Sequence[int],
        initial_state: int,
        accepting_states: Sequence[int],
    ) -> None:
        self.vars = list(vars)
        self.num_states = num_states
        self.num_symbols = num_symbols
        self.transitions = list(transitions)
        self.initial_state = initial_state
        self.accepting_states = list(accepting_states)


class _Subcircuit(_GlobalConstraint):
    def __init__(self, vars: Sequence[IntVar]) -> None:
        self.vars = list(vars)


class _Increasing(_GlobalConstraint):
    def __init__(self, vars: Sequence[IntVar], strict: bool) -> None:
        self.vars = list(vars)
        self.strict = strict


class _LexLessEq(_GlobalConstraint):
    def __init__(
        self, xs: Sequence[IntVar], ys: Sequence[IntVar], strict: bool
    ) -> None:
        self.xs = list(xs)
        self.ys = list(ys)
        self.strict = strict


class _ValuePrecede(_GlobalConstraint):
    def __init__(self, s: int, t: int, vars: Sequence[IntVar]) -> None:
        self.s = s
        self.t = t
        self.vars = list(vars)


class _GlobalCardinality(_GlobalConstraint):
    def __init__(
        self,
        vars: Sequence[IntVar],
        cover: Sequence[int],
        counts: Sequence[IntVar],
    ) -> None:
        self.vars = list(vars)
        self.cover = list(cover)
        self.counts = list(counts)


class _BinPackingLoad(_GlobalConstraint):
    def __init__(
        self,
        loads: Sequence[IntVar],
        bins: Sequence[IntVar],
        weights: Sequence[int],
    ) -> None:
        self.loads = list(loads)
        self.bins = list(bins)
        self.weights = list(weights)


# --- Expression-like globals (support comparison operators) ---


class _AggregateExpr(_NonLinearExpr):
    """Base for expression-like globals that return a value (max, min, count, nvalue)."""

    pass


class _MaxExpr(_AggregateExpr):
    def __init__(self, vars: Sequence[IntVar]) -> None:
        self.vars = list(vars)


class _MinExpr(_AggregateExpr):
    def __init__(self, vars: Sequence[IntVar]) -> None:
        self.vars = list(vars)


class _CountExpr(_AggregateExpr):
    def __init__(
        self, vars: Sequence[IntVar], value: Union[int, IntVar]
    ) -> None:
        self.vars = list(vars)
        self.value = value


class _NValueExpr(_AggregateExpr):
    def __init__(self, vars: Sequence[IntVar]) -> None:
        self.vars = list(vars)


# --- Public API functions ---


def all_different(vars: Sequence[IntVar]) -> _AllDifferent:
    """All variables must take distinct values."""
    return _AllDifferent(vars)


def all_different_except_0(vars: Sequence[IntVar]) -> _AllDifferentExcept0:
    """All non-zero variables must take distinct values."""
    return _AllDifferentExcept0(vars)


def circuit(vars: Sequence[IntVar]) -> _Circuit:
    """Variables form a Hamiltonian circuit (successor representation)."""
    return _Circuit(vars)


def element(
    index: IntVar,
    array: Sequence[Union[int, IntVar]],
    result: IntVar,
) -> _Element:
    """result = array[index]."""
    return _Element(index, array, result)


def table(
    vars: Sequence[IntVar], tuples: Sequence[Sequence[int]]
) -> _Table:
    """Variables must match one of the given tuples."""
    return _Table(vars, tuples)


def inverse(
    f: Sequence[IntVar], invf: Sequence[IntVar], offset: int = 0
) -> _Inverse:
    """f and invf are inverse permutations (0-based by default).

    With offset=0: f[i] = j ⟺ invf[j] = i, values in {0, ..., n-1}.
    With offset=1: f[i] = j ⟺ invf[j-1] = i+1, values in {1, ..., n} (FlatZinc convention).
    """
    return _Inverse(f, invf, offset)


def cumulative(
    starts: Sequence[IntVar],
    durations: Sequence[Union[IntVar, int]],
    demands: Sequence[Union[IntVar, int]],
    capacity: Union[IntVar, int],
) -> _Cumulative:
    """Cumulative resource constraint."""
    return _Cumulative(starts, durations, demands, capacity)


def disjunctive(
    starts: Sequence[IntVar],
    durations: Sequence[Union[IntVar, int]],
) -> _Disjunctive:
    """Tasks must not overlap (no-overlap constraint)."""
    return _Disjunctive(starts, durations)


def diffn(
    x: Sequence[IntVar],
    y: Sequence[IntVar],
    dx: Sequence[Union[IntVar, int]],
    dy: Sequence[Union[IntVar, int]],
) -> _Diffn:
    """Non-overlapping rectangles in 2D."""
    return _Diffn(x, y, dx, dy)


def regular(
    vars: Sequence[IntVar],
    num_states: int,
    num_symbols: int,
    transitions: Sequence[int],
    initial_state: int,
    accepting_states: Sequence[int],
) -> _Regular:
    """DFA-based regular constraint."""
    return _Regular(
        vars, num_states, num_symbols, transitions,
        initial_state, accepting_states,
    )


def subcircuit(vars: Sequence[IntVar]) -> _Subcircuit:
    """Variables form at most one circuit (successor representation, 0-based).

    x[i] = i means node i is not in the circuit. Nodes in the circuit
    form a single cycle of length >= 2 (or all nodes are self-loops).
    """
    return _Subcircuit(vars)


def increasing(vars: Sequence[IntVar], strict: bool = False) -> _Increasing:
    """Variables are in non-decreasing order (strictly increasing if strict=True)."""
    return _Increasing(vars, strict)


def lex_less(xs: Sequence[IntVar], ys: Sequence[IntVar]) -> _LexLessEq:
    """xs is lexicographically strictly less than ys."""
    return _LexLessEq(xs, ys, strict=True)


def lex_lesseq(xs: Sequence[IntVar], ys: Sequence[IntVar]) -> _LexLessEq:
    """xs is lexicographically less than or equal to ys."""
    return _LexLessEq(xs, ys, strict=False)


def value_precede(s: int, t: int, vars: Sequence[IntVar]) -> _ValuePrecede:
    """If value t appears, value s must appear at an earlier index.

    Standard symmetry-breaking constraint: x[j] == t implies
    exists i < j with x[i] == s.
    """
    return _ValuePrecede(s, t, vars)


def global_cardinality(
    vars: Sequence[IntVar],
    cover: Sequence[int],
    counts: Sequence[IntVar],
) -> _GlobalCardinality:
    """counts[k] = number of vars taking the value cover[k].

    Values outside cover are unconstrained (non-closed semantics).
    """
    return _GlobalCardinality(vars, cover, counts)


def bin_packing_load(
    loads: Sequence[IntVar],
    bins: Sequence[IntVar],
    weights: Sequence[int],
) -> _BinPackingLoad:
    """loads[b] = sum of weights[i] over items i with bins[i] == b (0-based)."""
    return _BinPackingLoad(loads, bins, weights)


def maximum(vars: Sequence[IntVar]) -> _MaxExpr:
    """Expression representing the maximum of variables.

    Usage: model.add(maximum([x, y, z]) == m)
    """
    return _MaxExpr(vars)


def minimum(vars: Sequence[IntVar]) -> _MinExpr:
    """Expression representing the minimum of variables.

    Usage: model.add(minimum([x, y, z]) == m)
    """
    return _MinExpr(vars)


def count(vars: Sequence[IntVar], value: Union[int, IntVar]) -> _CountExpr:
    """Expression representing the count of a value in variables.

    value may be a constant or a variable.

    Usage: model.add(count([x, y, z], 1) == n)
    """
    return _CountExpr(vars, value)


def nvalue(vars: Sequence[IntVar]) -> _NValueExpr:
    """Expression representing the number of distinct values.

    Usage: model.add(nvalue([x, y, z]) == n)
    """
    return _NValueExpr(vars)
