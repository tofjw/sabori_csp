#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <set>
#include <limits>

namespace sabori_csp {

// ============================================================================
// IntLinLeReifConstraint implementation
// ============================================================================

IntLinLeReifConstraint::IntLinLeReifConstraint(std::vector<int64_t> coeffs,
                                                 std::vector<VariablePtr> vars,
                                                 int64_t bound,
                                                 VariablePtr b)
    : Constraint(std::vector<VariablePtr>())  // 後で設定
    , bound_(bound)
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

    // 全ての係数が0になった場合: b ↔ (0 <= bound)
    if (coeffs_.empty()) {
        // vars_ には b だけを含める
        vars_.push_back(b_);
        update_var_ids();
        b_id_ = b_->id();
        return;
    }

    // b を末尾に追加
    vars_.push_back(b_);

    // 変数IDキャッシュを構築
    update_var_ids();
    b_id_ = b_->id();

    // 注意: 内部状態は presolve() で初期化
}

std::string IntLinLeReifConstraint::name() const {
    return "int_lin_le_reif";
}

std::vector<VariablePtr> IntLinLeReifConstraint::variables() const {
    return vars_;
}

std::optional<bool> IntLinLeReifConstraint::is_satisfied() const {
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

    bool le = (sum <= bound_);
    return le == (b_->assigned_value().value() == 1);
}

bool IntLinLeReifConstraint::presolve(Model& model) {
    // キャッシュ値ではなく変数ドメインから毎回計算
    // （イベント処理が組み上がる前なのでキャッシュは信頼できない）
    int64_t min_sum = 0;
    int64_t max_sum = 0;
    for (size_t i = 0; i < vars_.size() - 1; ++i) {
        int64_t c = coeffs_[i];
        if (vars_[i]->is_assigned()) {
            int64_t v = vars_[i]->assigned_value().value();
            min_sum += c * v;
            max_sum += c * v;
        } else if (c >= 0) {
            min_sum += c * vars_[i]->min();
            max_sum += c * vars_[i]->max();
        } else {
            min_sum += c * vars_[i]->max();
            max_sum += c * vars_[i]->min();
        }
    }

    // b = 1 の場合、sum <= bound を強制
    if (b_->is_assigned() && b_->assigned_value().value() == 1) {
        if (min_sum > bound_) {
            return false;
        }
    }

    // b = 0 の場合、sum > bound を強制
    if (b_->is_assigned() && b_->assigned_value().value() == 0) {
        if (max_sum <= bound_) {
            return false;
        }
    }

    // bounds から b を推論
    if (!b_->is_assigned()) {
        if (max_sum <= bound_) {
            // sum <= bound が常に真 → b = 1
            if (!b_->domain().contains(1)) {
                return false;
            }
            b_->assign(1);
        } else if (min_sum > bound_) {
            // sum <= bound が常に偽 → b = 0
            if (!b_->domain().contains(0)) {
                return false;
            }
            b_->assign(0);
        }
    }

    return true;
}

bool IntLinLeReifConstraint::on_instantiate(Model& model, int save_point,
                                              size_t var_idx, Domain::value_type value,
                                              Domain::value_type prev_min,
                                              Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, value, prev_min, prev_max)) {
        return false;
    }

    // b が確定した場合
    if (var_idx == b_id_) {
        if (value == 1) {
            // sum <= bound を強制
            if (current_fixed_sum_ + min_rem_potential_ > bound_) {
                return false;
            }
        } else {
            // sum > bound を強制
            if (current_fixed_sum_ + max_rem_potential_ <= bound_) {
                return false;
            }
        }
        return true;
    }

    // 線形変数が確定した場合
    size_t internal_idx = find_internal_idx(var_idx);

    // Trail に保存
    save_trail_if_needed(model, save_point);

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

    // b が確定している場合の矛盾チェック
    if (b_->is_assigned()) {
        if (b_->assigned_value().value() == 1) {
            if (current_fixed_sum_ + min_rem_potential_ > bound_) {
                return false;
            }
        } else {
            if (current_fixed_sum_ + max_rem_potential_ <= bound_) {
                return false;
            }
        }
    } else {
        // b を推論
        if (current_fixed_sum_ + max_rem_potential_ <= bound_) {
            // sum <= bound が常に真 → b = 1
            model.enqueue_instantiate(b_->id(), 1);
        } else if (current_fixed_sum_ + min_rem_potential_ > bound_) {
            // sum <= bound が常に偽 → b = 0
            model.enqueue_instantiate(b_->id(), 0);
        }
    }

    return true;
}

bool IntLinLeReifConstraint::on_final_instantiate() {
    int64_t sum = 0;
    for (size_t i = 0; i < vars_.size() - 1; ++i) {
        sum += coeffs_[i] * vars_[i]->assigned_value().value();
    }

    bool le = (sum <= bound_);
    return le == (b_->assigned_value().value() == 1);
}

void IntLinLeReifConstraint::rewind_to(int save_point) {
    while (!trail_.empty() && trail_.back().first > save_point) {
        const auto& entry = trail_.back().second;
        current_fixed_sum_ = entry.fixed_sum;
        min_rem_potential_ = entry.min_pot;
        max_rem_potential_ = entry.max_pot;
        unfixed_count_ = entry.unfixed_count;
        trail_.pop_back();
    }
}

void IntLinLeReifConstraint::check_initial_consistency() {
    if (b_->is_assigned()) {
        if (b_->assigned_value().value() == 1) {
            // sum <= bound が必要
            if (current_fixed_sum_ + min_rem_potential_ > bound_) {
                set_initially_inconsistent(true);
            }
        } else {
            // sum > bound が必要
            if (current_fixed_sum_ + max_rem_potential_ <= bound_) {
                set_initially_inconsistent(true);
            }
        }
    }
}

bool IntLinLeReifConstraint::prepare_propagation(Model& model) {
    // 全ての係数が0の場合の特別処理
    if (coeffs_.empty()) {
        bool trivially_true = (bound_ >= 0);
        if (b_->is_assigned()) {
            bool b_val = (b_->assigned_value().value() == 1);
            if (b_val != trivially_true) {
                return false;  // 矛盾
            }
        } else {
            b_->assign(trivially_true ? 1 : 0);
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
            auto min_val = model.var_min(vars_[i]->id());
            auto max_val = model.var_max(vars_[i]->id());

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
    if (b_->is_assigned()) {
        if (b_->assigned_value().value() == 1) {
            // sum <= bound が必要
            if (current_fixed_sum_ + min_rem_potential_ > bound_) {
                return false;  // 矛盾
            }
        } else {
            // sum > bound が必要
            if (current_fixed_sum_ + max_rem_potential_ <= bound_) {
                return false;  // 矛盾
            }
        }
    }

    return true;
}

void IntLinLeReifConstraint::save_trail_if_needed(Model& model, int save_point) {
    if (trail_.empty() || trail_.back().first != save_point) {
        trail_.push_back({save_point, {current_fixed_sum_, min_rem_potential_, max_rem_potential_, unfixed_count_}});
        model.mark_constraint_dirty(model_index(), save_point);
    }
}

bool IntLinLeReifConstraint::on_set_min(Model& model, int save_point,
                                         size_t var_idx, Domain::value_type new_min,
                                         Domain::value_type old_min) {
    if (var_idx == b_id_) return true;  // b_ の変更は無視
    size_t idx = find_internal_idx(var_idx);
    int64_t c = coeffs_[idx];

    if (c >= 0) {
        save_trail_if_needed(model, save_point);
        min_rem_potential_ += c * (new_min - old_min);
    } else {
        save_trail_if_needed(model, save_point);
        max_rem_potential_ += c * (new_min - old_min);
    }

    int64_t min_sum = current_fixed_sum_ + min_rem_potential_;
    int64_t max_sum = current_fixed_sum_ + max_rem_potential_;

    if (b_->is_assigned()) {
        if (b_->assigned_value().value() == 1 && min_sum > bound_) return false;
        if (b_->assigned_value().value() == 0 && max_sum <= bound_) return false;
    } else {
        if (max_sum <= bound_) {
            model.enqueue_instantiate(b_->id(), 1);
        } else if (min_sum > bound_) {
            model.enqueue_instantiate(b_->id(), 0);
        }
    }
    return true;
}

bool IntLinLeReifConstraint::on_set_max(Model& model, int save_point,
                                         size_t var_idx, Domain::value_type new_max,
                                         Domain::value_type old_max) {
    if (var_idx == b_id_) return true;  // b_ の変更は無視
    size_t idx = find_internal_idx(var_idx);
    int64_t c = coeffs_[idx];

    if (c >= 0) {
        save_trail_if_needed(model, save_point);
        max_rem_potential_ += c * (new_max - old_max);
    } else {
        save_trail_if_needed(model, save_point);
        min_rem_potential_ += c * (new_max - old_max);
    }

    int64_t min_sum = current_fixed_sum_ + min_rem_potential_;
    int64_t max_sum = current_fixed_sum_ + max_rem_potential_;

    if (b_->is_assigned()) {
        if (b_->assigned_value().value() == 1 && min_sum > bound_) return false;
        if (b_->assigned_value().value() == 0 && max_sum <= bound_) return false;
    } else {
        if (max_sum <= bound_) {
            model.enqueue_instantiate(b_->id(), 1);
        } else if (min_sum > bound_) {
            model.enqueue_instantiate(b_->id(), 0);
        }
    }
    return true;
}

bool IntLinLeReifConstraint::on_remove_value(Model& model, int save_point,
                                              size_t var_idx, Domain::value_type removed_value) {
    if (var_idx == b_id_) return true;  // b_ の変更は無視
    size_t idx = find_internal_idx(var_idx);
    int64_t c = coeffs_[idx];

    if (c >= 0) {
        auto current_min = model.var_min(var_idx);
        if (current_min > removed_value) {
            save_trail_if_needed(model, save_point);
            min_rem_potential_ += c * (current_min - removed_value);
        }
        auto current_max = model.var_max(var_idx);
        if (current_max < removed_value) {
            save_trail_if_needed(model, save_point);
            max_rem_potential_ += c * (current_max - removed_value);
        }
    } else {
        auto current_min = model.var_min(var_idx);
        if (current_min > removed_value) {
            save_trail_if_needed(model, save_point);
            max_rem_potential_ += c * (current_min - removed_value);
        }
        auto current_max = model.var_max(var_idx);
        if (current_max < removed_value) {
            save_trail_if_needed(model, save_point);
            min_rem_potential_ += c * (current_max - removed_value);
        }
    }

    int64_t min_sum = current_fixed_sum_ + min_rem_potential_;
    int64_t max_sum = current_fixed_sum_ + max_rem_potential_;

    if (b_->is_assigned()) {
        if (b_->assigned_value().value() == 1 && min_sum > bound_) return false;
        if (b_->assigned_value().value() == 0 && max_sum <= bound_) return false;
    } else {
        if (max_sum <= bound_) {
            model.enqueue_instantiate(b_->id(), 1);
        } else if (min_sum > bound_) {
            model.enqueue_instantiate(b_->id(), 0);
        }
    }
    return true;
}

// ============================================================================

}  // namespace sabori_csp

