"""Expression system for building constraints with operator overloading."""
from __future__ import annotations

from enum import Enum, auto
from typing import TYPE_CHECKING, Union

if TYPE_CHECKING:
    from sabori_csp.core import Variable


class ComparisonOp(Enum):
    EQ = auto()
    NE = auto()
    LE = auto()
    LT = auto()
    GE = auto()
    GT = auto()


LinearArg = Union["IntVar", "LinearExpr", int]


class LinearExpr:
    """A linear combination of IntVars: sum(coeff_i * var_i) + constant."""

    __slots__ = ("_terms", "_const")

    def __init__(
        self, terms: dict[IntVar, int] | None = None, const: int = 0
    ) -> None:
        self._terms: dict[IntVar, int] = terms if terms is not None else {}
        self._const: int = const

    @staticmethod
    def term(var: IntVar, coeff: int = 1) -> LinearExpr:
        return LinearExpr({var: coeff})

    @staticmethod
    def constant(val: int) -> LinearExpr:
        return LinearExpr(const=val)

    def _merge(self, other: LinearExpr, sign: int = 1) -> LinearExpr:
        terms = dict(self._terms)
        for var, coeff in other._terms.items():
            c = terms.get(var, 0) + sign * coeff
            if c == 0:
                terms.pop(var, None)
            else:
                terms[var] = c
        return LinearExpr(terms, self._const + sign * other._const)

    @staticmethod
    def _coerce(other: LinearArg) -> LinearExpr:
        if isinstance(other, LinearExpr):
            return other
        if isinstance(other, IntVar):
            return LinearExpr.term(other)
        if isinstance(other, int):
            return LinearExpr.constant(other)
        return NotImplemented

    # --- Arithmetic ---

    def __add__(self, other: LinearArg) -> LinearExpr:
        o = self._coerce(other)
        if o is NotImplemented:
            return NotImplemented
        return self._merge(o)

    def __radd__(self, other: LinearArg) -> LinearExpr:
        o = self._coerce(other)
        if o is NotImplemented:
            return NotImplemented
        return o._merge(self)

    def __sub__(self, other: LinearArg) -> LinearExpr:
        o = self._coerce(other)
        if o is NotImplemented:
            return NotImplemented
        return self._merge(o, sign=-1)

    def __rsub__(self, other: LinearArg) -> LinearExpr:
        o = self._coerce(other)
        if o is NotImplemented:
            return NotImplemented
        return o._merge(self, sign=-1)

    def __mul__(self, other: int) -> LinearExpr:
        if not isinstance(other, int):
            return NotImplemented
        if other == 0:
            return LinearExpr()
        terms = {var: coeff * other for var, coeff in self._terms.items()}
        return LinearExpr(terms, self._const * other)

    def __rmul__(self, other: int) -> LinearExpr:
        return self.__mul__(other)

    def __neg__(self) -> LinearExpr:
        return self * -1

    # --- Comparisons → BoundedExpr ---

    def _compare(self, other: LinearArg, op: ComparisonOp) -> BoundedExpr:
        o = self._coerce(other)
        if o is NotImplemented:
            raise TypeError(f"Cannot compare LinearExpr with {type(other)}")
        # Normalize to (self - other) <op> 0
        expr = self._merge(o, sign=-1)
        return BoundedExpr(expr, op)

    def __eq__(self, other: LinearArg) -> BoundedExpr:  # type: ignore[override]
        return self._compare(other, ComparisonOp.EQ)

    def __ne__(self, other: LinearArg) -> BoundedExpr:  # type: ignore[override]
        return self._compare(other, ComparisonOp.NE)

    def __le__(self, other: LinearArg) -> BoundedExpr:
        return self._compare(other, ComparisonOp.LE)

    def __lt__(self, other: LinearArg) -> BoundedExpr:
        # x < y  ⟺  x - y + 1 <= 0  (integer)
        o = self._coerce(other)
        if o is NotImplemented:
            raise TypeError(f"Cannot compare LinearExpr with {type(other)}")
        expr = self._merge(o, sign=-1)
        expr = LinearExpr(dict(expr._terms), expr._const + 1)
        return BoundedExpr(expr, ComparisonOp.LE)

    def __ge__(self, other: LinearArg) -> BoundedExpr:
        # x >= y  ⟺  y - x <= 0
        o = self._coerce(other)
        if o is NotImplemented:
            raise TypeError(f"Cannot compare LinearExpr with {type(other)}")
        expr = o._merge(self, sign=-1)
        return BoundedExpr(expr, ComparisonOp.LE)

    def __gt__(self, other: LinearArg) -> BoundedExpr:
        # x > y  ⟺  y - x + 1 <= 0  (integer)
        o = self._coerce(other)
        if o is NotImplemented:
            raise TypeError(f"Cannot compare LinearExpr with {type(other)}")
        expr = o._merge(self, sign=-1)
        expr = LinearExpr(dict(expr._terms), expr._const + 1)
        return BoundedExpr(expr, ComparisonOp.LE)

    def __hash__(self) -> int:
        return id(self)

    def __bool__(self) -> bool:
        raise TypeError(
            "LinearExpr cannot be used as a boolean. Use model.add() to post constraints."
        )

    def __repr__(self) -> str:
        parts = []
        for var, coeff in self._terms.items():
            if coeff == 1:
                parts.append(var.name)
            elif coeff == -1:
                parts.append(f"-{var.name}")
            else:
                parts.append(f"{coeff}*{var.name}")
        if self._const != 0 or not parts:
            parts.append(str(self._const))
        return " + ".join(parts).replace("+ -", "- ")


class IntVar:
    """Wraps a C++ Variable. Created only by CpModel."""

    __slots__ = ("_model", "_var")

    def __init__(self, model: object, var: Variable) -> None:
        self._model = model
        self._var = var

    @property
    def name(self) -> str:
        return self._var.name()

    @property
    def index(self) -> int:
        return self._var.id()

    def _as_linear(self) -> LinearExpr:
        return LinearExpr.term(self)

    # --- Arithmetic ---

    def __add__(self, other: LinearArg) -> LinearExpr:
        return self._as_linear().__add__(other)

    def __radd__(self, other: LinearArg) -> LinearExpr:
        return self._as_linear().__radd__(other)

    def __sub__(self, other: LinearArg) -> LinearExpr:
        return self._as_linear().__sub__(other)

    def __rsub__(self, other: LinearArg) -> LinearExpr:
        return self._as_linear().__rsub__(other)

    def __mul__(self, other: LinearArg) -> LinearExpr | TimesExpr:
        if isinstance(other, int):
            return self._as_linear().__mul__(other)
        if isinstance(other, IntVar):
            return TimesExpr(self, other)
        return NotImplemented

    def __rmul__(self, other: LinearArg) -> LinearExpr | TimesExpr:
        if isinstance(other, int):
            return self._as_linear().__rmul__(other)
        if isinstance(other, IntVar):
            return TimesExpr(other, self)
        return NotImplemented

    def __neg__(self) -> LinearExpr:
        return self._as_linear().__neg__()

    def __abs__(self) -> AbsExpr:
        return AbsExpr(self)

    def __floordiv__(self, other: LinearArg) -> DivExpr:
        return DivExpr(self, other)

    def __mod__(self, other: LinearArg) -> ModExpr:
        return ModExpr(self, other)

    # --- Comparisons ---

    def __eq__(self, other: LinearArg) -> BoundedExpr:  # type: ignore[override]
        return self._as_linear().__eq__(other)

    def __ne__(self, other: LinearArg) -> BoundedExpr:  # type: ignore[override]
        return self._as_linear().__ne__(other)

    def __le__(self, other: LinearArg) -> BoundedExpr:
        return self._as_linear().__le__(other)

    def __lt__(self, other: LinearArg) -> BoundedExpr:
        return self._as_linear().__lt__(other)

    def __ge__(self, other: LinearArg) -> BoundedExpr:
        return self._as_linear().__ge__(other)

    def __gt__(self, other: LinearArg) -> BoundedExpr:
        return self._as_linear().__gt__(other)

    def __hash__(self) -> int:
        return id(self)

    def __bool__(self) -> bool:
        raise TypeError(
            "IntVar cannot be used as a boolean. Use model.add() to post constraints."
        )

    def __repr__(self) -> str:
        return f"IntVar({self.name})"


class BoundedExpr:
    """A comparison constraint: expr <op> 0.

    Created by comparison operators on IntVar/LinearExpr.
    Passed to CpModel.add() to post the constraint.
    """

    __slots__ = ("_expr", "_op")

    def __init__(self, expr: LinearExpr, op: ComparisonOp) -> None:
        self._expr = expr
        self._op = op

    def __bool__(self) -> bool:
        raise TypeError(
            "BoundedExpr cannot be used as a boolean. Use model.add() to post constraints."
        )

    def __repr__(self) -> str:
        op_str = {
            ComparisonOp.EQ: "==", ComparisonOp.NE: "!=", ComparisonOp.LE: "<=",
            ComparisonOp.LT: "<", ComparisonOp.GE: ">=", ComparisonOp.GT: ">",
        }
        return f"({self._expr} {op_str[self._op]} 0)"


# --- Non-linear expression nodes ---


class _NonLinearExpr:
    """Base for non-linear expressions."""

    def _compare(self, other: LinearArg, op: ComparisonOp) -> NonLinearBoundedExpr:
        return NonLinearBoundedExpr(self, op, other)

    def __eq__(self, other: LinearArg) -> NonLinearBoundedExpr:  # type: ignore[override]
        return self._compare(other, ComparisonOp.EQ)

    def __ne__(self, other: LinearArg) -> NonLinearBoundedExpr:  # type: ignore[override]
        return self._compare(other, ComparisonOp.NE)

    def __le__(self, other: LinearArg) -> NonLinearBoundedExpr:
        return self._compare(other, ComparisonOp.LE)

    def __lt__(self, other: LinearArg) -> NonLinearBoundedExpr:
        return self._compare(other, ComparisonOp.LT)

    def __ge__(self, other: LinearArg) -> NonLinearBoundedExpr:
        return self._compare(other, ComparisonOp.GE)

    def __gt__(self, other: LinearArg) -> NonLinearBoundedExpr:
        return self._compare(other, ComparisonOp.GT)

    def __hash__(self) -> int:
        return id(self)

    def __bool__(self) -> bool:
        raise TypeError(
            "Expression cannot be used as a boolean. Use model.add() to post constraints."
        )


class TimesExpr(_NonLinearExpr):
    """x * y where both are IntVar."""

    __slots__ = ("left", "right")

    def __init__(self, left: IntVar, right: IntVar) -> None:
        self.left = left
        self.right = right


class DivExpr(_NonLinearExpr):
    """x // y (truncated integer division)."""

    __slots__ = ("dividend", "divisor")

    def __init__(self, dividend: LinearArg, divisor: LinearArg) -> None:
        self.dividend = dividend
        self.divisor = divisor


class ModExpr(_NonLinearExpr):
    """x % y (integer modulo)."""

    __slots__ = ("dividend", "divisor")

    def __init__(self, dividend: LinearArg, divisor: LinearArg) -> None:
        self.dividend = dividend
        self.divisor = divisor


class AbsExpr(_NonLinearExpr):
    """abs(x)."""

    __slots__ = ("operand",)

    def __init__(self, operand: LinearArg) -> None:
        self.operand = operand


class NonLinearBoundedExpr:
    """A comparison involving a non-linear expression: nl_expr <op> rhs."""

    __slots__ = ("_nl_expr", "_op", "_rhs")

    def __init__(
        self, nl_expr: _NonLinearExpr, op: ComparisonOp, rhs: LinearArg
    ) -> None:
        self._nl_expr = nl_expr
        self._op = op
        self._rhs = rhs

    def __bool__(self) -> bool:
        raise TypeError(
            "Constraint expression cannot be used as a boolean. "
            "Use model.add() to post constraints."
        )
