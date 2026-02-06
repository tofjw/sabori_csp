#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <set>
#include <limits>

namespace sabori_csp {

// ============================================================================
// IntLinNeConstraint implementation
// ============================================================================

IntLinNeConstraint::IntLinNeConstraint(std::vector<int64_t> coeffs,
                                         std::vector<VariablePtr> vars,
                                         int64_t target)
    : Constraint(std::vector<VariablePtr>())  // 後で設定
    , target_(target)
    , current_fixed_sum_(0)
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

    // 全ての係数が0になった場合: presolve で処理
    if (vars_.empty()) {
        return;
    }

    // 変数ポインタ → 内部インデックスマップを構築
    for (size_t i = 0; i < vars_.size(); ++i) {
        var_ptr_to_idx_[vars_[i].get()] = i;
    }

    // 注意: 内部状態は presolve() で初期化
}

std::string IntLinNeConstraint::name() const {
    return "int_lin_ne";
}

std::vector<VariablePtr> IntLinNeConstraint::variables() const {
    return vars_;
}

std::optional<bool> IntLinNeConstraint::is_satisfied() const {
    int64_t sum = 0;
    for (size_t i = 0; i < vars_.size(); ++i) {
        if (!vars_[i]->is_assigned()) {
            return std::nullopt;
        }
        sum += coeffs_[i] * vars_[i]->assigned_value().value();
    }
    return sum != target_;
}

bool IntLinNeConstraint::propagate(Model& model) {
    // int_lin_ne は bounds propagation が難しいため、
    // 特にアクティブな伝播は行わない
    return true;
}

bool IntLinNeConstraint::on_instantiate(Model& model, int save_point,
                                          size_t var_idx, Domain::value_type value,
                                          Domain::value_type /*prev_min*/,
                                          Domain::value_type /*prev_max*/) {
    // モデルから変数ポインタを取得し、O(1) で内部インデックスを特定
    Variable* var_ptr = model.variable(var_idx).get();
    auto it = var_ptr_to_idx_.find(var_ptr);
    if (it == var_ptr_to_idx_.end()) {
        // この制約に関係ない変数
        return true;
    }
    size_t internal_idx = it->second;

    // Trail に保存
    if (trail_.empty() || trail_.back().first != save_point) {
        trail_.push_back({save_point, {current_fixed_sum_, unfixed_count_}});
        model.mark_constraint_dirty(model_index(), save_point);
    }

    // 差分更新
    int64_t c = coeffs_[internal_idx];
    current_fixed_sum_ += c * value;

    // 未確定カウントをデクリメント
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

bool IntLinNeConstraint::on_last_uninstantiated(Model& model, int save_point,
                                                  size_t last_var_internal_idx) {
    int64_t last_coeff = coeffs_[last_var_internal_idx];
    int64_t remaining = target_ - current_fixed_sum_;
    auto& last_var = vars_[last_var_internal_idx];

    // 既に確定している場合は整合性チェックのみ
    if (last_var->is_assigned()) {
        int64_t actual = last_var->assigned_value().value();
        return (last_coeff * actual != remaining);
    }

    // 係数で割り切れる場合のみ禁止値を計算
    if (last_coeff != 0 && remaining % last_coeff == 0) {
        int64_t forbidden_value = remaining / last_coeff;

        // 禁止値がドメインに含まれている場合は除外
        // Model 経由で Trail に記録し、バックトラック時に復元可能にする
        if (last_var->domain().contains(forbidden_value)) {
            if (!model.remove_value(save_point, last_var->id(), forbidden_value)) {
                return false;
            }
        }
    }
    // 割り切れない場合は自動的に制約を満たす

    return true;
}

void IntLinNeConstraint::check_initial_consistency() {
    // 全変数が確定している場合のみチェック
    if (unfixed_count_ == 0) {
        if (current_fixed_sum_ == target_) {
            set_initially_inconsistent(true);
        }
    }
}

bool IntLinNeConstraint::on_final_instantiate() {
    int64_t sum = 0;
    for (size_t i = 0; i < vars_.size(); ++i) {
        sum += coeffs_[i] * vars_[i]->assigned_value().value();
    }
    return sum != target_;
}

void IntLinNeConstraint::rewind_to(int save_point) {
    while (!trail_.empty() && trail_.back().first > save_point) {
        const auto& entry = trail_.back().second;
        current_fixed_sum_ = entry.fixed_sum;
        unfixed_count_ = entry.unfixed_count;
        trail_.pop_back();
    }
}

bool IntLinNeConstraint::presolve(Model& model) {
    // 全ての係数が0の場合: 0 != target
    if (vars_.empty()) {
        return target_ != 0;
    }

    // 変数の現在状態に基づいて内部状態を初期化
    current_fixed_sum_ = 0;
    unfixed_count_ = 0;

    for (size_t i = 0; i < vars_.size(); ++i) {
        int64_t c = coeffs_[i];

        if (vars_[i]->is_assigned()) {
            current_fixed_sum_ += c * vars_[i]->assigned_value().value();
        } else {
            ++unfixed_count_;
        }
    }

    // 2WL を初期化
    init_watches();

    // trail をクリア
    trail_.clear();

    // 初期整合性チェック: 全変数確定で sum == target なら矛盾
    if (unfixed_count_ == 0 && current_fixed_sum_ == target_) {
        return false;
    }

    return true;
}

// ============================================================================

}  // namespace sabori_csp

