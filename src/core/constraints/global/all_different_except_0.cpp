#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <set>

namespace sabori_csp {

// ============================================================================
// AllDifferentExcept0Constraint implementation
// ============================================================================

AllDifferentExcept0Constraint::AllDifferentExcept0Constraint(std::vector<VariablePtr> vars)
    : Constraint(vars)
    , pool_n_(0)
    , unfixed_count_(0) {
    // 全変数の値の和集合をプールとして構築（値0を除外）
    std::set<Domain::value_type> all_values;
    for (const auto& var : vars) {
        for (auto v : var->domain().values()) {
            if (v != 0) {
                all_values.insert(v);
            }
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
            if (val != 0) {
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
            // 値0の場合はプール操作をスキップ
        } else {
            ++unfixed_count_;
        }
    }

    // 初期整合性チェック
    check_initial_consistency();
}

std::string AllDifferentExcept0Constraint::name() const {
    return "all_different_except_0";
}

std::vector<VariablePtr> AllDifferentExcept0Constraint::variables() const {
    return vars_;
}

std::optional<bool> AllDifferentExcept0Constraint::is_satisfied() const {
    std::set<Domain::value_type> used_values;
    for (const auto& var : vars_) {
        if (!var->is_assigned()) {
            return std::nullopt;
        }
        auto val = var->assigned_value().value();
        if (val != 0) {
            if (used_values.count(val) > 0) {
                return false;
            }
            used_values.insert(val);
        }
    }
    return true;
}

bool AllDifferentExcept0Constraint::prepare_propagation(Model& model) {
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
            if (val != 0) {
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
                    // 非0値がプールにない = 重複
                    return false;
                }
            }
            // 値0の場合はプール操作をスキップ
        } else {
            ++unfixed_count_;
        }
    }

    // 鳩の巣原理は適用不可（unfixed変数は0を取れる）

    return true;
}

bool AllDifferentExcept0Constraint::presolve(Model& model) {
    // 非0の確定値のみを他の変数から削除
    for (const auto& var : vars_) {
        if (var->is_assigned()) {
            auto val = var->assigned_value().value();
            if (val != 0) {
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
    }
    return true;
}

bool AllDifferentExcept0Constraint::remove_from_pool(int save_point, Domain::value_type value) {
    auto it = pool_sparse_.find(value);
    if (it == pool_sparse_.end() || it->second >= pool_n_) {
        return false;  // 既にプールにない
    }

    // Trail に保存
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

bool AllDifferentExcept0Constraint::on_instantiate(Model& model, int save_point,
                                                    size_t var_idx, size_t internal_var_idx,
                                                    Domain::value_type value,
                                                    Domain::value_type prev_min,
                                                    Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value, prev_min, prev_max)) {
        return false;
    }

    // Trail に保存（unfixed_count 更新のため）
    if (pool_trail_.empty() || pool_trail_.back().first != save_point) {
        pool_trail_.push_back({save_point, {pool_n_, unfixed_count_}});
        model.mark_constraint_dirty(model_index(), save_point);
    }

    // 未確定カウントをデクリメント
    --unfixed_count_;

    // 値が0の場合はプール操作をスキップ
    if (value == 0) {
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
            } else {
                return on_last_uninstantiated(model, save_point, last_idx);
            }
        }
        return true;
    }

    // 非0値: プールから削除
    auto it = pool_sparse_.find(value);
    if (it == pool_sparse_.end() || it->second >= pool_n_) {
        // 既にプールにない = 他の変数が使用済み
        return false;
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

    // 鳩の巣原理は適用不可（unfixed変数は0を取れるため）

    if (unfixed_count_ <= 1) {
        size_t last_var_idx = SIZE_MAX;
        if (!vars_[watch1()]->is_assigned()) {
            last_var_idx = watch1();
        }
        if (!vars_[watch2()]->is_assigned()) {
            last_var_idx = watch2();
        }

        if (last_var_idx == SIZE_MAX) {
            return on_final_instantiate();
        } else {
            return on_last_uninstantiated(model, save_point, last_var_idx);
        }
    }

    return true;
}

bool AllDifferentExcept0Constraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                                            size_t last_var_internal_idx) {
    auto& last_var = vars_[last_var_internal_idx];

    // 既に確定している場合は整合性チェックのみ
    if (last_var->is_assigned()) {
        auto val = last_var->assigned_value().value();
        if (val == 0) return true;  // 0は常にOK
        // その値がプールに残っているか
        auto it = pool_sparse_.find(val);
        return (it != pool_sparse_.end() && it->second < pool_n_);
    }

    if (pool_n_ == 0) {
        // 全ての非0値が使用済み。変数は0しか取れない
        if (last_var->domain().contains(0)) {
            model.enqueue_instantiate(last_var->id(), 0);
        } else {
            return false;
        }
    } else if (pool_n_ == 1 && !last_var->domain().contains(0)) {
        // 0がドメインにない場合のみ、残りの非0値で確定
        Domain::value_type remaining_value = pool_values_[0];
        if (last_var->domain().contains(remaining_value)) {
            model.enqueue_instantiate(last_var->id(), remaining_value);
        } else {
            return false;
        }
    }
    // pool_n_ >= 1 かつ 0がドメインにある場合: 0も取れるので確定しない

    return true;
}

void AllDifferentExcept0Constraint::check_initial_consistency() {
    // 非0の確定値が重複していれば矛盾
    std::set<Domain::value_type> used_values;
    for (const auto& var : vars_) {
        if (var->is_assigned()) {
            auto val = var->assigned_value().value();
            if (val != 0) {
                if (used_values.count(val) > 0) {
                    set_initially_inconsistent(true);
                    return;
                }
                used_values.insert(val);
            }
        }
    }

    // 鳩の巣原理は適用不可（unfixed変数は0を取れる）
}

bool AllDifferentExcept0Constraint::on_final_instantiate() {
    // 非0値のみユニークチェック
    std::set<Domain::value_type> used_values;
    for (const auto& var : vars_) {
        auto val = var->assigned_value().value();
        if (val != 0) {
            if (used_values.count(val) > 0) {
                return false;
            }
            used_values.insert(val);
        }
    }
    return true;
}

void AllDifferentExcept0Constraint::rewind_to(int save_point) {
    while (!pool_trail_.empty() && pool_trail_.back().first > save_point) {
        const auto& entry = pool_trail_.back().second;
        pool_n_ = entry.old_pool_n;
        unfixed_count_ = entry.old_unfixed_count;
        pool_trail_.pop_back();
    }
}

}  // namespace sabori_csp
