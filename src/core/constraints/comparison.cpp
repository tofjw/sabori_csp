#include "sabori_csp/constraints/comparison.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <set>

namespace sabori_csp {

// ============================================================================
// IntEqConstraint implementation
// ============================================================================

IntEqConstraint::IntEqConstraint(VariablePtr x, VariablePtr y)
    : Constraint({x, y})
    , x_(std::move(x))
    , y_(std::move(y)) {
    check_initial_consistency();
}

std::string IntEqConstraint::name() const {
    return "int_eq";
}

std::vector<VariablePtr> IntEqConstraint::variables() const {
    return {x_, y_};
}

std::optional<bool> IntEqConstraint::is_satisfied() const {
    if (x_->is_assigned() && y_->is_assigned()) {
        return x_->assigned_value() == y_->assigned_value();
    }
    return std::nullopt;
}

bool IntEqConstraint::propagate() {
    // Intersect domains
    auto x_vals = x_->domain().values();
    auto y_vals = y_->domain().values();

    std::set<Domain::value_type> x_set(x_vals.begin(), x_vals.end());
    std::set<Domain::value_type> y_set(y_vals.begin(), y_vals.end());

    // Remove values from x that are not in y
    for (auto v : x_vals) {
        if (y_set.count(v) == 0) {
            x_->domain().remove(v);
        }
    }

    // Remove values from y that are not in x
    for (auto v : y_vals) {
        if (x_set.count(v) == 0) {
            y_->domain().remove(v);
        }
    }

    return !x_->domain().empty() && !y_->domain().empty();
}

bool IntEqConstraint::on_instantiate(Model& model, int save_point,
                                      size_t var_idx, Domain::value_type value,
                                      Domain::value_type prev_min,
                                      Domain::value_type prev_max) {
    // 基底クラスの 2WL 処理
    if (!Constraint::on_instantiate(model, save_point, var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // x == y なので、一方が確定したら他方も同じ値に固定
    if (x_->is_assigned() && !y_->is_assigned()) {
        auto val = x_->assigned_value().value();
        if (!y_->domain().contains(val)) {
            return false;
        }
        // Note: Model を通じて instantiate すべきだが、
        // ここでは互換性のため直接操作
        return y_->domain().assign(val);
    }
    if (y_->is_assigned() && !x_->is_assigned()) {
        auto val = y_->assigned_value().value();
        if (!x_->domain().contains(val)) {
            return false;
        }
        return x_->domain().assign(val);
    }

    return true;
}

bool IntEqConstraint::on_final_instantiate() {
    return x_->assigned_value() == y_->assigned_value();
}

void IntEqConstraint::check_initial_consistency() {
    // x == y: ドメインに共通の値がなければ矛盾
    auto x_vals = x_->domain().values();
    auto y_vals = y_->domain().values();
    std::set<Domain::value_type> y_set(y_vals.begin(), y_vals.end());

    bool has_common = false;
    for (auto v : x_vals) {
        if (y_set.count(v) > 0) {
            has_common = true;
            break;
        }
    }

    if (!has_common) {
        set_initially_inconsistent(true);
    }
}

// ============================================================================
// IntEqReifConstraint implementation
// ============================================================================

IntEqReifConstraint::IntEqReifConstraint(VariablePtr x, VariablePtr y, VariablePtr b)
    : Constraint({x, y, b})
    , x_(std::move(x))
    , y_(std::move(y))
    , b_(std::move(b)) {
    check_initial_consistency();
}

std::string IntEqReifConstraint::name() const {
    return "int_eq_reif";
}

std::vector<VariablePtr> IntEqReifConstraint::variables() const {
    return {x_, y_, b_};
}

std::optional<bool> IntEqReifConstraint::is_satisfied() const {
    if (x_->is_assigned() && y_->is_assigned() && b_->is_assigned()) {
        bool eq = (x_->assigned_value() == y_->assigned_value());
        return eq == (b_->assigned_value().value() == 1);
    }
    return std::nullopt;
}

bool IntEqReifConstraint::propagate() {
    // If b is fixed to 1, enforce x == y
    if (b_->is_assigned() && b_->assigned_value().value() == 1) {
        auto x_vals = x_->domain().values();
        auto y_vals = y_->domain().values();
        std::set<Domain::value_type> x_set(x_vals.begin(), x_vals.end());
        std::set<Domain::value_type> y_set(y_vals.begin(), y_vals.end());

        for (auto v : x_vals) {
            if (y_set.count(v) == 0) {
                x_->domain().remove(v);
            }
        }
        for (auto v : y_vals) {
            if (x_set.count(v) == 0) {
                y_->domain().remove(v);
            }
        }
    }

    // If b is fixed to 0 and one variable is singleton, remove that value from the other
    if (b_->is_assigned() && b_->assigned_value().value() == 0) {
        if (x_->is_assigned()) {
            y_->domain().remove(x_->assigned_value().value());
        }
        if (y_->is_assigned()) {
            x_->domain().remove(y_->assigned_value().value());
        }
    }

    // If x and y are both singletons, fix b
    if (x_->is_assigned() && y_->is_assigned()) {
        bool eq = (x_->assigned_value() == y_->assigned_value());
        if (!b_->domain().assign(eq ? 1 : 0)) {
            return false;
        }
    }

    return !x_->domain().empty() && !y_->domain().empty() && !b_->domain().empty();
}

bool IntEqReifConstraint::on_instantiate(Model& model, int save_point,
                                          size_t var_idx, Domain::value_type value,
                                          Domain::value_type prev_min,
                                          Domain::value_type prev_max) {
    // 基底クラスの 2WL 処理
    if (!Constraint::on_instantiate(model, save_point, var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // 伝播ロジック
    if (b_->is_assigned()) {
        if (b_->assigned_value().value() == 1) {
            // x == y を強制
            if (x_->is_assigned() && !y_->is_assigned()) {
                return y_->domain().assign(x_->assigned_value().value());
            }
            if (y_->is_assigned() && !x_->is_assigned()) {
                return x_->domain().assign(y_->assigned_value().value());
            }
        } else {
            // x != y を強制
            if (x_->is_assigned()) {
                y_->domain().remove(x_->assigned_value().value());
            }
            if (y_->is_assigned()) {
                x_->domain().remove(y_->assigned_value().value());
            }
        }
    }

    // x と y が両方確定したら b を決定
    if (x_->is_assigned() && y_->is_assigned() && !b_->is_assigned()) {
        bool eq = (x_->assigned_value() == y_->assigned_value());
        return b_->domain().assign(eq ? 1 : 0);
    }

    return !x_->domain().empty() && !y_->domain().empty() && !b_->domain().empty();
}

bool IntEqReifConstraint::on_final_instantiate() {
    bool eq = (x_->assigned_value() == y_->assigned_value());
    return eq == (b_->assigned_value().value() == 1);
}

void IntEqReifConstraint::check_initial_consistency() {
    // (x == y) <-> b
    // b=1 が強制で x,y に共通値がない、または b=0 が強制で x,y が同じシングルトン
    if (b_->is_assigned()) {
        if (b_->assigned_value().value() == 1) {
            // x == y を満たす共通値が必要
            auto x_vals = x_->domain().values();
            auto y_vals = y_->domain().values();
            std::set<Domain::value_type> y_set(y_vals.begin(), y_vals.end());

            bool has_common = false;
            for (auto v : x_vals) {
                if (y_set.count(v) > 0) {
                    has_common = true;
                    break;
                }
            }
            if (!has_common) {
                set_initially_inconsistent(true);
            }
        } else {
            // x != y が必要: 両方シングルトンで同じ値なら矛盾
            if (x_->is_assigned() && y_->is_assigned() &&
                x_->assigned_value() == y_->assigned_value()) {
                set_initially_inconsistent(true);
            }
        }
    }
}

// ============================================================================
// IntNeConstraint implementation
// ============================================================================

IntNeConstraint::IntNeConstraint(VariablePtr x, VariablePtr y)
    : Constraint({x, y})
    , x_(std::move(x))
    , y_(std::move(y)) {
    check_initial_consistency();
}

std::string IntNeConstraint::name() const {
    return "int_ne";
}

std::vector<VariablePtr> IntNeConstraint::variables() const {
    return {x_, y_};
}

std::optional<bool> IntNeConstraint::is_satisfied() const {
    if (x_->is_assigned() && y_->is_assigned()) {
        return x_->assigned_value() != y_->assigned_value();
    }
    return std::nullopt;
}

bool IntNeConstraint::propagate() {
    // If one is singleton, remove that value from the other
    if (x_->is_assigned()) {
        y_->domain().remove(x_->assigned_value().value());
    }
    if (y_->is_assigned()) {
        x_->domain().remove(y_->assigned_value().value());
    }
    return !x_->domain().empty() && !y_->domain().empty();
}

bool IntNeConstraint::on_instantiate(Model& model, int save_point,
                                      size_t var_idx, Domain::value_type value,
                                      Domain::value_type prev_min,
                                      Domain::value_type prev_max) {
    // 基底クラスの 2WL 処理
    if (!Constraint::on_instantiate(model, save_point, var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // x != y なので、一方が確定したら他方からその値を削除
    if (x_->is_assigned()) {
        y_->domain().remove(x_->assigned_value().value());
    }
    if (y_->is_assigned()) {
        x_->domain().remove(y_->assigned_value().value());
    }

    return !x_->domain().empty() && !y_->domain().empty();
}

bool IntNeConstraint::on_final_instantiate() {
    return x_->assigned_value() != y_->assigned_value();
}

void IntNeConstraint::check_initial_consistency() {
    // x != y: 両方シングルトンで同じ値なら矛盾
    if (x_->is_assigned() && y_->is_assigned() &&
        x_->assigned_value() == y_->assigned_value()) {
        set_initially_inconsistent(true);
    }
}

// ============================================================================
// IntLtConstraint implementation
// ============================================================================

IntLtConstraint::IntLtConstraint(VariablePtr x, VariablePtr y)
    : Constraint({x, y})
    , x_(std::move(x))
    , y_(std::move(y)) {
    check_initial_consistency();
}

std::string IntLtConstraint::name() const {
    return "int_lt";
}

std::vector<VariablePtr> IntLtConstraint::variables() const {
    return {x_, y_};
}

std::optional<bool> IntLtConstraint::is_satisfied() const {
    if (x_->is_assigned() && y_->is_assigned()) {
        return x_->assigned_value() < y_->assigned_value();
    }
    return std::nullopt;
}

bool IntLtConstraint::propagate() {
    // x < y means x.max < y and y > x.min
    auto x_max = x_->domain().max();
    auto y_min = y_->domain().min();

    if (!x_max || !y_min) {
        return false;
    }

    // Remove values from x that are >= y.max
    auto y_max = y_->domain().max();
    if (y_max) {
        for (auto v : x_->domain().values()) {
            if (v >= *y_max) {
                x_->domain().remove(v);
            }
        }
    }

    // Remove values from y that are <= x.min
    auto x_min = x_->domain().min();
    if (x_min) {
        for (auto v : y_->domain().values()) {
            if (v <= *x_min) {
                y_->domain().remove(v);
            }
        }
    }

    return !x_->domain().empty() && !y_->domain().empty();
}

bool IntLtConstraint::on_instantiate(Model& model, int save_point,
                                      size_t var_idx, Domain::value_type value,
                                      Domain::value_type prev_min,
                                      Domain::value_type prev_max) {
    // 基底クラスの 2WL 処理
    if (!Constraint::on_instantiate(model, save_point, var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // x < y: x が確定したら y の下限を更新
    if (x_->is_assigned()) {
        auto x_val = x_->assigned_value().value();
        // y > x_val なので y の下限は x_val + 1
        for (auto v : y_->domain().values()) {
            if (v <= x_val) {
                y_->domain().remove(v);
            }
        }
    }

    // y が確定したら x の上限を更新
    if (y_->is_assigned()) {
        auto y_val = y_->assigned_value().value();
        // x < y_val なので x の上限は y_val - 1
        for (auto v : x_->domain().values()) {
            if (v >= y_val) {
                x_->domain().remove(v);
            }
        }
    }

    return !x_->domain().empty() && !y_->domain().empty();
}

bool IntLtConstraint::on_final_instantiate() {
    return x_->assigned_value() < y_->assigned_value();
}

void IntLtConstraint::check_initial_consistency() {
    // x < y: x.min >= y.max なら矛盾
    auto x_min = x_->domain().min();
    auto y_max = y_->domain().max();

    if (x_min && y_max && *x_min >= *y_max) {
        set_initially_inconsistent(true);
    }
}

// ============================================================================
// IntLeConstraint implementation
// ============================================================================

IntLeConstraint::IntLeConstraint(VariablePtr x, VariablePtr y)
    : Constraint({x, y})
    , x_(std::move(x))
    , y_(std::move(y)) {
    check_initial_consistency();
}

std::string IntLeConstraint::name() const {
    return "int_le";
}

std::vector<VariablePtr> IntLeConstraint::variables() const {
    return {x_, y_};
}

std::optional<bool> IntLeConstraint::is_satisfied() const {
    if (x_->is_assigned() && y_->is_assigned()) {
        return x_->assigned_value() <= y_->assigned_value();
    }
    return std::nullopt;
}

bool IntLeConstraint::propagate() {
    // x <= y
    auto y_max = y_->domain().max();
    if (y_max) {
        for (auto v : x_->domain().values()) {
            if (v > *y_max) {
                x_->domain().remove(v);
            }
        }
    }

    auto x_min = x_->domain().min();
    if (x_min) {
        for (auto v : y_->domain().values()) {
            if (v < *x_min) {
                y_->domain().remove(v);
            }
        }
    }

    return !x_->domain().empty() && !y_->domain().empty();
}

bool IntLeConstraint::on_instantiate(Model& model, int save_point,
                                      size_t var_idx, Domain::value_type value,
                                      Domain::value_type prev_min,
                                      Domain::value_type prev_max) {
    // 基底クラスの 2WL 処理
    if (!Constraint::on_instantiate(model, save_point, var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // x <= y: x が確定したら y の下限を更新
    if (x_->is_assigned()) {
        auto x_val = x_->assigned_value().value();
        // y >= x_val
        for (auto v : y_->domain().values()) {
            if (v < x_val) {
                y_->domain().remove(v);
            }
        }
    }

    // y が確定したら x の上限を更新
    if (y_->is_assigned()) {
        auto y_val = y_->assigned_value().value();
        // x <= y_val
        for (auto v : x_->domain().values()) {
            if (v > y_val) {
                x_->domain().remove(v);
            }
        }
    }

    return !x_->domain().empty() && !y_->domain().empty();
}

bool IntLeConstraint::on_final_instantiate() {
    return x_->assigned_value() <= y_->assigned_value();
}

void IntLeConstraint::check_initial_consistency() {
    // x <= y: x.min > y.max なら矛盾
    auto x_min = x_->domain().min();
    auto y_max = y_->domain().max();

    if (x_min && y_max && *x_min > *y_max) {
        set_initially_inconsistent(true);
    }
}

} // namespace sabori_csp
