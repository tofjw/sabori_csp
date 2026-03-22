#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <set>

namespace sabori_csp {

// ============================================================================
// AllDifferentExcept0Constraint implementation
// ============================================================================

AllDifferentExcept0Constraint::AllDifferentExcept0Constraint(std::vector<VariablePtr> vars)
    : Constraint(extract_var_ids(vars))
    , pool_n_(0)
    , unfixed_count_(0) {
    // 全変数の値の和集合をプールとして構築（値0を除外）
    std::set<Domain::value_type> all_values;
    for (const auto& var : vars) {
        var->domain().for_each_value([&](auto v) {
            if (v != 0) {
                all_values.insert(v);
            }
        });
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
}

std::string AllDifferentExcept0Constraint::name() const {
    return "all_different_except_0";
}

bool AllDifferentExcept0Constraint::prepare_propagation(Model& model) {
    // presolve 後の変数状態に基づいてプールと未確定カウントを再構築
    pool_n_ = pool_values_.size();
    for (size_t i = 0; i < pool_n_; ++i) {
        pool_sparse_[pool_values_[i]] = i;
    }
    unfixed_count_ = 0;
    pool_trail_.clear();

    for (size_t i = 0; i < var_ids_.size(); ++i) {
        if (model.is_instantiated(var_ids_[i])) {
            auto val = model.value(var_ids_[i]);
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
            model.set_no_bisect(var_ids_[i]);
        }
    }

    // 鳩の巣原理は適用不可（unfixed変数は0を取れる）

    return true;
}

PresolveResult AllDifferentExcept0Constraint::presolve(Model& model) {
    bool changed = false;
    // 非0の確定値のみを他の変数から削除
    for (size_t i = 0; i < var_ids_.size(); ++i) {
        auto* var = model.variable(var_ids_[i]);
        if (var->is_assigned()) {
            auto val = var->assigned_value().value();
            if (val != 0) {
                for (size_t j = 0; j < var_ids_.size(); ++j) {
                    if (j != i) {
                        auto* other = model.variable(var_ids_[j]);
                        if (!other->is_assigned()) {
                            if (other->domain().contains(val)) {
                                other->remove(val);
                                changed = true;
                            }
                            if (other->domain().empty()) {
                                return PresolveResult::Contradiction;
                            }
                        }
                    }
                }
            }
        }
    }
    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
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
            if (!model.is_instantiated(var_ids_[watch1()])) {
                last_idx = watch1();
            }
            if (!model.is_instantiated(var_ids_[watch2()])) {
                last_idx = watch2();
            }

            if (last_idx == SIZE_MAX) {
                return on_final_instantiate(model);
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

    // 確定した非0値を他の未確定変数のドメインから削除
    for (size_t i = 0; i < var_ids_.size(); ++i) {
        auto vid = var_ids_[i];
        if (!model.is_instantiated(vid) && model.contains(vid, value)) {
            model.enqueue_remove_value(vid, value);
        }
    }

    if (unfixed_count_ <= 1) {
        size_t last_var_idx = SIZE_MAX;
        if (!model.is_instantiated(var_ids_[watch1()])) {
            last_var_idx = watch1();
        }
        if (!model.is_instantiated(var_ids_[watch2()])) {
            last_var_idx = watch2();
        }

        if (last_var_idx == SIZE_MAX) {
            return on_final_instantiate(model);
        } else {
            return on_last_uninstantiated(model, save_point, last_var_idx);
        }
    }

    return true;
}

bool AllDifferentExcept0Constraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                                            size_t last_var_internal_idx) {
    auto last_var_id = var_ids_[last_var_internal_idx];

    // 既に確定している場合は整合性チェックのみ
    if (model.is_instantiated(last_var_id)) {
        auto val = model.value(last_var_id);
        if (val == 0) return true;  // 0は常にOK
        // その値がプールに残っているか
        auto it = pool_sparse_.find(val);
        return (it != pool_sparse_.end() && it->second < pool_n_);
    }

    if (pool_n_ == 0) {
        // 全ての非0値が使用済み。変数は0しか取れない
        if (model.contains(last_var_id, 0)) {
            model.enqueue_instantiate(last_var_id, 0);
        } else {
            return false;
        }
    } else if (pool_n_ == 1 && !model.contains(last_var_id, 0)) {
        // 0がドメインにない場合のみ、残りの非0値で確定
        Domain::value_type remaining_value = pool_values_[0];
        if (model.contains(last_var_id, remaining_value)) {
            model.enqueue_instantiate(last_var_id, remaining_value);
        } else {
            return false;
        }
    }
    // pool_n_ >= 1 かつ 0がドメインにある場合: 0も取れるので確定しない

    return true;
}

bool AllDifferentExcept0Constraint::on_final_instantiate(const Model& model) {
    // 非0値のみユニークチェック
    std::set<Domain::value_type> used_values;
    for (size_t i = 0; i < var_ids_.size(); ++i) {
        auto val = model.value(var_ids_[i]);
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

bool AllDifferentExcept0Constraint::check_hall_pair(Model& model, size_t trigger_var_idx) {
    auto& vd = model.var_data(trigger_var_idx);
    if (vd.size != 2) return true;

    auto v1 = vd.min;
    auto v2 = vd.max;

    // 0を含むドメインは Hall pair 候補としない（0は重複可能）
    if (v1 == 0 || v2 == 0) return true;

    // 同じ {v1, v2} ドメインを持つ未確定変数を数える
    size_t match_count = 0;
    for (size_t i = 0; i < var_ids_.size(); ++i) {
        auto vid = var_ids_[i];
        if (model.is_instantiated(vid)) continue;
        auto& d = model.var_data(vid);
        if (d.size == 2 && d.min == v1 && d.max == v2) {
            ++match_count;
        }
    }

    if (match_count >= 3) {
        return false;
    }

    if (match_count == 2) {
        // 残りの変数から v1, v2 を削除
        for (size_t i = 0; i < var_ids_.size(); ++i) {
            auto vid = var_ids_[i];
            if (model.is_instantiated(vid)) continue;
            auto& d = model.var_data(vid);
            if (d.size == 2 && d.min == v1 && d.max == v2) continue;
            if (model.contains(vid, v1)) {
                model.enqueue_remove_value(vid, v1);
            }
            if (model.contains(vid, v2)) {
                model.enqueue_remove_value(vid, v2);
            }
        }
    }

    return true;
}

bool AllDifferentExcept0Constraint::on_remove_value(Model& model, int save_point,
                                                     size_t var_idx, size_t internal_var_idx,
                                                     Domain::value_type removed_value) {
    return check_hall_pair(model, var_idx);
}

bool AllDifferentExcept0Constraint::on_set_min(Model& model, int save_point,
                                                size_t var_idx, size_t internal_var_idx,
                                                Domain::value_type new_min,
                                                Domain::value_type old_min) {
    return check_hall_pair(model, var_idx);
}

bool AllDifferentExcept0Constraint::on_set_max(Model& model, int save_point,
                                                size_t var_idx, size_t internal_var_idx,
                                                Domain::value_type new_max,
                                                Domain::value_type old_max) {
    return check_hall_pair(model, var_idx);
}

void AllDifferentExcept0Constraint::bump_activity(const Model& model, size_t trigger_var_idx,
                                                   double* activity, double activity_inc,
                                                   bool& need_rescale, std::mt19937& rng) const {
    // --- Case 1: trigger 変数が未確定 (Hall pair 失敗等) ---
    if (!model.is_instantiated(trigger_var_idx)) {
        auto& vd = model.var_data(trigger_var_idx);
        if (vd.size == 2) {
            auto v1 = vd.min;
            auto v2 = vd.max;
            if (v1 != 0 && v2 != 0) {
                // Hall pair 失敗: 同じ {v1,v2} ドメインを持つ未確定変数を bump
                // + v1, v2 を既に使っている確定変数も bump（それらが Hall pair を作った原因）
                size_t count = 0;
                for (size_t vid : var_ids_) {
                    if (model.is_instantiated(vid)) {
                        auto v = model.value(vid);
                        if (v == v1 || v == v2) ++count;
                    } else {
                        auto& d = model.var_data(vid);
                        if (d.size == 2 && d.min == v1 && d.max == v2) ++count;
                    }
                }
                if (count == 0) count = 1;
                double inc = activity_inc / count;
                for (size_t vid : var_ids_) {
                    if (model.is_instantiated(vid)) {
                        auto v = model.value(vid);
                        if (v == v1 || v == v2) {
                            bump_variable_activity(activity, vid, inc, need_rescale, rng);
                        }
                    } else {
                        auto& d = model.var_data(vid);
                        if (d.size == 2 && d.min == v1 && d.max == v2) {
                            bump_variable_activity(activity, vid, inc, need_rescale, rng);
                        }
                    }
                }
                return;
            }
        }
        // Hall pair 以外の未確定 trigger → デフォルト
        Constraint::bump_activity(model, trigger_var_idx, activity, activity_inc, need_rescale, rng);
        return;
    }

    auto val = model.value(trigger_var_idx);

    // --- Case 2: 値が 0 の場合 ---
    if (val == 0) {
        // プール枯渇が原因の可能性: 非0値を使った変数を優先的に bump
        // (0を選んだこと自体は問題ないが、プールを使い切った変数が原因)
        size_t nonzero_count = 0;
        for (size_t vid : var_ids_) {
            if (model.is_instantiated(vid) && model.value(vid) != 0) {
                ++nonzero_count;
            }
        }
        if (nonzero_count > 0 && pool_n_ == 0) {
            // プール枯渇: 非0確定変数に重み付け bump
            double inc = activity_inc / nonzero_count;
            for (size_t vid : var_ids_) {
                if (model.is_instantiated(vid) && model.value(vid) != 0) {
                    bump_variable_activity(activity, vid, inc, need_rescale, rng);
                }
            }
            return;
        }
        Constraint::bump_activity(model, trigger_var_idx, activity, activity_inc, need_rescale, rng);
        return;
    }

    // --- Case 3: 非0値の重複 ---
    auto it = pool_sparse_.find(val);
    if (it != pool_sparse_.end() && it->second >= pool_n_) {
        // 主犯: 同じ非0値を持つ変数
        // 共犯: 0を選んだが、conflicting value をドメインに持っていた変数
        //        (0ではなくその値を選んでいれば衝突しなかった可能性)
        size_t primary_count = 0;
        size_t accomplice_count = 0;
        for (size_t vid : var_ids_) {
            if (model.is_instantiated(vid)) {
                if (model.value(vid) == val) {
                    ++primary_count;
                }
            }
        }
        if (primary_count == 0) primary_count = 1;

        // 主犯に 70%, 共犯に 30% を分配
        double primary_inc = activity_inc * 0.7 / primary_count;
        for (size_t vid : var_ids_) {
            if (model.is_instantiated(vid) && model.value(vid) == val) {
                bump_variable_activity(activity, vid, primary_inc, need_rescale, rng);
            }
        }

        // 未確定変数で、conflicting value をドメインに持つものも bump
        // (次の判定で優先的に選ばれるように)
        for (size_t vid : var_ids_) {
            if (!model.is_instantiated(vid) && model.contains(vid, val)) {
                ++accomplice_count;
            }
        }
        if (accomplice_count > 0) {
            double acc_inc = activity_inc * 0.3 / accomplice_count;
            for (size_t vid : var_ids_) {
                if (!model.is_instantiated(vid) && model.contains(vid, val)) {
                    bump_variable_activity(activity, vid, acc_inc, need_rescale, rng);
                }
            }
        }
        return;
    }

    // --- Case 4: その他 (他の制約が原因の伝播失敗等) ---
    Constraint::bump_activity(model, trigger_var_idx, activity, activity_inc, need_rescale, rng);
}

}  // namespace sabori_csp
