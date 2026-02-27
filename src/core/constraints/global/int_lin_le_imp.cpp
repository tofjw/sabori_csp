#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <set>
#include <limits>

namespace sabori_csp {

// ============================================================================
// IntLinLeImpConstraint implementation
// ============================================================================

IntLinLeImpConstraint::IntLinLeImpConstraint(std::vector<int64_t> coeffs,
                                               std::vector<VariablePtr> vars,
                                               int64_t bound,
                                               VariablePtr b)
    : Constraint()
    , bound_(bound)
    , current_fixed_sum_(0)
    , min_rem_potential_(0) {
    b_id_ = b->id();

    // 同一変数の係数を集約
    std::unordered_map<Variable*, int64_t> aggregated;
    for (size_t i = 0; i < vars.size(); ++i) {
        aggregated[vars[i]] += coeffs[i];
    }

    // 一意な変数リストと係数リストを再構築（係数が0の変数は除外）
    std::vector<VariablePtr> unique_vars;
    for (const auto& [var_ptr, coeff] : aggregated) {
        if (coeff == 0) continue;  // 係数が0の変数は除外
        unique_vars.push_back(var_ptr);
        coeffs_.push_back(coeff);
    }

    // 全ての係数が0になった場合: presolve で処理
    if (coeffs_.empty()) {
        unique_vars.push_back(b);
        var_ids_ = extract_var_ids(unique_vars);
        return;
    }

    // b を末尾に追加
    unique_vars.push_back(b);

    // 変数IDキャッシュを構築
    var_ids_ = extract_var_ids(unique_vars);

    // 注意: 内部状態は presolve() で初期化
}

std::string IntLinLeImpConstraint::name() const {
    return "int_lin_le_imp";
}

bool IntLinLeImpConstraint::presolve(Model& model) {
    auto* bvar = model.variable(b_id_);
    // b = 1 の場合のみ伝播
    if (!bvar->is_assigned() || bvar->assigned_value().value() == 0) {
        return true;  // b が未確定または 0 なら何もしない
    }

    // b = 1: sum(coeffs[i] * vars[i]) <= bound を強制
    // 初期矛盾チェックは check_initial_consistency で行う
    return true;
}

bool IntLinLeImpConstraint::on_instantiate(Model& model, int save_point,
                                            size_t var_idx, size_t internal_var_idx,
                                            Domain::value_type value,
                                            Domain::value_type prev_min,
                                            Domain::value_type prev_max) {
    // b が確定した場合
    if (var_idx == b_id_) {
        // b = 0 なら何もしない
        if (value == 0) {
            return true;
        }
        // b = 1 なら、現在の状態で矛盾がないかチェック
        if (current_fixed_sum_ + min_rem_potential_ > bound_) {
            return false;
        }
        return true;
    }

    // b が確定していない場合は状態のみ更新
    // b = 0 の場合も状態を更新（b が後で 1 になる可能性があるため）
    size_t internal_idx = internal_var_idx;

    // Trail に保存
    save_trail_if_needed(model, save_point);

    // 差分更新
    int64_t c = coeffs_[internal_idx];
    current_fixed_sum_ += c * value;
    if (c >= 0) {
        min_rem_potential_ -= c * prev_min;
    } else {
        min_rem_potential_ -= c * prev_max;
    }

    // b = 1 の場合のみ矛盾チェック
    if (model.is_instantiated(b_id_) && model.value(b_id_) == 1) {
        if (current_fixed_sum_ + min_rem_potential_ > bound_) {
            return false;
        }
    }

    return true;
}

bool IntLinLeImpConstraint::on_final_instantiate(const Model& model) {
    // b = 0 なら常に充足
    if (model.value(b_id_) == 0) {
        return true;
    }

    // b = 1: 全変数の和が bound 以下か確認
    int64_t sum = 0;
    size_t n_linear = coeffs_.size();
    for (size_t i = 0; i < n_linear; ++i) {
        sum += coeffs_[i] * model.value(var_ids_[i]);
    }
    return sum <= bound_;
}

void IntLinLeImpConstraint::rewind_to(int save_point) {
    while (!trail_.empty() && trail_.back().first > save_point) {
        const auto& entry = trail_.back().second;
        current_fixed_sum_ = entry.fixed_sum;
        min_rem_potential_ = entry.min_pot;
        trail_.pop_back();
    }
}

bool IntLinLeImpConstraint::prepare_propagation(Model& model) {
    // 全ての係数が0の場合: b -> (0 <= bound)
    if (coeffs_.empty()) {
        auto* bvar = model.variable(b_id_);
        // b = 1 で bound < 0 なら矛盾
        if (bvar->is_assigned() && bvar->assigned_value().value() == 1 && bound_ < 0) {
            return false;
        }
        return true;
    }

    // 変数の現在状態に基づいて内部状態を初期化
    current_fixed_sum_ = 0;
    min_rem_potential_ = 0;

    size_t n_linear = coeffs_.size();
    for (size_t i = 0; i < n_linear; ++i) {
        int64_t c = coeffs_[i];
        auto* var = model.variable(var_ids_[i]);

        if (var->is_assigned()) {
            current_fixed_sum_ += c * var->assigned_value().value();
        } else {
            auto min_val = model.var_min(var_ids_[i]);
            auto max_val = model.var_max(var_ids_[i]);

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
    auto* bvar = model.variable(b_id_);
    if (bvar->is_assigned() && bvar->assigned_value().value() == 1) {
        if (current_fixed_sum_ + min_rem_potential_ > bound_) {
            return false;
        }
    }

    return true;
}

void IntLinLeImpConstraint::save_trail_if_needed(Model& model, int save_point) {
    if (trail_.empty() || trail_.back().first != save_point) {
        trail_.push_back({save_point, {current_fixed_sum_, min_rem_potential_}});
        model.mark_constraint_dirty(model_index(), save_point);
    }
}

bool IntLinLeImpConstraint::on_set_min(Model& model, int save_point,
                                        size_t var_idx, size_t internal_var_idx,
                                        Domain::value_type new_min,
                                        Domain::value_type old_min) {
    if (var_idx == b_id_) return true;  // b の変更は無視
    size_t idx = internal_var_idx;
    int64_t c = coeffs_[idx];

    // c >= 0 の変数のみ影響: min_rem_potential_ は c * min で寄与
    if (c >= 0) {
        save_trail_if_needed(model, save_point);
        min_rem_potential_ += c * (new_min - old_min);

        if (model.is_instantiated(b_id_) && model.value(b_id_) == 1) {
            if (current_fixed_sum_ + min_rem_potential_ > bound_) {
                return false;
            }
        }
    }
    return true;
}

bool IntLinLeImpConstraint::on_set_max(Model& model, int save_point,
                                        size_t var_idx, size_t internal_var_idx,
                                        Domain::value_type new_max,
                                        Domain::value_type old_max) {
    if (var_idx == b_id_) return true;  // b の変更は無視
    size_t idx = internal_var_idx;
    int64_t c = coeffs_[idx];

    // c < 0 の変数のみ影響: min_rem_potential_ は c * max で寄与
    if (c < 0) {
        save_trail_if_needed(model, save_point);
        min_rem_potential_ += c * (new_max - old_max);

        if (model.is_instantiated(b_id_) && model.value(b_id_) == 1) {
            if (current_fixed_sum_ + min_rem_potential_ > bound_) {
                return false;
            }
        }
    }
    return true;
}

bool IntLinLeImpConstraint::on_remove_value(Model& /*model*/, int /*save_point*/,
                                             size_t /*var_idx*/, size_t /*internal_var_idx*/,
                                             Domain::value_type /*removed_value*/) {
    // 境界変化は solver が on_set_min/on_set_max をディスパッチするため、
    // 内部値の除去では bounds が変わらず potentials も不変。
    return true;
}

}  // namespace sabori_csp
