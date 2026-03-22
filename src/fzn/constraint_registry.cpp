#include "constraint_registry.hpp"
#include "sabori_csp/constraints/arithmetic.hpp"
#include "sabori_csp/constraints/comparison.hpp"
#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/constraints/all_different_gac.hpp"
#include "sabori_csp/constraints/logical.hpp"
#include <algorithm>
#include <stdexcept>

namespace sabori_csp {
namespace fzn {

void ConstraintRegistry::register_constraint(const std::string& name, ConstraintFactory factory) {
    factories_[name] = std::move(factory);
}

bool ConstraintRegistry::has(const std::string& name) const {
    return factories_.count(name) > 0;
}

std::optional<ConstraintPtr> ConstraintRegistry::create(
    const std::string& name,
    const ConstraintDecl& decl,
    FznBuildContext& ctx) const
{
    auto it = factories_.find(name);
    if (it == factories_.end()) {
        throw std::runtime_error("Unsupported constraint: " + name);
    }
    return it->second(decl, ctx);
}

// ============================================================
// Helper: resolve variable array and get VariablePtr vector
// ============================================================
static std::vector<VariablePtr> resolve_vars(const ConstraintArg& arg, FznBuildContext& ctx) {
    const auto names = ctx.resolve_var_array(arg);
    std::vector<VariablePtr> vars;
    vars.reserve(names.size());
    for (const auto& name : names) {
        vars.push_back(ctx.get_var_by_name(name));
    }
    return vars;
}

// ============================================================
// Pattern A: Simple 2-arg constraints
// ============================================================
static std::optional<ConstraintPtr> make_int_eq(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 2) throw std::runtime_error("int_eq requires 2 arguments");
    auto x = ctx.get_var(decl.args[0]);
    auto y = ctx.get_var(decl.args[1]);
    return std::make_shared<IntEqConstraint>(x, y);
}

static std::optional<ConstraintPtr> make_int_ne(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 2) throw std::runtime_error("int_ne requires 2 arguments");
    auto x = ctx.get_var(decl.args[0]);
    auto y = ctx.get_var(decl.args[1]);
    return std::make_shared<IntNeConstraint>(x, y);
}

static std::optional<ConstraintPtr> make_int_lt(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 2) throw std::runtime_error("int_lt requires 2 arguments");
    auto x = ctx.get_var(decl.args[0]);
    auto y = ctx.get_var(decl.args[1]);
    return std::make_shared<IntLtConstraint>(x, y);
}

static std::optional<ConstraintPtr> make_int_le(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 2) throw std::runtime_error("int_le requires 2 arguments");
    auto x = ctx.get_var(decl.args[0]);
    auto y = ctx.get_var(decl.args[1]);
    return std::make_shared<IntLeConstraint>(x, y);
}

static std::optional<ConstraintPtr> make_int_eq_reif(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 3) throw std::runtime_error("int_eq_reif requires 3 arguments");
    auto x = ctx.get_var(decl.args[0]);
    auto y = ctx.get_var(decl.args[1]);
    auto b = ctx.get_var(decl.args[2]);
    if (!ctx.model->is_defined_var(x->id()) && !ctx.model->is_defined_var(y->id()) &&
        !ctx.model->is_defined_var(b->id())) {
        ctx.model->set_defined_var(b->id());
    }
    return std::make_shared<IntEqReifConstraint>(x, y, b);
}

static std::optional<ConstraintPtr> make_int_eq_imp(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 3) throw std::runtime_error("int_eq_imp requires 3 arguments");
    auto x = ctx.get_var(decl.args[0]);
    auto y = ctx.get_var(decl.args[1]);
    auto b = ctx.get_var(decl.args[2]);
    return std::make_shared<IntEqImpConstraint>(x, y, b);
}

static std::optional<ConstraintPtr> make_int_ne_reif(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 3) throw std::runtime_error("int_ne_reif requires 3 arguments");
    auto x = ctx.get_var(decl.args[0]);
    auto y = ctx.get_var(decl.args[1]);
    auto b = ctx.get_var(decl.args[2]);
    if (!ctx.model->is_defined_var(x->id()) && !ctx.model->is_defined_var(y->id()) &&
        !ctx.model->is_defined_var(b->id())) {
        ctx.model->set_defined_var(b->id());
    }
    return std::make_shared<IntNeReifConstraint>(x, y, b);
}

static std::optional<ConstraintPtr> make_int_le_reif(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 3) throw std::runtime_error("int_le_reif requires 3 arguments");
    auto x = ctx.get_var(decl.args[0]);
    auto y = ctx.get_var(decl.args[1]);
    auto b = ctx.get_var(decl.args[2]);
    if (!x->is_assigned() && !y->is_assigned() &&
        !ctx.model->is_defined_var(x->id()) && !ctx.model->is_defined_var(y->id()) &&
        !ctx.model->is_defined_var(b->id())) {
        ctx.model->set_defined_var(b->id());
    }
    return std::make_shared<IntLeReifConstraint>(x, y, b);
}

static std::optional<ConstraintPtr> make_bool_not(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 2) throw std::runtime_error("bool_not requires 2 arguments");
    auto a = ctx.get_var(decl.args[0]);
    auto b = ctx.get_var(decl.args[1]);
    if (!ctx.model->is_defined_var(a->id()) && !ctx.model->is_defined_var(b->id())) {
        ctx.model->set_defined_var(b->id());
    }
    return std::make_shared<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1}, std::vector<VariablePtr>{a, b}, int64_t{1});
}

static std::optional<ConstraintPtr> make_bool_xor(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 3) throw std::runtime_error("bool_xor requires 3 arguments");
    auto a = ctx.get_var(decl.args[0]);
    auto b = ctx.get_var(decl.args[1]);
    auto c = ctx.get_var(decl.args[2]);
    if (!ctx.model->is_defined_var(a->id()) && !ctx.model->is_defined_var(b->id()) &&
        !ctx.model->is_defined_var(c->id())) {
        ctx.model->set_defined_var(c->id());
    }
    return std::make_shared<BoolXorConstraint>(a, b, c);
}

static std::optional<ConstraintPtr> make_int_min(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 3) throw std::runtime_error("int_min requires 3 arguments (x, y, m)");
    auto x = ctx.get_var(decl.args[0]);
    auto y = ctx.get_var(decl.args[1]);
    auto m = ctx.get_var(decl.args[2]);
    if (!ctx.model->is_defined_var(m->id())) {
        ctx.model->set_defined_var(m->id());
    }
    return std::make_shared<IntMinConstraint>(x, y, m);
}

static std::optional<ConstraintPtr> make_int_max(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 3) throw std::runtime_error("int_max requires 3 arguments (x, y, m)");
    auto x = ctx.get_var(decl.args[0]);
    auto y = ctx.get_var(decl.args[1]);
    auto m = ctx.get_var(decl.args[2]);
    if (!ctx.model->is_defined_var(m->id())) {
        ctx.model->set_defined_var(m->id());
    }
    return std::make_shared<IntMaxConstraint>(x, y, m);
}

static std::optional<ConstraintPtr> make_int_times(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 3) throw std::runtime_error("int_times requires 3 arguments (x, y, z)");
    auto x = ctx.get_var(decl.args[0]);
    auto y = ctx.get_var(decl.args[1]);
    auto z = ctx.get_var(decl.args[2]);
    if (!ctx.model->is_defined_var(z->id())) {
        ctx.model->set_defined_var(z->id());
    }
    return std::make_shared<IntTimesConstraint>(x, y, z);
}

static std::optional<ConstraintPtr> make_int_div(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 3) throw std::runtime_error("int_div requires 3 arguments (x, y, z)");
    auto x = ctx.get_var(decl.args[0]);
    auto y = ctx.get_var(decl.args[1]);
    auto z = ctx.get_var(decl.args[2]);
    if (!ctx.model->is_defined_var(z->id())) {
        ctx.model->set_defined_var(z->id());
    }
    return std::make_shared<IntDivConstraint>(x, y, z);
}

static std::optional<ConstraintPtr> make_int_mod(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 3) throw std::runtime_error("int_mod requires 3 arguments (x, y, z)");
    auto x = ctx.get_var(decl.args[0]);
    auto y = ctx.get_var(decl.args[1]);
    auto z = ctx.get_var(decl.args[2]);
    if (!ctx.model->is_defined_var(z->id())) {
        ctx.model->set_defined_var(z->id());
    }
    return std::make_shared<IntModConstraint>(x, y, z);
}

static std::optional<ConstraintPtr> make_int_abs(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 2) throw std::runtime_error("int_abs requires 2 arguments (x, y)");
    auto x = ctx.get_var(decl.args[0]);
    auto y = ctx.get_var(decl.args[1]);
    if (!ctx.model->is_defined_var(y->id())) {
        ctx.model->set_defined_var(y->id());
    }
    return std::make_shared<IntAbsConstraint>(x, y);
}

// ============================================================
// Pattern B: Array constraints
// ============================================================
static std::optional<ConstraintPtr> make_all_different(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 1) throw std::runtime_error("all_different_int requires 1 argument (array)");
    auto vars = resolve_vars(decl.args[0], ctx);
    if (ctx.use_gac) {
        return std::make_shared<AllDifferentGACConstraint>(std::move(vars));
    }
    return std::make_shared<AllDifferentConstraint>(std::move(vars));
}

static std::optional<ConstraintPtr> make_alldifferent_except_0(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 1) throw std::runtime_error("alldifferent_except_0 requires 1 argument (array)");
    return std::make_shared<AllDifferentExcept0Constraint>(resolve_vars(decl.args[0], ctx));
}

static std::optional<ConstraintPtr> make_circuit(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 1) throw std::runtime_error("circuit requires 1 argument (array)");
    return std::make_shared<CircuitConstraint>(resolve_vars(decl.args[0], ctx));
}

static std::optional<ConstraintPtr> make_array_bool_and(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 2) throw std::runtime_error("array_bool_and requires 2 arguments");
    auto vars = resolve_vars(decl.args[0], ctx);
    auto r = ctx.get_var(decl.args[1]);
    if (!ctx.model->is_defined_var(r->id())) {
        ctx.model->set_defined_var(r->id());
    }
    return std::make_shared<ArrayBoolAndConstraint>(vars, r);
}

static std::optional<ConstraintPtr> make_array_bool_or(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 2) throw std::runtime_error("array_bool_or requires 2 arguments");
    auto vars = resolve_vars(decl.args[0], ctx);
    auto r = ctx.get_var(decl.args[1]);
    if (!ctx.model->is_defined_var(r->id())) {
        ctx.model->set_defined_var(r->id());
    }
    return std::make_shared<ArrayBoolOrConstraint>(vars, r);
}

static std::optional<ConstraintPtr> make_array_bool_xor(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 1) throw std::runtime_error("array_bool_xor requires 1 argument (array)");
    auto vars = resolve_vars(decl.args[0], ctx);
    return std::make_shared<ArrayBoolXorConstraint>(vars);
}

static std::optional<ConstraintPtr> make_bool_clause(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 2) throw std::runtime_error("bool_clause requires 2 arguments");
    auto pos_vars = resolve_vars(decl.args[0], ctx);
    auto neg_vars = resolve_vars(decl.args[1], ctx);
    return std::make_shared<BoolClauseConstraint>(pos_vars, neg_vars);
}

static std::optional<ConstraintPtr> make_array_int_maximum(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 2) throw std::runtime_error("array_int_maximum requires 2 arguments (max_var, array)");
    auto m = ctx.get_var(decl.args[0]);
    auto vars = resolve_vars(decl.args[1], ctx);
    return std::make_shared<ArrayIntMaximumConstraint>(m, vars);
}

static std::optional<ConstraintPtr> make_array_int_minimum(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 2) throw std::runtime_error("array_int_minimum requires 2 arguments (min_var, array)");
    auto m = ctx.get_var(decl.args[0]);
    auto vars = resolve_vars(decl.args[1], ctx);
    return std::make_shared<ArrayIntMinimumConstraint>(m, vars);
}

static std::optional<ConstraintPtr> make_table_int(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 2) throw std::runtime_error("table_int requires 2 arguments (vars, tuples)");
    auto vars = resolve_vars(decl.args[0], ctx);
    auto tuples = ctx.resolve_int_array(decl.args[1]);
    return std::make_shared<TableConstraint>(vars, tuples);
}

static std::optional<ConstraintPtr> make_diffn(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 4) throw std::runtime_error(decl.name + " requires 4 arguments (x, y, dx, dy)");
    auto x_vars  = resolve_vars(decl.args[0], ctx);
    auto y_vars  = resolve_vars(decl.args[1], ctx);
    auto dx_vars = resolve_vars(decl.args[2], ctx);
    auto dy_vars = resolve_vars(decl.args[3], ctx);
    bool strict = (decl.name == "fzn_diffn");
    return std::make_shared<DiffnConstraint>(
        std::move(x_vars), std::move(y_vars),
        std::move(dx_vars), std::move(dy_vars), strict);
}

static std::optional<ConstraintPtr> make_cumulative(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 4) throw std::runtime_error("fzn_cumulative requires 4 arguments (starts, durations, requirements, capacity)");
    auto starts = resolve_vars(decl.args[0], ctx);
    auto durations = resolve_vars(decl.args[1], ctx);
    auto requirements = resolve_vars(decl.args[2], ctx);
    auto capacity = ctx.get_var(decl.args[3]);
    return std::make_shared<CumulativeConstraint>(
        std::move(starts), std::move(durations),
        std::move(requirements), std::move(capacity));
}

static std::optional<ConstraintPtr> make_inverse(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 2) throw std::runtime_error("fzn_inverse requires 2 arguments (f, invf)");
    auto f = resolve_vars(decl.args[0], ctx);
    auto invf = resolve_vars(decl.args[1], ctx);
    return std::make_shared<InverseConstraint>(std::move(f), std::move(invf));
}

static std::optional<ConstraintPtr> make_all_equal(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 1) throw std::runtime_error("fzn_all_equal_int requires 1 argument (array)");
    auto vars = resolve_vars(decl.args[0], ctx);
    if (vars.size() <= 1) return std::nullopt;

    // is_defined_var heuristic: 1つだけ残して残りを is_defined_var にする
    // まず既に is_defined_var でない変数を探す
    size_t kept = vars.size(); // 残す変数のインデックス（未定）
    for (size_t i = 0; i < vars.size(); ++i) {
        if (!ctx.model->is_defined_var(vars[i]->id())) {
            if (kept == vars.size()) {
                kept = i;  // 最初の非defined変数を残す
            } else {
                ctx.model->set_defined_var(vars[i]->id());
            }
        }
    }

    // int_eq のチェインに分解: vars[0]=vars[1], vars[0]=vars[2], ...
    for (size_t i = 1; i < vars.size(); ++i) {
        ctx.model->add_constraint(std::make_shared<IntEqConstraint>(vars[0], vars[i]));
    }
    return std::nullopt;  // 全て add_constraint で追加済み
}

static std::optional<ConstraintPtr> make_disjunctive(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 2) throw std::runtime_error(decl.name + " requires 2 arguments (starts, durations)");
    auto starts = resolve_vars(decl.args[0], ctx);
    auto durations = resolve_vars(decl.args[1], ctx);
    bool strict = (decl.name == "fzn_disjunctive_strict");
    return std::make_shared<DisjunctiveConstraint>(
        std::move(starts), std::move(durations), strict);
}

// ============================================================
// Pattern C: Linear + substitution constraints
// ============================================================
static std::optional<ConstraintPtr> make_int_lin_eq(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 3) throw std::runtime_error("int_lin_eq requires 3 arguments (coeffs, vars, sum)");
    if (!std::holds_alternative<Domain::value_type>(decl.args[2]))
        throw std::runtime_error("int_lin_eq: third argument must be an integer");

    const auto coeffs_raw = ctx.resolve_int_array(decl.args[0]);
    auto var_names = ctx.resolve_var_array(decl.args[1]);
    auto sum = std::get<Domain::value_type>(decl.args[2]);
    std::vector<int64_t> coeffs(coeffs_raw.begin(), coeffs_raw.end());

    std::vector<VariablePtr> vars;
    for (const auto& name : var_names) vars.push_back(ctx.get_var_by_name(name));
    auto constraint = std::make_shared<IntLinEqConstraint>(coeffs, vars, sum);

    // defined_var heuristic
    bool any_defined = false;
    for (const auto& v : vars) {
        if (ctx.model->is_defined_var(v->id())) { any_defined = true; break; }
    }
    if (!any_defined && !vars.empty()) {
        size_t n = vars.size();
        size_t best = n;
        size_t best_size = 0;
        for (size_t i = 0; i < n; ++i) {
            if (std::abs(coeffs[i]) != 1) continue;
            if (ctx.model->is_defined_var(vars[i]->id())) continue;
            bool duplicate = false;
            for (size_t j = 0; j < n; ++j) {
                if (i != j && vars[i]->id() == vars[j]->id()) { duplicate = true; break; }
            }
            if (duplicate) continue;
            size_t ds = vars[i]->domain().size();
            if (ds > best_size) { best_size = ds; best = i; }
        }
        if (best < n) ctx.model->set_defined_var(vars[best]->id());
    }

    return constraint;
}

static std::optional<ConstraintPtr> make_int_lin_le(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 3) throw std::runtime_error("int_lin_le requires 3 arguments (coeffs, vars, bound)");
    if (!std::holds_alternative<Domain::value_type>(decl.args[2]))
        throw std::runtime_error("int_lin_le: third argument must be an integer");

    const auto coeffs_raw = ctx.resolve_int_array(decl.args[0]);
    auto var_names = ctx.resolve_var_array(decl.args[1]);
    auto bound = std::get<Domain::value_type>(decl.args[2]);
    std::vector<int64_t> coeffs(coeffs_raw.begin(), coeffs_raw.end());

    std::vector<VariablePtr> vars;
    for (const auto& name : var_names) vars.push_back(ctx.get_var_by_name(name));
    return std::make_shared<IntLinLeConstraint>(coeffs, vars, bound);
}

static std::optional<ConstraintPtr> make_int_lin_ne(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 3) throw std::runtime_error("int_lin_ne requires 3 arguments (coeffs, vars, target)");
    if (!std::holds_alternative<Domain::value_type>(decl.args[2]))
        throw std::runtime_error("int_lin_ne: third argument must be an integer");

    const auto coeffs_raw = ctx.resolve_int_array(decl.args[0]);
    auto var_names = ctx.resolve_var_array(decl.args[1]);
    auto target = std::get<Domain::value_type>(decl.args[2]);
    std::vector<int64_t> coeffs(coeffs_raw.begin(), coeffs_raw.end());

    std::vector<VariablePtr> vars;
    for (const auto& name : var_names) vars.push_back(ctx.get_var_by_name(name));
    return std::make_shared<IntLinNeConstraint>(coeffs, vars, target);
}

static std::optional<ConstraintPtr> make_int_lin_eq_reif(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 4) throw std::runtime_error("int_lin_eq_reif requires 4 arguments (coeffs, vars, target, b)");
    if (!std::holds_alternative<Domain::value_type>(decl.args[2]))
        throw std::runtime_error("int_lin_eq_reif: third argument must be an integer");

    const auto coeffs_raw = ctx.resolve_int_array(decl.args[0]);
    auto var_names = ctx.resolve_var_array(decl.args[1]);
    auto target = std::get<Domain::value_type>(decl.args[2]);
    auto b = ctx.get_var(decl.args[3]);
    std::vector<int64_t> coeffs(coeffs_raw.begin(), coeffs_raw.end());

    std::vector<VariablePtr> vars;
    for (const auto& name : var_names) vars.push_back(ctx.get_var_by_name(name));
    if (!ctx.model->is_defined_var(b->id())) {
        bool any_defined = false;
        for (const auto& v : vars) {
            if (ctx.model->is_defined_var(v->id())) { any_defined = true; break; }
        }
        if (!any_defined) ctx.model->set_defined_var(b->id());
    }
    return std::make_shared<IntLinEqReifConstraint>(coeffs, vars, target, b);
}

static std::optional<ConstraintPtr> make_int_lin_ne_reif(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 4) throw std::runtime_error("int_lin_ne_reif requires 4 arguments (coeffs, vars, target, b)");
    if (!std::holds_alternative<Domain::value_type>(decl.args[2]))
        throw std::runtime_error("int_lin_ne_reif: third argument must be an integer");

    const auto coeffs_raw = ctx.resolve_int_array(decl.args[0]);
    auto var_names = ctx.resolve_var_array(decl.args[1]);
    auto target = std::get<Domain::value_type>(decl.args[2]);
    auto b = ctx.get_var(decl.args[3]);
    std::vector<int64_t> coeffs(coeffs_raw.begin(), coeffs_raw.end());

    std::vector<VariablePtr> vars;
    for (const auto& name : var_names) vars.push_back(ctx.get_var_by_name(name));
    if (!ctx.model->is_defined_var(b->id())) {
        bool any_defined = false;
        for (const auto& v : vars) {
            if (ctx.model->is_defined_var(v->id())) { any_defined = true; break; }
        }
        if (!any_defined) ctx.model->set_defined_var(b->id());
    }
    return std::make_shared<IntLinNeReifConstraint>(coeffs, vars, target, b);
}

static std::optional<ConstraintPtr> make_int_lin_le_reif(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 4) throw std::runtime_error("int_lin_le_reif requires 4 arguments (coeffs, vars, bound, b)");
    if (!std::holds_alternative<Domain::value_type>(decl.args[2]))
        throw std::runtime_error("int_lin_le_reif: third argument must be an integer");

    const auto coeffs_raw = ctx.resolve_int_array(decl.args[0]);
    auto var_names = ctx.resolve_var_array(decl.args[1]);
    auto bound = std::get<Domain::value_type>(decl.args[2]);
    auto b = ctx.get_var(decl.args[3]);
    std::vector<int64_t> coeffs(coeffs_raw.begin(), coeffs_raw.end());

    std::vector<VariablePtr> vars;
    for (const auto& name : var_names) vars.push_back(ctx.get_var_by_name(name));
    if (!ctx.model->is_defined_var(b->id())) {
        bool any_defined = false;
        for (const auto& v : vars) {
            if (ctx.model->is_defined_var(v->id())) { any_defined = true; break; }
        }
        if (!any_defined) ctx.model->set_defined_var(b->id());
    }
    return std::make_shared<IntLinLeReifConstraint>(coeffs, vars, bound, b);
}

static std::optional<ConstraintPtr> make_int_lin_le_imp(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 4) throw std::runtime_error("int_lin_le_imp requires 4 arguments (coeffs, vars, bound, b)");
    if (!std::holds_alternative<Domain::value_type>(decl.args[2]))
        throw std::runtime_error("int_lin_le_imp: third argument must be an integer");

    const auto coeffs_raw = ctx.resolve_int_array(decl.args[0]);
    auto var_names = ctx.resolve_var_array(decl.args[1]);
    auto bound = std::get<Domain::value_type>(decl.args[2]);
    auto b = ctx.get_var(decl.args[3]);
    std::vector<int64_t> coeffs(coeffs_raw.begin(), coeffs_raw.end());

    std::vector<VariablePtr> vars;
    for (const auto& name : var_names) vars.push_back(ctx.get_var_by_name(name));
    return std::make_shared<IntLinLeImpConstraint>(coeffs, vars, bound, b);
}

static std::optional<ConstraintPtr> make_bool_lin_eq(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 3) throw std::runtime_error("bool_lin_eq requires 3 arguments");

    const auto coeffs_raw = ctx.resolve_int_array(decl.args[0]);
    auto var_names = ctx.resolve_var_array(decl.args[1]);
    std::vector<int64_t> coeffs(coeffs_raw.begin(), coeffs_raw.end());

    if (std::holds_alternative<Domain::value_type>(decl.args[2])) {
        auto sum = std::get<Domain::value_type>(decl.args[2]);
        std::vector<VariablePtr> vars;
        for (const auto& name : var_names) vars.push_back(ctx.get_var_by_name(name));
        return std::make_shared<IntLinEqConstraint>(coeffs, vars, sum);
    } else {
        auto rhs_var = ctx.get_var(decl.args[2]);
        std::vector<VariablePtr> vars;
        for (const auto& name : var_names) vars.push_back(ctx.get_var_by_name(name));
        coeffs.push_back(-1);
        vars.push_back(rhs_var);
        return std::make_shared<IntLinEqConstraint>(coeffs, vars, 0);
    }
}

static std::optional<ConstraintPtr> make_bool_lin_le(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 3) throw std::runtime_error("bool_lin_le requires 3 arguments");

    const auto coeffs_raw = ctx.resolve_int_array(decl.args[0]);
    auto var_names = ctx.resolve_var_array(decl.args[1]);
    std::vector<int64_t> coeffs(coeffs_raw.begin(), coeffs_raw.end());

    if (std::holds_alternative<Domain::value_type>(decl.args[2])) {
        auto bound = std::get<Domain::value_type>(decl.args[2]);
        std::vector<VariablePtr> vars;
        for (const auto& name : var_names) vars.push_back(ctx.get_var_by_name(name));
        return std::make_shared<IntLinLeConstraint>(coeffs, vars, bound);
    } else {
        auto rhs_var = ctx.get_var(decl.args[2]);
        std::vector<VariablePtr> vars;
        for (const auto& name : var_names) vars.push_back(ctx.get_var_by_name(name));
        coeffs.push_back(-1);
        vars.push_back(rhs_var);
        return std::make_shared<IntLinLeConstraint>(coeffs, vars, 0);
    }
}

// ============================================================
// Pattern D: Special constraints
// ============================================================
static std::optional<ConstraintPtr> make_bool2int(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 2) throw std::runtime_error("bool2int requires 2 arguments");
    if (std::holds_alternative<std::string>(decl.args[1]) &&
        ctx.alias_map.count(std::get<std::string>(decl.args[1]))) {
        return std::nullopt;
    }
    auto b = ctx.get_var(decl.args[0]);
    auto i = ctx.get_var(decl.args[1]);
    if (!ctx.model->is_defined_var(i->id())) {
        ctx.model->set_defined_var(i->id());
    }
    return std::make_shared<IntEqConstraint>(b, i);
}

static std::optional<ConstraintPtr> make_set_in(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 2) throw std::runtime_error("set_in requires 2 arguments");
    auto x = ctx.get_var(decl.args[0]);
    if (std::holds_alternative<IntRange>(decl.args[1])) {
        const auto& range = std::get<IntRange>(decl.args[1]);
        auto lb_var = ctx.model->create_variable("__set_in_lb_" + x->name(), range.lb);
        auto ub_var = ctx.model->create_variable("__set_in_ub_" + x->name(), range.ub);
        ctx.model->add_constraint(std::make_shared<IntLeConstraint>(lb_var, x));
        ctx.model->add_constraint(std::make_shared<IntLeConstraint>(x, ub_var));
        return std::nullopt;
    } else if (std::holds_alternative<std::vector<Domain::value_type>>(decl.args[1])) {
        const auto& values = std::get<std::vector<Domain::value_type>>(decl.args[1]);
        std::vector<Domain::value_type> domain_vals;
        x->domain().copy_values_to(domain_vals);
        for (auto v : domain_vals) {
            if (std::find(values.begin(), values.end(), v) == values.end()) {
                x->remove(v);
            }
        }
        return std::nullopt;
    }
    throw std::runtime_error("set_in requires range or set argument");
}

static std::optional<ConstraintPtr> make_set_in_reif(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 3) throw std::runtime_error("set_in_reif requires 3 arguments");
    auto x = ctx.get_var(decl.args[0]);
    auto b = ctx.get_var(decl.args[2]);
    if (!ctx.model->is_defined_var(x->id()) && !ctx.model->is_defined_var(b->id())) {
        ctx.model->set_defined_var(b->id());
    }

    if (std::holds_alternative<IntRange>(decl.args[1])) {
        const auto& range = std::get<IntRange>(decl.args[1]);
        static int set_in_reif_counter = 0;
        int id = set_in_reif_counter++;
        auto lb_var = ctx.model->create_variable("__sir_lb_" + std::to_string(id), range.lb);
        auto ub_var = ctx.model->create_variable("__sir_ub_" + std::to_string(id), range.ub);
        auto b1 = ctx.model->create_variable("__sir_b1_" + std::to_string(id), 0, 1);
        auto b2 = ctx.model->create_variable("__sir_b2_" + std::to_string(id), 0, 1);
        ctx.model->set_defined_var(b1->id());
        ctx.model->set_defined_var(b2->id());
        ctx.model->add_constraint(std::make_shared<IntLeReifConstraint>(lb_var, x, b1));
        ctx.model->add_constraint(std::make_shared<IntLeReifConstraint>(x, ub_var, b2));
        ctx.model->add_constraint(std::make_shared<ArrayBoolAndConstraint>(
            std::vector<VariablePtr>{b1, b2}, b));
        return std::nullopt;
    } else if (std::holds_alternative<std::vector<Domain::value_type>>(decl.args[1])) {
        const auto& values = std::get<std::vector<Domain::value_type>>(decl.args[1]);
        static int set_in_reif_set_counter = 0;
        int id = set_in_reif_set_counter++;
        std::vector<VariablePtr> bool_vars;
        for (size_t i = 0; i < values.size(); ++i) {
            auto vi_var = ctx.model->create_variable(
                "__sir_v_" + std::to_string(id) + "_" + std::to_string(i), values[i]);
            auto bi = ctx.model->create_variable(
                "__sir_bi_" + std::to_string(id) + "_" + std::to_string(i), 0, 1);
            ctx.model->set_defined_var(bi->id());
            ctx.model->add_constraint(std::make_shared<IntEqReifConstraint>(x, vi_var, bi));
            bool_vars.push_back(bi);
        }
        ctx.model->add_constraint(std::make_shared<ArrayBoolOrConstraint>(bool_vars, b));
        return std::nullopt;
    }
    throw std::runtime_error("set_in_reif requires range or set argument");
}

// ============================================================
// Pattern E: Element constraints
// ============================================================
static std::optional<ConstraintPtr> make_int_element(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 3) throw std::runtime_error("int_element requires 3 arguments (index, array, result)");

    // index variable
    VariablePtr index_var;
    if (std::holds_alternative<std::string>(decl.args[0])) {
        index_var = ctx.get_var_by_name(std::get<std::string>(decl.args[0]));
    } else if (std::holds_alternative<Domain::value_type>(decl.args[0])) {
        auto val = std::get<Domain::value_type>(decl.args[0]);
        static int idx_const_counter = 0;
        std::string name = "__idx_const_" + std::to_string(idx_const_counter++);
        index_var = ctx.model->create_variable(name, val);
        ctx.var_map[name] = index_var;
    } else {
        throw std::runtime_error("int_element: first argument must be a variable or integer");
    }

    // array of integers
    std::vector<Domain::value_type> array;
    if (std::holds_alternative<std::vector<Domain::value_type>>(decl.args[1])) {
        array = std::get<std::vector<Domain::value_type>>(decl.args[1]);
    } else if (std::holds_alternative<std::string>(decl.args[1])) {
        const auto& array_name = std::get<std::string>(decl.args[1]);
        auto arr_it = ctx.array_decls.find(array_name);
        if (arr_it == ctx.array_decls.end())
            throw std::runtime_error("Unknown array in int_element: " + array_name);
        for (const auto& elem_name : arr_it->second.elements) {
            auto var_it = ctx.var_decls.find(elem_name);
            if (var_it == ctx.var_decls.end())
                throw std::runtime_error("Unknown array element in int_element: " + elem_name);
            if (!var_it->second.fixed_value)
                throw std::runtime_error("int_element: array element must be a constant: " + elem_name);
            array.push_back(*var_it->second.fixed_value);
        }
    } else {
        throw std::runtime_error("int_element: second argument must be an array of integers or array name");
    }

    // result variable
    VariablePtr result_var;
    if (std::holds_alternative<std::string>(decl.args[2])) {
        result_var = ctx.get_var_by_name(std::get<std::string>(decl.args[2]));
    } else if (std::holds_alternative<Domain::value_type>(decl.args[2])) {
        auto val = std::get<Domain::value_type>(decl.args[2]);
        static int res_const_counter = 0;
        std::string name = "__res_const_" + std::to_string(res_const_counter++);
        result_var = ctx.model->create_variable(name, val);
        ctx.var_map[name] = result_var;
    } else {
        throw std::runtime_error("int_element: third argument must be a variable or integer");
    }

    // monotonicity check
    bool non_decreasing = true, non_increasing = true;
    for (size_t i = 1; i < array.size(); ++i) {
        if (array[i] < array[i-1]) non_decreasing = false;
        if (array[i] > array[i-1]) non_increasing = false;
        if (!non_decreasing && !non_increasing) break;
    }

    ConstraintPtr constraint;
    if ((non_decreasing || non_increasing) && array.size() > 1) {
        auto mono = non_decreasing
            ? IntElementMonotonicConstraint::Monotonicity::NON_DECREASING
            : IntElementMonotonicConstraint::Monotonicity::NON_INCREASING;
        constraint = std::make_shared<IntElementMonotonicConstraint>(
            index_var, array, result_var, mono, false);
    } else {
        constraint = std::make_shared<IntElementConstraint>(index_var, array, result_var, false);
    }

    if (!((non_decreasing || non_increasing) && array.size() > 1)) {
        ctx.model->set_no_bisect(index_var->id());
    }

    if (!ctx.model->is_defined_var(result_var->id())) {
        ctx.model->set_defined_var(result_var->id());
    }

    return constraint;
}

static std::optional<ConstraintPtr> make_array_var_int_element(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 3)
        throw std::runtime_error(decl.name + " requires 3 arguments (index, array, result)");

    VariablePtr index_var = ctx.get_var(decl.args[0]);
    auto array_vars = resolve_vars(decl.args[1], ctx);
    VariablePtr result_var = ctx.get_var(decl.args[2]);

    auto constraint = std::make_shared<ArrayVarIntElementConstraint>(
        index_var, array_vars, result_var, false);
    ctx.model->set_no_bisect(index_var->id());
    if (!ctx.model->is_defined_var(result_var->id())) {
        ctx.model->set_defined_var(result_var->id());
    }
    return constraint;
}

// ============================================================
// Pattern F: Count constraints
// ============================================================
static std::optional<ConstraintPtr> make_count_eq(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 3) throw std::runtime_error("fzn_count_eq requires 3 arguments (array, target, count)");

    auto x_vars = resolve_vars(decl.args[0], ctx);
    auto c_var = ctx.get_var(decl.args[2]);

    if (std::holds_alternative<Domain::value_type>(decl.args[1])) {
        auto target_val = std::get<Domain::value_type>(decl.args[1]);
        return std::make_shared<CountEqConstraint>(x_vars, target_val, c_var);
    } else if (std::holds_alternative<std::string>(decl.args[1])) {
        auto y_var = ctx.get_var(decl.args[1]);
        if (y_var->is_assigned()) {
            auto target_val = y_var->assigned_value().value();
            return std::make_shared<CountEqConstraint>(x_vars, target_val, c_var);
        } else {
            return std::make_shared<CountEqVarTargetConstraint>(x_vars, y_var, c_var);
        }
    }
    throw std::runtime_error("fzn_count_eq: target (y) must be an integer or variable");
}

// ============================================================
// Pattern G: Regular constraint
// ============================================================
static std::optional<ConstraintPtr> make_regular(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 6) throw std::runtime_error("regular requires 6 arguments (x, Q, S, d, q0, F)");
    auto vars = resolve_vars(decl.args[0], ctx);
    auto Q = std::get<Domain::value_type>(decl.args[1]);
    auto S = std::get<Domain::value_type>(decl.args[2]);
    auto d = ctx.resolve_int_array(decl.args[3]);
    auto q0 = std::get<Domain::value_type>(decl.args[4]);

    // F can be a set literal {1,3}, a range 1..7, or an array reference
    std::vector<int> accepting;
    if (std::holds_alternative<IntRange>(decl.args[5])) {
        const auto& range = std::get<IntRange>(decl.args[5]);
        for (auto v = range.lb; v <= range.ub; ++v) {
            accepting.push_back((int)v);
        }
    } else {
        auto F = ctx.resolve_int_array(decl.args[5]);
        accepting.assign(F.begin(), F.end());
    }

    for (const auto& v : vars) {
        ctx.model->set_no_bisect(v->id());
    }
    return std::make_shared<RegularConstraint>(
        std::move(vars), (int)Q, (int)S,
        std::vector<int>(d.begin(), d.end()),
        (int)q0, std::move(accepting));
}

// ============================================================
// Pattern I: NValue constraint
// ============================================================
static std::optional<ConstraintPtr> make_nvalue(const ConstraintDecl& decl, FznBuildContext& ctx) {
    if (decl.args.size() != 2) throw std::runtime_error("fzn_nvalue requires 2 arguments (n, x)");
    auto n_var = ctx.get_var(decl.args[0]);
    auto x_vars = resolve_vars(decl.args[1], ctx);
    return std::make_shared<NValueConstraint>(std::move(n_var), std::move(x_vars));
}

// ============================================================
// Registration
// ============================================================
void register_all_constraints(ConstraintRegistry& registry) {
    // Pattern A: Simple constraints
    registry.register_constraint("int_eq", make_int_eq);
    registry.register_constraint("int_ne", make_int_ne);
    registry.register_constraint("int_lt", make_int_lt);
    registry.register_constraint("int_le", make_int_le);
    registry.register_constraint("int_eq_reif", make_int_eq_reif);
    registry.register_constraint("int_eq_imp", make_int_eq_imp);
    registry.register_constraint("int_ne_reif", make_int_ne_reif);
    registry.register_constraint("int_le_reif", make_int_le_reif);
    registry.register_constraint("bool_not", make_bool_not);
    registry.register_constraint("bool_xor", make_bool_xor);
    registry.register_constraint("int_min", make_int_min);
    registry.register_constraint("int_max", make_int_max);
    registry.register_constraint("int_times", make_int_times);
    registry.register_constraint("int_div", make_int_div);
    registry.register_constraint("int_mod", make_int_mod);
    registry.register_constraint("int_abs", make_int_abs);

    // Bool aliases → same as int versions
    registry.register_constraint("bool_eq", make_int_eq);
    registry.register_constraint("bool_ne", make_int_ne);
    registry.register_constraint("bool_lt", make_int_lt);
    registry.register_constraint("bool_le", make_int_le);
    registry.register_constraint("bool_eq_reif", make_int_eq_reif);
    registry.register_constraint("bool_eq_imp", make_int_eq_imp);
    registry.register_constraint("bool_le_reif", make_int_le_reif);

    // Pattern B: Array constraints
    registry.register_constraint("all_different_int", make_all_different);
    registry.register_constraint("alldifferent_int", make_all_different);
    registry.register_constraint("fzn_all_different_int", make_all_different);
    registry.register_constraint("alldifferent_except_0", make_alldifferent_except_0);
    registry.register_constraint("fzn_alldifferent_except_0", make_alldifferent_except_0);
    registry.register_constraint("circuit", make_circuit);
    registry.register_constraint("fzn_circuit", make_circuit);
    registry.register_constraint("array_bool_and", make_array_bool_and);
    registry.register_constraint("array_bool_or", make_array_bool_or);
    registry.register_constraint("array_bool_xor", make_array_bool_xor);
    registry.register_constraint("bool_clause", make_bool_clause);
    registry.register_constraint("array_int_maximum", make_array_int_maximum);
    registry.register_constraint("array_int_minimum", make_array_int_minimum);
    registry.register_constraint("table_int", make_table_int);
    registry.register_constraint("sabori_table_int", make_table_int);
    registry.register_constraint("fzn_diffn", make_diffn);
    registry.register_constraint("fzn_diffn_nonstrict", make_diffn);
    registry.register_constraint("fzn_cumulative", make_cumulative);
    registry.register_constraint("fzn_disjunctive", make_disjunctive);
    registry.register_constraint("fzn_disjunctive_strict", make_disjunctive);
    registry.register_constraint("fzn_inverse", make_inverse);
    registry.register_constraint("fzn_all_equal_int", make_all_equal);
    registry.register_constraint("all_equal_int", make_all_equal);

    // Pattern C: Linear + substitution
    registry.register_constraint("int_lin_eq", make_int_lin_eq);
    registry.register_constraint("int_lin_le", make_int_lin_le);
    registry.register_constraint("int_lin_ne", make_int_lin_ne);
    registry.register_constraint("int_lin_eq_reif", make_int_lin_eq_reif);
    registry.register_constraint("int_lin_ne_reif", make_int_lin_ne_reif);
    registry.register_constraint("int_lin_le_reif", make_int_lin_le_reif);
    registry.register_constraint("int_lin_le_imp", make_int_lin_le_imp);
    registry.register_constraint("bool_lin_eq", make_bool_lin_eq);
    registry.register_constraint("bool_lin_le", make_bool_lin_le);

    // Pattern D: Special
    registry.register_constraint("bool2int", make_bool2int);
    registry.register_constraint("set_in", make_set_in);
    registry.register_constraint("set_in_reif", make_set_in_reif);

    // Pattern E: Element
    registry.register_constraint("array_int_element", make_int_element);
    registry.register_constraint("int_element", make_int_element);
    registry.register_constraint("array_bool_element", make_int_element);
    registry.register_constraint("array_var_int_element", make_array_var_int_element);
    registry.register_constraint("array_var_bool_element", make_array_var_int_element);

    // Pattern F: Count
    registry.register_constraint("fzn_count_eq", make_count_eq);
    registry.register_constraint("count_eq", make_count_eq);

    // Pattern G: Regular
    registry.register_constraint("fzn_regular", make_regular);
    registry.register_constraint("sabori_regular", make_regular);

    // Pattern I: NValue
    registry.register_constraint("fzn_nvalue", make_nvalue);
}

} // namespace fzn
} // namespace sabori_csp
