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
    : Constraint(std::vector<VariablePtr>())  // 後で設定
    , bound_(bound)
    , b_(std::move(b))
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
    if (coeffs_.empty()) {
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

std::string IntLinLeImpConstraint::name() const {
    return "int_lin_le_imp";
}

std::vector<VariablePtr> IntLinLeImpConstraint::variables() const {
    return vars_;
}

std::optional<bool> IntLinLeImpConstraint::is_satisfied() const {
    // b が未確定なら判定不能
    if (!b_->is_assigned()) {
        return std::nullopt;
    }

    // b = 0 なら制約は無条件で充足
    if (b_->assigned_value().value() == 0) {
        return true;
    }

    // b = 1 の場合、線形変数が全て確定しているか確認
    int64_t sum = 0;
    for (size_t i = 0; i < vars_.size() - 1; ++i) {  // b_ を除く
        if (!vars_[i]->is_assigned()) {
            return std::nullopt;
        }
        sum += coeffs_[i] * vars_[i]->assigned_value().value();
    }
    return sum <= bound_;
}

bool IntLinLeImpConstraint::propagate(Model& /*model*/) {
    // b = 1 の場合のみ伝播
    if (!b_->is_assigned() || b_->assigned_value().value() == 0) {
        return true;  // b が未確定または 0 なら何もしない
    }

    // b = 1: sum(coeffs[i] * vars[i]) <= bound を強制
    // 初期矛盾チェックは check_initial_consistency で行う
    return true;
}

bool IntLinLeImpConstraint::on_instantiate(Model& model, int save_point,
                                            size_t var_idx, Domain::value_type value,
                                            Domain::value_type prev_min,
                                            Domain::value_type prev_max) {
    Variable* var_ptr = model.variable(var_idx).get();
    auto it = var_ptr_to_idx_.find(var_ptr);
    if (it == var_ptr_to_idx_.end()) {
        // この制約に関係ない変数
        return true;
    }

    size_t internal_idx = it->second;

    // b_ が確定した場合
    if (internal_idx == SIZE_MAX) {
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

    // Trail に保存
    if (trail_.empty() || trail_.back().first != save_point) {
        trail_.push_back({save_point, {current_fixed_sum_, min_rem_potential_}});
    }

    // 差分更新
    int64_t c = coeffs_[internal_idx];
    current_fixed_sum_ += c * value;
    if (c >= 0) {
        min_rem_potential_ -= c * prev_min;
    } else {
        min_rem_potential_ -= c * prev_max;
    }

    // b = 1 の場合のみ矛盾チェック
    if (b_->is_assigned() && b_->assigned_value().value() == 1) {
        if (current_fixed_sum_ + min_rem_potential_ > bound_) {
            return false;
        }
    }

    return true;
}

bool IntLinLeImpConstraint::on_final_instantiate() {
    // b = 0 なら常に充足
    if (b_->assigned_value().value() == 0) {
        return true;
    }

    // b = 1: 全変数の和が bound 以下か確認
    int64_t sum = 0;
    for (size_t i = 0; i < vars_.size() - 1; ++i) {  // b_ を除く
        sum += coeffs_[i] * vars_[i]->assigned_value().value();
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

void IntLinLeImpConstraint::check_initial_consistency() {
    // b = 1 が確定していて、かつ最小ポテンシャルが bound を超えていれば矛盾
    if (b_->is_assigned() && b_->assigned_value().value() == 1) {
        if (current_fixed_sum_ + min_rem_potential_ > bound_) {
            set_initially_inconsistent(true);
        }
    }
    // b が未確定または b = 0 の場合は矛盾なし
}

bool IntLinLeImpConstraint::presolve(Model& /*model*/) {
    // 全ての係数が0の場合: b -> (0 <= bound)
    if (coeffs_.empty()) {
        // b = 1 で bound < 0 なら矛盾
        if (b_->is_assigned() && b_->assigned_value().value() == 1 && bound_ < 0) {
            return false;
        }
        return true;
    }

    // 変数の現在状態に基づいて内部状態を初期化
    current_fixed_sum_ = 0;
    min_rem_potential_ = 0;

    for (size_t i = 0; i < vars_.size() - 1; ++i) {
        int64_t c = coeffs_[i];

        if (vars_[i]->is_assigned()) {
            current_fixed_sum_ += c * vars_[i]->assigned_value().value();
        } else {
            auto min_val = vars_[i]->domain().min().value();
            auto max_val = vars_[i]->domain().max().value();

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
    if (b_->is_assigned() && b_->assigned_value().value() == 1) {
        if (current_fixed_sum_ + min_rem_potential_ > bound_) {
            return false;
        }
    }

    return true;
}

}  // namespace sabori_csp
