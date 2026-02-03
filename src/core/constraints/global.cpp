#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <set>
#include <limits>

namespace sabori_csp {

// ============================================================================
// AllDifferentConstraint implementation
// ============================================================================

AllDifferentConstraint::AllDifferentConstraint(std::vector<VariablePtr> vars)
    : Constraint(vars)
    , pool_n_(0) {
    // 全変数の値の和集合をプールとして構築
    std::set<Domain::value_type> all_values;
    for (const auto& var : vars) {
        for (auto v : var->domain().values()) {
            all_values.insert(v);
        }
    }

    pool_values_.assign(all_values.begin(), all_values.end());
    pool_n_ = pool_values_.size();
    for (size_t i = 0; i < pool_n_; ++i) {
        pool_sparse_[pool_values_[i]] = i;
    }

    // 既に確定している変数の値をプールから削除
    for (const auto& var : vars) {
        if (var->is_assigned()) {
            auto val = var->assigned_value().value();
            auto it = pool_sparse_.find(val);
            if (it != pool_sparse_.end() && it->second < pool_n_) {
                // Sparse Set から削除（スワップ）
                size_t idx = it->second;
                size_t last_idx = pool_n_ - 1;
                Domain::value_type last_val = pool_values_[last_idx];

                pool_values_[idx] = last_val;
                pool_values_[last_idx] = val;
                pool_sparse_[last_val] = idx;
                pool_sparse_[val] = last_idx;
                --pool_n_;
            }
        }
    }

    // 初期整合性チェック
    check_initial_consistency();
}

std::string AllDifferentConstraint::name() const {
    return "all_different";
}

std::vector<VariablePtr> AllDifferentConstraint::variables() const {
    return vars_;
}

std::optional<bool> AllDifferentConstraint::is_satisfied() const {
    std::set<Domain::value_type> used_values;
    for (const auto& var : vars_) {
        if (!var->is_assigned()) {
            return std::nullopt;
        }
        auto val = var->assigned_value().value();
        if (used_values.count(val) > 0) {
            return false;
        }
        used_values.insert(val);
    }
    return true;
}

bool AllDifferentConstraint::propagate() {
    // 確定した変数の値を他の変数から削除
    for (const auto& var : vars_) {
        if (var->is_assigned()) {
            auto val = var->assigned_value().value();
            for (const auto& other : vars_) {
                if (other != var && !other->is_assigned()) {
                    other->domain().remove(val);
                    if (other->domain().empty()) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

bool AllDifferentConstraint::remove_from_pool(int save_point, Domain::value_type value) {
    auto it = pool_sparse_.find(value);
    if (it == pool_sparse_.end() || it->second >= pool_n_) {
        return false;  // 既にプールにない
    }

    // Trail に保存（同一レベルでも複数回保存する可能性あり）
    if (pool_trail_.empty() || pool_trail_.back().first != save_point) {
        pool_trail_.push_back({save_point, pool_n_});
    }

    // Sparse Set から削除（スワップ）
    size_t idx = it->second;
    size_t last_idx = pool_n_ - 1;
    Domain::value_type last_val = pool_values_[last_idx];

    pool_values_[idx] = last_val;
    pool_values_[last_idx] = value;
    pool_sparse_[last_val] = idx;
    pool_sparse_[value] = last_idx;
    --pool_n_;

    return true;
}

bool AllDifferentConstraint::on_instantiate(Model& /*model*/, int save_point,
                                             size_t /*var_idx*/, Domain::value_type value,
                                             Domain::value_type /*prev_min*/,
                                             Domain::value_type /*prev_max*/) {
    // 基底クラスの 2WL 処理は省略（AllDifferent は全変数を監視）

    // プールから値を削除
    auto it = pool_sparse_.find(value);
    if (it == pool_sparse_.end() || it->second >= pool_n_) {
        // 既にプールにない = 他の変数が使用済み
        return false;
    }

    remove_from_pool(save_point, value);

    // 鳩の巣原理: 未確定変数 > 利用可能な値 なら矛盾
    size_t unfixed_count = 0;
    for (size_t i = 0; i < vars_.size(); ++i) {
        if (!vars_[i]->is_assigned()) {
            ++unfixed_count;
        }
    }
    if (unfixed_count > pool_n_) {
        return false;
    }

    // 残り1変数の処理は on_last_uninstantiated() に委譲
    return true;
}

bool AllDifferentConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                                      size_t last_var_internal_idx) {
    auto& last_var = vars_[last_var_internal_idx];

    // 既に確定している場合は整合性チェックのみ
    if (last_var->is_assigned()) {
        auto val = last_var->assigned_value().value();
        // その値がプールに残っているか（他の変数と重複していないか）
        auto it = pool_sparse_.find(val);
        return (it != pool_sparse_.end() && it->second < pool_n_);
    }

    // 利用可能な値が1つだけなら自動決定
    if (pool_n_ == 1) {
        Domain::value_type remaining_value = pool_values_[0];
        // モデル内の変数インデックスを特定
        for (size_t model_idx = 0; model_idx < model.variables().size(); ++model_idx) {
            if (model.variable(model_idx) == last_var) {
                model.enqueue_instantiate(model_idx, remaining_value);
                break;
            }
        }
    }
    // 利用可能な値が0なら矛盾
    else if (pool_n_ == 0) {
        return false;
    }
    // 複数の値が利用可能な場合は、ドメインを絞り込む
    // 注: ここでは単純化のため、確定のみを行う

    return true;
}

void AllDifferentConstraint::check_initial_consistency() {
    // 既に確定している変数の値が重複していれば矛盾
    std::set<Domain::value_type> used_values;
    size_t uninstantiated_count = 0;
    for (const auto& var : vars_) {
        if (var->is_assigned()) {
            auto val = var->assigned_value().value();
            if (used_values.count(val) > 0) {
                set_initially_inconsistent(true);
                return;
            }
            used_values.insert(val);
        } else {
            ++uninstantiated_count;
        }
    }

    // 鳩の巣原理: 未確定変数の数 > 利用可能な値の数 なら矛盾
    if (uninstantiated_count > pool_n_) {
        set_initially_inconsistent(true);
    }
}

bool AllDifferentConstraint::on_final_instantiate() {
    // 全変数が異なる値を持つか確認
    std::set<Domain::value_type> used_values;
    for (const auto& var : vars_) {
        auto val = var->assigned_value().value();
        if (used_values.count(val) > 0) {
            return false;
        }
        used_values.insert(val);
    }
    return true;
}

void AllDifferentConstraint::rewind_to(int save_point) {
    while (!pool_trail_.empty() && pool_trail_.back().first > save_point) {
        pool_n_ = pool_trail_.back().second;
        pool_trail_.pop_back();
    }
}

// ============================================================================
// IntLinEqConstraint implementation
// ============================================================================

IntLinEqConstraint::IntLinEqConstraint(std::vector<int64_t> coeffs,
                                         std::vector<VariablePtr> vars,
                                         int64_t target_sum)
    : Constraint(vars)
    , target_sum_(target_sum)
    , current_fixed_sum_(0)
    , min_rem_potential_(0)
    , max_rem_potential_(0) {
    // 同一変数の係数を集約
    std::unordered_map<Variable*, int64_t> aggregated;
    for (size_t i = 0; i < vars.size(); ++i) {
        aggregated[vars[i].get()] += coeffs[i];
    }

    // 一意な変数リストと係数リストを再構築
    vars_.clear();
    coeffs_.clear();
    for (const auto& [var_ptr, coeff] : aggregated) {
        // shared_ptr を探す
        for (const auto& var : vars) {
            if (var.get() == var_ptr) {
                vars_.push_back(var);
                coeffs_.push_back(coeff);
                break;
            }
        }
    }

    // 初期ポテンシャルを計算（既に確定している変数は fixed_sum に加算）
    for (size_t i = 0; i < vars_.size(); ++i) {
        int64_t c = coeffs_[i];

        if (vars_[i]->is_assigned()) {
            // 既に確定している変数
            current_fixed_sum_ += c * vars_[i]->assigned_value().value();
        } else {
            // 未確定の変数
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

    // 2WL を再初期化
    init_watches();

    // 初期整合性チェック
    check_initial_consistency();
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

bool IntLinEqConstraint::propagate() {
    // 簡易的な bounds propagation
    // TODO: より効率的な実装
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

bool IntLinEqConstraint::on_instantiate(Model& /*model*/, int save_point,
                                          size_t var_idx, Domain::value_type value,
                                          Domain::value_type prev_min,
                                          Domain::value_type prev_max) {
    // var_idx は Model 内のインデックス。内部インデックスを特定
    size_t internal_idx = SIZE_MAX;
    for (size_t i = 0; i < vars_.size(); ++i) {
        // Variable ポインタで照合（または別の方法で特定）
        if (vars_[i]->is_assigned() && vars_[i]->assigned_value().value() == value) {
            // 確定直後の変数を特定
            // 注: この方法は完全ではないが、暫定的な実装
            if (vars_[i]->domain().min() == prev_min && vars_[i]->domain().max() == prev_max) {
                internal_idx = i;
                break;
            }
        }
    }

    // 見つからない場合は最初に見つかった singleton を使用
    if (internal_idx == SIZE_MAX) {
        for (size_t i = 0; i < vars_.size(); ++i) {
            if (vars_[i]->is_assigned() && vars_[i]->assigned_value().value() == value) {
                internal_idx = i;
                break;
            }
        }
    }

    if (internal_idx == SIZE_MAX) {
        // この制約に関係ない変数
        return true;
    }

    // Look-ahead チェック
    if (!can_assign(internal_idx, value, prev_min, prev_max)) {
        return false;
    }

    // Trail に保存
    if (trail_.empty() || trail_.back().first != save_point) {
        trail_.push_back({save_point, {current_fixed_sum_, min_rem_potential_, max_rem_potential_}});
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

    // 残り1変数の処理は on_last_uninstantiated() に委譲
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
        trail_.pop_back();
    }
}

// ============================================================================
// IntLinLeConstraint implementation
// ============================================================================

IntLinLeConstraint::IntLinLeConstraint(std::vector<int64_t> coeffs,
                                         std::vector<VariablePtr> vars,
                                         int64_t bound)
    : Constraint(vars)
    , coeffs_(std::move(coeffs))
    , bound_(bound)
    , current_fixed_sum_(0)
    , min_rem_potential_(0) {
    // 初期ポテンシャルを計算（既に確定している変数は fixed_sum に加算）
    for (size_t i = 0; i < vars_.size(); ++i) {
        int64_t c = coeffs_[i];

        if (vars_[i]->is_assigned()) {
            // 既に確定している変数
            current_fixed_sum_ += c * vars_[i]->assigned_value().value();
        } else {
            // 未確定の変数
            auto min_val = vars_[i]->domain().min().value();
            auto max_val = vars_[i]->domain().max().value();

            if (c >= 0) {
                min_rem_potential_ += c * min_val;
            } else {
                min_rem_potential_ += c * max_val;
            }
        }
    }

    // 初期整合性チェック
    check_initial_consistency();
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

bool IntLinLeConstraint::propagate() {
    return true;
}

bool IntLinLeConstraint::on_instantiate(Model& /*model*/, int save_point,
                                          size_t /*var_idx*/, Domain::value_type value,
                                          Domain::value_type prev_min,
                                          Domain::value_type prev_max) {
    // 確定した変数を特定
    size_t internal_idx = SIZE_MAX;
    for (size_t i = 0; i < vars_.size(); ++i) {
        if (vars_[i]->is_assigned() && vars_[i]->assigned_value().value() == value) {
            internal_idx = i;
            break;
        }
    }

    if (internal_idx == SIZE_MAX) {
        return true;
    }

    int64_t c = coeffs_[internal_idx];

    // Trail に保存
    if (trail_.empty() || trail_.back().first != save_point) {
        trail_.push_back({save_point, {current_fixed_sum_, min_rem_potential_}});
    }

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

} // namespace sabori_csp
