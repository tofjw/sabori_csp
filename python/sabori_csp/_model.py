"""CpModel: high-level constraint model builder."""
from __future__ import annotations

from typing import Sequence, Union

from sabori_csp import core
from sabori_csp._expressions import (
    AbsExpr,
    BoundedExpr,
    ComparisonOp,
    DivExpr,
    IntVar,
    LinearArg,
    LinearExpr,
    ModExpr,
    NonLinearBoundedExpr,
    TimesExpr,
    _NonLinearExpr,
)
from sabori_csp._globals import (
    _AggregateExpr,
    _AllDifferent,
    _AllDifferentExcept0,
    _Circuit,
    _CountExpr,
    _Cumulative,
    _Diffn,
    _Disjunctive,
    _Element,
    _GlobalConstraint,
    _Inverse,
    _MaxExpr,
    _MinExpr,
    _NValueExpr,
    _Regular,
    _Table,
)


class CpModel:
    """High-level constraint model.

    Example::

        m = CpModel()
        x = m.int_var(1, 10, "x")
        y = m.int_var(1, 10, "y")
        m.add(x + y == 10)
        m.add(x != y)
    """

    def __init__(self) -> None:
        self._model = core.Model()
        self._vars: list[IntVar] = []
        self._aux_count: int = 0
        self._var_count: int = 0
        self._constant_cache: dict[int, IntVar] = {}
        self._objective: tuple[IntVar, bool] | None = None  # (var, minimize)

    @property
    def raw_model(self) -> core.Model:
        """Access the underlying C++ Model for advanced usage."""
        return self._model

    def int_var(self, lb: int, ub: int, name: str = "") -> IntVar:
        """Create an integer variable with domain [lb, ub]."""
        if not name:
            name = f"v{self._var_count}"
        self._var_count += 1
        raw = self._model.create_variable(name, lb, ub)
        v = IntVar(self, raw)
        self._vars.append(v)
        return v

    def int_var_from_domain(
        self, values: Sequence[int], name: str = ""
    ) -> IntVar:
        """Create an integer variable with an explicit set of values."""
        if not name:
            name = f"v{self._var_count}"
        self._var_count += 1
        raw = self._model.create_variable(name, list(values))
        v = IntVar(self, raw)
        self._vars.append(v)
        return v

    def bool_var(self, name: str = "") -> IntVar:
        """Create a Boolean variable (domain {0, 1})."""
        return self.int_var(0, 1, name)

    def constant(self, val: int) -> IntVar:
        """Get or create a constant variable with the given value."""
        if val in self._constant_cache:
            return self._constant_cache[val]
        v = self.int_var(val, val, f"__const_{val}")
        self._constant_cache[val] = v
        return v

    def add(
        self,
        ct: Union[BoundedExpr, NonLinearBoundedExpr, _GlobalConstraint],
    ) -> None:
        """Post a constraint to the model."""
        if isinstance(ct, BoundedExpr):
            self._post_bounded_expr(ct)
        elif isinstance(ct, NonLinearBoundedExpr):
            self._post_nonlinear_bounded(ct)
        elif isinstance(ct, _GlobalConstraint):
            self._post_global(ct)
        else:
            raise TypeError(
                f"Cannot add {type(ct).__name__} as a constraint. "
                "Use comparison operators (==, !=, <=, etc.) or global constraint functions."
            )

    def minimize(self, obj: LinearArg) -> None:
        """Set the objective to minimize."""
        var = self._realize(obj)
        self._objective = (var, True)

    def maximize(self, obj: LinearArg) -> None:
        """Set the objective to maximize."""
        var = self._realize(obj)
        self._objective = (var, False)

    # --- Internal helpers ---

    def _new_aux_var(self, lb: int, ub: int) -> IntVar:
        name = f"__aux_{self._aux_count}"
        self._aux_count += 1
        raw = self._model.create_variable(name, lb, ub)
        v = IntVar(self, raw)
        self._vars.append(v)
        return v

    def _ensure_var(self, arg: Union[IntVar, int]) -> IntVar:
        """Ensure arg is an IntVar; wrap int as a constant."""
        if isinstance(arg, IntVar):
            return arg
        if isinstance(arg, int):
            return self.constant(arg)
        raise TypeError(f"Expected IntVar or int, got {type(arg)}")

    def _realize(self, arg: LinearArg) -> IntVar:
        """Convert a LinearArg to a single IntVar.

        If arg is already a single IntVar (possibly with coeff 1 and no constant),
        return it directly. Otherwise, create an aux var constrained to equal the expression.
        """
        if isinstance(arg, IntVar):
            return arg
        if isinstance(arg, int):
            return self.constant(arg)
        if isinstance(arg, LinearExpr):
            # Check if it's just a single variable
            if len(arg._terms) == 1 and arg._const == 0:
                var, coeff = next(iter(arg._terms.items()))
                if coeff == 1:
                    return var
            # Create aux var: aux == expr
            lb, ub = self._estimate_bounds(arg)
            aux = self._new_aux_var(lb, ub)
            # Post: expr - aux == 0
            eq_expr = LinearExpr(dict(arg._terms), arg._const)
            eq_expr._terms[aux] = eq_expr._terms.get(aux, 0) - 1
            if eq_expr._terms.get(aux) == 0:
                del eq_expr._terms[aux]
            self._post_bounded_expr(BoundedExpr(eq_expr, ComparisonOp.EQ))
            return aux
        raise TypeError(f"Cannot realize {type(arg)} as IntVar")

    def _estimate_bounds(self, expr: LinearExpr) -> tuple[int, int]:
        """Estimate bounds for a linear expression based on variable domains."""
        lb = expr._const
        ub = expr._const
        for var, coeff in expr._terms.items():
            vmin = var._var.min()
            vmax = var._var.max()
            if coeff > 0:
                lb += coeff * vmin
                ub += coeff * vmax
            else:
                lb += coeff * vmax
                ub += coeff * vmin
        return lb, ub

    # --- Constraint posting ---

    def _post_bounded_expr(self, ct: BoundedExpr) -> None:
        expr = ct._expr
        op = ct._op

        # Normalize GE/GT/LT to EQ/NE/LE (integer domain)
        if op == ComparisonOp.GE:
            # expr >= 0  ⟺  -expr <= 0
            expr = -expr
            op = ComparisonOp.LE
        elif op == ComparisonOp.GT:
            # expr > 0  ⟺  -expr + 1 <= 0
            expr = LinearExpr(
                {v: -c for v, c in expr._terms.items()},
                -expr._const + 1,
            )
            op = ComparisonOp.LE
        elif op == ComparisonOp.LT:
            # expr < 0  ⟺  expr + 1 <= 0
            expr = LinearExpr(dict(expr._terms), expr._const + 1)
            op = ComparisonOp.LE

        terms = expr._terms
        const = expr._const

        # Extract sorted terms for determinism
        sorted_items = sorted(terms.items(), key=lambda kv: kv[0].index)

        if not sorted_items:
            # Pure constant constraint
            if op == ComparisonOp.EQ and const != 0:
                raise ValueError(f"Unsatisfiable: {const} == 0")
            if op == ComparisonOp.NE and const == 0:
                raise ValueError(f"Unsatisfiable: 0 != 0")
            if op == ComparisonOp.LE and const > 0:
                raise ValueError(f"Unsatisfiable: {const} <= 0")
            return  # Trivially satisfied

        # Try to use binary comparison constraints for 2-var cases
        if len(sorted_items) == 2 and const == 0:
            (v1, c1), (v2, c2) = sorted_items
            if c1 == 1 and c2 == -1:
                self._post_binary(v1, v2, op)
                return
            if c1 == -1 and c2 == 1:
                self._post_binary(v2, v1, op)
                return

        # Single var == constant: also use binary if possible
        if len(sorted_items) == 1:
            v, c = sorted_items[0]
            if c == 1:
                # v + const <op> 0  ⟹  v <op> -const
                rhs = -const
                k = self.constant(rhs)
                self._post_binary(v, k, op)
                return
            if c == -1:
                # -v + const <op> 0  ⟹  const <op> v  ⟹  depends on op
                rhs = const
                k = self.constant(rhs)
                if op == ComparisonOp.EQ:
                    self._post_binary(v, k, ComparisonOp.EQ)
                    return
                elif op == ComparisonOp.NE:
                    self._post_binary(v, k, ComparisonOp.NE)
                    return
                elif op == ComparisonOp.LE:
                    # -v + const <= 0  ⟹  const <= v  ⟹  v >= const
                    self._post_binary(k, v, ComparisonOp.LE)
                    return

        # General linear constraint
        coeffs = [c for _, c in sorted_items]
        raw_vars = [v._var for v, _ in sorted_items]
        target = -const

        if op == ComparisonOp.EQ:
            self._model.add_constraint(
                core.IntLinEqConstraint(coeffs, raw_vars, target)
            )
        elif op == ComparisonOp.NE:
            self._model.add_constraint(
                core.IntLinNeConstraint(coeffs, raw_vars, target)
            )
        elif op == ComparisonOp.LE:
            self._model.add_constraint(
                core.IntLinLeConstraint(coeffs, raw_vars, target)
            )

    def _post_binary(self, lhs: IntVar, rhs: IntVar, op: ComparisonOp) -> None:
        """Post a binary comparison constraint: lhs <op> rhs."""
        lv = lhs._var
        rv = rhs._var
        if op == ComparisonOp.EQ:
            self._model.add_constraint(core.IntEqConstraint(lv, rv))
        elif op == ComparisonOp.NE:
            self._model.add_constraint(core.IntNeConstraint(lv, rv))
        elif op == ComparisonOp.LE:
            self._model.add_constraint(core.IntLeConstraint(lv, rv))

    def _post_nonlinear_bounded(self, ct: NonLinearBoundedExpr) -> None:
        nl = ct._nl_expr
        op = ct._op
        rhs = ct._rhs

        if isinstance(nl, _AggregateExpr):
            self._post_aggregate_bounded(nl, op, rhs)
            return

        # Realize rhs as a single IntVar
        rhs_var = self._realize(rhs)

        if isinstance(nl, TimesExpr):
            if op == ComparisonOp.EQ:
                self._model.add_constraint(
                    core.IntTimesConstraint(nl.left._var, nl.right._var, rhs_var._var)
                )
            else:
                # Create aux: aux = left * right, then aux <op> rhs
                aux = self._new_aux_var(*self._estimate_times_bounds(nl))
                self._model.add_constraint(
                    core.IntTimesConstraint(nl.left._var, nl.right._var, aux._var)
                )
                self._post_bounded_expr(
                    BoundedExpr(
                        LinearExpr({aux: 1, rhs_var: -1}), op
                    )
                )
        elif isinstance(nl, AbsExpr):
            operand = self._realize(nl.operand)
            if op == ComparisonOp.EQ:
                self._model.add_constraint(
                    core.IntAbsConstraint(operand._var, rhs_var._var)
                )
            else:
                lb = operand._var.min()
                ub = operand._var.max()
                abs_ub = max(abs(lb), abs(ub))
                aux = self._new_aux_var(0, abs_ub)
                self._model.add_constraint(
                    core.IntAbsConstraint(operand._var, aux._var)
                )
                self._post_bounded_expr(
                    BoundedExpr(LinearExpr({aux: 1, rhs_var: -1}), op)
                )
        elif isinstance(nl, DivExpr):
            dividend = self._realize(nl.dividend)
            divisor = self._realize(nl.divisor)
            if op == ComparisonOp.EQ:
                self._model.add_constraint(
                    core.IntDivConstraint(dividend._var, divisor._var, rhs_var._var)
                )
            else:
                aux = self._new_aux_var(
                    min(dividend._var.min(), -dividend._var.max()),
                    max(dividend._var.max(), -dividend._var.min()),
                )
                self._model.add_constraint(
                    core.IntDivConstraint(dividend._var, divisor._var, aux._var)
                )
                self._post_bounded_expr(
                    BoundedExpr(LinearExpr({aux: 1, rhs_var: -1}), op)
                )
        elif isinstance(nl, ModExpr):
            dividend = self._realize(nl.dividend)
            divisor = self._realize(nl.divisor)
            if op == ComparisonOp.EQ:
                self._model.add_constraint(
                    core.IntModConstraint(dividend._var, divisor._var, rhs_var._var)
                )
            else:
                dmax = max(abs(divisor._var.min()), abs(divisor._var.max()))
                aux = self._new_aux_var(-(dmax - 1), dmax - 1)
                self._model.add_constraint(
                    core.IntModConstraint(dividend._var, divisor._var, aux._var)
                )
                self._post_bounded_expr(
                    BoundedExpr(LinearExpr({aux: 1, rhs_var: -1}), op)
                )
        else:
            raise TypeError(f"Unsupported non-linear expression: {type(nl)}")

    def _post_aggregate_bounded(
        self, agg: _AggregateExpr, op: ComparisonOp, rhs: LinearArg
    ) -> None:
        rhs_var = self._realize(rhs)
        raw_vars = [v._var for v in agg.vars]

        if isinstance(agg, _MaxExpr):
            if op == ComparisonOp.EQ:
                self._model.add_constraint(
                    core.ArrayIntMaximumConstraint(rhs_var._var, raw_vars)
                )
            else:
                all_mins = [v._var.min() for v in agg.vars]
                all_maxs = [v._var.max() for v in agg.vars]
                aux = self._new_aux_var(max(all_mins), max(all_maxs))
                self._model.add_constraint(
                    core.ArrayIntMaximumConstraint(aux._var, raw_vars)
                )
                self._post_bounded_expr(
                    BoundedExpr(LinearExpr({aux: 1, rhs_var: -1}), op)
                )
        elif isinstance(agg, _MinExpr):
            if op == ComparisonOp.EQ:
                self._model.add_constraint(
                    core.ArrayIntMinimumConstraint(rhs_var._var, raw_vars)
                )
            else:
                all_mins = [v._var.min() for v in agg.vars]
                all_maxs = [v._var.max() for v in agg.vars]
                aux = self._new_aux_var(min(all_mins), min(all_maxs))
                self._model.add_constraint(
                    core.ArrayIntMinimumConstraint(aux._var, raw_vars)
                )
                self._post_bounded_expr(
                    BoundedExpr(LinearExpr({aux: 1, rhs_var: -1}), op)
                )
        elif isinstance(agg, _CountExpr):
            if op == ComparisonOp.EQ:
                self._model.add_constraint(
                    core.CountEqConstraint(raw_vars, agg.value, rhs_var._var)
                )
            else:
                aux = self._new_aux_var(0, len(agg.vars))
                self._model.add_constraint(
                    core.CountEqConstraint(raw_vars, agg.value, aux._var)
                )
                self._post_bounded_expr(
                    BoundedExpr(LinearExpr({aux: 1, rhs_var: -1}), op)
                )
        elif isinstance(agg, _NValueExpr):
            if op == ComparisonOp.EQ:
                self._model.add_constraint(
                    core.NValueConstraint(rhs_var._var, raw_vars)
                )
            else:
                aux = self._new_aux_var(0, len(agg.vars))
                self._model.add_constraint(
                    core.NValueConstraint(aux._var, raw_vars)
                )
                self._post_bounded_expr(
                    BoundedExpr(LinearExpr({aux: 1, rhs_var: -1}), op)
                )
        else:
            raise TypeError(f"Unsupported aggregate expression: {type(agg)}")

    def _realize_list(self, args: list) -> list[IntVar]:
        """Realize a list of LinearArg to IntVar, creating aux vars as needed."""
        return [self._realize(a) for a in args]

    def _post_global(self, ct: _GlobalConstraint) -> None:
        if isinstance(ct, _AllDifferent):
            vs = self._realize_list(ct.vars)
            raw = [v._var for v in vs]
            self._model.add_constraint(core.AllDifferentConstraint(raw))
        elif isinstance(ct, _AllDifferentExcept0):
            vs = self._realize_list(ct.vars)
            raw = [v._var for v in vs]
            self._model.add_constraint(core.AllDifferentExcept0Constraint(raw))
        elif isinstance(ct, _Circuit):
            raw = [v._var for v in ct.vars]
            self._model.add_constraint(core.CircuitConstraint(raw))
        elif isinstance(ct, _Element):
            idx = ct.index._var
            res = ct.result._var
            if ct.array and all(isinstance(x, int) for x in ct.array):
                # Constant array
                arr = [int(x) for x in ct.array]
                self._model.add_constraint(
                    core.IntElementConstraint(idx, arr, res, True)
                )
            else:
                # Variable array
                arr = [self._ensure_var(x)._var for x in ct.array]
                self._model.add_constraint(
                    core.ArrayVarIntElementConstraint(idx, arr, res, True)
                )
        elif isinstance(ct, _Table):
            raw = [v._var for v in ct.vars]
            arity = len(ct.vars)
            flat = []
            for t in ct.tuples:
                if len(t) != arity:
                    raise ValueError(
                        f"Tuple length {len(t)} does not match variable count {arity}"
                    )
                flat.extend(t)
            self._model.add_constraint(core.TableConstraint(raw, flat))
        elif isinstance(ct, _Inverse):
            f_raw = [v._var for v in ct.f]
            invf_raw = [v._var for v in ct.invf]
            self._model.add_constraint(
                core.InverseConstraint(f_raw, invf_raw, ct.offset)
            )
        elif isinstance(ct, _Cumulative):
            starts = [v._var for v in ct.starts]
            durs = [self._ensure_var(d)._var for d in ct.durations]
            demands = [self._ensure_var(d)._var for d in ct.demands]
            cap = self._ensure_var(ct.capacity)._var
            self._model.add_constraint(
                core.CumulativeConstraint(starts, durs, demands, cap)
            )
        elif isinstance(ct, _Disjunctive):
            starts = [v._var for v in ct.starts]
            durs = [self._ensure_var(d)._var for d in ct.durations]
            self._model.add_constraint(core.DisjunctiveConstraint(starts, durs))
        elif isinstance(ct, _Diffn):
            x = [v._var for v in ct.x]
            y = [v._var for v in ct.y]
            dx = [self._ensure_var(d)._var for d in ct.dx]
            dy = [self._ensure_var(d)._var for d in ct.dy]
            self._model.add_constraint(core.DiffnConstraint(x, y, dx, dy))
        elif isinstance(ct, _Regular):
            raw = [v._var for v in ct.vars]
            self._model.add_constraint(
                core.RegularConstraint(
                    raw,
                    ct.num_states,
                    ct.num_symbols,
                    ct.transitions,
                    ct.initial_state,
                    ct.accepting_states,
                )
            )
        else:
            raise TypeError(f"Unsupported global constraint: {type(ct)}")

    def _estimate_times_bounds(self, expr: TimesExpr) -> tuple[int, int]:
        l_min = expr.left._var.min()
        l_max = expr.left._var.max()
        r_min = expr.right._var.min()
        r_max = expr.right._var.max()
        products = [l_min * r_min, l_min * r_max, l_max * r_min, l_max * r_max]
        return min(products), max(products)
