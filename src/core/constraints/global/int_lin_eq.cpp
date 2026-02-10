#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <set>
#include <limits>

namespace sabori_csp {

// ============================================================================
// IntLinEqConstraint implementation
// ============================================================================

IntLinEqConstraint::IntLinEqConstraint(std::vector<int64_t> coeffs,
                                         std::vector<VariablePtr> vars,
                                         int64_t target_sum)
    : Constraint(std::vector<VariablePtr>())  // 後で設定
    , target_sum_(target_sum)
    , current_fixed_sum_(0)
    , min_rem_potential_(0)
    , max_rem_potential_(0) {
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

    // 全ての係数が0になった場合: 0 == target_sum
    if (vars_.empty()) {
        if (target_sum_ != 0) {
            set_initially_inconsistent(true);
        }
        return;
    }

    // 変数ポインタ → 内部インデックスマップを構築
    for (size_t i = 0; i < vars_.size(); ++i) {
        var_ptr_to_idx_[vars_[i].get()] = i;
    }

    // 注意: 内部状態（current_fixed_sum_ 等）は presolve() で初期化
    // コンストラクタでは変数の状態を参照しない
}

std::string IntLinEqConstraint::name() const {
    return "int_lin_eq";
}

std::vector<VariablePtr> IntLinEqConstraint::variables() const {
    return vars_;
}

std::optional<bool> IntLinEqConstraint::is_satisfied() const {
    int64_t sum = 0;
    for (size_t i = 0; i < vars_.size(); ++i) {
        if (!vars_[i]->is_assigned()) {
            return std::nullopt;
        }
        sum += coeffs_[i] * vars_[i]->assigned_value().value();
    }
    return sum == target_sum_;
}

bool IntLinEqConstraint::presolve(Model& model) {
    // Bounds propagation: 各変数の上下限を直接絞り込む（従来の方式）

    // 全体の min/max potential を計算
    int64_t total_min = 0;
    int64_t total_max = 0;
    for (size_t i = 0; i < vars_.size(); ++i) {
        int64_t c = coeffs_[i];
        auto min_val = vars_[i]->min();
        auto max_val = vars_[i]->max();
        if (c >= 0) {
            total_min += c * min_val;
            total_max += c * max_val;
        } else {
            total_min += c * max_val;
            total_max += c * min_val;
        }
    }

    // target が [total_min, total_max] の範囲外なら矛盾
    if (target_sum_ < total_min || target_sum_ > total_max) {
        return false;
    }

    // 各変数の bounds を絞り込む（固定点まで繰り返し）
    bool changed = true;
    while (changed) {
        changed = false;

        for (size_t j = 0; j < vars_.size(); ++j) {
            if (vars_[j]->is_assigned()) continue;

            int64_t c = coeffs_[j];
            if (c == 0) continue;

            auto cur_min = vars_[j]->min();
            auto cur_max = vars_[j]->max();
            // rest の min/max を計算
            int64_t rest_min = total_min;
            int64_t rest_max = total_max;
            if (c >= 0) {
                rest_min -= c * cur_min;
                rest_max -= c * cur_max;
            } else {
                rest_min -= c * cur_max;
                rest_max -= c * cur_min;
            }

            // 新しい bounds を計算
            int64_t new_min, new_max;
            if (c > 0) {
                int64_t num_min = target_sum_ - rest_max;
                int64_t num_max = target_sum_ - rest_min;
                new_min = (num_min >= 0) ? (num_min + c - 1) / c : -((-num_min) / c);
                new_max = (num_max >= 0) ? num_max / c : -(((-num_max) + c - 1) / c);
            } else {
                int64_t num_min = target_sum_ - rest_max;
                int64_t num_max = target_sum_ - rest_min;
                int64_t abs_c = -c;

                if (num_min >= 0) {
                    new_max = -((num_min + abs_c - 1) / abs_c);
                } else {
                    new_max = (-num_min) / abs_c;
                }

                if (num_max >= 0) {
                    new_min = -(num_max / abs_c);
                } else {
                    new_min = ((-num_max) + abs_c - 1) / abs_c;
                }
            }

            // ドメインの範囲を変更
            if (new_min > cur_min) {
                if (new_min > cur_max) return false;
                if (!vars_[j]->remove_below(new_min)) return false;
                auto new_cur_min = vars_[j]->min();
                if (c >= 0) {
                    total_min += c * (new_cur_min - cur_min);
                } else {
                    total_max += c * (new_cur_min - cur_min);
                }
                changed = true;
            }
            if (new_max < cur_max) {
                auto cur_min_after = vars_[j]->min();
                if (new_max < cur_min_after) return false;
                if (!vars_[j]->remove_above(new_max)) return false;
                auto new_cur_max = vars_[j]->max();
                if (c >= 0) {
                    total_max += c * (new_cur_max - cur_max);
                } else {
                    total_min += c * (new_cur_max - cur_max);
                }
                changed = true;
            }
        }
    }

    return true;
}

bool IntLinEqConstraint::on_instantiate(Model& model, int save_point,
                                          size_t var_idx, Domain::value_type value,
                                          Domain::value_type prev_min,
                                          Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, value, prev_min, prev_max)) {
        return false;
    }

    // モデルから変数ポインタを取得し、O(1) で内部インデックスを特定
    Variable* var_ptr = model.variable(var_idx).get();
    auto it = var_ptr_to_idx_.find(var_ptr);

    if (it == var_ptr_to_idx_.end()) {
        // この制約に関係ない変数
        return true;
    }
    size_t internal_idx = it->second;

    // Trail に保存
    save_trail_if_needed(model, save_point);

    // 差分更新
    int64_t c = coeffs_[internal_idx];
    current_fixed_sum_ += c * value;
    if (c >= 0) {
        min_rem_potential_ -= c * prev_min;
        max_rem_potential_ -= c * prev_max;
    } else {
        min_rem_potential_ -= c * prev_max;
        max_rem_potential_ -= c * prev_min;
    }

    // 残り変数が 1 or 0 の時
    if (has_uninstantiated()) {
        size_t last_idx = find_last_uninstantiated();
        if (last_idx != SIZE_MAX) {
            if (!on_last_uninstantiated(model, save_point, last_idx)) {
                return false;
            }
        }
    }
    else {
        return on_final_instantiate();
    }

    return true;
}

bool IntLinEqConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                                  size_t last_var_internal_idx) {
    int64_t last_coeff = coeffs_[last_var_internal_idx];
    int64_t remaining = target_sum_ - current_fixed_sum_;
    auto& last_var = vars_[last_var_internal_idx];

    // 既に確定している場合は整合性チェックのみ
    if (last_var->is_assigned()) {
        int64_t actual = last_var->assigned_value().value();
        return (last_coeff * actual == remaining);
    }

    // 係数で割り切れる場合のみ値を決定
    if (remaining % last_coeff == 0) {
        int64_t required_value = remaining / last_coeff;

        if (last_var->domain().contains(required_value)) {
            model.enqueue_instantiate(last_var->id(), required_value);
        } else {
            // 確定する値がドメインに含まれない
            return false;
        }
    } else {
        // 割り切れなければ制約は満たせない
        return false;
    }
    
    return true;
}

void IntLinEqConstraint::check_initial_consistency() {
    // 確定済み + 残りの最小/最大ポテンシャルをチェック
    // current_fixed_sum_ + min_rem_potential_ > target_sum_ または
    // current_fixed_sum_ + max_rem_potential_ < target_sum_ なら矛盾
    int64_t total_min = current_fixed_sum_ + min_rem_potential_;
    int64_t total_max = current_fixed_sum_ + max_rem_potential_;
    if (total_min > target_sum_ || total_max < target_sum_) {
        set_initially_inconsistent(true);
    }
}

// constraint の親クラスからは呼ばない場合も、verify で使うので実装
bool IntLinEqConstraint::on_final_instantiate() {
    int64_t sum = 0;
    for (size_t i = 0; i < vars_.size(); ++i) {
        sum += coeffs_[i] * vars_[i]->assigned_value().value();
    }
    return sum == target_sum_;
}

void IntLinEqConstraint::rewind_to(int save_point) {
    while (!trail_.empty() && trail_.back().first > save_point) {
        const auto& entry = trail_.back().second;
        current_fixed_sum_ = entry.fixed_sum;
        min_rem_potential_ = entry.min_pot;
        max_rem_potential_ = entry.max_pot;
        trail_.pop_back();
    }
}

void IntLinEqConstraint::save_trail_if_needed(Model& model, int save_point) {
    if (trail_.empty() || trail_.back().first != save_point) {
        trail_.push_back({save_point, {current_fixed_sum_, min_rem_potential_, max_rem_potential_}});
        model.mark_constraint_dirty(model_index(), save_point);
    }
}

bool IntLinEqConstraint::on_set_min(Model& model, int save_point,
                                     size_t var_idx, Domain::value_type new_min,
                                     Domain::value_type old_min) {
    Variable* var_ptr = model.variable(var_idx).get();
    auto it = var_ptr_to_idx_.find(var_ptr);
    if (it == var_ptr_to_idx_.end()) return true;

    int64_t c = coeffs_[it->second];

    if (c >= 0) {
        // min_rem_potential_ は c * min で寄与
        save_trail_if_needed(model, save_point);
        min_rem_potential_ += c * (new_min - old_min);
    } else {
        // max_rem_potential_ は c * min で寄与
        save_trail_if_needed(model, save_point);
        max_rem_potential_ += c * (new_min - old_min);
    }

    int64_t min_sum = current_fixed_sum_ + min_rem_potential_;
    int64_t max_sum = current_fixed_sum_ + max_rem_potential_;
    if (min_sum > target_sum_ || max_sum < target_sum_) {
        return false;
    }
    return true;
}

bool IntLinEqConstraint::on_set_max(Model& model, int save_point,
                                     size_t var_idx, Domain::value_type new_max,
                                     Domain::value_type old_max) {
  Variable* var_ptr = model.variable(var_idx).get();
    auto it = var_ptr_to_idx_.find(var_ptr);
    if (it == var_ptr_to_idx_.end()) return true;

    int64_t c = coeffs_[it->second];

    if (c >= 0) {
        // max_rem_potential_ は c * max で寄与
        save_trail_if_needed(model, save_point);
        max_rem_potential_ += c * (new_max - old_max);
    } else {
        // min_rem_potential_ は c * max で寄与
        save_trail_if_needed(model, save_point);
        min_rem_potential_ += c * (new_max - old_max);
    }

    int64_t min_sum = current_fixed_sum_ + min_rem_potential_;
    int64_t max_sum = current_fixed_sum_ + max_rem_potential_;
    if (min_sum > target_sum_ || max_sum < target_sum_) {
        return false;
    }
    return true;
}

bool IntLinEqConstraint::on_remove_value(Model& model, int save_point,
                                          size_t var_idx, Domain::value_type removed_value) {
    Variable* var_ptr = model.variable(var_idx).get();
    auto it = var_ptr_to_idx_.find(var_ptr);
    if (it == var_ptr_to_idx_.end()) return true;

    int64_t c = coeffs_[it->second];

    if (c >= 0) {
        auto current_min = model.var_min(var_idx);
        if (current_min > removed_value) {
            save_trail_if_needed(model, save_point);
            min_rem_potential_ += c * (current_min - removed_value);
        }
        auto current_max = model.var_max(var_idx);
        if (current_max < removed_value) {
            save_trail_if_needed(model, save_point);
            max_rem_potential_ += c * (current_max - removed_value);
        }
    } else {
        auto current_min = model.var_min(var_idx);
        if (current_min > removed_value) {
            save_trail_if_needed(model, save_point);
            max_rem_potential_ += c * (current_min - removed_value);
        }
        auto current_max = model.var_max(var_idx);
        if (current_max < removed_value) {
            save_trail_if_needed(model, save_point);
            min_rem_potential_ += c * (current_max - removed_value);
        }
    }

    int64_t min_sum = current_fixed_sum_ + min_rem_potential_;
    int64_t max_sum = current_fixed_sum_ + max_rem_potential_;
    if (min_sum > target_sum_ || max_sum < target_sum_) {
        return false;
    }
    return true;
}

bool IntLinEqConstraint::prepare_propagation(Model& model) {
    // 変数の現在状態に基づいて内部状態を初期化
    current_fixed_sum_ = 0;
    min_rem_potential_ = 0;
    max_rem_potential_ = 0;

    for (size_t i = 0; i < vars_.size(); ++i) {
        int64_t c = coeffs_[i];

        if (vars_[i]->is_assigned()) {
            // 確定している変数
            current_fixed_sum_ += c * vars_[i]->assigned_value().value();
        } else {
            // 未確定の変数
            auto min_val = model.var_min(vars_[i]->id());
            auto max_val = model.var_max(vars_[i]->id());

            if (c >= 0) {
                min_rem_potential_ += c * min_val;
                max_rem_potential_ += c * max_val;
            } else {
                min_rem_potential_ += c * max_val;
                max_rem_potential_ += c * min_val;
            }
        }
    }

    // 2WL を初期化
    init_watches();

    // trail をクリア
    trail_.clear();

    // 初期整合性チェック
    int64_t total_min = current_fixed_sum_ + min_rem_potential_;
    int64_t total_max = current_fixed_sum_ + max_rem_potential_;
    if (total_min > target_sum_ || total_max < target_sum_) {
        return false;  // 矛盾
    }

    return true;
}


}  // namespace sabori_csp

