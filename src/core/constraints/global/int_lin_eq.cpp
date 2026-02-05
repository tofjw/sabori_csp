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
    , max_rem_potential_(0)
    , unfixed_count_(0) {
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

    // 注意: 内部状態（current_fixed_sum_, unfixed_count_ 等）は presolve() で初期化
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

bool IntLinEqConstraint::propagate(Model& /*model*/) {
    // Bounds propagation: 各変数の上下限を直接絞り込む（従来の方式）

    // 全体の min/max potential を計算
    int64_t total_min = 0;
    int64_t total_max = 0;
    for (size_t i = 0; i < vars_.size(); ++i) {
        int64_t c = coeffs_[i];
        auto min_val = vars_[i]->domain().min();
        auto max_val = vars_[i]->domain().max();
        if (!min_val || !max_val) return false;

        if (c >= 0) {
            total_min += c * (*min_val);
            total_max += c * (*max_val);
        } else {
            total_min += c * (*max_val);
            total_max += c * (*min_val);
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

            auto cur_min = vars_[j]->domain().min();
            auto cur_max = vars_[j]->domain().max();
            if (!cur_min || !cur_max) return false;

            // rest の min/max を計算
            int64_t rest_min = total_min;
            int64_t rest_max = total_max;
            if (c >= 0) {
                rest_min -= c * (*cur_min);
                rest_max -= c * (*cur_max);
            } else {
                rest_min -= c * (*cur_max);
                rest_max -= c * (*cur_min);
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

            // 直接ドメインを修正
            if (new_min > *cur_min) {
                if (new_min > *cur_max) return false;
                auto vals = vars_[j]->domain().values();
                for (auto v : vals) {
                    if (v < new_min) {
                        if (!vars_[j]->domain().remove(v)) {
                            return false;
                        }
                    }
                }
                auto new_cur_min = vars_[j]->domain().min().value();
                if (c >= 0) {
                    total_min += c * (new_cur_min - *cur_min);
                } else {
                    total_max += c * (new_cur_min - *cur_min);
                }
                changed = true;
            }
            if (new_max < *cur_max) {
                auto cur_min_after = vars_[j]->domain().min();
                if (!cur_min_after || new_max < *cur_min_after) return false;
                auto vals = vars_[j]->domain().values();
                for (auto v : vals) {
                    if (v > new_max) {
                        if (!vars_[j]->domain().remove(v)) {
                            return false;
                        }
                    }
                }
                auto new_cur_max = vars_[j]->domain().max().value();
                if (c >= 0) {
                    total_max += c * (new_cur_max - *cur_max);
                } else {
                    total_min += c * (new_cur_max - *cur_max);
                }
                changed = true;
            }
        }
    }

    return true;
}

bool IntLinEqConstraint::can_assign(size_t internal_idx, Domain::value_type value,
                                     Domain::value_type prev_min,
                                     Domain::value_type prev_max) const {
    int64_t c = coeffs_[internal_idx];

    // 1. 自分が value を取った時、他が「最小」でも target を超えないか？
    int64_t potential_min;
    if (c >= 0) {
        potential_min = current_fixed_sum_ + (min_rem_potential_ - c * prev_min) + c * value;
    } else {
        potential_min = current_fixed_sum_ + (min_rem_potential_ - c * prev_max) + c * value;
    }
    if (potential_min > target_sum_) {
        return false;
    }

    // 2. 自分が value を取った時、他が「最大」でも target に届くか？
    int64_t potential_max;
    if (c >= 0) {
        potential_max = current_fixed_sum_ + (max_rem_potential_ - c * prev_max) + c * value;
    } else {
        potential_max = current_fixed_sum_ + (max_rem_potential_ - c * prev_min) + c * value;
    }
    if (potential_max < target_sum_) {
        return false;
    }

    return true;
}

bool IntLinEqConstraint::on_instantiate(Model& model, int save_point,
                                          size_t var_idx, Domain::value_type value,
                                          Domain::value_type prev_min,
                                          Domain::value_type prev_max) {
    // モデルから変数ポインタを取得し、O(1) で内部インデックスを特定
    Variable* var_ptr = model.variable(var_idx).get();
    auto it = var_ptr_to_idx_.find(var_ptr);

    if (it == var_ptr_to_idx_.end()) {
        // この制約に関係ない変数
        return true;
    }
    size_t internal_idx = it->second;

    // Look-ahead チェック
    if (!can_assign(internal_idx, value, prev_min, prev_max)) {
        return false;
    }

    // Trail に保存
    if (trail_.empty() || trail_.back().first != save_point) {
        trail_.push_back({save_point, {current_fixed_sum_, min_rem_potential_, max_rem_potential_, unfixed_count_}});
    }

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

    // 未確定カウントをデクリメント（O(1)）
    --unfixed_count_;

    // 残り1変数になったら on_last_uninstantiated を呼び出す
    if (unfixed_count_ == 1) {
        size_t last_idx = find_last_uninstantiated();
        if (last_idx != SIZE_MAX) {
            return on_last_uninstantiated(model, save_point, last_idx);
        }
    } else if (unfixed_count_ == 0) {
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
            // モデル内の変数インデックスを特定
            for (size_t model_idx = 0; model_idx < model.variables().size(); ++model_idx) {
                if (model.variable(model_idx) == last_var) {
                    model.enqueue_instantiate(model_idx, required_value);
                    break;
                }
            }
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
        unfixed_count_ = entry.unfixed_count;
        trail_.pop_back();
    }
}

bool IntLinEqConstraint::on_set_min(Model& /*model*/, int /*save_point*/,
                                     size_t /*var_idx*/, Domain::value_type /*new_min*/,
                                     Domain::value_type /*old_min*/) {
    // 初期伝播は propagate() で、探索中は on_instantiate で処理
    // on_set_min/on_set_max は今のところ使用しない
    return true;
}

bool IntLinEqConstraint::on_set_max(Model& /*model*/, int /*save_point*/,
                                     size_t /*var_idx*/, Domain::value_type /*new_max*/,
                                     Domain::value_type /*old_max*/) {
    // 初期伝播は propagate() で、探索中は on_instantiate で処理
    // on_set_min/on_set_max は今のところ使用しない
    return true;
}

bool IntLinEqConstraint::presolve(Model& /*model*/) {
    // 変数の現在状態に基づいて内部状態を初期化
    current_fixed_sum_ = 0;
    min_rem_potential_ = 0;
    max_rem_potential_ = 0;
    unfixed_count_ = 0;

    for (size_t i = 0; i < vars_.size(); ++i) {
        int64_t c = coeffs_[i];

        if (vars_[i]->is_assigned()) {
            // 確定している変数
            current_fixed_sum_ += c * vars_[i]->assigned_value().value();
        } else {
            // 未確定の変数
            ++unfixed_count_;
            auto min_val = vars_[i]->domain().min().value();
            auto max_val = vars_[i]->domain().max().value();

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

void IntLinEqConstraint::sync_after_propagation() {
    // presolve() に統合されたため、空実装
    // 後方互換性のために残す
}


}  // namespace sabori_csp

