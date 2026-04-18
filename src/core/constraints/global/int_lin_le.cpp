#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <set>
#include <limits>

namespace sabori_csp {

// ============================================================================
// IntLinLeConstraint implementation
// ============================================================================

IntLinLeConstraint::IntLinLeConstraint(std::vector<int64_t> coeffs,
                                         std::vector<VariablePtr> vars,
                                         int64_t bound)
    : Constraint()
    , bound_(bound)
    , current_fixed_sum_(0)
    , min_rem_potential_(0) {
    // 同一変数の係数を集約
    std::unordered_map<Variable*, int64_t> aggregated;
    for (size_t i = 0; i < vars.size(); ++i) {
        aggregated[vars[i]] += coeffs[i];
    }

    // 一意な変数リストと係数リストを再構築（係数が0の変数は除外）
    std::vector<VariablePtr> unique_vars;
    for (const auto& [var_ptr, coeff] : aggregated) {
        if (coeff == 0) continue;  // 係数が0の変数は除外
        // shared_ptr を探す
        for (const auto& var : vars) {
            if (var == var_ptr) {
                unique_vars.push_back(var);
                coeffs_.push_back(coeff);
                break;
            }
        }
    }

    // 全ての係数が0になった場合: presolve で処理
    if (unique_vars.empty()) {
        return;
    }

    // 変数IDキャッシュを構築
    var_ids_ = extract_var_ids(unique_vars);

    // 注意: 内部状態は presolve() で初期化
}

std::string IntLinLeConstraint::name() const {
    return "int_lin_le";
}

PresolveResult IntLinLeConstraint::presolve(Model& model) {
    if (var_ids_.empty()) {
        return bound_ >= 0 ? PresolveResult::Unchanged : PresolveResult::Contradiction;
    }

    bool changed = false;
    bool progress = true;

    while (progress) {
        progress = false;

        // 全変数の最小寄与合計を計算
        int64_t total_min = 0;
        for (size_t i = 0; i < var_ids_.size(); ++i) {
            int64_t c = coeffs_[i];
            auto* var = model.variable(var_ids_[i]);
            if (c >= 0) {
                total_min += c * var->min();
            } else {
                total_min += c * var->max();
            }
        }

        if (total_min > bound_) {
            return PresolveResult::Contradiction;
        }

        for (size_t j = 0; j < var_ids_.size(); ++j) {
            int64_t c = coeffs_[j];
            auto* var = model.variable(var_ids_[j]);
            if (var->is_assigned()) continue;

            // rest_min = total_min minus j's contribution
            int64_t rest_min;
            if (c >= 0) {
                rest_min = total_min - c * var->min();
            } else {
                rest_min = total_min - c * var->max();
            }
            int64_t available = bound_ - rest_min;  // c * x_j <= available

            if (c > 0) {
                int64_t new_max = available / c;  // floor division (available >= 0)
                if (new_max < var->max()) {
                    if (!var->remove_above(new_max)) return PresolveResult::Contradiction;
                    progress = true;
                    changed = true;
                }
            } else {
                int64_t abs_c = -c;
                // c * x_j <= available → x_j >= ceil(-available / abs_c)
                int64_t new_min;
                if (available >= 0) {
                    new_min = -(available / abs_c);
                } else {
                    new_min = ((-available) + abs_c - 1) / abs_c;
                }
                if (new_min > var->min()) {
                    if (!var->remove_below(new_min)) return PresolveResult::Contradiction;
                    progress = true;
                    changed = true;
                }
            }
        }
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool IntLinLeConstraint::on_instantiate(Model& model, int save_point,
                                          size_t var_idx, size_t internal_var_idx,
                                          Domain::value_type value,
                                          Domain::value_type prev_min,
                                          Domain::value_type prev_max) {
    size_t internal_idx = internal_var_idx;
    int64_t c = coeffs_[internal_idx];

    // Trail に保存
    save_trail_if_needed(model, save_point);

    // 差分更新
    current_fixed_sum_ += c * value;
    if (c >= 0) {
        min_rem_potential_ -= c * prev_min;
    } else {
        min_rem_potential_ -= c * prev_max;
    }

    // 下限チェック: 現在の確定和 + 残りの最小 > bound なら矛盾
    if (current_fixed_sum_ + min_rem_potential_ > bound_) {
        return false;
    }

    return propagate_bounds(model, internal_idx);
}

bool IntLinLeConstraint::on_final_instantiate(const Model& model) {
    int64_t sum = 0;
    for (size_t i = 0; i < var_ids_.size(); ++i) {
        sum += coeffs_[i] * model.value(var_ids_[i]);
    }
    return sum <= bound_;
}

void IntLinLeConstraint::rewind_to(int save_point) {
    while (!trail_.empty() && trail_.back().first > save_point) {
        const auto& entry = trail_.back().second;
        current_fixed_sum_ = entry.fixed_sum;
        min_rem_potential_ = entry.min_pot;
        trail_.pop_back();
    }
}

bool IntLinLeConstraint::prepare_propagation(Model& model) {
    // 全ての係数が0の場合: 0 <= bound
    if (var_ids_.empty()) {
        return bound_ >= 0;
    }

    // 変数の現在状態に基づいて内部状態を初期化
    current_fixed_sum_ = 0;
    min_rem_potential_ = 0;

    for (size_t i = 0; i < var_ids_.size(); ++i) {
        int64_t c = coeffs_[i];
        size_t vid = var_ids_[i];

        if (model.variable(vid)->is_assigned()) {
            current_fixed_sum_ += c * model.variable(vid)->assigned_value().value();
        } else {
            auto min_val = model.var_min(vid);
            auto max_val = model.var_max(vid);

            if (c >= 0) {
                min_rem_potential_ += c * min_val;
            } else {
                min_rem_potential_ += c * max_val;
            }
        }
    }

    // 2WL を初期化
    init_watches();

    // trail をクリア
    trail_.clear();

    // 初期整合性チェック
    if (current_fixed_sum_ + min_rem_potential_ > bound_) {
        return false;  // 矛盾
    }

    return true;
}

void IntLinLeConstraint::save_trail_if_needed(Model& model, int save_point) {
    if (trail_.empty() || trail_.back().first != save_point) {
        trail_.push_back({save_point, {current_fixed_sum_, min_rem_potential_}});
        model.mark_constraint_dirty(model_index(), save_point);
    }
}

bool IntLinLeConstraint::on_set_min(Model& model, int save_point,
                                     size_t var_idx, size_t internal_var_idx,
                                     Domain::value_type new_min,
                                     Domain::value_type old_min) {
    size_t idx = internal_var_idx;
    int64_t c = coeffs_[idx];

    // c >= 0 の変数のみ影響: min_rem_potential_ は c * min で寄与
    if (c >= 0) {
        save_trail_if_needed(model, save_point);
        min_rem_potential_ += c * (new_min - old_min);

        if (current_fixed_sum_ + min_rem_potential_ > bound_) {
            return false;
        }

        return propagate_bounds(model, idx);
    }
    return true;
}

bool IntLinLeConstraint::on_set_max(Model& model, int save_point,
                                     size_t var_idx, size_t internal_var_idx,
                                     Domain::value_type new_max,
                                     Domain::value_type old_max) {
    size_t idx = internal_var_idx;
    int64_t c = coeffs_[idx];

    // c < 0 の変数のみ影響: min_rem_potential_ は c * max で寄与
    if (c < 0) {
        save_trail_if_needed(model, save_point);
        min_rem_potential_ += c * (new_max - old_max);

        if (current_fixed_sum_ + min_rem_potential_ > bound_) {
            return false;
        }

        return propagate_bounds(model, idx);
    }
    return true;
}

bool IntLinLeConstraint::on_remove_value(Model& /*model*/, int /*save_point*/,
                                          size_t /*var_idx*/, size_t /*internal_var_idx*/,
                                          Domain::value_type /*removed_value*/) {
    // 境界変化は solver が on_set_min/on_set_max をディスパッチするため、
    // 内部値の除去では bounds が変わらず potentials も不変。
    return true;
}

bool IntLinLeConstraint::propagate_bounds(Model& model, size_t skip_idx) {
    int64_t slack = bound_ - current_fixed_sum_;
    // slack >= min_rem_potential_ is guaranteed (checked before calling)

    // Entailment check: if max possible remaining sum <= slack, constraint is
    // satisfied regardless of future assignments — skip propagation entirely.
    {
        int64_t max_rem = 0;
        for (size_t j = 0; j < var_ids_.size(); ++j) {
            size_t vid = var_ids_[j];
            if (model.is_instantiated(vid)) continue;
            int64_t c = coeffs_[j];
            max_rem += (c >= 0) ? c * model.var_max(vid) : c * model.var_min(vid);
            if (max_rem > slack) goto not_entailed;
        }
        return true;  // Entailed
    }
not_entailed:

    for (size_t j = 0; j < var_ids_.size(); ++j) {
        if (j == skip_idx) continue;
        size_t vid = var_ids_[j];
        if (model.is_instantiated(vid)) continue;

        int64_t c = coeffs_[j];

        // rest_min = min_rem_potential_ minus j's min contribution
        int64_t rest_min;
        if (c >= 0) {
            rest_min = min_rem_potential_ - c * model.var_min(vid);
        } else {
            rest_min = min_rem_potential_ - c * model.var_max(vid);
        }
        int64_t available = slack - rest_min;  // c * x_j <= available

        if (c > 0) {
            int64_t new_max = available / c;  // floor division (available >= 0)
            if (new_max < model.var_max(vid)) {
                model.enqueue_set_max(vid, new_max);
            }
        } else {
            int64_t abs_c = -c;
            // c * x_j <= available → x_j >= ceil(-available / abs_c)
            int64_t new_min;
            if (available >= 0) {
                new_min = -(available / abs_c);
            } else {
                new_min = ((-available) + abs_c - 1) / abs_c;
            }
            if (new_min > model.var_min(vid)) {
                model.enqueue_set_min(vid, new_min);
            }
        }
    }
    return true;
}

// ============================================================================

void IntLinLeConstraint::init_activity(const Model& model, double* activity) const {
    int64_t max_abs = 0;
    for (auto c : coeffs_) {
        int64_t a = c < 0 ? -c : c;
        if (a > max_abs) max_abs = a;
    }
    if (max_abs <= 100) return;

    double sum_abs = 0.0;
    for (auto c : coeffs_) {
        sum_abs += std::abs(static_cast<double>(c));
    }

    for (size_t i = 0; i < coeffs_.size(); ++i) {
        size_t vid = var_ids_[i];
        if (!model.is_instantiated(vid)) {
            activity[vid] += std::abs(static_cast<double>(coeffs_[i])) / sum_abs;
        }
    }
}

#if 0
void IntLinLeConstraint::bump_activity(const Model& model, size_t trigger_var_idx,
                                        double* activity, double activity_inc,
                                        bool& need_rescale, std::mt19937& rng) const {
    Constraint::bump_activity(model, trigger_var_idx, activity, activity_inc, need_rescale, rng);
    return;
    
    //    Constraint::bump_activity(model, trigger_var_idx, activity, activity_inc, need_rescale, rng);
    //    return;

    size_t n = 0;
    
    for (size_t i = 0; i < coeffs_.size(); ++i) {
        size_t vid = var_ids_[i];
        // if (!model.is_instantiated(vid)) continue;
        auto c = coeffs_[i];
        // auto val = model.value(vid);

        if ((c > 0 && model.var_max(vid) != model.presolve_min(vid))
            || (c < 0 && model.var_min(vid) != model.presolve_max(vid))) {
            n++;
        }
    }

    if (n == 0) {
        abort();
        Constraint::bump_activity(model, trigger_var_idx, activity, activity_inc, need_rescale, rng);
        return;
    }
    
    double inc = activity_inc / n;

    for (size_t i = 0; i < coeffs_.size(); ++i) {
        size_t vid = var_ids_[i];
        // if (!model.is_instantiated(vid)) continue;
        auto c = coeffs_[i];
        // auto val = model.value(vid);

        if ((c > 0 && model.var_max(vid) != model.presolve_min(vid))
            || (c < 0 && model.var_min(vid) != model.presolve_max(vid))) {
            // double inc = activity_inc / n / model.var_size((vid));
            bump_variable_activity(activity, vid, inc, need_rescale, rng);
        }
    }
}
#endif

}  // namespace sabori_csp

