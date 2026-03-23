#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <limits>

namespace sabori_csp {

// ============================================================================
// ArrayVarIntElementConstraint implementation
// ============================================================================

ArrayVarIntElementConstraint::ArrayVarIntElementConstraint(
    VariablePtr index,
    std::vector<VariablePtr> array,
    VariablePtr result,
    bool zero_based)
    : Constraint()
    , n_(array.size())
    , zero_based_(zero_based)
    , current_result_min_support_(std::numeric_limits<Domain::value_type>::max())
    , current_result_max_support_(std::numeric_limits<Domain::value_type>::min()) {

    std::vector<VariablePtr> all_vars;
    all_vars.push_back(index);
    all_vars.push_back(result);
    for (const auto& v : array) {
        all_vars.push_back(v);
    }
    var_ids_ = extract_var_ids(all_vars);

    index_id_ = index->id();
    result_id_ = result->id();
}

std::string ArrayVarIntElementConstraint::name() const {
    return "array_var_int_element";
}

Domain::value_type ArrayVarIntElementConstraint::index_to_0based(Domain::value_type idx) const {
    return zero_based_ ? idx : idx - 1;
}

Domain::value_type ArrayVarIntElementConstraint::index_from_0based(size_t idx_0based) const {
    return zero_based_ ? static_cast<Domain::value_type>(idx_0based)
                       : static_cast<Domain::value_type>(idx_0based) + 1;
}

void ArrayVarIntElementConstraint::recompute_bounds_support(Model& model) {
    current_result_min_support_ = std::numeric_limits<Domain::value_type>::max();
    current_result_max_support_ = std::numeric_limits<Domain::value_type>::min();
    min_support_arr_idx_ = SIZE_MAX;
    max_support_arr_idx_ = SIZE_MAX;

    const auto& idx_dom = model.variable(index_id_)->domain();
    for (auto it = idx_dom.begin(); it != idx_dom.end(); ++it) {
        auto idx_0based = index_to_0based(*it);
        if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
            size_t ai = static_cast<size_t>(idx_0based);
            auto arr_id = var_ids_[2 + ai];
            auto arr_min = model.var_min(arr_id);
            auto arr_max = model.var_max(arr_id);
            if (arr_min < current_result_min_support_) {
                current_result_min_support_ = arr_min;
                min_support_arr_idx_ = ai;
            }
            if (arr_max > current_result_max_support_) {
                current_result_max_support_ = arr_max;
                max_support_arr_idx_ = ai;
            }
        }
    }
}

bool ArrayVarIntElementConstraint::propagate_bounds(Model& model, int save_point) {
    auto* index_var = model.variable(index_id_);
    auto* result_var = model.variable(result_id_);

    Domain::value_type new_result_min = std::numeric_limits<Domain::value_type>::max();
    Domain::value_type new_result_max = std::numeric_limits<Domain::value_type>::min();

    std::vector<Domain::value_type> index_values;
    index_var->domain().copy_values_to(index_values);
    for (auto idx : index_values) {
        auto idx_0based = index_to_0based(idx);
        if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
            auto arr_id = var_ids_[2 + static_cast<size_t>(idx_0based)];
            new_result_min = std::min(new_result_min, model.var_min(arr_id));
            new_result_max = std::max(new_result_max, model.var_max(arr_id));
        }
    }

    if (new_result_min > new_result_max) return false;

    auto& result_domain = result_var->domain();
    if (save_point >= 0) {
        if (!model.set_min(save_point, result_id_, new_result_min)) return false;
        if (!model.set_max(save_point, result_id_, new_result_max)) return false;
    } else {
        if (!result_domain.remove_below(new_result_min)) return false;
        if (!result_domain.remove_above(new_result_max)) return false;
    }

    auto result_min = model.var_min(result_id_);
    auto result_max = model.var_max(result_id_);
    index_var->domain().copy_values_to(index_values);
    auto& index_domain = index_var->domain();

    for (auto idx : index_values) {
        auto idx_0based = index_to_0based(idx);
        if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) {
            bool ok = (save_point >= 0) ? model.remove_value(save_point, index_id_, idx)
                                         : index_domain.remove(idx);
            if (!ok) return false;
            continue;
        }
        auto arr_id = var_ids_[2 + static_cast<size_t>(idx_0based)];
        if (model.var_max(arr_id) < result_min || model.var_min(arr_id) > result_max) {
            bool ok = (save_point >= 0) ? model.remove_value(save_point, index_id_, idx)
                                         : index_domain.remove(idx);
            if (!ok) return false;
        }
    }

    if (index_var->is_assigned()) {
        auto idx = index_var->assigned_value().value();
        auto idx_0based = index_to_0based(idx);
        if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
            auto arr_id = var_ids_[2 + static_cast<size_t>(idx_0based)];
            auto common_min = std::max(model.var_min(arr_id), model.var_min(result_id_));
            auto common_max = std::min(model.var_max(arr_id), model.var_max(result_id_));
            if (common_min > common_max) return false;
            auto* arr_var = model.variable(arr_id);
            if (save_point >= 0) {
                if (!model.set_min(save_point, arr_id, common_min)) return false;
                if (!model.set_max(save_point, arr_id, common_max)) return false;
                if (!model.set_min(save_point, result_id_, common_min)) return false;
                if (!model.set_max(save_point, result_id_, common_max)) return false;
            } else {
                if (!arr_var->domain().remove_below(common_min)) return false;
                if (!arr_var->domain().remove_above(common_max)) return false;
                if (!result_domain.remove_below(common_min)) return false;
                if (!result_domain.remove_above(common_max)) return false;
            }
        }
    }

    return true;
}

bool ArrayVarIntElementConstraint::prepare_propagation(Model& model) {
    recompute_bounds_support(model);
    init_watches();
    trail_.clear();

    bool has_valid_index = false;
    model.variable(index_id_)->domain().for_each_value([&](auto idx) {
        if (!has_valid_index) {
            auto idx_0based = index_to_0based(idx);
            if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
                has_valid_index = true;
            }
        }
    });
    if (!has_valid_index) return false;
    if (current_result_min_support_ > current_result_max_support_) return false;

    // 密度判定: elem_dom_avg が小さければ support tracking 有効
    size_t total_dom = 0;
    for (size_t i = 0; i < n_; ++i) {
        total_dom += static_cast<size_t>(
            model.var_max(var_ids_[2 + i]) - model.var_min(var_ids_[2 + i]) + 1);
    }
    use_support_tracking_ = (n_ > 0 && total_dom / n_ <= 4);

    return true;
}

PresolveResult ArrayVarIntElementConstraint::presolve(Model& model) {
    size_t total_size_before = 0;
    for (size_t vid : var_ids_) {
        total_size_before += model.variable(vid)->domain().size();
    }
    if (!propagate_bounds(model)) return PresolveResult::Contradiction;
    size_t total_size_after = 0;
    for (size_t vid : var_ids_) {
        total_size_after += model.variable(vid)->domain().size();
    }
    return (total_size_after < total_size_before) ? PresolveResult::Changed : PresolveResult::Unchanged;
}

// 全スキャン: result bounds 再計算 + index フィルタリング + support 更新
bool ArrayVarIntElementConstraint::propagate_via_queue(Model& model) {
    Domain::value_type new_result_min = std::numeric_limits<Domain::value_type>::max();
    Domain::value_type new_result_max = std::numeric_limits<Domain::value_type>::min();
    size_t new_min_idx = SIZE_MAX;
    size_t new_max_idx = SIZE_MAX;

    auto result_min = model.var_min(result_id_);
    auto result_max = model.var_max(result_id_);

    const auto& idx_dom = model.variable(index_id_)->domain();
    for (auto it = idx_dom.begin(); it != idx_dom.end(); ++it) {
        auto idx = *it;
        auto idx_0based = index_to_0based(idx);
        if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) {
            model.enqueue_remove_value(index_id_, idx);
            continue;
        }

        size_t ai = static_cast<size_t>(idx_0based);
        auto arr_id = var_ids_[2 + ai];
        auto arr_min = model.var_min(arr_id);
        auto arr_max = model.var_max(arr_id);

        if (arr_max < result_min || arr_min > result_max) {
            model.enqueue_remove_value(index_id_, idx);
            continue;
        }

        if (arr_min < new_result_min) { new_result_min = arr_min; new_min_idx = ai; }
        if (arr_max > new_result_max) { new_result_max = arr_max; new_max_idx = ai; }
    }

    if (new_result_min > new_result_max) return false;

    current_result_min_support_ = new_result_min;
    current_result_max_support_ = new_result_max;
    min_support_arr_idx_ = new_min_idx;
    max_support_arr_idx_ = new_max_idx;

    model.enqueue_set_min(result_id_, new_result_min);
    model.enqueue_set_max(result_id_, new_result_max);

    if (model.is_instantiated(index_id_)) {
        auto idx = model.value(index_id_);
        auto idx_0based = index_to_0based(idx);
        if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
            auto arr_id = var_ids_[2 + static_cast<size_t>(idx_0based)];
            auto common_min = std::max(model.var_min(arr_id), model.var_min(result_id_));
            auto common_max = std::min(model.var_max(arr_id), model.var_max(result_id_));
            if (common_min > common_max) return false;
            model.enqueue_set_min(result_id_, common_min);
            model.enqueue_set_max(result_id_, common_max);
            model.enqueue_set_min(arr_id, common_min);
            model.enqueue_set_max(arr_id, common_max);
            if (model.is_instantiated(arr_id))
                model.enqueue_instantiate(result_id_, model.value(arr_id));
            if (model.is_instantiated(result_id_))
                model.enqueue_instantiate(arr_id, model.value(result_id_));
        }
    }

    return true;
}

// index フィルタリングのみ（result bounds は再計算しない）
bool ArrayVarIntElementConstraint::filter_index_against_result(Model& model) {
    auto result_min = model.var_min(result_id_);
    auto result_max = model.var_max(result_id_);

    const auto& idx_dom = model.variable(index_id_)->domain();
    for (auto it = idx_dom.begin(); it != idx_dom.end(); ++it) {
        auto idx = *it;
        auto idx_0based = index_to_0based(idx);
        if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) {
            model.enqueue_remove_value(index_id_, idx);
            continue;
        }
        auto arr_id = var_ids_[2 + static_cast<size_t>(idx_0based)];
        if (model.var_max(arr_id) < result_min || model.var_min(arr_id) > result_max) {
            model.enqueue_remove_value(index_id_, idx);
        }
    }
    return true;
}

bool ArrayVarIntElementConstraint::on_instantiate(
    Model& model, int save_point,
    size_t /*var_idx*/, size_t /*internal_var_idx*/, Domain::value_type /*value*/,
    Domain::value_type /*prev_min*/, Domain::value_type /*prev_max*/) {

    trail_.push_back({save_point, {current_result_min_support_, current_result_max_support_,
                                    min_support_arr_idx_, max_support_arr_idx_}});
    model.mark_constraint_dirty(model_index(), save_point);
    return propagate_via_queue(model);
}

bool ArrayVarIntElementConstraint::on_set_min(
    Model& model, int save_point,
    size_t /*var_idx*/, size_t internal_var_idx, Domain::value_type /*new_min*/,
    Domain::value_type /*old_min*/) {

    if (!use_support_tracking_) {
        // デフォルトモード: 無条件全スキャン
        trail_.push_back({save_point, {current_result_min_support_, current_result_max_support_,
                                        min_support_arr_idx_, max_support_arr_idx_}});
        model.mark_constraint_dirty(model_index(), save_point);
        return propagate_via_queue(model);
    }

    // Support tracking モード
    if (internal_var_idx == 1) {
        // result の min が増加 → index フィルタリングのみ（result bounds 不変）
        return filter_index_against_result(model);
    }

    if (internal_var_idx >= 2) {
        size_t arr_idx = internal_var_idx - 2;
        if (arr_idx != min_support_arr_idx_) {
            // min support ではない → result min は変わらない
            // ただしこの array[i] が result と交差しなくなった可能性
            auto arr_id = var_ids_[2 + arr_idx];
            if (model.var_min(arr_id) > model.var_max(result_id_)) {
                model.enqueue_remove_value(index_id_, index_from_0based(arr_idx));
            }
            return true;
        }
        // min support が invalidate → 全スキャン
    }

    // index bounds 変更 or support invalidation → 全スキャン
    trail_.push_back({save_point, {current_result_min_support_, current_result_max_support_,
                                    min_support_arr_idx_, max_support_arr_idx_}});
    model.mark_constraint_dirty(model_index(), save_point);
    return propagate_via_queue(model);
}

bool ArrayVarIntElementConstraint::on_set_max(
    Model& model, int save_point,
    size_t /*var_idx*/, size_t internal_var_idx, Domain::value_type /*new_max*/,
    Domain::value_type /*old_max*/) {

    if (!use_support_tracking_) {
        trail_.push_back({save_point, {current_result_min_support_, current_result_max_support_,
                                        min_support_arr_idx_, max_support_arr_idx_}});
        model.mark_constraint_dirty(model_index(), save_point);
        return propagate_via_queue(model);
    }

    // Support tracking モード
    if (internal_var_idx == 1) {
        // result の max が減少 → index フィルタリングのみ
        return filter_index_against_result(model);
    }

    if (internal_var_idx >= 2) {
        size_t arr_idx = internal_var_idx - 2;
        if (arr_idx != max_support_arr_idx_) {
            // max support ではない → result max は変わらない
            auto arr_id = var_ids_[2 + arr_idx];
            if (model.var_max(arr_id) < model.var_min(result_id_)) {
                model.enqueue_remove_value(index_id_, index_from_0based(arr_idx));
            }
            return true;
        }
        // max support が invalidate → 全スキャン
    }

    trail_.push_back({save_point, {current_result_min_support_, current_result_max_support_,
                                    min_support_arr_idx_, max_support_arr_idx_}});
    model.mark_constraint_dirty(model_index(), save_point);
    return propagate_via_queue(model);
}

bool ArrayVarIntElementConstraint::on_remove_value(
    Model& model, int save_point,
    size_t /*var_idx*/, size_t internal_var_idx,
    Domain::value_type removed_value) {

    if (!use_support_tracking_) return true;

    if (internal_var_idx == 0) {
        // index から値が除去された → support が除去された場合のみ再計算
        auto idx_0based = index_to_0based(removed_value);
        if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
            size_t ai = static_cast<size_t>(idx_0based);
            if (ai == min_support_arr_idx_ || ai == max_support_arr_idx_) {
                if (trail_.empty() || trail_.back().first != save_point) {
                    trail_.push_back({save_point, {current_result_min_support_, current_result_max_support_,
                                                    min_support_arr_idx_, max_support_arr_idx_}});
                    model.mark_constraint_dirty(model_index(), save_point);
                }
                return propagate_via_queue(model);
            }
        }
    }

    return true;
}

bool ArrayVarIntElementConstraint::on_last_uninstantiated(
    Model& model, int /*save_point*/,
    size_t last_var_internal_idx) {

    auto last_var_id = var_ids_[last_var_internal_idx];

    if (last_var_id == index_id_) {
        if (model.is_instantiated(index_id_)) {
            model.enqueue_instantiate(index_id_, model.var_min(index_id_));
        }
    } else if (last_var_id == result_id_) {
        if (model.is_instantiated(index_id_)) {
            auto idx_0based = index_to_0based(model.value(index_id_));
            if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
                auto arr_id = var_ids_[2 + static_cast<size_t>(idx_0based)];
                if (model.is_instantiated(arr_id)) {
                    auto expected = model.value(arr_id);
                    if (model.contains(result_id_, expected)) {
                        model.enqueue_instantiate(result_id_, expected);
                    } else {
                        return false;
                    }
                }
            }
        }
    } else {
        if (model.is_instantiated(index_id_) && model.is_instantiated(result_id_)) {
            auto idx_0based = index_to_0based(model.value(index_id_));
            if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
                auto arr_id = var_ids_[2 + static_cast<size_t>(idx_0based)];
                if (last_var_id == arr_id) {
                    auto expected = model.value(result_id_);
                    if (model.contains(arr_id, expected)) {
                        model.enqueue_instantiate(arr_id, expected);
                    } else {
                        return false;
                    }
                }
            }
        }
    }

    return true;
}

bool ArrayVarIntElementConstraint::on_final_instantiate(const Model& model) {
    if (!model.is_instantiated(index_id_) || !model.is_instantiated(result_id_)) return false;
    auto idx_0based = index_to_0based(model.value(index_id_));
    if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) return false;
    size_t arr_var_id = var_ids_[2 + static_cast<size_t>(idx_0based)];
    if (!model.is_instantiated(arr_var_id)) return false;
    return model.value(arr_var_id) == model.value(result_id_);
}

void ArrayVarIntElementConstraint::rewind_to(int save_point) {
    while (!trail_.empty() && trail_.back().first > save_point) {
        auto& entry = trail_.back().second;
        current_result_min_support_ = entry.min_support;
        current_result_max_support_ = entry.max_support;
        min_support_arr_idx_ = entry.min_support_idx;
        max_support_arr_idx_ = entry.max_support_idx;
        trail_.pop_back();
    }
}

void ArrayVarIntElementConstraint::bump_activity(const Model& model, size_t trigger_var_idx,
                                                   double* activity, double activity_inc,
                                                   bool& need_rescale, std::mt19937& rng) const {
    if (model.is_instantiated(index_id_)) {
        auto idx_0based = index_to_0based(model.value(index_id_));
        size_t count = 1;
        if (model.is_instantiated(result_id_)) ++count;
        bool arr_valid = (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_);
        size_t arr_id = SIZE_MAX;
        if (arr_valid) {
            arr_id = var_ids_[2 + static_cast<size_t>(idx_0based)];
            if (model.is_instantiated(arr_id)) ++count;
        }
        double inc = activity_inc / count;
        bump_variable_activity(activity, index_id_, inc, need_rescale, rng);
        if (model.is_instantiated(result_id_))
            bump_variable_activity(activity, result_id_, inc, need_rescale, rng);
        if (arr_valid && model.is_instantiated(arr_id))
            bump_variable_activity(activity, arr_id, inc, need_rescale, rng);
        return;
    }
    Constraint::bump_activity(model, trigger_var_idx, activity, activity_inc, need_rescale, rng);
}

}  // namespace sabori_csp
