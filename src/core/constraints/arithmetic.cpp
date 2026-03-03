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
    : Constraint(extract_var_ids({x, y, z}))
    , x_id_(x->id())
    , y_id_(y->id())
    , z_id_(z->id()) {
}

std::string IntTimesConstraint::name() const {
    return "int_times";
}

PresolveResult IntTimesConstraint::presolve(Model& model) {
    bool changed = false;
    auto x_min = model.var_min(x_id_);
    auto x_max = model.var_max(x_id_);
    auto y_min = model.var_min(y_id_);
    auto y_max = model.var_max(y_id_);

    std::vector<Domain::value_type> products = {
        x_min * y_min, x_min * y_max, x_max * y_min, x_max * y_max
    };
    auto [prod_min, prod_max] = std::minmax_element(products.begin(), products.end());

    if (model.var_min(z_id_) < *prod_min) {
        if (!model.variable(z_id_)->remove_below(*prod_min)) return PresolveResult::Contradiction;
        changed = true;
    }
    if (model.var_max(z_id_) > *prod_max) {
        if (!model.variable(z_id_)->remove_above(*prod_max)) return PresolveResult::Contradiction;
        changed = true;
    }

    if (x_min == 0 && x_max == 0) {
        if (!model.variable(z_id_)->is_assigned() || model.variable(z_id_)->assigned_value().value() != 0) {
            if (!model.variable(z_id_)->assign(0)) return PresolveResult::Contradiction;
            changed = true;
        }
    }
    if (y_min == 0 && y_max == 0) {
        if (!model.variable(z_id_)->is_assigned() || model.variable(z_id_)->assigned_value().value() != 0) {
            if (!model.variable(z_id_)->assign(0)) return PresolveResult::Contradiction;
            changed = true;
        }
    }

    auto z_min = model.var_min(z_id_);
    auto z_max = model.var_max(z_id_);
    if (z_min == 0 && z_max == 0) {
        if (!model.variable(x_id_)->domain().contains(0) && !model.variable(y_id_)->domain().contains(0)) {
            return PresolveResult::Contradiction;
        }
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool IntTimesConstraint::propagate_bounds(Model& model) {
    // x * y = z の bounds propagation

    auto x_min = model.var_min(x_id_);
    auto x_max = model.var_max(x_id_);
    auto y_min = model.var_min(y_id_);
    auto y_max = model.var_max(y_id_);
    auto z_min = model.var_min(z_id_);
    auto z_max = model.var_max(z_id_);

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
    if (!model.variable(z_id_)->remove_below(*prod_min)) return false;
    if (!model.variable(z_id_)->remove_above(*prod_max)) return false;

    // x が 0 のみを含む場合、z = 0
    if (x_min == 0 && x_max == 0) {
        if (!model.variable(z_id_)->assign(0)) {
            return false;
        }
    }

    // y が 0 のみを含む場合、z = 0
    if (y_min == 0 && y_max == 0) {
        if (!model.variable(z_id_)->assign(0)) {
            return false;
        }
    }

    // z が 0 のみを含む場合、x = 0 または y = 0
    z_min = model.var_min(z_id_);
    z_max = model.var_max(z_id_);
    if (z_min == 0 && z_max == 0) {
        // x か y のどちらかが 0 を含む必要がある
        if (!model.variable(x_id_)->domain().contains(0) && !model.variable(y_id_)->domain().contains(0)) {
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
        if (!model.contains(z_id_, product)) {
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
            if (!model.contains(y_id_, y_val)) {
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
            if (!model.contains(x_id_, x_val)) {
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
            if (!model.contains(z_id_, 0)) {
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
            if (!model.contains(z_id_, 0)) {
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

bool IntTimesConstraint::on_final_instantiate(const Model& model) {
    return model.value(x_id_) * model.value(y_id_) == model.value(z_id_);
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
            if (!model.contains(x_id_, x_val)) {
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
            if (!model.contains(y_id_, y_val)) {
                return false;
            }
            model.enqueue_instantiate(y_id_, y_val);
        }
    } else if (last_var_internal_idx == 2) {
        // z が未確定、x と y は確定
        auto product = model.value(x_id_) * model.value(y_id_);
        if (!model.contains(z_id_, product)) {
            return false;
        }
        model.enqueue_instantiate(z_id_, product);
    }

    return true;
}

// ============================================================================
// IntAbsConstraint implementation
// ============================================================================

IntAbsConstraint::IntAbsConstraint(VariablePtr x, VariablePtr y)
    : Constraint(extract_var_ids({x, y}))
    , x_id_(x->id())
    , y_id_(y->id()) {
}

std::string IntAbsConstraint::name() const {
    return "int_abs";
}

PresolveResult IntAbsConstraint::presolve(Model& model) {
    // Snapshot domain sizes before propagation
    size_t x_size_before = model.variable(x_id_)->domain().size();
    size_t y_size_before = model.variable(y_id_)->domain().size();
    if (!propagate_bounds(model)) return PresolveResult::Contradiction;
    bool changed = (model.variable(x_id_)->domain().size() != x_size_before ||
                    model.variable(y_id_)->domain().size() != y_size_before);
    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool IntAbsConstraint::propagate_bounds(Model& model) {
    // |x| = y の bounds propagation

    auto x_min = model.var_min(x_id_);
    auto x_max = model.var_max(x_id_);
    auto y_min = model.var_min(y_id_);
    auto y_max = model.var_max(y_id_);

    // y >= 0 を強制
    if (y_min < 0) {
        if (!model.variable(y_id_)->remove_below(0)) return false;
        y_min = model.var_min(y_id_);
        y_max = model.var_max(y_id_);
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
    if (!model.variable(y_id_)->remove_below(abs_x_min)) return false;
    if (!model.variable(y_id_)->remove_above(abs_x_max)) return false;

    // x の範囲を y の範囲から制限
    // |x| <= y_max → -y_max <= x <= y_max
    y_max = model.var_max(y_id_);
    if (!model.variable(x_id_)->remove_below(-y_max)) return false;
    if (!model.variable(x_id_)->remove_above(y_max)) return false;

    // x のドメインから、対応する |x| が y のドメインにない値を削除
    std::vector<Domain::value_type> buf;
    std::set<Domain::value_type> y_set;
    model.variable(y_id_)->domain().for_each_value([&](auto v) { y_set.insert(v); });
    model.variable(x_id_)->domain().copy_values_to(buf);
    for (auto v : buf) {
        auto abs_v = (v >= 0) ? v : -v;
        if (y_set.count(abs_v) == 0) {
            if (!model.variable(x_id_)->remove(v)) {
                return false;
            }
        }
    }

    // y のドメインから、対応する x が x のドメインにない値を削除
    std::set<Domain::value_type> x_set;
    model.variable(x_id_)->domain().for_each_value([&](auto v) { x_set.insert(v); });
    model.variable(y_id_)->domain().copy_values_to(buf);
    for (auto v : buf) {
        // v = |x| となる x は v または -v
        if (x_set.count(v) == 0 && x_set.count(-v) == 0) {
            if (!model.variable(y_id_)->remove(v)) {
                return false;
            }
        }
    }

    return !model.variable(x_id_)->domain().empty() && !model.variable(y_id_)->domain().empty();
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
        if (!model.contains(y_id_, abs_x)) {
            return false;
        }
        model.enqueue_instantiate(y_id_, abs_x);
    }

    // y が確定したら x を |x| = y に制約
    if (model.is_instantiated(y_id_) && !model.is_instantiated(x_id_)) {
        auto y_val = model.value(y_id_);
        if (y_val == 0) {
            if (!model.contains(x_id_, 0)) {
                return false;
            }
            model.enqueue_instantiate(x_id_, 0);
        } else {
            bool has_pos = model.contains(x_id_, y_val);
            bool has_neg = model.contains(x_id_, -y_val);
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

bool IntAbsConstraint::on_final_instantiate(const Model& model) {
    auto x_val = model.value(x_id_);
    auto y_val = model.value(y_id_);
    return (x_val >= 0 ? x_val : -x_val) == y_val;
}

// ============================================================================
// IntModConstraint implementation
// ============================================================================

IntModConstraint::IntModConstraint(VariablePtr x, VariablePtr y, VariablePtr z)
    : Constraint(extract_var_ids({x, y, z}))
    , x_id_(x->id())
    , y_id_(y->id())
    , z_id_(z->id()) {
}

std::string IntModConstraint::name() const {
    return "int_mod";
}

PresolveResult IntModConstraint::presolve(Model& model) {
    bool changed = false;
    // y != 0 を強制
    if (model.variable(y_id_)->domain().contains(0)) {
        if (!model.variable(y_id_)->remove(0)) return PresolveResult::Contradiction;
        changed = true;
    }

    // z の bounds を直接設定
    {
        auto x_min = model.variable(x_id_)->min();
        auto x_max = model.variable(x_id_)->max();
        auto y_min = model.variable(y_id_)->min();
        auto y_max = model.variable(y_id_)->max();
        auto abs_y_max = std::max(std::abs(y_min), std::abs(y_max));
        Domain::value_type z_lo, z_hi;
        if (x_min >= 0) {
            z_lo = 0;
            z_hi = std::min(abs_y_max - 1, x_max);
        } else if (x_max <= 0) {
            z_lo = std::max(-(abs_y_max - 1), x_min);
            z_hi = 0;
        } else {
            z_lo = std::max(-(abs_y_max - 1), x_min);
            z_hi = std::min(abs_y_max - 1, x_max);
        }
        if (model.variable(z_id_)->min() < z_lo) {
            if (!model.variable(z_id_)->remove_below(z_lo)) return PresolveResult::Contradiction;
            changed = true;
        }
        if (model.variable(z_id_)->max() > z_hi) {
            if (!model.variable(z_id_)->remove_above(z_hi)) return PresolveResult::Contradiction;
            changed = true;
        }
    }

    // x, y 両方確定 → z を直接計算
    if (model.variable(x_id_)->is_assigned() && model.variable(y_id_)->is_assigned()) {
        auto y_val = model.variable(y_id_)->assigned_value().value();
        if (y_val == 0) return PresolveResult::Contradiction;
        auto z_val = model.variable(x_id_)->assigned_value().value() % y_val;
        if (!model.variable(z_id_)->domain().contains(z_val)) return PresolveResult::Contradiction;
        if (!model.variable(z_id_)->is_assigned()) {
            if (!model.variable(z_id_)->assign(z_val)) return PresolveResult::Contradiction;
            changed = true;
        }
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool IntModConstraint::propagate_bounds(Model& model) {
    auto x_min = model.var_min(x_id_);
    auto x_max = model.var_max(x_id_);
    auto y_min = model.var_min(y_id_);
    auto y_max = model.var_max(y_id_);

    // |y| の最大値を計算
    auto abs_y_max = std::max(std::abs(y_min), std::abs(y_max));

    // z の bounds を計算
    // |z| < |y| は常に成立、かつ sign(z) = sign(x) (or z=0)
    // また、x >= 0 → 0 <= z <= x, x <= 0 → x <= z <= 0
    Domain::value_type z_lo, z_hi;

    if (x_min >= 0) {
        // x は常に非負 → z in [0, min(|y_max|-1, x_max)]
        z_lo = 0;
        z_hi = std::min(abs_y_max - 1, x_max);
    } else if (x_max <= 0) {
        // x は常に非正 → z in [max(-(|y_max|-1), x_min), 0]
        z_lo = std::max(-(abs_y_max - 1), x_min);
        z_hi = 0;
    } else {
        // x が正負にまたがる → z in [max(-(|y_max|-1), x_min), min(|y_max|-1, x_max)]
        z_lo = std::max(-(abs_y_max - 1), x_min);
        z_hi = std::min(abs_y_max - 1, x_max);
    }

    model.enqueue_set_min(z_id_, z_lo);
    model.enqueue_set_max(z_id_, z_hi);

    return true;
}

bool IntModConstraint::on_instantiate(Model& model, int save_point,
                                       size_t var_idx, size_t internal_var_idx,
                                       Domain::value_type value,
                                       Domain::value_type prev_min,
                                       Domain::value_type prev_max) {
    // 基底クラスの 2WL 処理
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // y != 0 を強制
    if (model.contains(y_id_, 0)) {
        model.enqueue_remove_value(y_id_, 0);
    }

    // x と y が確定 → z = x % y
    if (model.is_instantiated(x_id_) && model.is_instantiated(y_id_) && !model.is_instantiated(z_id_)) {
        auto x_val = model.value(x_id_);
        auto y_val = model.value(y_id_);
        if (y_val == 0) return false;
        auto z_val = x_val % y_val;
        if (!model.contains(z_id_, z_val)) return false;
        model.enqueue_instantiate(z_id_, z_val);
        return true;
    }

    // x と z が確定 → y のドメインをフィルタ
    if (model.is_instantiated(x_id_) && model.is_instantiated(z_id_) && !model.is_instantiated(y_id_)) {
        auto x_val = model.value(x_id_);
        auto z_val = model.value(z_id_);
        auto diff = x_val - z_val;
        if (diff == 0) {
            auto abs_x = std::abs(x_val);
            const auto& y_dom = model.variable(y_id_)->domain();
            for (auto it = y_dom.begin(); it != y_dom.end(); ++it) {
                if (std::abs(*it) <= abs_x) {
                    model.enqueue_remove_value(y_id_, *it);
                }
            }
        } else {
            const auto& y_dom = model.variable(y_id_)->domain();
            for (auto it = y_dom.begin(); it != y_dom.end(); ++it) {
                if (*it == 0 || x_val % *it != z_val) {
                    model.enqueue_remove_value(y_id_, *it);
                }
            }
        }
        return true;
    }

    // y と z が確定 → x のドメインをフィルタ
    if (model.is_instantiated(y_id_) && model.is_instantiated(z_id_) && !model.is_instantiated(x_id_)) {
        auto y_val = model.value(y_id_);
        auto z_val = model.value(z_id_);
        if (y_val == 0) return false;
        const auto& x_dom = model.variable(x_id_)->domain();
        for (auto it = x_dom.begin(); it != x_dom.end(); ++it) {
            if (*it % y_val != z_val) {
                model.enqueue_remove_value(x_id_, *it);
            }
        }
        return true;
    }

    // x のみ確定 → z の bounds を制限
    if (model.is_instantiated(x_id_) && !model.is_instantiated(y_id_) && !model.is_instantiated(z_id_)) {
        auto x_val = model.value(x_id_);
        auto abs_y_max = std::max(std::abs(model.var_min(y_id_)), std::abs(model.var_max(y_id_)));
        Domain::value_type z_lo, z_hi;
        if (x_val >= 0) {
            z_lo = 0;
            z_hi = std::min(x_val, abs_y_max - 1);
        } else {
            z_lo = std::max(x_val, -(abs_y_max - 1));
            z_hi = 0;
        }
        model.enqueue_set_min(z_id_, z_lo);
        model.enqueue_set_max(z_id_, z_hi);
    }

    // y のみ確定 → z の bounds を制限 (|z| < |y|)
    if (model.is_instantiated(y_id_) && !model.is_instantiated(x_id_) && !model.is_instantiated(z_id_)) {
        auto y_val = model.value(y_id_);
        if (y_val == 0) return false;
        auto abs_y = std::abs(y_val);
        auto x_min = model.var_min(x_id_);
        auto x_max = model.var_max(x_id_);
        Domain::value_type z_lo = -(abs_y - 1);
        Domain::value_type z_hi = abs_y - 1;
        if (x_min >= 0) { z_lo = 0; z_hi = std::min(z_hi, x_max); }
        else if (x_max <= 0) { z_hi = 0; z_lo = std::max(z_lo, x_min); }
        else { z_lo = std::max(z_lo, x_min); z_hi = std::min(z_hi, x_max); }
        model.enqueue_set_min(z_id_, z_lo);
        model.enqueue_set_max(z_id_, z_hi);
    }

    return true;
}

bool IntModConstraint::on_set_min(Model& model, int /*save_point*/,
                                   size_t /*var_idx*/, size_t /*internal_var_idx*/,
                                   Domain::value_type /*new_min*/,
                                   Domain::value_type /*old_min*/) {
    return propagate_bounds(model);
}

bool IntModConstraint::on_set_max(Model& model, int save_point,
                                   size_t var_idx, size_t internal_var_idx,
                                   Domain::value_type new_max,
                                   Domain::value_type /*old_max*/) {
    return propagate_bounds(model);
}

bool IntModConstraint::on_final_instantiate(const Model& model) {
    auto y_val = model.value(y_id_);
    if (y_val == 0) return false;
    return model.value(x_id_) % y_val == model.value(z_id_);
}

bool IntModConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                               size_t last_var_internal_idx) {
    if (last_var_internal_idx == 0) {
        // x が未確定、y と z は確定
        auto y_val = model.value(y_id_);
        auto z_val = model.value(z_id_);
        if (y_val == 0) return false;
        // x = k * y + z (k は整数) かつ x のドメイン内
        const auto& x_dom = model.variable(x_id_)->domain();
        for (auto it = x_dom.begin(); it != x_dom.end(); ++it) {
            if (*it % y_val != z_val) {
                model.enqueue_remove_value(x_id_, *it);
            }
        }
    } else if (last_var_internal_idx == 1) {
        // y が未確定、x と z は確定
        auto x_val = model.value(x_id_);
        auto z_val = model.value(z_id_);
        const auto& y_dom = model.variable(y_id_)->domain();
        for (auto it = y_dom.begin(); it != y_dom.end(); ++it) {
            if (*it == 0 || x_val % *it != z_val) {
                model.enqueue_remove_value(y_id_, *it);
            }
        }
    } else if (last_var_internal_idx == 2) {
        // z が未確定、x と y は確定
        auto x_val = model.value(x_id_);
        auto y_val = model.value(y_id_);
        if (y_val == 0) return false;
        auto z_val = x_val % y_val;
        if (!model.contains(z_id_, z_val)) return false;
        model.enqueue_instantiate(z_id_, z_val);
    }

    return true;
}

} // namespace sabori_csp
