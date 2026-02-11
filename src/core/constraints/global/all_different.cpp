#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <set>

namespace sabori_csp {

// ============================================================================
// AllDifferentConstraint implementation
// ============================================================================

AllDifferentConstraint::AllDifferentConstraint(std::vector<VariablePtr> vars)
    : Constraint(vars)
    , pool_n_(0)
    , unfixed_count_(0) {
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

    // 既に確定している変数の値をプールから削除 + 未確定カウント初期化
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
        } else {
            ++unfixed_count_;
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

bool AllDifferentConstraint::prepare_propagation(Model& model) {
    // presolve 後の変数状態に基づいてプールと未確定カウントを再構築
    pool_n_ = pool_values_.size();
    for (size_t i = 0; i < pool_n_; ++i) {
        pool_sparse_[pool_values_[i]] = i;
    }
    unfixed_count_ = 0;
    pool_trail_.clear();

    for (const auto& var : vars_) {
        if (var->is_assigned()) {
            auto val = var->assigned_value().value();
            auto it = pool_sparse_.find(val);
            if (it != pool_sparse_.end() && it->second < pool_n_) {
                size_t idx = it->second;
                size_t last_idx = pool_n_ - 1;
                Domain::value_type last_val = pool_values_[last_idx];

                pool_values_[idx] = last_val;
                pool_values_[last_idx] = val;
                pool_sparse_[last_val] = idx;
                pool_sparse_[val] = last_idx;
                --pool_n_;
            } else {
                // 値がプールにない = 重複
                return false;
            }
        } else {
            ++unfixed_count_;
        }
    }

    if (unfixed_count_ > pool_n_) return false;

    return true;
}

bool AllDifferentConstraint::presolve(Model& model) {
    // 確定した変数の値を他の変数から削除
    for (const auto& var : vars_) {
        if (var->is_assigned()) {
            auto val = var->assigned_value().value();
            for (const auto& other : vars_) {
                if (other != var && !other->is_assigned()) {
                    other->remove(val);
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
        pool_trail_.push_back({save_point, {pool_n_, unfixed_count_}});
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

bool AllDifferentConstraint::on_instantiate(Model& model, int save_point,
					    size_t var_idx, Domain::value_type value,
					    Domain::value_type prev_min,
					    Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, value, prev_min, prev_max)) {
      return false;
    }

    // プールから値を削除
    auto it = pool_sparse_.find(value);
    if (it == pool_sparse_.end() || it->second >= pool_n_) {
        // 既にプールにない = 他の変数が使用済み
        return false;
    }

    // Trail に保存
    if (pool_trail_.empty() || pool_trail_.back().first != save_point) {
        pool_trail_.push_back({save_point, {pool_n_, unfixed_count_}});
        model.mark_constraint_dirty(model_index(), save_point);
    }

    // プールから削除
    size_t idx = it->second;
    size_t last_idx = pool_n_ - 1;
    Domain::value_type last_val = pool_values_[last_idx];

    pool_values_[idx] = last_val;
    pool_values_[last_idx] = value;
    pool_sparse_[last_val] = idx;
    pool_sparse_[value] = last_idx;
    --pool_n_;

    // 未確定カウントをデクリメント（O(1)）
    --unfixed_count_;

    // 鳩の巣原理: 未確定変数 > 利用可能な値 なら矛盾
    if (unfixed_count_ > pool_n_) {
        return false;
    }

    if (unfixed_count_ <= 1) {
      size_t last_idx = SIZE_MAX;
      if (!vars_[watch1()]->is_assigned()) {
	last_idx = watch1();
      }
      if (!vars_[watch2()]->is_assigned()) {
	last_idx = watch2();
      }

      if (last_idx == SIZE_MAX) {
        return on_final_instantiate();
      }
      else {
	// 残り1変数になったら on_last_uninstantiated を呼び出す
	return on_last_uninstantiated(model, save_point, last_idx);
      }
    }

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
        model.enqueue_instantiate(last_var->id(), remaining_value);
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
    for (const auto& var : vars_) {
        if (var->is_assigned()) {
            auto val = var->assigned_value().value();
            if (used_values.count(val) > 0) {
                set_initially_inconsistent(true);
                return;
            }
            used_values.insert(val);
        }
    }

    // 鳩の巣原理: 未確定変数の数 > 利用可能な値の数 なら矛盾
    if (unfixed_count_ > pool_n_) {
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
        const auto& entry = pool_trail_.back().second;
        pool_n_ = entry.old_pool_n;
        unfixed_count_ = entry.old_unfixed_count;
        pool_trail_.pop_back();
    }
}

}  // namespace sabori_csp
