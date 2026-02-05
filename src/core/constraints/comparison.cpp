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

bool IntEqConstraint::propagate(Model& /*model*/) {
    // Intersect domains
    auto x_vals = x_->domain().values();
    auto y_vals = y_->domain().values();

    std::set<Domain::value_type> x_set(x_vals.begin(), x_vals.end());
    std::set<Domain::value_type> y_set(y_vals.begin(), y_vals.end());

    // Remove values from x that are not in y
    for (auto v : x_vals) {
        if (y_set.count(v) == 0) {
            if (!x_->domain().remove(v)) {
                return false;
            }
        }
    }

    // Remove values from y that are not in x
    for (auto v : y_vals) {
        if (x_set.count(v) == 0) {
            if (!y_->domain().remove(v)) {
                return false;
            }
        }
    }

    return true;
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

    // x == y なので、一方が確定したら他方も同じ値に固定（キューイング）
    if (x_->is_assigned() && !y_->is_assigned()) {
        auto val = x_->assigned_value().value();
        if (!y_->domain().contains(val)) {
            return false;
        }
        model.enqueue_instantiate(y_->id(), val);
    }
    if (y_->is_assigned() && !x_->is_assigned()) {
        auto val = y_->assigned_value().value();
        if (!x_->domain().contains(val)) {
            return false;
        }
        model.enqueue_instantiate(x_->id(), val);
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
    // 注意: 内部状態は presolve() で初期化
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

bool IntEqReifConstraint::presolve(Model& /*model*/) {
    // 2WL を初期化
    init_watches();

    // 初期整合性チェック
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
                return false;
            }
        } else {
            // x != y が必要: 両方シングルトンで同じ値なら矛盾
            if (x_->is_assigned() && y_->is_assigned() &&
                x_->assigned_value() == y_->assigned_value()) {
                return false;
            }
        }
    }

    return true;
}

bool IntEqReifConstraint::propagate(Model& /*model*/) {
    // If b is fixed to 1, enforce x == y
    if (b_->is_assigned() && b_->assigned_value().value() == 1) {
        auto x_vals = x_->domain().values();
        auto y_vals = y_->domain().values();
        std::set<Domain::value_type> x_set(x_vals.begin(), x_vals.end());
        std::set<Domain::value_type> y_set(y_vals.begin(), y_vals.end());

        for (auto v : x_vals) {
            if (y_set.count(v) == 0) {
                if (!x_->domain().remove(v)) {
                    return false;
                }
            }
        }
        for (auto v : y_vals) {
            if (x_set.count(v) == 0) {
                if (!y_->domain().remove(v)) {
                    return false;
                }
            }
        }
    }

    // If b is fixed to 0 and one variable is singleton, remove that value from the other
    if (b_->is_assigned() && b_->assigned_value().value() == 0) {
        if (x_->is_assigned()) {
            if (!y_->domain().remove(x_->assigned_value().value())) {
                return false;
            }
        }
        if (y_->is_assigned()) {
            if (!x_->domain().remove(y_->assigned_value().value())) {
                return false;
            }
        }
    }

    // If x and y are both singletons, fix b
    // NOTE: propagate() は初期伝播で使用され、Trail は使用しない
    if (x_->is_assigned() && y_->is_assigned() && !b_->is_assigned()) {
        bool eq = (x_->assigned_value() == y_->assigned_value());
        if (!b_->domain().contains(eq ? 1 : 0)) {
            return false;
        }
        b_->domain().assign(eq ? 1 : 0);
    }

    // If y is a singleton and x's domain doesn't contain y's value, then b = 0
    // (x can never equal y)
    if (!b_->is_assigned() && y_->is_assigned()) {
        auto y_val = y_->assigned_value().value();
        if (!x_->domain().contains(y_val)) {
            if (!b_->domain().contains(0)) {
                return false;
            }
            b_->domain().assign(0);
        }
    }

    // If x is a singleton and y's domain doesn't contain x's value, then b = 0
    if (!b_->is_assigned() && x_->is_assigned()) {
        auto x_val = x_->assigned_value().value();
        if (!y_->domain().contains(x_val)) {
            if (!b_->domain().contains(0)) {
                return false;
            }
            b_->domain().assign(0);
        }
    }

    return true;
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

    // 伝播ロジック（キューイング）
    if (b_->is_assigned()) {
        if (b_->assigned_value().value() == 1) {
            // x == y を強制
            if (x_->is_assigned() && !y_->is_assigned()) {
                auto val = x_->assigned_value().value();
                if (!y_->domain().contains(val)) {
                    return false;
                }
                model.enqueue_instantiate(y_->id(), val);
            }
            if (y_->is_assigned() && !x_->is_assigned()) {
                auto val = y_->assigned_value().value();
                if (!x_->domain().contains(val)) {
                    return false;
                }
                model.enqueue_instantiate(x_->id(), val);
            }
        } else {
            // x != y を強制
            if (x_->is_assigned()) {
                model.enqueue_remove_value(y_->id(), x_->assigned_value().value());
            }
            if (y_->is_assigned()) {
                model.enqueue_remove_value(x_->id(), y_->assigned_value().value());
            }
        }
    }

    // x と y が両方確定したら b を決定
    if (x_->is_assigned() && y_->is_assigned() && !b_->is_assigned()) {
        bool eq = (x_->assigned_value() == y_->assigned_value());
        model.enqueue_instantiate(b_->id(), eq ? 1 : 0);
    }

    return true;
}

bool IntEqReifConstraint::on_remove_value(Model& model, int /*save_point*/,
                                           size_t var_idx, Domain::value_type removed_value) {
    (void)removed_value;

    // x または y から値が削除された場合、b を更新
    if (!b_->is_assigned()) {
        // y がシングルトンで、x から値が削除された場合
        if (y_->is_assigned() && var_idx == x_->id()) {
            auto y_val = y_->assigned_value().value();
            // x の現在のドメインに y_val がない場合、b = 0
            if (!x_->domain().contains(y_val)) {
                model.enqueue_instantiate(b_->id(), 0);
            }
        }
        // x がシングルトンで、y から値が削除された場合
        if (x_->is_assigned() && var_idx == y_->id()) {
            auto x_val = x_->assigned_value().value();
            // y の現在のドメインに x_val がない場合、b = 0
            if (!y_->domain().contains(x_val)) {
                model.enqueue_instantiate(b_->id(), 0);
            }
        }
    }

    return true;
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

bool IntNeConstraint::propagate(Model& /*model*/) {
    // If one is singleton, remove that value from the other
    if (x_->is_assigned()) {
        if (!y_->domain().remove(x_->assigned_value().value())) {
            return false;
        }
    }
    if (y_->is_assigned()) {
        if (!x_->domain().remove(y_->assigned_value().value())) {
            return false;
        }
    }
    return true;
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

    // x != y なので、一方が確定したら他方からその値を削除（キューイング）
    if (x_->is_assigned()) {
        model.enqueue_remove_value(y_->id(), x_->assigned_value().value());
    }
    if (y_->is_assigned()) {
        model.enqueue_remove_value(x_->id(), y_->assigned_value().value());
    }

    return true;
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
// IntNeReifConstraint implementation
// ============================================================================

IntNeReifConstraint::IntNeReifConstraint(VariablePtr x, VariablePtr y, VariablePtr b)
    : Constraint({x, y, b})
    , x_(std::move(x))
    , y_(std::move(y))
    , b_(std::move(b)) {
    // 注意: 内部状態は presolve() で初期化
}

std::string IntNeReifConstraint::name() const {
    return "int_ne_reif";
}

std::vector<VariablePtr> IntNeReifConstraint::variables() const {
    return {x_, y_, b_};
}

std::optional<bool> IntNeReifConstraint::is_satisfied() const {
    if (x_->is_assigned() && y_->is_assigned() && b_->is_assigned()) {
        bool ne = (x_->assigned_value() != y_->assigned_value());
        return ne == (b_->assigned_value().value() == 1);
    }
    return std::nullopt;
}

bool IntNeReifConstraint::presolve(Model& /*model*/) {
    // 2WL を初期化
    init_watches();

    // 初期整合性チェック
    // (x != y) <-> b
    if (b_->is_assigned()) {
        if (b_->assigned_value().value() == 1) {
            // x != y が必要: 両方シングルトンで同じ値なら矛盾
            if (x_->is_assigned() && y_->is_assigned() &&
                x_->assigned_value() == y_->assigned_value()) {
                return false;
            }
        } else {
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
                return false;
            }
        }
    }

    return true;
}

bool IntNeReifConstraint::propagate(Model& /*model*/) {
    // If b is fixed to 1, enforce x != y
    if (b_->is_assigned() && b_->assigned_value().value() == 1) {
        if (x_->is_assigned()) {
            if (!y_->domain().remove(x_->assigned_value().value())) {
                return false;
            }
        }
        if (y_->is_assigned()) {
            if (!x_->domain().remove(y_->assigned_value().value())) {
                return false;
            }
        }
    }

    // If b is fixed to 0, enforce x == y
    if (b_->is_assigned() && b_->assigned_value().value() == 0) {
        auto x_vals = x_->domain().values();
        auto y_vals = y_->domain().values();
        std::set<Domain::value_type> x_set(x_vals.begin(), x_vals.end());
        std::set<Domain::value_type> y_set(y_vals.begin(), y_vals.end());

        for (auto v : x_vals) {
            if (y_set.count(v) == 0) {
                if (!x_->domain().remove(v)) {
                    return false;
                }
            }
        }
        for (auto v : y_vals) {
            if (x_set.count(v) == 0) {
                if (!y_->domain().remove(v)) {
                    return false;
                }
            }
        }
    }

    // If x and y are both singletons, fix b
    if (x_->is_assigned() && y_->is_assigned() && !b_->is_assigned()) {
        bool ne = (x_->assigned_value() != y_->assigned_value());
        if (!b_->domain().contains(ne ? 1 : 0)) {
            return false;
        }
        b_->domain().assign(ne ? 1 : 0);
    }

    // If y is a singleton and x's domain doesn't contain y's value, then b = 1
    // (x can never equal y, so x != y is always true)
    if (!b_->is_assigned() && y_->is_assigned()) {
        auto y_val = y_->assigned_value().value();
        if (!x_->domain().contains(y_val)) {
            if (!b_->domain().contains(1)) {
                return false;
            }
            b_->domain().assign(1);
        }
    }

    // If x is a singleton and y's domain doesn't contain x's value, then b = 1
    if (!b_->is_assigned() && x_->is_assigned()) {
        auto x_val = x_->assigned_value().value();
        if (!y_->domain().contains(x_val)) {
            if (!b_->domain().contains(1)) {
                return false;
            }
            b_->domain().assign(1);
        }
    }

    return true;
}

bool IntNeReifConstraint::on_instantiate(Model& model, int save_point,
                                          size_t var_idx, Domain::value_type value,
                                          Domain::value_type prev_min,
                                          Domain::value_type prev_max) {
    // 基底クラスの 2WL 処理
    if (!Constraint::on_instantiate(model, save_point, var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // 伝播ロジック（キューイング）
    if (b_->is_assigned()) {
        if (b_->assigned_value().value() == 1) {
            // x != y を強制
            if (x_->is_assigned()) {
                model.enqueue_remove_value(y_->id(), x_->assigned_value().value());
            }
            if (y_->is_assigned()) {
                model.enqueue_remove_value(x_->id(), y_->assigned_value().value());
            }
        } else {
            // x == y を強制
            if (x_->is_assigned() && !y_->is_assigned()) {
                auto val = x_->assigned_value().value();
                if (!y_->domain().contains(val)) {
                    return false;
                }
                model.enqueue_instantiate(y_->id(), val);
            }
            if (y_->is_assigned() && !x_->is_assigned()) {
                auto val = y_->assigned_value().value();
                if (!x_->domain().contains(val)) {
                    return false;
                }
                model.enqueue_instantiate(x_->id(), val);
            }
        }
    }

    // x と y が両方確定したら b を決定
    if (x_->is_assigned() && y_->is_assigned() && !b_->is_assigned()) {
        bool ne = (x_->assigned_value() != y_->assigned_value());
        model.enqueue_instantiate(b_->id(), ne ? 1 : 0);
    }

    return true;
}

bool IntNeReifConstraint::on_final_instantiate() {
    bool ne = (x_->assigned_value() != y_->assigned_value());
    return ne == (b_->assigned_value().value() == 1);
}

bool IntNeReifConstraint::on_remove_value(Model& model, int /*save_point*/,
                                           size_t var_idx, Domain::value_type removed_value) {
    (void)removed_value;

    // x または y から値が削除された場合、b を更新
    // int_ne_reif: (x != y) <-> b
    // y がシングルトンで、その値が x から削除された場合、x != y は確定で真、よって b = 1
    if (!b_->is_assigned()) {
        if (y_->is_assigned() && var_idx == x_->id()) {
            auto y_val = y_->assigned_value().value();
            // x の現在のドメインに y_val がない場合、x != y は確定、b = 1
            if (!x_->domain().contains(y_val)) {
                model.enqueue_instantiate(b_->id(), 1);
            }
        }
        if (x_->is_assigned() && var_idx == y_->id()) {
            auto x_val = x_->assigned_value().value();
            // y の現在のドメインに x_val がない場合、x != y は確定、b = 1
            if (!y_->domain().contains(x_val)) {
                model.enqueue_instantiate(b_->id(), 1);
            }
        }
    }

    return true;
}

void IntNeReifConstraint::check_initial_consistency() {
    // (x != y) <-> b
    if (b_->is_assigned()) {
        if (b_->assigned_value().value() == 1) {
            // x != y が必要: 両方シングルトンで同じ値なら矛盾
            if (x_->is_assigned() && y_->is_assigned() &&
                x_->assigned_value() == y_->assigned_value()) {
                set_initially_inconsistent(true);
            }
        } else {
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
        }
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

bool IntLtConstraint::propagate(Model& /*model*/) {
    // x < y means x.max < y and y > x.min

    // Remove values from x that are >= y.max
    auto y_max = y_->max();
    for (auto v : x_->domain().values()) {
        if (v >= y_max) {
            if (!x_->domain().remove(v)) {
                return false;
            }
        }
    }

    // Remove values from y that are <= x.min
    auto x_min = x_->min();
    for (auto v : y_->domain().values()) {
        if (v <= x_min) {
            if (!y_->domain().remove(v)) {
                return false;
            }
        }
    }

    return true;
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

    // x < y: x が確定したら y の下限を更新（キューイング）
    if (x_->is_assigned()) {
        auto x_val = x_->assigned_value().value();
        // y > x_val なので y の下限は x_val + 1
        model.enqueue_set_min(y_->id(), x_val + 1);
    }

    // y が確定したら x の上限を更新（キューイング）
    if (y_->is_assigned()) {
        auto y_val = y_->assigned_value().value();
        // x < y_val なので x の上限は y_val - 1
        model.enqueue_set_max(x_->id(), y_val - 1);
    }

    return true;
}

bool IntLtConstraint::on_final_instantiate() {
    return x_->assigned_value() < y_->assigned_value();
}

void IntLtConstraint::check_initial_consistency() {
    // x < y: x.min >= y.max なら矛盾
    auto x_min = x_->min();
    auto y_max = y_->max();

    if (x_min >= y_max) {
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

bool IntLeConstraint::propagate(Model& /*model*/) {
    // x <= y
    auto y_max = y_->max();
    for (auto v : x_->domain().values()) {
        if (v > y_max) {
            if (!x_->domain().remove(v)) {
                return false;
            }
        }
    }

    auto x_min = x_->min();
    for (auto v : y_->domain().values()) {
        if (v < x_min) {
            if (!y_->domain().remove(v)) {
                return false;
            }
        }
    }

    return true;
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

    // x <= y: x が確定したら y の下限を更新（キューイング）
    if (x_->is_assigned()) {
        auto x_val = x_->assigned_value().value();
        // y >= x_val
        model.enqueue_set_min(y_->id(), x_val);
    }

    // y が確定したら x の上限を更新（キューイング）
    if (y_->is_assigned()) {
        auto y_val = y_->assigned_value().value();
        // x <= y_val
        model.enqueue_set_max(x_->id(), y_val);
    }

    return true;
}

bool IntLeConstraint::on_final_instantiate() {
    return x_->assigned_value() <= y_->assigned_value();
}

void IntLeConstraint::check_initial_consistency() {
    // x <= y: x.min > y.max なら矛盾
    auto x_min = x_->min();
    auto y_max = y_->max();

    if (x_min > y_max) {
        set_initially_inconsistent(true);
    }
}

// ============================================================================
// IntLeReifConstraint implementation
// ============================================================================

IntLeReifConstraint::IntLeReifConstraint(VariablePtr x, VariablePtr y, VariablePtr b)
    : Constraint({x, y, b})
    , x_(std::move(x))
    , y_(std::move(y))
    , b_(std::move(b)) {
    check_initial_consistency();
}

std::string IntLeReifConstraint::name() const {
    return "int_le_reif";
}

std::vector<VariablePtr> IntLeReifConstraint::variables() const {
    return {x_, y_, b_};
}

std::optional<bool> IntLeReifConstraint::is_satisfied() const {
    if (x_->is_assigned() && y_->is_assigned() && b_->is_assigned()) {
        bool le = (x_->assigned_value() <= y_->assigned_value());
        return le == (b_->assigned_value().value() == 1);
    }
    return std::nullopt;
}

bool IntLeReifConstraint::propagate(Model& /*model*/) {
    // If b is fixed to 1, enforce x <= y
    if (b_->is_assigned() && b_->assigned_value().value() == 1) {
        // x <= y: x の上限を y.max に、y の下限を x.min に
        auto y_max = y_->max();
        for (auto v : x_->domain().values()) {
            if (v > y_max) {
                if (!x_->domain().remove(v)) {
                    return false;
                }
            }
        }

        auto x_min = x_->min();
        for (auto v : y_->domain().values()) {
            if (v < x_min) {
                if (!y_->domain().remove(v)) {
                    return false;
                }
            }
        }
    }

    // If b is fixed to 0, enforce x > y
    if (b_->is_assigned() && b_->assigned_value().value() == 0) {
        // x > y: x の下限を y.min + 1 に、y の上限を x.max - 1 に
        auto y_min = y_->min();
        for (auto v : x_->domain().values()) {
            if (v <= y_min) {
                if (!x_->domain().remove(v)) {
                    return false;
                }
            }
        }

        auto x_max = x_->max();
        for (auto v : y_->domain().values()) {
            if (v >= x_max) {
                if (!y_->domain().remove(v)) {
                    return false;
                }
            }
        }
    }

    // If x and y bounds determine the relation, fix b
    // NOTE: propagate() は初期伝播で使用され、Trail は使用しない
    // on_instantiate で Model を通じた Trail 付き更新を行う
    auto x_max = x_->max();
    auto y_min = y_->min();
    auto x_min = x_->min();
    auto y_max = y_->max();

    if (x_max <= y_min) {
        // x <= y is always true
        if (!b_->domain().contains(1)) {
            return false;
        }
        if (!b_->is_assigned()) {
            b_->domain().assign(1);
        }
    } else if (x_min > y_max) {
        // x <= y is always false (x > y)
        if (!b_->domain().contains(0)) {
            return false;
        }
        if (!b_->is_assigned()) {
            b_->domain().assign(0);
        }
    }

    return !x_->domain().empty() && !y_->domain().empty() && !b_->domain().empty();
}

bool IntLeReifConstraint::on_instantiate(Model& model, int save_point,
                                          size_t var_idx, Domain::value_type value,
                                          Domain::value_type prev_min,
                                          Domain::value_type prev_max) {
    // 基底クラスの 2WL 処理
    if (!Constraint::on_instantiate(model, save_point, var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // b が確定した場合の伝播（キューイング）
    if (b_->is_assigned()) {
        if (b_->assigned_value().value() == 1) {
            // x <= y を強制
            if (x_->is_assigned()) {
                auto x_val = x_->assigned_value().value();
                // y >= x_val
                model.enqueue_set_min(y_->id(), x_val);
            }
            if (y_->is_assigned()) {
                auto y_val = y_->assigned_value().value();
                // x <= y_val
                model.enqueue_set_max(x_->id(), y_val);
            }
        } else {
            // x > y を強制
            if (x_->is_assigned()) {
                auto x_val = x_->assigned_value().value();
                // y < x_val, つまり y <= x_val - 1
                model.enqueue_set_max(y_->id(), x_val - 1);
            }
            if (y_->is_assigned()) {
                auto y_val = y_->assigned_value().value();
                // x > y_val, つまり x >= y_val + 1
                model.enqueue_set_min(x_->id(), y_val + 1);
            }
        }
    }

    // x と y の bounds から b を決定できるか
    auto x_max = x_->max();
    auto y_min = y_->min();
    auto x_min = x_->min();
    auto y_max = y_->max();

    if (!b_->is_assigned()) {
        if (x_max <= y_min) {
            // x <= y is always true
            model.enqueue_instantiate(b_->id(), 1);
        } else if (x_min > y_max) {
            // x <= y is always false
            model.enqueue_instantiate(b_->id(), 0);
        }
    }

    // x と y が両方確定したら b を決定
    if (x_->is_assigned() && y_->is_assigned() && !b_->is_assigned()) {
        bool le = (x_->assigned_value() <= y_->assigned_value());
        model.enqueue_instantiate(b_->id(), le ? 1 : 0);
    }

    return true;
}

bool IntLeReifConstraint::on_final_instantiate() {
    bool le = (x_->assigned_value() <= y_->assigned_value());
    return le == (b_->assigned_value().value() == 1);
}

void IntLeReifConstraint::check_initial_consistency() {
    // (x <= y) <-> b
    if (b_->is_assigned()) {
        if (b_->assigned_value().value() == 1) {
            // x <= y が必要: x.min > y.max なら矛盾
            auto x_min = x_->min();
            auto y_max = y_->max();
            if (x_min > y_max) {
                set_initially_inconsistent(true);
            }
        } else {
            // x > y が必要: x.max <= y.min なら矛盾
            auto x_max = x_->max();
            auto y_min = y_->min();
            if (x_max <= y_min) {
                set_initially_inconsistent(true);
            }
        }
    }
}

// ============================================================================
// IntMaxConstraint implementation
// ============================================================================

IntMaxConstraint::IntMaxConstraint(VariablePtr x, VariablePtr y, VariablePtr m)
    : Constraint({x, y, m})
    , x_(std::move(x))
    , y_(std::move(y))
    , m_(std::move(m)) {
    check_initial_consistency();
}

std::string IntMaxConstraint::name() const {
    return "int_max";
}

std::vector<VariablePtr> IntMaxConstraint::variables() const {
    return {x_, y_, m_};
}

std::optional<bool> IntMaxConstraint::is_satisfied() const {
    if (x_->is_assigned() && y_->is_assigned() && m_->is_assigned()) {
        auto x_val = x_->assigned_value().value();
        auto y_val = y_->assigned_value().value();
        auto m_val = m_->assigned_value().value();
        return m_val == std::max(x_val, y_val);
    }
    return std::nullopt;
}

bool IntMaxConstraint::propagate(Model& /*model*/) {
    // m = max(x, y) の bounds propagation
    auto x_min = x_->min();
    auto x_max = x_->max();
    auto y_min = y_->min();
    auto y_max = y_->max();

    // m.min = max(x.min, y.min)
    // m.max = max(x.max, y.max)
    auto m_lower = std::max(x_min, y_min);
    auto m_upper = std::max(x_max, y_max);

    // m のドメインを絞る
    auto m_values = m_->domain().values();
    for (auto v : m_values) {
        if (v < m_lower || v > m_upper) {
            if (!m_->domain().remove(v)) {
                return false;
            }
        }
    }

    // x.max <= m.max, y.max <= m.max
    auto m_max = m_->max();
    auto x_values = x_->domain().values();
    for (auto v : x_values) {
        if (v > m_max) {
            if (!x_->domain().remove(v)) {
                return false;
            }
        }
    }

    auto y_values = y_->domain().values();
    for (auto v : y_values) {
        if (v > m_max) {
            if (!y_->domain().remove(v)) {
                return false;
            }
        }
    }

    return true;
}

bool IntMaxConstraint::on_instantiate(Model& model, int save_point,
                                       size_t var_idx, Domain::value_type value,
                                       Domain::value_type prev_min,
                                       Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // m が確定した場合
    if (m_->is_assigned()) {
        auto m_val = m_->assigned_value().value();

        // x.max と y.max を m に制限
        if (!x_->is_assigned()) {
            auto x_max = x_->max();
            if (x_max > m_val) {
                model.enqueue_set_max(x_->id(), m_val);
            }
        }
        if (!y_->is_assigned()) {
            auto y_max = y_->max();
            if (y_max > m_val) {
                model.enqueue_set_max(y_->id(), m_val);
            }
        }

        // x または y が確定していて m と等しい場合は OK
        // 両方確定していて max != m なら矛盾
        if (x_->is_assigned() && y_->is_assigned()) {
            auto x_val = x_->assigned_value().value();
            auto y_val = y_->assigned_value().value();
            if (std::max(x_val, y_val) != m_val) {
                return false;
            }
        }
        // 片方だけ確定している場合
        else if (x_->is_assigned()) {
            auto x_val = x_->assigned_value().value();
            if (x_val == m_val) {
                // y <= m で OK
            } else {
                // y == m が必要
                if (!y_->domain().contains(m_val)) {
                    return false;
                }
            }
        } else if (y_->is_assigned()) {
            auto y_val = y_->assigned_value().value();
            if (y_val == m_val) {
                // x <= m で OK
            } else {
                // x == m が必要
                if (!x_->domain().contains(m_val)) {
                    return false;
                }
            }
        } else {
            // 両方未確定: 少なくとも一方が m になれる必要
            if (!x_->domain().contains(m_val) && !y_->domain().contains(m_val)) {
                return false;
            }
        }
    }

    // x と y が両方確定した場合、m を確定
    if (x_->is_assigned() && y_->is_assigned() && !m_->is_assigned()) {
        auto x_val = x_->assigned_value().value();
        auto y_val = y_->assigned_value().value();
        auto max_val = std::max(x_val, y_val);
        model.enqueue_instantiate(m_->id(), max_val);
    }

    // x または y が確定した場合、m の下限を更新
    if (x_->is_assigned() || y_->is_assigned()) {
        auto x_min_val = x_->is_assigned() ? x_->assigned_value().value() : x_->min();
        auto y_min_val = y_->is_assigned() ? y_->assigned_value().value() : y_->min();
        auto new_m_min = std::max(x_min_val, y_min_val);

        if (!m_->is_assigned()) {
            auto m_min = m_->min();
            if (m_min < new_m_min) {
                model.enqueue_set_min(m_->id(), new_m_min);
            }
        }
    }

    return true;
}

bool IntMaxConstraint::on_final_instantiate() {
    auto x_val = x_->assigned_value().value();
    auto y_val = y_->assigned_value().value();
    auto m_val = m_->assigned_value().value();
    return m_val == std::max(x_val, y_val);
}

void IntMaxConstraint::check_initial_consistency() {
    auto x_min = x_->min();
    auto x_max = x_->max();
    auto y_min = y_->min();
    auto y_max = y_->max();
    auto m_min = m_->min();
    auto m_max = m_->max();

    // m.max < max(x.min, y.min) なら矛盾
    if (m_max < std::max(x_min, y_min)) {
        set_initially_inconsistent(true);
        return;
    }

    // m.min > max(x.max, y.max) なら矛盾
    if (m_min > std::max(x_max, y_max)) {
        set_initially_inconsistent(true);
        return;
    }
}

// ============================================================================
// IntMinConstraint implementation
// ============================================================================

IntMinConstraint::IntMinConstraint(VariablePtr x, VariablePtr y, VariablePtr m)
    : Constraint({x, y, m})
    , x_(std::move(x))
    , y_(std::move(y))
    , m_(std::move(m)) {
    check_initial_consistency();
}

std::string IntMinConstraint::name() const {
    return "int_min";
}

std::vector<VariablePtr> IntMinConstraint::variables() const {
    return {x_, y_, m_};
}

std::optional<bool> IntMinConstraint::is_satisfied() const {
    if (x_->is_assigned() && y_->is_assigned() && m_->is_assigned()) {
        auto x_val = x_->assigned_value().value();
        auto y_val = y_->assigned_value().value();
        auto m_val = m_->assigned_value().value();
        return m_val == std::min(x_val, y_val);
    }
    return std::nullopt;
}

bool IntMinConstraint::propagate(Model& /*model*/) {
    // m = min(x, y) の bounds propagation
    auto x_min = x_->min();
    auto x_max = x_->max();
    auto y_min = y_->min();
    auto y_max = y_->max();

    // m.min = min(x.min, y.min)
    // m.max = min(x.max, y.max)
    auto m_lower = std::min(x_min, y_min);
    auto m_upper = std::min(x_max, y_max);

    // m のドメインを絞る
    auto m_values = m_->domain().values();
    for (auto v : m_values) {
        if (v < m_lower || v > m_upper) {
            if (!m_->domain().remove(v)) {
                return false;
            }
        }
    }

    // x.min >= m.min, y.min >= m.min
    auto m_min_val = m_->min();
    auto x_values = x_->domain().values();
    for (auto v : x_values) {
        if (v < m_min_val) {
            if (!x_->domain().remove(v)) {
                return false;
            }
        }
    }

    auto y_values = y_->domain().values();
    for (auto v : y_values) {
        if (v < m_min_val) {
            if (!y_->domain().remove(v)) {
                return false;
            }
        }
    }

    return true;
}

bool IntMinConstraint::on_instantiate(Model& model, int save_point,
                                       size_t var_idx, Domain::value_type value,
                                       Domain::value_type prev_min,
                                       Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // m が確定した場合
    if (m_->is_assigned()) {
        auto m_val = m_->assigned_value().value();

        // x.min と y.min を m に制限
        if (!x_->is_assigned()) {
            auto x_min = x_->min();
            if (x_min < m_val) {
                model.enqueue_set_min(x_->id(), m_val);
            }
        }
        if (!y_->is_assigned()) {
            auto y_min = y_->min();
            if (y_min < m_val) {
                model.enqueue_set_min(y_->id(), m_val);
            }
        }

        // x または y が確定していて m と等しい場合は OK
        // 両方確定していて min != m なら矛盾
        if (x_->is_assigned() && y_->is_assigned()) {
            auto x_val = x_->assigned_value().value();
            auto y_val = y_->assigned_value().value();
            if (std::min(x_val, y_val) != m_val) {
                return false;
            }
        }
        // 片方だけ確定している場合
        else if (x_->is_assigned()) {
            auto x_val = x_->assigned_value().value();
            if (x_val == m_val) {
                // y >= m で OK
            } else {
                // y == m が必要
                if (!y_->domain().contains(m_val)) {
                    return false;
                }
            }
        } else if (y_->is_assigned()) {
            auto y_val = y_->assigned_value().value();
            if (y_val == m_val) {
                // x >= m で OK
            } else {
                // x == m が必要
                if (!x_->domain().contains(m_val)) {
                    return false;
                }
            }
        } else {
            // 両方未確定: 少なくとも一方が m になれる必要
            if (!x_->domain().contains(m_val) && !y_->domain().contains(m_val)) {
                return false;
            }
        }
    }

    // x と y が両方確定した場合、m を確定
    if (x_->is_assigned() && y_->is_assigned() && !m_->is_assigned()) {
        auto x_val = x_->assigned_value().value();
        auto y_val = y_->assigned_value().value();
        auto min_val = std::min(x_val, y_val);
        model.enqueue_instantiate(m_->id(), min_val);
    }

    // x または y が確定した場合、m の上限を更新
    if (x_->is_assigned() || y_->is_assigned()) {
        auto x_max_val = x_->is_assigned() ? x_->assigned_value().value() : x_->max();
        auto y_max_val = y_->is_assigned() ? y_->assigned_value().value() : y_->max();
        auto new_m_max = std::min(x_max_val, y_max_val);

        if (!m_->is_assigned()) {
            auto m_max = m_->max();
            if (m_max > new_m_max) {
                model.enqueue_set_max(m_->id(), new_m_max);
            }
        }
    }

    return true;
}

bool IntMinConstraint::on_final_instantiate() {
    auto x_val = x_->assigned_value().value();
    auto y_val = y_->assigned_value().value();
    auto m_val = m_->assigned_value().value();
    return m_val == std::min(x_val, y_val);
}

void IntMinConstraint::check_initial_consistency() {
    auto x_min = x_->min();
    auto x_max = x_->max();
    auto y_min = y_->min();
    auto y_max = y_->max();
    auto m_min = m_->min();
    auto m_max = m_->max();

    // m.max < min(x.min, y.min) なら矛盾
    if (m_max < std::min(x_min, y_min)) {
        set_initially_inconsistent(true);
        return;
    }

    // m.min > min(x.max, y.max) なら矛盾
    if (m_min > std::min(x_max, y_max)) {
        set_initially_inconsistent(true);
        return;
    }
}

} // namespace sabori_csp
