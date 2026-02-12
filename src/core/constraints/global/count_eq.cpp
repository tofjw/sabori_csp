#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>

namespace sabori_csp {

// ============================================================================
// CountEqConstraint implementation
// ============================================================================

CountEqConstraint::CountEqConstraint(std::vector<VariablePtr> x_vars,
                                       Domain::value_type target,
                                       VariablePtr count_var)
    : Constraint(std::vector<VariablePtr>())  // 後で設定
    , target_(target)
    , n_(x_vars.size())
    , definite_count_(0)
    , possible_count_(0) {
    // vars_ = [x[0], x[1], ..., x[n-1], c]
    vars_.reserve(n_ + 1);
    for (auto& v : x_vars) {
        vars_.push_back(std::move(v));
    }
    vars_.push_back(std::move(count_var));

    is_possible_.resize(n_, false);

    // 変数IDキャッシュを構築
    update_var_ids();
}

std::string CountEqConstraint::name() const {
    return "count_eq";
}

std::vector<VariablePtr> CountEqConstraint::variables() const {
    return vars_;
}

std::optional<bool> CountEqConstraint::is_satisfied() const {
    // 全変数が確定しているか確認
    for (const auto& var : vars_) {
        if (!var->is_assigned()) {
            return std::nullopt;
        }
    }
    // 全確定: count を計算してチェック
    int64_t count = 0;
    for (size_t i = 0; i < n_; ++i) {
        if (vars_[i]->assigned_value().value() == target_) {
            count++;
        }
    }
    return count == vars_[n_]->assigned_value().value();
}

bool CountEqConstraint::presolve(Model& model) {
    // 初期 definite/possible カウントを計算
    definite_count_ = 0;
    possible_count_ = 0;
    for (size_t i = 0; i < n_; ++i) {
        if (vars_[i]->is_assigned()) {
            if (vars_[i]->assigned_value().value() == target_) {
                definite_count_++;
            }
            is_possible_[i] = false;
        } else if (vars_[i]->domain().contains(target_)) {
            is_possible_[i] = true;
            possible_count_++;
        } else {
            is_possible_[i] = false;
        }
    }

    // c の bounds を絞り込む
    auto& c_var = vars_[n_];
    auto c_min = c_var->min();
    auto c_max = c_var->max();
    auto new_min = static_cast<Domain::value_type>(definite_count_);
    auto new_max = static_cast<Domain::value_type>(definite_count_ + possible_count_);

    if (new_min > c_max || new_max < c_min) {
        return false;  // 矛盾
    }
    if (new_min > c_min) {
        if (!c_var->remove_below(new_min)) return false;
    }
    if (new_max < c_max) {
        if (!c_var->remove_above(new_max)) return false;
    }

    // Forward propagation
    c_min = c_var->min();
    c_max = c_var->max();

    // c.max == definite_count_ → 残りの possible な x[i] から target を除去
    if (c_max == static_cast<Domain::value_type>(definite_count_)) {
        for (size_t i = 0; i < n_; ++i) {
            if (is_possible_[i]) {
                vars_[i]->domain().remove(target_);
                if (vars_[i]->domain().empty()) return false;
                is_possible_[i] = false;
                possible_count_--;
            }
        }
    }

    // c.min == definite_count_ + possible_count_ → 残りの possible な x[i] を target に確定
    if (c_min == static_cast<Domain::value_type>(definite_count_ + possible_count_)) {
        for (size_t i = 0; i < n_; ++i) {
            if (is_possible_[i] && !vars_[i]->is_assigned()) {
                if (!vars_[i]->domain().contains(target_)) return false;
                // Presolve 中は直接ドメインを操作
                auto vals = vars_[i]->domain().values();
                for (auto v : vals) {
                    if (v != target_) {
                        vars_[i]->domain().remove(v);
                    }
                }
                if (vars_[i]->domain().empty()) return false;
                // 確定した
                definite_count_++;
                is_possible_[i] = false;
                possible_count_--;
            }
        }
    }

    return true;
}

bool CountEqConstraint::prepare_propagation(Model& model) {
    // 変数の現在状態に基づいて内部状態を初期化
    definite_count_ = 0;
    possible_count_ = 0;
    for (size_t i = 0; i < n_; ++i) {
        if (vars_[i]->is_assigned()) {
            if (vars_[i]->assigned_value().value() == target_) {
                definite_count_++;
            }
            is_possible_[i] = false;
        } else if (vars_[i]->domain().contains(target_)) {
            is_possible_[i] = true;
            possible_count_++;
        } else {
            is_possible_[i] = false;
        }
    }

    // 2WL を初期化
    init_watches();

    // trail をクリア
    trail_.clear();

    // 初期整合性チェック
    auto c_min = vars_[n_]->min();
    auto c_max = vars_[n_]->max();
    auto def = static_cast<Domain::value_type>(definite_count_);
    auto def_plus_poss = static_cast<Domain::value_type>(definite_count_ + possible_count_);

    if (def > c_max || def_plus_poss < c_min) {
        return false;
    }

    return true;
}

bool CountEqConstraint::on_instantiate(Model& model, int save_point,
                                        size_t var_idx, Domain::value_type value,
                                        Domain::value_type prev_min,
                                        Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, value, prev_min, prev_max)) {
        return false;
    }

    size_t internal_idx = find_internal_idx(var_idx);

    // Trail に保存
    save_trail_if_needed(model, save_point);

    if (internal_idx < n_) {
        // x[i] が確定した
        if (is_possible_[internal_idx]) {
            // is_possible_ の変更を trail の最新エントリに記録
            trail_.back().second.is_possible_changes.push_back({internal_idx, true});
            is_possible_[internal_idx] = false;
            possible_count_--;
            if (value == target_) {
                definite_count_++;
            }
        } else {
            // is_possible_ でなかった場合（target が既にドメインに含まれていなかった）
            // definite_count_ は変わらない
        }
    }
    // else: c が確定した → propagate で処理

    // 残り変数が 1 or 0 の時
    if (has_uninstantiated()) {
        size_t last_idx = find_last_uninstantiated();
        if (last_idx != SIZE_MAX) {
            if (!on_last_uninstantiated(model, save_point, last_idx)) {
                return false;
            }
            return true;
        }
    } else {
        return on_final_instantiate();
    }

    return propagate(model);
}

bool CountEqConstraint::on_final_instantiate() {
    int64_t count = 0;
    for (size_t i = 0; i < n_; ++i) {
        if (vars_[i]->assigned_value().value() == target_) {
            count++;
        }
    }
    return count == vars_[n_]->assigned_value().value();
}

bool CountEqConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                                 size_t last_var_internal_idx) {
    if (last_var_internal_idx == n_) {
        // 最後の未確定変数が c（count 変数）
        // 全 x[i] は確定済み
        auto def = static_cast<Domain::value_type>(definite_count_);
        if (vars_[n_]->is_assigned()) {
            return vars_[n_]->assigned_value().value() == def;
        }
        if (vars_[n_]->domain().contains(def)) {
            model.enqueue_instantiate(vars_[n_]->id(), def);
        } else {
            return false;
        }
    } else {
        // 最後の未確定変数が x[j]
        size_t j = last_var_internal_idx;

        if (vars_[j]->is_assigned()) {
            // 既に確定: final check
            return on_final_instantiate();
        }

        // c は確定済みのはず
        if (!vars_[n_]->is_assigned()) {
            // c もまだ未確定ならロジックエラーだが、安全のため propagate に任せる
            return propagate(model);
        }

        auto cv = vars_[n_]->assigned_value().value();
        auto remaining_needed = cv - static_cast<Domain::value_type>(definite_count_);

        if (remaining_needed == 0) {
            // x[j] は target 以外の値を取る必要がある
            if (is_possible_[j]) {
                if (vars_[j]->domain().size() == 1 && vars_[j]->domain().contains(target_)) {
                    return false;  // target しかない
                }
                model.enqueue_remove_value(vars_[j]->id(), target_);
            }
        } else if (remaining_needed == 1) {
            // x[j] は target でなければならない
            if (vars_[j]->domain().contains(target_)) {
                model.enqueue_instantiate(vars_[j]->id(), target_);
            } else {
                return false;
            }
        } else {
            // remaining_needed < 0 or > 1: 矛盾
            return false;
        }
    }

    return true;
}

bool CountEqConstraint::on_set_min(Model& model, int save_point,
                                    size_t var_idx, Domain::value_type new_min,
                                    Domain::value_type old_min) {
    size_t internal_idx = find_internal_idx(var_idx);

    if (internal_idx < n_) {
        // x[i] の下限更新: target < new_min なら possible でなくなる
        if (is_possible_[internal_idx] && target_ < new_min) {
            save_trail_if_needed(model, save_point);
            trail_.back().second.is_possible_changes.push_back({internal_idx, true});
            is_possible_[internal_idx] = false;
            possible_count_--;
            return propagate(model);
        }
    } else {
        // c の下限更新 → propagate
        save_trail_if_needed(model, save_point);
        return propagate(model);
    }
    return true;
}

bool CountEqConstraint::on_set_max(Model& model, int save_point,
                                    size_t var_idx, Domain::value_type new_max,
                                    Domain::value_type old_max) {
    size_t internal_idx = find_internal_idx(var_idx);

    if (internal_idx < n_) {
        // x[i] の上限更新: target > new_max なら possible でなくなる
        if (is_possible_[internal_idx] && target_ > new_max) {
            save_trail_if_needed(model, save_point);
            trail_.back().second.is_possible_changes.push_back({internal_idx, true});
            is_possible_[internal_idx] = false;
            possible_count_--;
            return propagate(model);
        }
    } else {
        // c の上限更新 → propagate
        save_trail_if_needed(model, save_point);
        return propagate(model);
    }
    return true;
}

bool CountEqConstraint::on_remove_value(Model& model, int save_point,
                                          size_t var_idx, Domain::value_type removed_value) {
    size_t internal_idx = find_internal_idx(var_idx);

    if (internal_idx < n_ && removed_value == target_ && is_possible_[internal_idx]) {
        save_trail_if_needed(model, save_point);
        trail_.back().second.is_possible_changes.push_back({internal_idx, true});
        is_possible_[internal_idx] = false;
        possible_count_--;
        return propagate(model);
    }
    return true;
}

void CountEqConstraint::check_initial_consistency() {
    auto c_min = vars_[n_]->min();
    auto c_max = vars_[n_]->max();
    auto def = static_cast<Domain::value_type>(definite_count_);
    auto def_plus_poss = static_cast<Domain::value_type>(definite_count_ + possible_count_);
    if (def > c_max || def_plus_poss < c_min) {
        set_initially_inconsistent(true);
    }
}

void CountEqConstraint::rewind_to(int save_point) {
    while (!trail_.empty() && trail_.back().first > save_point) {
        const auto& entry = trail_.back().second;
        // is_possible_ の変更を巻き戻す
        for (auto it = entry.is_possible_changes.rbegin();
             it != entry.is_possible_changes.rend(); ++it) {
            is_possible_[it->first] = it->second;
        }
        definite_count_ = entry.definite_count;
        possible_count_ = entry.possible_count;
        trail_.pop_back();
    }
}

void CountEqConstraint::save_trail_if_needed(Model& model, int save_point) {
    if (trail_.empty() || trail_.back().first != save_point) {
        trail_.push_back({save_point, {definite_count_, possible_count_, {}}});
        model.mark_constraint_dirty(model_index(), save_point);
    }
}

bool CountEqConstraint::propagate(Model& model) {
    auto c_min = model.var_min(vars_[n_]->id());
    auto c_max = model.var_max(vars_[n_]->id());
    auto def = static_cast<Domain::value_type>(definite_count_);
    auto def_plus_poss = static_cast<Domain::value_type>(definite_count_ + possible_count_);

    // 不変条件チェック
    if (def > c_max || def_plus_poss < c_min) {
        return false;
    }

    // Bounds propagation on c
    if (def > c_min) {
        model.enqueue_set_min(vars_[n_]->id(), def);
    }
    if (def_plus_poss < c_max) {
        model.enqueue_set_max(vars_[n_]->id(), def_plus_poss);
    }

    // Forward propagation
    // c.max == definite_count_ → 残りの possible な x[i] から target を除去
    if (c_max == def) {
        for (size_t i = 0; i < n_; ++i) {
            if (is_possible_[i]) {
                model.enqueue_remove_value(vars_[i]->id(), target_);
            }
        }
    }

    // c.min == definite_count_ + possible_count_ → 残りの possible な x[i] を target に確定
    if (c_min == def_plus_poss) {
        for (size_t i = 0; i < n_; ++i) {
            if (is_possible_[i] && !vars_[i]->is_assigned()) {
                model.enqueue_instantiate(vars_[i]->id(), target_);
            }
        }
    }

    return true;
}

}  // namespace sabori_csp
