#include "sabori_csp/constraints/arithmetic.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <set>
#include <cmath>

namespace sabori_csp {

namespace {

/// 整数除算（-∞ 方向への丸め）
Domain::value_type div_floor_int(Domain::value_type a, Domain::value_type b) {
    auto d = a / b;
    auto r = a % b;
    if (r != 0 && ((a ^ b) < 0)) {
        d--;
    }
    return d;
}

/// 整数除算（+∞ 方向への丸め）
Domain::value_type div_ceil_int(Domain::value_type a, Domain::value_type b) {
    auto d = a / b;
    auto r = a % b;
    if (r != 0 && ((a ^ b) >= 0)) {
        d++;
    }
    return d;
}

} // anonymous namespace

// ============================================================================
// IntTimesConstraint implementation
// ============================================================================

IntTimesConstraint::IntTimesConstraint(VariablePtr x, VariablePtr y, VariablePtr z)
    : Constraint({x, y, z})
    , x_(std::move(x))
    , y_(std::move(y))
    , z_(std::move(z))
    , x_id_(x_->id())
    , y_id_(y_->id())
    , z_id_(z_->id()) {
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

bool IntTimesConstraint::presolve(Model& model) {
    return propagate_bounds(model);
}

bool IntTimesConstraint::propagate_bounds(Model& model) {
    // x * y = z の bounds propagation

    auto x_min = model.var_min(x_->id());
    auto x_max = model.var_max(x_->id());
    auto y_min = model.var_min(y_->id());
    auto y_max = model.var_max(y_->id());
    auto z_min = model.var_min(z_->id());
    auto z_max = model.var_max(z_->id());

    // z の範囲を x * y の可能な範囲で制限
    // 負の数を含む場合、積の範囲は複雑になる
    std::vector<Domain::value_type> products = {
        x_min * y_min,
        x_min * y_max,
        x_max * y_min,
        x_max * y_max
    };
    auto [prod_min, prod_max] = std::minmax_element(products.begin(), products.end());

    // z のドメインから範囲外の値を削除
    if (!z_->remove_below(*prod_min)) return false;
    if (!z_->remove_above(*prod_max)) return false;

    // x が 0 のみを含む場合、z = 0
    if (x_min == 0 && x_max == 0) {
        if (!z_->assign(0)) {
            return false;
        }
    }

    // y が 0 のみを含む場合、z = 0
    if (y_min == 0 && y_max == 0) {
        if (!z_->assign(0)) {
            return false;
        }
    }

    // z が 0 のみを含む場合、x = 0 または y = 0
    z_min = model.var_min(z_->id());
    z_max = model.var_max(z_->id());
    if (z_min == 0 && z_max == 0) {
        // x か y のどちらかが 0 を含む必要がある
        if (!x_->domain().contains(0) && !y_->domain().contains(0)) {
            return false;
        }
    }

    return true;
}

bool IntTimesConstraint::on_instantiate(Model& model, int save_point,
                                         size_t var_idx, size_t internal_var_idx, Domain::value_type value,
                                         Domain::value_type prev_min,
                                         Domain::value_type prev_max) {
    // 基底クラスの 2WL 処理
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // x * y = z の伝播
    if (model.is_instantiated(x_id_) && model.is_instantiated(y_id_) && !model.is_instantiated(z_id_)) {
        // x と y が確定したら z を確定
        auto product = model.value(x_id_) * model.value(y_id_);
        if (!z_->domain().contains(product)) {
            return false;
        }
        model.enqueue_instantiate(z_id_, product);
    }

    if (model.is_instantiated(x_id_) && model.is_instantiated(z_id_) && !model.is_instantiated(y_id_)) {
        // x と z が確定、y を計算
        auto x_val = model.value(x_id_);
        auto z_val = model.value(z_id_);
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
            model.enqueue_instantiate(y_id_, y_val);
        }
    }

    if (model.is_instantiated(y_id_) && model.is_instantiated(z_id_) && !model.is_instantiated(x_id_)) {
        // y と z が確定、x を計算
        auto y_val = model.value(y_id_);
        auto z_val = model.value(z_id_);
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
            model.enqueue_instantiate(x_id_, x_val);
        }
    }

    // 1変数のみ確定の場合、z のドメインをフィルタリング
    if (model.is_instantiated(x_id_) && !model.is_instantiated(y_id_) && !model.is_instantiated(z_id_)) {
        auto x_val = model.value(x_id_);
        if (x_val == 0) {
            // z = 0 に確定
            if (!z_->domain().contains(0)) {
                return false;
            }
            model.enqueue_instantiate(z_id_, 0);
        } else {
            // z のバウンドを x_val * y の範囲で制限
            auto y_min = model.var_min(y_id_);
            auto y_max = model.var_max(y_id_);
            Domain::value_type new_z_min, new_z_max;
            if (x_val > 0) {
                new_z_min = x_val * y_min;
                new_z_max = x_val * y_max;
            } else {
                new_z_min = x_val * y_max;
                new_z_max = x_val * y_min;
            }
            model.enqueue_set_min(z_id_, new_z_min);
            model.enqueue_set_max(z_id_, new_z_max);
        }
    }

    if (model.is_instantiated(y_id_) && !model.is_instantiated(x_id_) && !model.is_instantiated(z_id_)) {
        auto y_val = model.value(y_id_);
        if (y_val == 0) {
            // z = 0 に確定
            if (!z_->domain().contains(0)) {
                return false;
            }
            model.enqueue_instantiate(z_id_, 0);
        } else {
            // z のバウンドを x * y_val の範囲で制限
            auto x_min = model.var_min(x_id_);
            auto x_max = model.var_max(x_id_);
            Domain::value_type new_z_min, new_z_max;
            if (y_val > 0) {
                new_z_min = x_min * y_val;
                new_z_max = x_max * y_val;
            } else {
                new_z_min = x_max * y_val;
                new_z_max = x_min * y_val;
            }
            model.enqueue_set_min(z_id_, new_z_min);
            model.enqueue_set_max(z_id_, new_z_max);
        }
    }

    return true;
}

bool IntTimesConstraint::on_set_min(Model& model, int /*save_point*/,
                                     size_t /*var_idx*/, size_t /*internal_var_idx*/,
                                     Domain::value_type /*new_min*/,
                                     Domain::value_type /*old_min*/) {
    auto x_min = model.var_min(x_id_);
    auto x_max = model.var_max(x_id_);
    auto y_min = model.var_min(y_id_);
    auto y_max = model.var_max(y_id_);
    auto z_min = model.var_min(z_id_);
    auto z_max = model.var_max(z_id_);

    // Forward: z ∈ hull(x * y)
    model.enqueue_set_min(z_id_, std::min({x_min * y_min, x_min * y_max,
                                            x_max * y_min, x_max * y_max}));
    model.enqueue_set_max(z_id_, std::max({x_min * y_min, x_min * y_max,
                                            x_max * y_min, x_max * y_max}));

    // Backward: x ∈ hull(z / y)  （y が 0 を含まない場合のみ）
    if (y_min > 0 || y_max < 0) {
        model.enqueue_set_min(x_id_, std::min({div_ceil_int(z_min, y_min), div_ceil_int(z_min, y_max),
                                                div_ceil_int(z_max, y_min), div_ceil_int(z_max, y_max)}));
        model.enqueue_set_max(x_id_, std::max({div_floor_int(z_min, y_min), div_floor_int(z_min, y_max),
                                                div_floor_int(z_max, y_min), div_floor_int(z_max, y_max)}));
    }

    // Backward: y ∈ hull(z / x)  （x が 0 を含まない場合のみ）
    if (x_min > 0 || x_max < 0) {
        model.enqueue_set_min(y_id_, std::min({div_ceil_int(z_min, x_min), div_ceil_int(z_min, x_max),
                                                div_ceil_int(z_max, x_min), div_ceil_int(z_max, x_max)}));
        model.enqueue_set_max(y_id_, std::max({div_floor_int(z_min, x_min), div_floor_int(z_min, x_max),
                                                div_floor_int(z_max, x_min), div_floor_int(z_max, x_max)}));
    }

    return true;
}

bool IntTimesConstraint::on_set_max(Model& model, int save_point,
                                     size_t var_idx, size_t internal_var_idx,
                                     Domain::value_type new_max,
                                     Domain::value_type /*old_max*/) {
    // on_set_min と同じ伝播（全変数の境界が相互に影響するため）
    return on_set_min(model, save_point, var_idx, internal_var_idx, new_max, 0);
}

bool IntTimesConstraint::on_final_instantiate() {
    return x_->assigned_value().value() * y_->assigned_value().value()
           == z_->assigned_value().value();
}

bool IntTimesConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                                 size_t last_var_internal_idx) {
    if (last_var_internal_idx == 0) {
        // x が未確定、y と z は確定
        auto y_val = model.value(y_id_);
        auto z_val = model.value(z_id_);
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
            model.enqueue_instantiate(x_id_, x_val);
        }
    } else if (last_var_internal_idx == 1) {
        // y が未確定、x と z は確定
        auto x_val = model.value(x_id_);
        auto z_val = model.value(z_id_);
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
            model.enqueue_instantiate(y_id_, y_val);
        }
    } else if (last_var_internal_idx == 2) {
        // z が未確定、x と y は確定
        auto product = model.value(x_id_) * model.value(y_id_);
        if (!z_->domain().contains(product)) {
            return false;
        }
        model.enqueue_instantiate(z_id_, product);
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
    auto x_min = x_->min();
    auto x_max = x_->max();
    auto y_min = y_->min();
    auto y_max = y_->max();
    auto z_min = z_->min();
    auto z_max = z_->max();

    // 積の範囲を計算
    std::vector<Domain::value_type> products = {
        x_min * y_min,
        x_min * y_max,
        x_max * y_min,
        x_max * y_max
    };
    auto [prod_min, prod_max] = std::minmax_element(products.begin(), products.end());

    // z の範囲と積の範囲が交差しなければ矛盾
    if (*prod_max < z_min || *prod_min > z_max) {
        set_initially_inconsistent(true);
    }
}

// ============================================================================
// IntAbsConstraint implementation
// ============================================================================

IntAbsConstraint::IntAbsConstraint(VariablePtr x, VariablePtr y)
    : Constraint({x, y})
    , x_(std::move(x))
    , y_(std::move(y))
    , x_id_(x_->id())
    , y_id_(y_->id()) {
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

bool IntAbsConstraint::presolve(Model& model) {
    return propagate_bounds(model);
}

bool IntAbsConstraint::propagate_bounds(Model& model) {
    // |x| = y の bounds propagation

    auto x_min = model.var_min(x_->id());
    auto x_max = model.var_max(x_->id());
    auto y_min = model.var_min(y_->id());
    auto y_max = model.var_max(y_->id());

    // y >= 0 を強制
    if (y_min < 0) {
        if (!y_->remove_below(0)) return false;
        y_min = model.var_min(y_->id());
        y_max = model.var_max(y_->id());
    }

    // y の範囲を |x| の可能な範囲で制限
    // |x| の最小値: x が 0 を含む場合は 0、そうでなければ min(|x_min|, |x_max|)
    // |x| の最大値: max(|x_min|, |x_max|)
    Domain::value_type abs_x_min, abs_x_max;
    auto abs_x_min_val = (x_min >= 0) ? x_min : -x_min;
    auto abs_x_max_val = (x_max >= 0) ? x_max : -x_max;

    if (x_min <= 0 && x_max >= 0) {
        // x が 0 を含む
        abs_x_min = 0;
    } else {
        abs_x_min = std::min(abs_x_min_val, abs_x_max_val);
    }
    abs_x_max = std::max(abs_x_min_val, abs_x_max_val);

    // y のドメインから範囲外の値を削除
    if (!y_->remove_below(abs_x_min)) return false;
    if (!y_->remove_above(abs_x_max)) return false;

    // x の範囲を y の範囲から制限
    // |x| <= y_max → -y_max <= x <= y_max
    y_max = model.var_max(y_->id());
    if (!x_->remove_below(-y_max)) return false;
    if (!x_->remove_above(y_max)) return false;

    // x のドメインから、対応する |x| が y のドメインにない値を削除
    auto x_vals = x_->domain().values();
    auto y_vals = y_->domain().values();
    std::set<Domain::value_type> y_set(y_vals.begin(), y_vals.end());
    for (auto v : x_vals) {
        auto abs_v = (v >= 0) ? v : -v;
        if (y_set.count(abs_v) == 0) {
            if (!x_->remove(v)) {
                return false;
            }
        }
    }

    // y のドメインから、対応する x が x のドメインにない値を削除
    x_vals = x_->domain().values();
    std::set<Domain::value_type> x_set(x_vals.begin(), x_vals.end());
    auto y_vals2 = y_->domain().values();
    for (auto v : y_vals2) {
        // v = |x| となる x は v または -v
        if (x_set.count(v) == 0 && x_set.count(-v) == 0) {
            if (!y_->remove(v)) {
                return false;
            }
        }
    }

    return !x_->domain().empty() && !y_->domain().empty();
}

bool IntAbsConstraint::on_instantiate(Model& model, int save_point,
                                       size_t var_idx, size_t internal_var_idx, Domain::value_type value,
                                       Domain::value_type prev_min,
                                       Domain::value_type prev_max) {
    // 基底クラスの処理
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // x が確定したら y を確定
    if (model.is_instantiated(x_id_) && !model.is_instantiated(y_id_)) {
        auto x_val = model.value(x_id_);
        auto abs_x = (x_val >= 0) ? x_val : -x_val;
        if (!y_->domain().contains(abs_x)) {
            return false;
        }
        model.enqueue_instantiate(y_id_, abs_x);
    }

    // y が確定したら x を |x| = y に制約
    if (model.is_instantiated(y_id_) && !model.is_instantiated(x_id_)) {
        auto y_val = model.value(y_id_);
        if (y_val == 0) {
            if (!x_->domain().contains(0)) {
                return false;
            }
            model.enqueue_instantiate(x_id_, 0);
        } else {
            bool has_pos = x_->domain().contains(y_val);
            bool has_neg = x_->domain().contains(-y_val);
            if (!has_pos && !has_neg) {
                return false;
            }
            if (has_pos && !has_neg) {
                model.enqueue_instantiate(x_id_, y_val);
            } else if (!has_pos && has_neg) {
                model.enqueue_instantiate(x_id_, -y_val);
            } else {
                // 両方可能: bounds を絞り、中間値を除去
                model.enqueue_set_min(x_id_, -y_val);
                model.enqueue_set_max(x_id_, y_val);
                for (auto v = -y_val + 1; v < y_val; ++v) {
                    model.enqueue_remove_value(x_id_, v);
                }
            }
        }
    }

    // 両方確定: 整合性チェック（基底クラスの2WLが2変数制約で
    // on_final_instantiate を呼ばないため、ここで検査する）
    if (model.is_instantiated(x_id_) && model.is_instantiated(y_id_)) {
        auto x_val = model.value(x_id_);
        auto y_val = model.value(y_id_);
        if ((x_val >= 0 ? x_val : -x_val) != y_val) {
            return false;
        }
    }

    return true;
}

bool IntAbsConstraint::on_set_min(Model& model, int /*save_point*/,
                                   size_t /*var_idx*/, size_t /*internal_var_idx*/,
                                   Domain::value_type /*new_min*/,
                                   Domain::value_type /*old_min*/) {
    auto x_min = model.var_min(x_id_);
    auto x_max = model.var_max(x_id_);
    auto y_min = model.var_min(y_id_);
    auto y_max = model.var_max(y_id_);

    // x → y: y = |x|
    auto abs_x_min = (x_min >= 0) ? x_min : -x_min;
    auto abs_x_max = (x_max >= 0) ? x_max : -x_max;
    model.enqueue_set_max(y_id_, std::max(abs_x_min, abs_x_max));
    if (x_min > 0 || x_max < 0) {
        // x が 0 を含まない → y_min = min(|x_min|, |x_max|)
        model.enqueue_set_min(y_id_, std::min(abs_x_min, abs_x_max));
    }

    // y → x: -y_max <= x <= y_max
    model.enqueue_set_min(x_id_, -y_max);
    model.enqueue_set_max(x_id_, y_max);

    // |x| >= y_min のバウンド伝播
    if (x_min >= 0) {
        // x >= 0 なので |x| = x >= y_min
        model.enqueue_set_min(x_id_, y_min);
    } else if (x_max <= 0) {
        // x <= 0 なので |x| = -x >= y_min → x <= -y_min
        model.enqueue_set_max(x_id_, -y_min);
    }

    return true;
}

bool IntAbsConstraint::on_set_max(Model& model, int save_point,
                                   size_t var_idx, size_t internal_var_idx,
                                   Domain::value_type new_max,
                                   Domain::value_type /*old_max*/) {
    // |x| = y は全変数の境界が相互に影響するため on_set_min と同一のロジック
    return on_set_min(model, save_point, var_idx, internal_var_idx, new_max, 0);
}

bool IntAbsConstraint::on_final_instantiate() {
    auto x_val = x_->assigned_value().value();
    auto y_val = y_->assigned_value().value();
    return (x_val >= 0 ? x_val : -x_val) == y_val;
}

void IntAbsConstraint::check_initial_consistency() {
    // |x| = y の初期整合性チェック

    // y < 0 なら矛盾
    auto y_min = y_->min();
    if (y_min < 0) {
        // y に負の値しかない場合は矛盾
        auto y_max = y_->max();
        if (y_max < 0) {
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
