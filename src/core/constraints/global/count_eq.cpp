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
    : Constraint()
    , target_(target)
    , n_(x_vars.size())
    , definite_count_(0)
    , possible_count_(0) {
    // var_ids_ = [x[0], x[1], ..., x[n-1], c]
    std::vector<VariablePtr> all_vars;
    all_vars.reserve(n_ + 1);
    for (auto& v : x_vars) {
        all_vars.push_back(std::move(v));
    }
    all_vars.push_back(std::move(count_var));

    is_possible_.resize(n_, false);

    // 変数IDキャッシュを構築
    var_ids_ = extract_var_ids(all_vars);
    c_id_ = all_vars[n_]->id();
}

std::string CountEqConstraint::name() const {
    return "count_eq";
}

bool CountEqConstraint::presolve(Model& model) {
    // 初期 definite/possible カウントを計算
    definite_count_ = 0;
    possible_count_ = 0;
    for (size_t i = 0; i < n_; ++i) {
        auto var = model.variable(var_ids_[i]);
        if (var->is_assigned()) {
            if (var->assigned_value().value() == target_) {
                definite_count_++;
            }
            is_possible_[i] = false;
        } else if (var->domain().contains(target_)) {
            is_possible_[i] = true;
            possible_count_++;
        } else {
            is_possible_[i] = false;
        }
    }

    // c の bounds を絞り込む
    auto c_var = model.variable(var_ids_[n_]);
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
                auto xi = model.variable(var_ids_[i]);
                if (!xi->remove(target_)) return false;
                is_possible_[i] = false;
                possible_count_--;
            }
        }
    }

    // c.min == definite_count_ + possible_count_ → 残りの possible な x[i] を target に確定
    if (c_min == static_cast<Domain::value_type>(definite_count_ + possible_count_)) {
        for (size_t i = 0; i < n_; ++i) {
            if (is_possible_[i]) {
                auto xi = model.variable(var_ids_[i]);
                if (xi->is_assigned()) continue;
                if (!xi->domain().contains(target_)) return false;
                if (!xi->assign(target_)) return false;
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
        if (model.is_instantiated(var_ids_[i])) {
            if (model.value(var_ids_[i]) == target_) {
                definite_count_++;
            }
            is_possible_[i] = false;
        } else if (model.contains(var_ids_[i], target_)) {
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
    auto c_min = model.var_min(c_id_);
    auto c_max = model.var_max(c_id_);
    auto def = static_cast<Domain::value_type>(definite_count_);
    auto def_plus_poss = static_cast<Domain::value_type>(definite_count_ + possible_count_);

    if (def > c_max || def_plus_poss < c_min) {
        return false;
    }

    return true;
}

bool CountEqConstraint::on_instantiate(Model& model, int save_point,
                                        size_t var_idx, size_t internal_var_idx,
                                        Domain::value_type value,
                                        Domain::value_type prev_min,
                                        Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value, prev_min, prev_max)) {
        return false;
    }

    size_t internal_idx = internal_var_idx;

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
    if (has_uninstantiated(model)) {
        size_t last_idx = find_last_uninstantiated(model);
        if (last_idx != SIZE_MAX) {
            if (!on_last_uninstantiated(model, save_point, last_idx)) {
                return false;
            }
            return true;
        }
    } else {
        return on_final_instantiate(model);
    }

    return propagate(model);
}

bool CountEqConstraint::on_final_instantiate(const Model& model) {
    int64_t count = 0;
    for (size_t i = 0; i < n_; ++i) {
        if (model.value(var_ids_[i]) == target_) {
            count++;
        }
    }
    return count == model.value(var_ids_[n_]);
}

bool CountEqConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                                 size_t last_var_internal_idx) {
    if (last_var_internal_idx == n_) {
        // 最後の未確定変数が c（count 変数）
        // 全 x[i] は確定済み
        auto def = static_cast<Domain::value_type>(definite_count_);
        if (model.is_instantiated(c_id_)) {
            return model.value(c_id_) == def;
        }
        if (model.contains(c_id_, def)) {
            model.enqueue_instantiate(c_id_, def);
        } else {
            return false;
        }
    } else {
        // 最後の未確定変数が x[j]
        size_t j = last_var_internal_idx;
        auto j_id = var_ids_[j];

        if (model.is_instantiated(j_id)) {
            // 既に確定: final check
            return on_final_instantiate(model);
        }

        // c は確定済みのはず
        if (!model.is_instantiated(c_id_)) {
            // c もまだ未確定ならロジックエラーだが、安全のため propagate に任せる
            return propagate(model);
        }

        auto cv = model.value(c_id_);
        auto remaining_needed = cv - static_cast<Domain::value_type>(definite_count_);

        if (remaining_needed == 0) {
            // x[j] は target 以外の値を取る必要がある
            if (is_possible_[j]) {
                if (model.is_instantiated(j_id) && model.value(j_id) == target_) {
                    return false;  // target しかない
                }
                model.enqueue_remove_value(j_id, target_);
            }
        } else if (remaining_needed == 1) {
            // x[j] は target でなければならない
            if (model.contains(j_id, target_)) {
                model.enqueue_instantiate(j_id, target_);
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
                                    size_t var_idx, size_t internal_var_idx,
                                    Domain::value_type new_min,
                                    Domain::value_type old_min) {
    size_t internal_idx = internal_var_idx;

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
                                    size_t var_idx, size_t internal_var_idx,
                                    Domain::value_type new_max,
                                    Domain::value_type old_max) {
    size_t internal_idx = internal_var_idx;

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
                                          size_t var_idx, size_t internal_var_idx,
                                          Domain::value_type removed_value) {
    size_t internal_idx = internal_var_idx;

    if (internal_idx < n_ && removed_value == target_ && is_possible_[internal_idx]) {
        save_trail_if_needed(model, save_point);
        trail_.back().second.is_possible_changes.push_back({internal_idx, true});
        is_possible_[internal_idx] = false;
        possible_count_--;
        return propagate(model);
    }
    return true;
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
    auto c_min = model.var_min(c_id_);
    auto c_max = model.var_max(c_id_);
    auto def = static_cast<Domain::value_type>(definite_count_);
    auto def_plus_poss = static_cast<Domain::value_type>(definite_count_ + possible_count_);

    // 不変条件チェック
    if (def > c_max || def_plus_poss < c_min) {
        return false;
    }

    // Bounds propagation on c
    if (def > c_min) {
        model.enqueue_set_min(c_id_, def);
    }
    if (def_plus_poss < c_max) {
        model.enqueue_set_max(c_id_, def_plus_poss);
    }

    // Forward propagation
    // c.max == definite_count_ → 残りの possible な x[i] から target を除去
    if (c_max == def) {
        for (size_t i = 0; i < n_; ++i) {
            if (is_possible_[i]) {
                model.enqueue_remove_value(var_ids_[i], target_);
            }
        }
    }

    // c.min == definite_count_ + possible_count_ → 残りの possible な x[i] を target に確定
    if (c_min == def_plus_poss) {
        for (size_t i = 0; i < n_; ++i) {
            if (is_possible_[i] && !model.is_instantiated(var_ids_[i])) {
                model.enqueue_instantiate(var_ids_[i], target_);
            }
        }
    }

    return true;
}

// ============================================================================
// CountEqVarTargetConstraint implementation
// ============================================================================

CountEqVarTargetConstraint::CountEqVarTargetConstraint(
    std::vector<VariablePtr> x_vars,
    VariablePtr y_var,
    VariablePtr count_var)
    : Constraint()
    , n_(x_vars.size())
    , target_known_(false)
    , target_(0)
    , definite_count_(0)
    , possible_count_(0) {
    // var_ids_ = [x[0], ..., x[n-1], y, c]
    std::vector<VariablePtr> all_vars;
    all_vars.reserve(n_ + 2);
    for (auto& v : x_vars) {
        all_vars.push_back(std::move(v));
    }
    all_vars.push_back(std::move(y_var));
    all_vars.push_back(std::move(count_var));

    is_possible_.resize(n_, false);

    var_ids_ = extract_var_ids(all_vars);
    y_id_ = all_vars[n_]->id();
    c_id_ = all_vars[n_ + 1]->id();
}

std::string CountEqVarTargetConstraint::name() const {
    return "count_eq_var";
}

void CountEqVarTargetConstraint::initialize_counts(Model& model) {
    definite_count_ = 0;
    possible_count_ = 0;
    for (size_t i = 0; i < n_; ++i) {
        if (model.is_instantiated(var_ids_[i])) {
            if (model.value(var_ids_[i]) == target_) {
                definite_count_++;
            }
            is_possible_[i] = false;
        } else if (model.contains(var_ids_[i], target_)) {
            is_possible_[i] = true;
            possible_count_++;
        } else {
            is_possible_[i] = false;
        }
    }
}

bool CountEqVarTargetConstraint::presolve(Model& model) {
    auto y_var = model.variable(var_ids_[n_]);
    if (y_var->is_assigned()) {
        // y が既に確定
        target_known_ = true;
        target_ = y_var->assigned_value().value();
        initialize_counts(model);

        // c の bounds を絞り込む
        auto c_var = model.variable(var_ids_[n_ + 1]);
        auto c_min = c_var->min();
        auto c_max = c_var->max();
        auto new_min = static_cast<Domain::value_type>(definite_count_);
        auto new_max = static_cast<Domain::value_type>(definite_count_ + possible_count_);

        if (new_min > c_max || new_max < c_min) return false;
        if (new_min > c_min) {
            if (!c_var->remove_below(new_min)) return false;
        }
        if (new_max < c_max) {
            if (!c_var->remove_above(new_max)) return false;
        }

        // Forward propagation
        c_min = c_var->min();
        c_max = c_var->max();

        if (c_max == static_cast<Domain::value_type>(definite_count_)) {
            for (size_t i = 0; i < n_; ++i) {
                if (is_possible_[i]) {
                    auto xi = model.variable(var_ids_[i]);
                    xi->remove(target_);
                    if (xi->domain().empty()) return false;
                    is_possible_[i] = false;
                    possible_count_--;
                }
            }
        }

        if (c_min == static_cast<Domain::value_type>(definite_count_ + possible_count_)) {
            for (size_t i = 0; i < n_; ++i) {
                if (is_possible_[i]) {
                    auto xi = model.variable(var_ids_[i]);
                    if (xi->is_assigned()) continue;
                    if (!xi->domain().contains(target_)) return false;
                    if (!xi->assign(target_)) return false;
                    definite_count_++;
                    is_possible_[i] = false;
                    possible_count_--;
                }
            }
        }
    } else {
        // y 未確定: 弱い bounds のみ
        auto c_var = model.variable(var_ids_[n_ + 1]);
        auto n_val = static_cast<Domain::value_type>(n_);
        if (c_var->min() > n_val) return false;
        if (0 > c_var->max()) return false;
        if (c_var->min() < 0) {
            if (!c_var->remove_below(0)) return false;
        }
        if (c_var->max() > n_val) {
            if (!c_var->remove_above(n_val)) return false;
        }
    }
    return true;
}

bool CountEqVarTargetConstraint::prepare_propagation(Model& model) {
    if (model.is_instantiated(y_id_)) {
        target_known_ = true;
        target_ = model.value(y_id_);
        initialize_counts(model);
    } else {
        target_known_ = false;
        target_ = 0;
        definite_count_ = 0;
        possible_count_ = 0;
        std::fill(is_possible_.begin(), is_possible_.end(), false);
    }

    init_watches();
    trail_.clear();

    if (target_known_) {
        auto c_min = model.var_min(c_id_);
        auto c_max = model.var_max(c_id_);
        auto def = static_cast<Domain::value_type>(definite_count_);
        auto def_plus_poss = static_cast<Domain::value_type>(definite_count_ + possible_count_);
        if (def > c_max || def_plus_poss < c_min) return false;
    }

    return true;
}

bool CountEqVarTargetConstraint::on_instantiate(Model& model, int save_point,
                                                  size_t var_idx, size_t internal_var_idx,
                                                  Domain::value_type value,
                                                  Domain::value_type prev_min,
                                                  Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value, prev_min, prev_max)) {
        return false;
    }

    save_trail_if_needed(model, save_point);

    if (internal_var_idx == n_) {
        // y が確定した
        target_known_ = true;
        target_ = value;
        // initialize_counts の代わりに、差分を trail に記録しながら再計算
        definite_count_ = 0;
        possible_count_ = 0;
        for (size_t i = 0; i < n_; ++i) {
            bool new_val;
            if (model.is_instantiated(var_ids_[i])) {
                if (model.value(var_ids_[i]) == target_) {
                    definite_count_++;
                }
                new_val = false;
            } else if (model.contains(var_ids_[i], target_)) {
                new_val = true;
                possible_count_++;
            } else {
                new_val = false;
            }
            if (is_possible_[i] != new_val) {
                trail_.back().second.is_possible_changes.push_back({i, is_possible_[i]});
                is_possible_[i] = new_val;
            }
        }
    } else if (internal_var_idx < n_ && target_known_) {
        // x[i] が確定した（target 既知の場合のみ更新）
        if (is_possible_[internal_var_idx]) {
            trail_.back().second.is_possible_changes.push_back({internal_var_idx, true});
            is_possible_[internal_var_idx] = false;
            possible_count_--;
            if (value == target_) {
                definite_count_++;
            }
        }
    }
    // c が確定 or target 未知の x[i] 確定 → propagate で処理

    // 残り変数チェック
    if (has_uninstantiated(model)) {
        size_t last_idx = find_last_uninstantiated(model);
        if (last_idx != SIZE_MAX) {
            if (!on_last_uninstantiated(model, save_point, last_idx)) {
                return false;
            }
            return true;
        }
    } else {
        return on_final_instantiate(model);
    }

    if (!target_known_) {
        // y 未確定: 弱い bounds のみ
        auto n_val = static_cast<Domain::value_type>(n_);
        auto c_min = model.var_min(c_id_);
        auto c_max = model.var_max(c_id_);
        if (c_min < 0) model.enqueue_set_min(c_id_, 0);
        if (c_max > n_val) model.enqueue_set_max(c_id_, n_val);
        return true;
    }

    return propagate(model);
}

bool CountEqVarTargetConstraint::on_final_instantiate(const Model& model) {
    auto y_val = model.value(var_ids_[n_]);
    int64_t count = 0;
    for (size_t i = 0; i < n_; ++i) {
        if (model.value(var_ids_[i]) == y_val) {
            count++;
        }
    }
    return count == model.value(var_ids_[n_ + 1]);
}

bool CountEqVarTargetConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                                          size_t last_var_internal_idx) {
    if (!target_known_) {
        // y 未確定: 弱い bounds のみ
        auto n_val = static_cast<Domain::value_type>(n_);
        auto c_min = model.var_min(c_id_);
        auto c_max = model.var_max(c_id_);
        if (c_min < 0) model.enqueue_set_min(c_id_, 0);
        if (c_max > n_val) model.enqueue_set_max(c_id_, n_val);
        return true;
    }

    if (last_var_internal_idx == n_ + 1) {
        // 最後の未確定が c
        auto def = static_cast<Domain::value_type>(definite_count_);
        if (model.is_instantiated(c_id_)) {
            return model.value(c_id_) == def;
        }
        if (model.contains(c_id_, def)) {
            model.enqueue_instantiate(c_id_, def);
        } else {
            return false;
        }
    } else if (last_var_internal_idx == n_) {
        // 最後の未確定が y → 弱い bounds
        auto n_val = static_cast<Domain::value_type>(n_);
        auto c_min = model.var_min(c_id_);
        auto c_max = model.var_max(c_id_);
        if (c_min < 0) model.enqueue_set_min(c_id_, 0);
        if (c_max > n_val) model.enqueue_set_max(c_id_, n_val);
    } else {
        // 最後の未確定が x[j]
        size_t j = last_var_internal_idx;
        auto j_id = var_ids_[j];

        if (model.is_instantiated(j_id)) {
            return on_final_instantiate(model);
        }

        if (!model.is_instantiated(c_id_)) {
            return propagate(model);
        }

        auto cv = model.value(c_id_);
        auto remaining_needed = cv - static_cast<Domain::value_type>(definite_count_);

        if (remaining_needed == 0) {
            if (is_possible_[j]) {
                if (model.is_instantiated(j_id) && model.value(j_id) == target_) {
                    return false;
                }
                model.enqueue_remove_value(j_id, target_);
            }
        } else if (remaining_needed == 1) {
            if (model.contains(j_id, target_)) {
                model.enqueue_instantiate(j_id, target_);
            } else {
                return false;
            }
        } else {
            return false;
        }
    }

    return true;
}

bool CountEqVarTargetConstraint::on_set_min(Model& model, int save_point,
                                              size_t var_idx, size_t internal_var_idx,
                                              Domain::value_type new_min,
                                              Domain::value_type old_min) {
    if (!target_known_) return true;

    if (internal_var_idx < n_) {
        if (is_possible_[internal_var_idx] && target_ < new_min) {
            save_trail_if_needed(model, save_point);
            trail_.back().second.is_possible_changes.push_back({internal_var_idx, true});
            is_possible_[internal_var_idx] = false;
            possible_count_--;
            return propagate(model);
        }
    } else if (internal_var_idx == n_ + 1) {
        // c の下限更新
        save_trail_if_needed(model, save_point);
        return propagate(model);
    }
    return true;
}

bool CountEqVarTargetConstraint::on_set_max(Model& model, int save_point,
                                              size_t var_idx, size_t internal_var_idx,
                                              Domain::value_type new_max,
                                              Domain::value_type old_max) {
    if (!target_known_) return true;

    if (internal_var_idx < n_) {
        if (is_possible_[internal_var_idx] && target_ > new_max) {
            save_trail_if_needed(model, save_point);
            trail_.back().second.is_possible_changes.push_back({internal_var_idx, true});
            is_possible_[internal_var_idx] = false;
            possible_count_--;
            return propagate(model);
        }
    } else if (internal_var_idx == n_ + 1) {
        // c の上限更新
        save_trail_if_needed(model, save_point);
        return propagate(model);
    }
    return true;
}

bool CountEqVarTargetConstraint::on_remove_value(Model& model, int save_point,
                                                    size_t var_idx, size_t internal_var_idx,
                                                    Domain::value_type removed_value) {
    if (!target_known_) return true;

    if (internal_var_idx < n_ && removed_value == target_ && is_possible_[internal_var_idx]) {
        save_trail_if_needed(model, save_point);
        trail_.back().second.is_possible_changes.push_back({internal_var_idx, true});
        is_possible_[internal_var_idx] = false;
        possible_count_--;
        return propagate(model);
    }
    return true;
}

void CountEqVarTargetConstraint::rewind_to(int save_point) {
    while (!trail_.empty() && trail_.back().first > save_point) {
        const auto& entry = trail_.back().second;
        // is_possible_ の変更を巻き戻す
        for (auto it = entry.is_possible_changes.rbegin();
             it != entry.is_possible_changes.rend(); ++it) {
            is_possible_[it->first] = it->second;
        }
        target_known_ = entry.target_known;
        target_ = entry.target;
        definite_count_ = entry.definite_count;
        possible_count_ = entry.possible_count;
        trail_.pop_back();
    }
}

void CountEqVarTargetConstraint::save_trail_if_needed(Model& model, int save_point) {
    if (trail_.empty() || trail_.back().first != save_point) {
        trail_.push_back({save_point, {target_known_, target_, definite_count_, possible_count_, {}}});
        model.mark_constraint_dirty(model_index(), save_point);
    }
}

bool CountEqVarTargetConstraint::propagate(Model& model) {
    auto c_min = model.var_min(c_id_);
    auto c_max = model.var_max(c_id_);
    auto def = static_cast<Domain::value_type>(definite_count_);
    auto def_plus_poss = static_cast<Domain::value_type>(definite_count_ + possible_count_);

    if (def > c_max || def_plus_poss < c_min) {
        return false;
    }

    // Bounds propagation on c
    if (def > c_min) {
        model.enqueue_set_min(c_id_, def);
    }
    if (def_plus_poss < c_max) {
        model.enqueue_set_max(c_id_, def_plus_poss);
    }

    // c.max == definite_count_ → 残りの possible な x[i] から target を除去
    if (c_max == def) {
        for (size_t i = 0; i < n_; ++i) {
            if (is_possible_[i]) {
                model.enqueue_remove_value(var_ids_[i], target_);
            }
        }
    }

    // c.min == definite_count_ + possible_count_ → 残りの possible な x[i] を target に確定
    if (c_min == def_plus_poss) {
        for (size_t i = 0; i < n_; ++i) {
            if (is_possible_[i] && !model.is_instantiated(var_ids_[i])) {
                model.enqueue_instantiate(var_ids_[i], target_);
            }
        }
    }

    return true;
}

}  // namespace sabori_csp
