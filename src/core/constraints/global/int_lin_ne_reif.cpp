#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <set>
#include <limits>

namespace sabori_csp {

// ============================================================================
// IntLinNeReifConstraint implementation
// ============================================================================

IntLinNeReifConstraint::IntLinNeReifConstraint(std::vector<int64_t> coeffs,
                                                 std::vector<VariablePtr> vars,
                                                 int64_t target,
                                                 VariablePtr b)
    : Constraint(std::vector<VariablePtr>())  // 後で設定
    , target_(target)
    , b_(std::move(b))
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

    // 全ての係数が0になった場合: b ↔ (0 != target)
    if (coeffs_.empty()) {
        // vars_ には b だけを含める
        vars_.push_back(b_);
        var_ptr_to_idx_[b_.get()] = SIZE_MAX;
        return;
    }

    // b を末尾に追加
    vars_.push_back(b_);

    // 変数ポインタ → 内部インデックスマップを構築
    for (size_t i = 0; i < vars_.size() - 1; ++i) {
        var_ptr_to_idx_[vars_[i].get()] = i;
    }
    var_ptr_to_idx_[b_.get()] = SIZE_MAX;

    // 注意: 内部状態は presolve() で初期化
}

std::string IntLinNeReifConstraint::name() const {
    return "int_lin_ne_reif";
}

std::vector<VariablePtr> IntLinNeReifConstraint::variables() const {
    return vars_;
}

std::optional<bool> IntLinNeReifConstraint::is_satisfied() const {
    if (!b_->is_assigned()) {
        return std::nullopt;
    }

    int64_t sum = 0;
    for (size_t i = 0; i < vars_.size() - 1; ++i) {
        if (!vars_[i]->is_assigned()) {
            return std::nullopt;
        }
        sum += coeffs_[i] * vars_[i]->assigned_value().value();
    }

    bool ne = (sum != target_);
    return ne == (b_->assigned_value().value() == 1);
}

bool IntLinNeReifConstraint::propagate(Model& /*model*/) {
    int64_t min_sum = current_fixed_sum_ + min_rem_potential_;
    int64_t max_sum = current_fixed_sum_ + max_rem_potential_;

    // b = 1 の場合、sum != target を強制
    if (b_->is_assigned() && b_->assigned_value().value() == 1) {
        // sum が target にしかなりえない場合は矛盾
        if (min_sum == target_ && max_sum == target_) {
            return false;
        }
    }

    // b = 0 の場合、sum == target を強制
    if (b_->is_assigned() && b_->assigned_value().value() == 0) {
        // target が [min_sum, max_sum] に含まれていなければ矛盾
        if (target_ < min_sum || target_ > max_sum) {
            return false;
        }
    }

    // bounds から b を推論
    if (!b_->is_assigned()) {
        if (target_ < min_sum || target_ > max_sum) {
            // sum != target が常に真 → b = 1
            if (!b_->domain().contains(1)) {
                return false;
            }
            b_->domain().assign(1);
        } else if (min_sum == target_ && max_sum == target_) {
            // sum == target が常に真 → sum != target は常に偽 → b = 0
            if (!b_->domain().contains(0)) {
                return false;
            }
            b_->domain().assign(0);
        }
    }

    return true;
}

bool IntLinNeReifConstraint::on_instantiate(Model& model, int save_point,
                                              size_t var_idx, Domain::value_type value,
                                              Domain::value_type prev_min,
                                              Domain::value_type prev_max) {
    Variable* var_ptr = model.variable(var_idx).get();
    auto it = var_ptr_to_idx_.find(var_ptr);
    if (it == var_ptr_to_idx_.end()) {
        return true;
    }

    size_t internal_idx = it->second;

    // b が確定した場合
    if (internal_idx == SIZE_MAX) {
        int64_t min_sum = current_fixed_sum_ + min_rem_potential_;
        int64_t max_sum = current_fixed_sum_ + max_rem_potential_;

        if (value == 1) {
            // sum != target を強制
            if (min_sum == target_ && max_sum == target_) {
                return false;
            }
        } else {
            // sum == target を強制
            if (target_ < min_sum || target_ > max_sum) {
                return false;
            }
        }
        return true;
    }

    // 線形変数が確定した場合
    // Trail に保存
    if (trail_.empty() || trail_.back().first != save_point) {
        trail_.push_back({save_point, {current_fixed_sum_, min_rem_potential_, max_rem_potential_, unfixed_count_}});
        model.mark_constraint_dirty(model_index(), save_point);
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
    --unfixed_count_;

    int64_t min_sum = current_fixed_sum_ + min_rem_potential_;
    int64_t max_sum = current_fixed_sum_ + max_rem_potential_;

    // b が確定している場合の矛盾チェック
    if (b_->is_assigned()) {
        if (b_->assigned_value().value() == 1) {
            // sum != target を強制
            if (min_sum == target_ && max_sum == target_) {
                return false;
            }
        } else {
            // sum == target を強制
            if (target_ < min_sum || target_ > max_sum) {
                return false;
            }
        }
    } else {
        // b を推論
        if (target_ < min_sum || target_ > max_sum) {
            // sum != target が常に真 → b = 1
            model.enqueue_instantiate(b_->id(), 1);
        } else if (min_sum == target_ && max_sum == target_) {
            // sum == target が常に真 → b = 0
            model.enqueue_instantiate(b_->id(), 0);
        }
    }

    return true;
}

bool IntLinNeReifConstraint::on_final_instantiate() {
    int64_t sum = 0;
    for (size_t i = 0; i < vars_.size() - 1; ++i) {
        sum += coeffs_[i] * vars_[i]->assigned_value().value();
    }

    bool ne = (sum != target_);
    return ne == (b_->assigned_value().value() == 1);
}

void IntLinNeReifConstraint::rewind_to(int save_point) {
    while (!trail_.empty() && trail_.back().first > save_point) {
        const auto& entry = trail_.back().second;
        current_fixed_sum_ = entry.fixed_sum;
        min_rem_potential_ = entry.min_pot;
        max_rem_potential_ = entry.max_pot;
        unfixed_count_ = entry.unfixed_count;
        trail_.pop_back();
    }
}

void IntLinNeReifConstraint::check_initial_consistency() {
    int64_t min_sum = current_fixed_sum_ + min_rem_potential_;
    int64_t max_sum = current_fixed_sum_ + max_rem_potential_;

    if (b_->is_assigned()) {
        if (b_->assigned_value().value() == 1) {
            // sum != target が必要
            if (min_sum == target_ && max_sum == target_) {
                set_initially_inconsistent(true);
            }
        } else {
            // sum == target が必要
            if (target_ < min_sum || target_ > max_sum) {
                set_initially_inconsistent(true);
            }
        }
    }
}

bool IntLinNeReifConstraint::presolve(Model& /*model*/) {
    // 全ての係数が0の場合の特別処理
    if (coeffs_.empty()) {
        bool trivially_true = (target_ != 0);
        if (b_->is_assigned()) {
            bool b_val = (b_->assigned_value().value() == 1);
            if (b_val != trivially_true) {
                return false;  // 矛盾
            }
        } else {
            b_->domain().assign(trivially_true ? 1 : 0);
        }
        return true;
    }

    // 変数の現在状態に基づいて内部状態を初期化
    current_fixed_sum_ = 0;
    min_rem_potential_ = 0;
    max_rem_potential_ = 0;
    unfixed_count_ = 0;

    for (size_t i = 0; i < vars_.size() - 1; ++i) {
        int64_t c = coeffs_[i];

        if (vars_[i]->is_assigned()) {
            current_fixed_sum_ += c * vars_[i]->assigned_value().value();
        } else {
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
    int64_t min_sum = current_fixed_sum_ + min_rem_potential_;
    int64_t max_sum = current_fixed_sum_ + max_rem_potential_;

    if (b_->is_assigned()) {
        if (b_->assigned_value().value() == 1) {
            // sum != target が必要
            if (min_sum == target_ && max_sum == target_) {
                return false;  // 矛盾
            }
        } else {
            // sum == target が必要
            if (target_ < min_sum || target_ > max_sum) {
                return false;  // 矛盾
            }
        }
    }

    return true;
}

// ============================================================================

}  // namespace sabori_csp
