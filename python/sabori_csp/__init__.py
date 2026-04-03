"""sabori_csp: Python interface for the Sabori CSP constraint solver.

High-level API example::

    from sabori_csp import CpModel, CpSolver, all_different

    m = CpModel()
    x = m.int_var(1, 10, "x")
    y = m.int_var(1, 10, "y")
    z = m.int_var(1, 10, "z")

    m.add(x + y == z)
    m.add(all_different([x, y, z]))
    m.minimize(x + y)

    solver = CpSolver()
    status = solver.solve(m)
    print(solver.value(x))

For low-level access to the C++ bindings, use ``sabori_csp.core``.
"""
from sabori_csp._expressions import IntVar, LinearExpr
from sabori_csp._globals import (
    all_different,
    all_different_except_0,
    circuit,
    count,
    cumulative,
    diffn,
    disjunctive,
    element,
    inverse,
    maximum,
    minimum,
    nvalue,
    regular,
    table,
)
from sabori_csp._model import CpModel
from sabori_csp._solver import CpSolver, SolveStatus

__all__ = [
    # Core classes
    "CpModel",
    "CpSolver",
    "SolveStatus",
    "IntVar",
    "LinearExpr",
    # Global constraints
    "all_different",
    "all_different_except_0",
    "circuit",
    "count",
    "cumulative",
    "diffn",
    "disjunctive",
    "element",
    "inverse",
    "maximum",
    "minimum",
    "nvalue",
    "regular",
    "table",
    # Helper module
    "claude_helper",
]
