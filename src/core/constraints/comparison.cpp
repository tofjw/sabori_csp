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

    // 変数のモデル内インデックスを検索するヘルパー
    auto find_model_idx = [&model](const VariablePtr& var) -> size_t {
        for (size_t i = 0; i < model.variables().size(); ++i) {
            if (model.variable(i) == var) {
                return i;
            }
        }
        return SIZE_MAX;
    };

    // x == y なので、一方が確定したら他方も同じ値に固定（キューイング）
    if (x_->is_assigned() && !y_->is_assigned()) {
        auto val = x_->assigned_value().value();
        if (!y_->domain().contains(val)) {
            return false;
        }
        size_t y_idx = find_model_idx(y_);
        if (y_idx != SIZE_MAX) {
            model.enqueue_instantiate(y_idx, val);
        }
    }
    if (y_->is_assigned() && !x_->is_assigned()) {
        auto val = y_->assigned_value().value();
        if (!x_->domain().contains(val)) {
            return false;
        }
        size_t x_idx = find_model_idx(x_);
        if (x_idx != SIZE_MAX) {
            model.enqueue_instantiate(x_idx, val);
        }
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

bool IntEqReifConstraint::propagate(Model& /*model*/) {
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
    // NOTE: propagate() は初期伝播で使用され、Trail は使用しない
    if (x_->is_assigned() && y_->is_assigned() && !b_->is_assigned()) {
        bool eq = (x_->assigned_value() == y_->assigned_value());
        if (!b_->domain().contains(eq ? 1 : 0)) {
            return false;
        }
        b_->domain().assign(eq ? 1 : 0);
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

    // 変数のモデル内インデックスを検索するヘルパー
    auto find_model_idx = [&model](const VariablePtr& var) -> size_t {
        for (size_t i = 0; i < model.variables().size(); ++i) {
            if (model.variable(i) == var) {
                return i;
            }
        }
        return SIZE_MAX;
    };

    // 伝播ロジック（キューイング）
    if (b_->is_assigned()) {
        if (b_->assigned_value().value() == 1) {
            // x == y を強制
            if (x_->is_assigned() && !y_->is_assigned()) {
                auto val = x_->assigned_value().value();
                if (!y_->domain().contains(val)) {
                    return false;
                }
                size_t y_idx = find_model_idx(y_);
                if (y_idx != SIZE_MAX) {
                    model.enqueue_instantiate(y_idx, val);
                }
            }
            if (y_->is_assigned() && !x_->is_assigned()) {
                auto val = y_->assigned_value().value();
                if (!x_->domain().contains(val)) {
                    return false;
                }
                size_t x_idx = find_model_idx(x_);
                if (x_idx != SIZE_MAX) {
                    model.enqueue_instantiate(x_idx, val);
                }
            }
        } else {
            // x != y を強制
            if (x_->is_assigned()) {
                size_t y_idx = find_model_idx(y_);
                if (y_idx != SIZE_MAX) {
                    model.enqueue_remove_value(y_idx, x_->assigned_value().value());
                }
            }
            if (y_->is_assigned()) {
                size_t x_idx = find_model_idx(x_);
                if (x_idx != SIZE_MAX) {
                    model.enqueue_remove_value(x_idx, y_->assigned_value().value());
                }
            }
        }
    }

    // x と y が両方確定したら b を決定
    if (x_->is_assigned() && y_->is_assigned() && !b_->is_assigned()) {
        bool eq = (x_->assigned_value() == y_->assigned_value());
        size_t b_idx = find_model_idx(b_);
        if (b_idx != SIZE_MAX) {
            model.enqueue_instantiate(b_idx, eq ? 1 : 0);
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

    // 変数のモデル内インデックスを検索するヘルパー
    auto find_model_idx = [&model](const VariablePtr& var) -> size_t {
        for (size_t i = 0; i < model.variables().size(); ++i) {
            if (model.variable(i) == var) {
                return i;
            }
        }
        return SIZE_MAX;
    };

    // x != y なので、一方が確定したら他方からその値を削除（キューイング）
    if (x_->is_assigned()) {
        size_t y_idx = find_model_idx(y_);
        if (y_idx != SIZE_MAX) {
            model.enqueue_remove_value(y_idx, x_->assigned_value().value());
        }
    }
    if (y_->is_assigned()) {
        size_t x_idx = find_model_idx(x_);
        if (x_idx != SIZE_MAX) {
            model.enqueue_remove_value(x_idx, y_->assigned_value().value());
        }
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

    // 変数のモデル内インデックスを検索するヘルパー
    auto find_model_idx = [&model](const VariablePtr& var) -> size_t {
        for (size_t i = 0; i < model.variables().size(); ++i) {
            if (model.variable(i) == var) {
                return i;
            }
        }
        return SIZE_MAX;
    };

    // x < y: x が確定したら y の下限を更新（キューイング）
    if (x_->is_assigned()) {
        auto x_val = x_->assigned_value().value();
        // y > x_val なので y の下限は x_val + 1
        size_t y_idx = find_model_idx(y_);
        if (y_idx != SIZE_MAX) {
            model.enqueue_set_min(y_idx, x_val + 1);
        }
    }

    // y が確定したら x の上限を更新（キューイング）
    if (y_->is_assigned()) {
        auto y_val = y_->assigned_value().value();
        // x < y_val なので x の上限は y_val - 1
        size_t x_idx = find_model_idx(x_);
        if (x_idx != SIZE_MAX) {
            model.enqueue_set_max(x_idx, y_val - 1);
        }
    }

    return true;
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

bool IntLeConstraint::propagate(Model& /*model*/) {
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

    // 変数のモデル内インデックスを検索するヘルパー
    auto find_model_idx = [&model](const VariablePtr& var) -> size_t {
        for (size_t i = 0; i < model.variables().size(); ++i) {
            if (model.variable(i) == var) {
                return i;
            }
        }
        return SIZE_MAX;
    };

    // x <= y: x が確定したら y の下限を更新（キューイング）
    if (x_->is_assigned()) {
        auto x_val = x_->assigned_value().value();
        // y >= x_val
        size_t y_idx = find_model_idx(y_);
        if (y_idx != SIZE_MAX) {
            model.enqueue_set_min(y_idx, x_val);
        }
    }

    // y が確定したら x の上限を更新（キューイング）
    if (y_->is_assigned()) {
        auto y_val = y_->assigned_value().value();
        // x <= y_val
        size_t x_idx = find_model_idx(x_);
        if (x_idx != SIZE_MAX) {
            model.enqueue_set_max(x_idx, y_val);
        }
    }

    return true;
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
    }

    // If b is fixed to 0, enforce x > y
    if (b_->is_assigned() && b_->assigned_value().value() == 0) {
        // x > y: x の下限を y.min + 1 に、y の上限を x.max - 1 に
        auto y_min = y_->domain().min();
        if (y_min) {
            for (auto v : x_->domain().values()) {
                if (v <= *y_min) {
                    x_->domain().remove(v);
                }
            }
        }

        auto x_max = x_->domain().max();
        if (x_max) {
            for (auto v : y_->domain().values()) {
                if (v >= *x_max) {
                    y_->domain().remove(v);
                }
            }
        }
    }

    // If x and y bounds determine the relation, fix b
    // NOTE: propagate() は初期伝播で使用され、Trail は使用しない
    // on_instantiate で Model を通じた Trail 付き更新を行う
    auto x_max = x_->domain().max();
    auto y_min = y_->domain().min();
    auto x_min = x_->domain().min();
    auto y_max = y_->domain().max();

    if (x_max && y_min && *x_max <= *y_min) {
        // x <= y is always true
        if (!b_->domain().contains(1)) {
            return false;
        }
        if (!b_->is_assigned()) {
            b_->domain().assign(1);
        }
    } else if (x_min && y_max && *x_min > *y_max) {
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

    // 変数のモデル内インデックスを検索するヘルパー
    auto find_model_idx = [&model](const VariablePtr& var) -> size_t {
        for (size_t i = 0; i < model.variables().size(); ++i) {
            if (model.variable(i) == var) {
                return i;
            }
        }
        return SIZE_MAX;
    };

    // b が確定した場合の伝播（キューイング）
    if (b_->is_assigned()) {
        if (b_->assigned_value().value() == 1) {
            // x <= y を強制
            if (x_->is_assigned()) {
                auto x_val = x_->assigned_value().value();
                size_t y_idx = find_model_idx(y_);
                if (y_idx != SIZE_MAX) {
                    // y >= x_val
                    model.enqueue_set_min(y_idx, x_val);
                }
            }
            if (y_->is_assigned()) {
                auto y_val = y_->assigned_value().value();
                size_t x_idx = find_model_idx(x_);
                if (x_idx != SIZE_MAX) {
                    // x <= y_val
                    model.enqueue_set_max(x_idx, y_val);
                }
            }
        } else {
            // x > y を強制
            if (x_->is_assigned()) {
                auto x_val = x_->assigned_value().value();
                size_t y_idx = find_model_idx(y_);
                if (y_idx != SIZE_MAX) {
                    // y < x_val, つまり y <= x_val - 1
                    model.enqueue_set_max(y_idx, x_val - 1);
                }
            }
            if (y_->is_assigned()) {
                auto y_val = y_->assigned_value().value();
                size_t x_idx = find_model_idx(x_);
                if (x_idx != SIZE_MAX) {
                    // x > y_val, つまり x >= y_val + 1
                    model.enqueue_set_min(x_idx, y_val + 1);
                }
            }
        }
    }

    // x と y の bounds から b を決定できるか
    auto x_max = x_->domain().max();
    auto y_min = y_->domain().min();
    auto x_min = x_->domain().min();
    auto y_max = y_->domain().max();

    if (!b_->is_assigned()) {
        size_t b_idx = find_model_idx(b_);
        if (b_idx != SIZE_MAX) {
            if (x_max && y_min && *x_max <= *y_min) {
                // x <= y is always true
                model.enqueue_instantiate(b_idx, 1);
            } else if (x_min && y_max && *x_min > *y_max) {
                // x <= y is always false
                model.enqueue_instantiate(b_idx, 0);
            }
        }
    }

    // x と y が両方確定したら b を決定
    if (x_->is_assigned() && y_->is_assigned() && !b_->is_assigned()) {
        bool le = (x_->assigned_value() <= y_->assigned_value());
        size_t b_idx = find_model_idx(b_);
        if (b_idx != SIZE_MAX) {
            model.enqueue_instantiate(b_idx, le ? 1 : 0);
        }
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
            auto x_min = x_->domain().min();
            auto y_max = y_->domain().max();
            if (x_min && y_max && *x_min > *y_max) {
                set_initially_inconsistent(true);
            }
        } else {
            // x > y が必要: x.max <= y.min なら矛盾
            auto x_max = x_->domain().max();
            auto y_min = y_->domain().min();
            if (x_max && y_min && *x_max <= *y_min) {
                set_initially_inconsistent(true);
            }
        }
    }
}

} // namespace sabori_csp
