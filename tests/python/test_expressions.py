"""Tests for the expression system (_expressions.py)."""
import pytest
import sys
import os

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "../../python"))

from sabori_csp import CpModel
from sabori_csp._expressions import (
    BoundedExpr,
    ComparisonOp,
    IntVar,
    LinearExpr,
    NonLinearBoundedExpr,
    TimesExpr,
    AbsExpr,
    DivExpr,
    ModExpr,
)


@pytest.fixture
def model():
    m = CpModel()
    return m


@pytest.fixture
def xyz(model):
    x = model.int_var(1, 10, "x")
    y = model.int_var(1, 10, "y")
    z = model.int_var(1, 10, "z")
    return x, y, z


# --- LinearExpr construction ---


class TestLinearExpr:
    def test_var_plus_var(self, xyz):
        x, y, z = xyz
        expr = x + y
        assert isinstance(expr, LinearExpr)
        assert expr._terms[x] == 1
        assert expr._terms[y] == 1
        assert expr._const == 0

    def test_var_plus_int(self, xyz):
        x, y, z = xyz
        expr = x + 5
        assert expr._terms[x] == 1
        assert expr._const == 5

    def test_int_plus_var(self, xyz):
        x, y, z = xyz
        expr = 5 + x
        assert expr._terms[x] == 1
        assert expr._const == 5

    def test_var_minus_var(self, xyz):
        x, y, z = xyz
        expr = x - y
        assert expr._terms[x] == 1
        assert expr._terms[y] == -1

    def test_var_minus_int(self, xyz):
        x, y, z = xyz
        expr = x - 3
        assert expr._terms[x] == 1
        assert expr._const == -3

    def test_int_minus_var(self, xyz):
        x, y, z = xyz
        expr = 3 - x
        assert expr._terms[x] == -1
        assert expr._const == 3

    def test_scalar_mul(self, xyz):
        x, y, z = xyz
        expr = 2 * x
        assert expr._terms[x] == 2
        expr2 = x * 3
        assert expr2._terms[x] == 3

    def test_neg(self, xyz):
        x, y, z = xyz
        expr = -x
        assert expr._terms[x] == -1

    def test_chain(self, xyz):
        x, y, z = xyz
        expr = 2 * x + 3 * y - z + 5
        assert expr._terms[x] == 2
        assert expr._terms[y] == 3
        assert expr._terms[z] == -1
        assert expr._const == 5

    def test_cancel(self, xyz):
        x, y, z = xyz
        expr = x - x
        assert x not in expr._terms
        assert expr._const == 0

    def test_sum_builtin(self, xyz):
        x, y, z = xyz
        expr = sum([x, y, z])
        assert isinstance(expr, LinearExpr)
        assert expr._terms[x] == 1
        assert expr._terms[y] == 1
        assert expr._terms[z] == 1

    def test_mul_zero(self, xyz):
        x, y, z = xyz
        expr = 0 * x
        assert len(expr._terms) == 0
        assert expr._const == 0


# --- Comparison operators ---


class TestComparisons:
    def test_eq(self, xyz):
        x, y, z = xyz
        ct = x == y
        assert isinstance(ct, BoundedExpr)
        assert ct._op == ComparisonOp.EQ
        # x - y == 0
        assert ct._expr._terms[x] == 1
        assert ct._expr._terms[y] == -1

    def test_ne(self, xyz):
        x, y, z = xyz
        ct = x != y
        assert ct._op == ComparisonOp.NE

    def test_le(self, xyz):
        x, y, z = xyz
        ct = x <= y
        assert ct._op == ComparisonOp.LE
        # x - y <= 0
        assert ct._expr._terms[x] == 1
        assert ct._expr._terms[y] == -1

    def test_lt(self, xyz):
        x, y, z = xyz
        ct = x < y
        assert ct._op == ComparisonOp.LE
        # x - y + 1 <= 0
        assert ct._expr._const == 1

    def test_ge(self, xyz):
        x, y, z = xyz
        ct = x >= y
        assert ct._op == ComparisonOp.LE
        # y - x <= 0
        assert ct._expr._terms[y] == 1
        assert ct._expr._terms[x] == -1

    def test_gt(self, xyz):
        x, y, z = xyz
        ct = x > y
        assert ct._op == ComparisonOp.LE
        # y - x + 1 <= 0
        assert ct._expr._terms[y] == 1
        assert ct._expr._terms[x] == -1
        assert ct._expr._const == 1

    def test_eq_const(self, xyz):
        x, y, z = xyz
        ct = x == 5
        assert ct._op == ComparisonOp.EQ
        assert ct._expr._terms[x] == 1
        assert ct._expr._const == -5

    def test_linear_eq(self, xyz):
        x, y, z = xyz
        ct = 2 * x + 3 * y <= 10
        assert ct._op == ComparisonOp.LE
        assert ct._expr._terms[x] == 2
        assert ct._expr._terms[y] == 3
        assert ct._expr._const == -10


# --- Non-linear expressions ---


class TestNonLinear:
    def test_var_times_var(self, xyz):
        x, y, z = xyz
        expr = x * y
        assert isinstance(expr, TimesExpr)
        assert expr.left is x
        assert expr.right is y

    def test_abs(self, xyz):
        x, y, z = xyz
        expr = abs(x)
        assert isinstance(expr, AbsExpr)

    def test_floordiv(self, xyz):
        x, y, z = xyz
        expr = x // y
        assert isinstance(expr, DivExpr)

    def test_mod(self, xyz):
        x, y, z = xyz
        expr = x % y
        assert isinstance(expr, ModExpr)

    def test_times_eq(self, xyz):
        x, y, z = xyz
        ct = x * y == z
        assert isinstance(ct, NonLinearBoundedExpr)
        assert ct._op == ComparisonOp.EQ


# --- Hash and bool ---


class TestHashBool:
    def test_intvar_hashable(self, xyz):
        x, y, z = xyz
        s = {x, y, z}
        assert len(s) == 3
        d = {x: 1, y: 2}
        assert d[x] == 1

    def test_intvar_not_bool(self, xyz):
        x, y, z = xyz
        with pytest.raises(TypeError):
            bool(x)

    def test_bounded_not_bool(self, xyz):
        x, y, z = xyz
        with pytest.raises(TypeError):
            bool(x == y)

    def test_linear_not_bool(self, xyz):
        x, y, z = xyz
        with pytest.raises(TypeError):
            bool(x + y)
