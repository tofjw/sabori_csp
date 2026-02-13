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
    : Constraint(std::vector<VariablePtr>())  // 後で設定
    , bound_(bound)
    , current_fixed_sum_(0)
    , min_rem_potential_(0) {
    // 同一変数の係数を集約
    std::unordered_map<Variable*, int64_t> aggregated;
    for (size_t i = 0; i < vars.size(); ++i) {
        aggregated[vars[i].get()] += coeffs[i];
    }

    // 一意な変数リストと係数リストを再構築（係数が0の変数は除外）
    for (const auto& [var_ptr, coeff] : aggregated) {
        if (coeff == 0) continue;  // 係数が0の変数は除外
        // shared_ptr を探す
        for (const auto& var : vars) {
            if (var.get() == var_ptr) {
                vars_.push_back(var);
                coeffs_.push_back(coeff);
                break;
            }
        }
    }

    // 全ての係数が0になった場合: presolve で処理
    if (vars_.empty()) {
        return;
    }

    // 変数IDキャッシュを構築
    update_var_ids();

    // 注意: 内部状態は presolve() で初期化
}

std::string IntLinLeConstraint::name() const {
    return "int_lin_le";
}

std::vector<VariablePtr> IntLinLeConstraint::variables() const {
    return vars_;
}

std::optional<bool> IntLinLeConstraint::is_satisfied() const {
    int64_t sum = 0;
    for (size_t i = 0; i < vars_.size(); ++i) {
        if (!vars_[i]->is_assigned()) {
            return std::nullopt;
        }
        sum += coeffs_[i] * vars_[i]->assigned_value().value();
    }
    return sum <= bound_;
}

bool IntLinLeConstraint::presolve(Model& model) {
    return true;
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

    return true;
}

bool IntLinLeConstraint::on_final_instantiate() {
    int64_t sum = 0;
    for (size_t i = 0; i < vars_.size(); ++i) {
        sum += coeffs_[i] * vars_[i]->assigned_value().value();
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

void IntLinLeConstraint::check_initial_consistency() {
    // 確定済み + 残りの最小ポテンシャルが bound を超えるなら矛盾
    if (current_fixed_sum_ + min_rem_potential_ > bound_) {
        set_initially_inconsistent(true);
    }
}

bool IntLinLeConstraint::prepare_propagation(Model& model) {
    // 全ての係数が0の場合: 0 <= bound
    if (vars_.empty()) {
        return bound_ >= 0;
    }

    // 変数の現在状態に基づいて内部状態を初期化
    current_fixed_sum_ = 0;
    min_rem_potential_ = 0;

    for (size_t i = 0; i < vars_.size(); ++i) {
        int64_t c = coeffs_[i];

        if (vars_[i]->is_assigned()) {
            current_fixed_sum_ += c * vars_[i]->assigned_value().value();
        } else {
            auto min_val = model.var_min(vars_[i]->id());
            auto max_val = model.var_max(vars_[i]->id());

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

// ============================================================================

}  // namespace sabori_csp

