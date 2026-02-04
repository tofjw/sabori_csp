#include "sabori_csp/constraints/arithmetic.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <set>
#include <cmath>

namespace sabori_csp {

// ============================================================================
// IntTimesConstraint implementation
// ============================================================================

IntTimesConstraint::IntTimesConstraint(VariablePtr x, VariablePtr y, VariablePtr z)
    : Constraint({x, y, z})
    , x_(std::move(x))
    , y_(std::move(y))
    , z_(std::move(z)) {
    // var_ptr_to_idx_ を構築
    var_ptr_to_idx_[x_.get()] = 0;
    var_ptr_to_idx_[y_.get()] = 1;
    var_ptr_to_idx_[z_.get()] = 2;

    check_initial_consistency();
}

std::string IntTimesConstraint::name() const {
    return "int_times";
}

std::vector<VariablePtr> IntTimesConstraint::variables() const {
    return {x_, y_, z_};
}

std::optional<bool> IntTimesConstraint::is_satisfied() const {
    if (x_->is_assigned() && y_->is_assigned() && z_->is_assigned()) {
        return x_->assigned_value().value() * y_->assigned_value().value()
               == z_->assigned_value().value();
    }
    return std::nullopt;
}

bool IntTimesConstraint::propagate(Model& /*model*/) {
    return propagate_bounds();
}

bool IntTimesConstraint::propagate_bounds() {
    // x * y = z の bounds propagation

    auto x_min = x_->domain().min();
    auto x_max = x_->domain().max();
    auto y_min = y_->domain().min();
    auto y_max = y_->domain().max();
    auto z_min = z_->domain().min();
    auto z_max = z_->domain().max();

    if (!x_min || !x_max || !y_min || !y_max || !z_min || !z_max) {
        return false;
    }

    // z の範囲を x * y の可能な範囲で制限
    // 負の数を含む場合、積の範囲は複雑になる
    std::vector<Domain::value_type> products = {
        *x_min * *y_min,
        *x_min * *y_max,
        *x_max * *y_min,
        *x_max * *y_max
    };
    auto [prod_min, prod_max] = std::minmax_element(products.begin(), products.end());

    // z のドメインから範囲外の値を削除
    auto z_vals = z_->domain().values();
    for (auto v : z_vals) {
        if (v < *prod_min || v > *prod_max) {
            if (!z_->domain().remove(v)) {
                return false;
            }
        }
    }

    // x が 0 のみを含む場合、z = 0
    if (*x_min == 0 && *x_max == 0) {
        if (!z_->domain().contains(0)) {
            return false;
        }
        z_vals = z_->domain().values();
        for (auto v : z_vals) {
            if (v != 0) {
                if (!z_->domain().remove(v)) {
                    return false;
                }
            }
        }
    }

    // y が 0 のみを含む場合、z = 0
    if (*y_min == 0 && *y_max == 0) {
        if (!z_->domain().contains(0)) {
            return false;
        }
        z_vals = z_->domain().values();
        for (auto v : z_vals) {
            if (v != 0) {
                if (!z_->domain().remove(v)) {
                    return false;
                }
            }
        }
    }

    // z が 0 のみを含む場合、x = 0 または y = 0
    z_min = z_->domain().min();
    z_max = z_->domain().max();
    if (z_min && z_max && *z_min == 0 && *z_max == 0) {
        // x か y のどちらかが 0 を含む必要がある
        if (!x_->domain().contains(0) && !y_->domain().contains(0)) {
            return false;
        }
    }

    return true;
}

bool IntTimesConstraint::on_instantiate(Model& model, int save_point,
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

    // x * y = z の伝播
    if (x_->is_assigned() && y_->is_assigned() && !z_->is_assigned()) {
        // x と y が確定したら z を確定
        auto product = x_->assigned_value().value() * y_->assigned_value().value();
        if (!z_->domain().contains(product)) {
            return false;
        }
        size_t z_idx = find_model_idx(z_);
        if (z_idx != SIZE_MAX) {
            model.enqueue_instantiate(z_idx, product);
        }
    }

    if (x_->is_assigned() && z_->is_assigned() && !y_->is_assigned()) {
        // x と z が確定、y を計算
        auto x_val = x_->assigned_value().value();
        auto z_val = z_->assigned_value().value();
        if (x_val == 0) {
            // 0 * y = z → z は 0 でなければならない
            if (z_val != 0) {
                return false;
            }
            // y は任意（変更なし）
        } else {
            // y = z / x （割り切れる場合のみ）
            if (z_val % x_val != 0) {
                return false;
            }
            auto y_val = z_val / x_val;
            if (!y_->domain().contains(y_val)) {
                return false;
            }
            size_t y_idx = find_model_idx(y_);
            if (y_idx != SIZE_MAX) {
                model.enqueue_instantiate(y_idx, y_val);
            }
        }
    }

    if (y_->is_assigned() && z_->is_assigned() && !x_->is_assigned()) {
        // y と z が確定、x を計算
        auto y_val = y_->assigned_value().value();
        auto z_val = z_->assigned_value().value();
        if (y_val == 0) {
            // x * 0 = z → z は 0 でなければならない
            if (z_val != 0) {
                return false;
            }
            // x は任意（変更なし）
        } else {
            // x = z / y （割り切れる場合のみ）
            if (z_val % y_val != 0) {
                return false;
            }
            auto x_val = z_val / y_val;
            if (!x_->domain().contains(x_val)) {
                return false;
            }
            size_t x_idx = find_model_idx(x_);
            if (x_idx != SIZE_MAX) {
                model.enqueue_instantiate(x_idx, x_val);
            }
        }
    }

    // 1変数のみ確定の場合、z のドメインをフィルタリング
    if (x_->is_assigned() && !y_->is_assigned() && !z_->is_assigned()) {
        auto x_val = x_->assigned_value().value();
        if (x_val == 0) {
            // z = 0 に確定
            if (!z_->domain().contains(0)) {
                return false;
            }
            size_t z_idx = find_model_idx(z_);
            if (z_idx != SIZE_MAX) {
                model.enqueue_instantiate(z_idx, 0);
            }
        } else {
            // z のドメインから x * y で生成できない値を削除
            std::set<Domain::value_type> valid_z;
            for (auto y_val : y_->domain().values()) {
                valid_z.insert(x_val * y_val);
            }
            size_t z_idx = find_model_idx(z_);
            for (auto z_val : z_->domain().values()) {
                if (valid_z.count(z_val) == 0) {
                    if (z_idx != SIZE_MAX) {
                        model.enqueue_remove_value(z_idx, z_val);
                    }
                }
            }
        }
    }

    if (y_->is_assigned() && !x_->is_assigned() && !z_->is_assigned()) {
        auto y_val = y_->assigned_value().value();
        if (y_val == 0) {
            // z = 0 に確定
            if (!z_->domain().contains(0)) {
                return false;
            }
            size_t z_idx = find_model_idx(z_);
            if (z_idx != SIZE_MAX) {
                model.enqueue_instantiate(z_idx, 0);
            }
        } else {
            // z のドメインから x * y で生成できない値を削除
            std::set<Domain::value_type> valid_z;
            for (auto x_val : x_->domain().values()) {
                valid_z.insert(x_val * y_val);
            }
            size_t z_idx = find_model_idx(z_);
            for (auto z_val : z_->domain().values()) {
                if (valid_z.count(z_val) == 0) {
                    if (z_idx != SIZE_MAX) {
                        model.enqueue_remove_value(z_idx, z_val);
                    }
                }
            }
        }
    }

    return true;
}

bool IntTimesConstraint::on_final_instantiate() {
    return x_->assigned_value().value() * y_->assigned_value().value()
           == z_->assigned_value().value();
}

bool IntTimesConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                                 size_t last_var_internal_idx) {
    auto find_model_idx = [&model](const VariablePtr& var) -> size_t {
        for (size_t i = 0; i < model.variables().size(); ++i) {
            if (model.variable(i) == var) {
                return i;
            }
        }
        return SIZE_MAX;
    };

    if (last_var_internal_idx == 0) {
        // x が未確定、y と z は確定
        auto y_val = y_->assigned_value().value();
        auto z_val = z_->assigned_value().value();
        if (y_val == 0) {
            if (z_val != 0) {
                return false;
            }
            // x は任意、確定しない
        } else {
            if (z_val % y_val != 0) {
                return false;
            }
            auto x_val = z_val / y_val;
            if (!x_->domain().contains(x_val)) {
                return false;
            }
            size_t x_idx = find_model_idx(x_);
            if (x_idx != SIZE_MAX) {
                model.enqueue_instantiate(x_idx, x_val);
            }
        }
    } else if (last_var_internal_idx == 1) {
        // y が未確定、x と z は確定
        auto x_val = x_->assigned_value().value();
        auto z_val = z_->assigned_value().value();
        if (x_val == 0) {
            if (z_val != 0) {
                return false;
            }
            // y は任意、確定しない
        } else {
            if (z_val % x_val != 0) {
                return false;
            }
            auto y_val = z_val / x_val;
            if (!y_->domain().contains(y_val)) {
                return false;
            }
            size_t y_idx = find_model_idx(y_);
            if (y_idx != SIZE_MAX) {
                model.enqueue_instantiate(y_idx, y_val);
            }
        }
    } else if (last_var_internal_idx == 2) {
        // z が未確定、x と y は確定
        auto product = x_->assigned_value().value() * y_->assigned_value().value();
        if (!z_->domain().contains(product)) {
            return false;
        }
        size_t z_idx = find_model_idx(z_);
        if (z_idx != SIZE_MAX) {
            model.enqueue_instantiate(z_idx, product);
        }
    }

    return true;
}

void IntTimesConstraint::rewind_to(int /*save_point*/) {
    // 状態を持たないので何もしない
}

void IntTimesConstraint::check_initial_consistency() {
    // x * y = z の初期整合性チェック

    // 全て確定している場合、等式を確認
    if (x_->is_assigned() && y_->is_assigned() && z_->is_assigned()) {
        if (x_->assigned_value().value() * y_->assigned_value().value()
            != z_->assigned_value().value()) {
            set_initially_inconsistent(true);
            return;
        }
    }

    // bounds の簡易チェック
    auto x_min = x_->domain().min();
    auto x_max = x_->domain().max();
    auto y_min = y_->domain().min();
    auto y_max = y_->domain().max();
    auto z_min = z_->domain().min();
    auto z_max = z_->domain().max();

    if (!x_min || !x_max || !y_min || !y_max || !z_min || !z_max) {
        set_initially_inconsistent(true);
        return;
    }

    // 積の範囲を計算
    std::vector<Domain::value_type> products = {
        *x_min * *y_min,
        *x_min * *y_max,
        *x_max * *y_min,
        *x_max * *y_max
    };
    auto [prod_min, prod_max] = std::minmax_element(products.begin(), products.end());

    // z の範囲と積の範囲が交差しなければ矛盾
    if (*prod_max < *z_min || *prod_min > *z_max) {
        set_initially_inconsistent(true);
    }
}

// ============================================================================
// IntAbsConstraint implementation
// ============================================================================

IntAbsConstraint::IntAbsConstraint(VariablePtr x, VariablePtr y)
    : Constraint({x, y})
    , x_(std::move(x))
    , y_(std::move(y)) {
    check_initial_consistency();
}

std::string IntAbsConstraint::name() const {
    return "int_abs";
}

std::vector<VariablePtr> IntAbsConstraint::variables() const {
    return {x_, y_};
}

std::optional<bool> IntAbsConstraint::is_satisfied() const {
    if (x_->is_assigned() && y_->is_assigned()) {
        auto x_val = x_->assigned_value().value();
        auto y_val = y_->assigned_value().value();
        return (x_val >= 0 ? x_val : -x_val) == y_val;
    }
    return std::nullopt;
}

bool IntAbsConstraint::propagate(Model& /*model*/) {
    return propagate_bounds();
}

bool IntAbsConstraint::propagate_bounds() {
    // |x| = y の bounds propagation

    auto x_min = x_->domain().min();
    auto x_max = x_->domain().max();
    auto y_min = y_->domain().min();
    auto y_max = y_->domain().max();

    if (!x_min || !x_max || !y_min || !y_max) {
        return false;
    }

    // y >= 0 を強制
    if (*y_min < 0) {
        auto y_vals = y_->domain().values();
        for (auto v : y_vals) {
            if (v < 0) {
                if (!y_->domain().remove(v)) {
                    return false;
                }
            }
        }
        y_min = y_->domain().min();
        y_max = y_->domain().max();
    }

    // y の範囲を |x| の可能な範囲で制限
    // |x| の最小値: x が 0 を含む場合は 0、そうでなければ min(|x_min|, |x_max|)
    // |x| の最大値: max(|x_min|, |x_max|)
    Domain::value_type abs_x_min, abs_x_max;
    auto abs_x_min_val = (*x_min >= 0) ? *x_min : -*x_min;
    auto abs_x_max_val = (*x_max >= 0) ? *x_max : -*x_max;

    if (*x_min <= 0 && *x_max >= 0) {
        // x が 0 を含む
        abs_x_min = 0;
    } else {
        abs_x_min = std::min(abs_x_min_val, abs_x_max_val);
    }
    abs_x_max = std::max(abs_x_min_val, abs_x_max_val);

    // y のドメインから範囲外の値を削除
    auto y_vals = y_->domain().values();
    for (auto v : y_vals) {
        if (v < abs_x_min || v > abs_x_max) {
            if (!y_->domain().remove(v)) {
                return false;
            }
        }
    }

    // x の範囲を y の範囲から制限
    // |x| <= y_max → -y_max <= x <= y_max
    y_max = y_->domain().max();
    if (y_max) {
        auto x_vals = x_->domain().values();
        for (auto v : x_vals) {
            if (v < -*y_max || v > *y_max) {
                if (!x_->domain().remove(v)) {
                    return false;
                }
            }
        }
    }

    // x のドメインから、対応する |x| が y のドメインにない値を削除
    auto x_vals = x_->domain().values();
    y_vals = y_->domain().values();
    std::set<Domain::value_type> y_set(y_vals.begin(), y_vals.end());
    for (auto v : x_vals) {
        auto abs_v = (v >= 0) ? v : -v;
        if (y_set.count(abs_v) == 0) {
            if (!x_->domain().remove(v)) {
                return false;
            }
        }
    }

    // y のドメインから、対応する x が x のドメインにない値を削除
    x_vals = x_->domain().values();
    std::set<Domain::value_type> x_set(x_vals.begin(), x_vals.end());
    y_vals = y_->domain().values();
    for (auto v : y_vals) {
        // v = |x| となる x は v または -v
        if (x_set.count(v) == 0 && x_set.count(-v) == 0) {
            if (!y_->domain().remove(v)) {
                return false;
            }
        }
    }

    return !x_->domain().empty() && !y_->domain().empty();
}

bool IntAbsConstraint::on_instantiate(Model& model, int save_point,
                                       size_t var_idx, Domain::value_type value,
                                       Domain::value_type prev_min,
                                       Domain::value_type prev_max) {
    // 基底クラスの処理
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

    // x が確定したら y を確定
    if (x_->is_assigned() && !y_->is_assigned()) {
        auto x_val = x_->assigned_value().value();
        auto abs_x = (x_val >= 0) ? x_val : -x_val;
        if (!y_->domain().contains(abs_x)) {
            return false;
        }
        size_t y_idx = find_model_idx(y_);
        if (y_idx != SIZE_MAX) {
            model.enqueue_instantiate(y_idx, abs_x);
        }
    }

    // y が確定したら x のドメインをフィルタリング
    if (y_->is_assigned() && !x_->is_assigned()) {
        auto y_val = y_->assigned_value().value();
        size_t x_idx = find_model_idx(x_);
        if (x_idx != SIZE_MAX) {
            // x は y_val または -y_val のみ
            auto x_vals = x_->domain().values();
            for (auto v : x_vals) {
                if (v != y_val && v != -y_val) {
                    model.enqueue_remove_value(x_idx, v);
                }
            }
        }
    }

    return true;
}

bool IntAbsConstraint::on_final_instantiate() {
    auto x_val = x_->assigned_value().value();
    auto y_val = y_->assigned_value().value();
    return (x_val >= 0 ? x_val : -x_val) == y_val;
}

void IntAbsConstraint::check_initial_consistency() {
    // |x| = y の初期整合性チェック

    // y < 0 なら矛盾
    auto y_min = y_->domain().min();
    if (y_min && *y_min < 0) {
        // y に負の値しかない場合は矛盾
        auto y_max = y_->domain().max();
        if (y_max && *y_max < 0) {
            set_initially_inconsistent(true);
            return;
        }
    }

    // 全て確定している場合、等式を確認
    if (x_->is_assigned() && y_->is_assigned()) {
        auto x_val = x_->assigned_value().value();
        auto y_val = y_->assigned_value().value();
        if ((x_val >= 0 ? x_val : -x_val) != y_val) {
            set_initially_inconsistent(true);
        }
    }
}

} // namespace sabori_csp
