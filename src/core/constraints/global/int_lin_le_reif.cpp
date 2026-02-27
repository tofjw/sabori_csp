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
    : Constraint()
    , bound_(bound)
    , current_fixed_sum_(0)
    , min_rem_potential_(0)
    , max_rem_potential_(0)
    , unfixed_count_(0) {
    b_id_ = b->id();

    // 同一変数の係数を集約
    std::unordered_map<Variable*, int64_t> aggregated;
    for (size_t i = 0; i < vars.size(); ++i) {
        aggregated[vars[i]] += coeffs[i];
    }

    // 一意な変数リストと係数リストを再構築（係数が0の変数は除外）
    std::vector<VariablePtr> unique_vars;
    for (const auto& [var_ptr, coeff] : aggregated) {
        if (coeff == 0) continue;  // 係数が0の変数は除外
        unique_vars.push_back(var_ptr);
        coeffs_.push_back(coeff);
    }

    // 全ての係数が0になった場合: b ↔ (0 <= bound)
    if (coeffs_.empty()) {
        // var_ids_ には b だけを含める
        unique_vars.push_back(b);
        var_ids_ = extract_var_ids(unique_vars);
        return;
    }

    // b を末尾に追加
    unique_vars.push_back(b);

    // 変数IDキャッシュを構築
    var_ids_ = extract_var_ids(unique_vars);

    // 注意: 内部状態は presolve() で初期化
}

std::string IntLinLeReifConstraint::name() const {
    return "int_lin_le_reif";
}

bool IntLinLeReifConstraint::presolve(Model& model) {
    // キャッシュ値ではなく変数ドメインから毎回計算
    // （イベント処理が組み上がる前なのでキャッシュは信頼できない）
    int64_t min_sum = 0;
    int64_t max_sum = 0;
    size_t n_linear = coeffs_.size();
    for (size_t i = 0; i < n_linear; ++i) {
        auto* var = model.variable(var_ids_[i]);
        int64_t c = coeffs_[i];
        if (var->is_assigned()) {
            int64_t v = var->assigned_value().value();
            min_sum += c * v;
            max_sum += c * v;
        } else if (c >= 0) {
            min_sum += c * var->min();
            max_sum += c * var->max();
        } else {
            min_sum += c * var->max();
            max_sum += c * var->min();
        }
    }

    auto* bvar = model.variable(b_id_);

    // b = 1 の場合、sum <= bound を強制
    if (bvar->is_assigned() && bvar->assigned_value().value() == 1) {
        if (min_sum > bound_) {
            return false;
        }
    }

    // b = 0 の場合、sum > bound を強制
    if (bvar->is_assigned() && bvar->assigned_value().value() == 0) {
        if (max_sum <= bound_) {
            return false;
        }
    }

    // bounds から b を推論
    if (!bvar->is_assigned()) {
        if (max_sum <= bound_) {
            // sum <= bound が常に真 → b = 1
            if (!bvar->domain().contains(1)) {
                return false;
            }
            bvar->assign(1);
        } else if (min_sum > bound_) {
            // sum <= bound が常に偽 → b = 0
            if (!bvar->domain().contains(0)) {
                return false;
            }
            bvar->assign(0);
        }
    }

    return true;
}

bool IntLinLeReifConstraint::on_instantiate(Model& model, int save_point,
                                              size_t var_idx, size_t internal_var_idx,
                                              Domain::value_type value,
                                              Domain::value_type prev_min,
                                              Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value, prev_min, prev_max)) {
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
    size_t internal_idx = internal_var_idx;

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
    if (model.is_instantiated(b_id_)) {
        if (model.value(b_id_) == 1) {
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
            model.enqueue_instantiate(b_id_, 1);
        } else if (current_fixed_sum_ + min_rem_potential_ > bound_) {
            // sum <= bound が常に偽 → b = 0
            model.enqueue_instantiate(b_id_, 0);
        }
    }

    return true;
}

bool IntLinLeReifConstraint::on_final_instantiate(const Model& model) {
    int64_t sum = 0;
    size_t n_linear = coeffs_.size();
    for (size_t i = 0; i < n_linear; ++i) {
        sum += coeffs_[i] * model.value(var_ids_[i]);
    }

    bool le = (sum <= bound_);
    return le == (model.value(b_id_) == 1);
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

bool IntLinLeReifConstraint::prepare_propagation(Model& model) {
    // 全ての係数が0の場合の特別処理
    if (coeffs_.empty()) {
        auto* bvar = model.variable(b_id_);
        bool trivially_true = (bound_ >= 0);
        if (bvar->is_assigned()) {
            bool b_val = (bvar->assigned_value().value() == 1);
            if (b_val != trivially_true) {
                return false;  // 矛盾
            }
        } else {
            bvar->assign(trivially_true ? 1 : 0);
        }
        return true;
    }

    // 変数の現在状態に基づいて内部状態を初期化
    current_fixed_sum_ = 0;
    min_rem_potential_ = 0;
    max_rem_potential_ = 0;
    unfixed_count_ = 0;

    size_t n_linear = coeffs_.size();
    for (size_t i = 0; i < n_linear; ++i) {
        int64_t c = coeffs_[i];
        auto* var = model.variable(var_ids_[i]);

        if (var->is_assigned()) {
            current_fixed_sum_ += c * var->assigned_value().value();
        } else {
            ++unfixed_count_;
            auto min_val = model.var_min(var_ids_[i]);
            auto max_val = model.var_max(var_ids_[i]);

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
    auto* bvar = model.variable(b_id_);
    if (bvar->is_assigned()) {
        if (bvar->assigned_value().value() == 1) {
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
                                         size_t var_idx, size_t internal_var_idx,
                                         Domain::value_type new_min,
                                         Domain::value_type old_min) {
    if (var_idx == b_id_) return true;  // b の変更は無視
    size_t idx = internal_var_idx;
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

    if (model.is_instantiated(b_id_)) {
        if (model.value(b_id_) == 1 && min_sum > bound_) return false;
        if (model.value(b_id_) == 0 && max_sum <= bound_) return false;
    } else {
        if (max_sum <= bound_) {
            model.enqueue_instantiate(b_id_, 1);
        } else if (min_sum > bound_) {
            model.enqueue_instantiate(b_id_, 0);
        }
    }
    return true;
}

bool IntLinLeReifConstraint::on_set_max(Model& model, int save_point,
                                         size_t var_idx, size_t internal_var_idx,
                                         Domain::value_type new_max,
                                         Domain::value_type old_max) {
    if (var_idx == b_id_) return true;  // b の変更は無視
    size_t idx = internal_var_idx;
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

    if (model.is_instantiated(b_id_)) {
        if (model.value(b_id_) == 1 && min_sum > bound_) return false;
        if (model.value(b_id_) == 0 && max_sum <= bound_) return false;
    } else {
        if (max_sum <= bound_) {
            model.enqueue_instantiate(b_id_, 1);
        } else if (min_sum > bound_) {
            model.enqueue_instantiate(b_id_, 0);
        }
    }
    return true;
}

bool IntLinLeReifConstraint::on_remove_value(Model& /*model*/, int /*save_point*/,
                                              size_t /*var_idx*/, size_t /*internal_var_idx*/,
                                              Domain::value_type /*removed_value*/) {
    // 境界変化は solver が on_set_min/on_set_max をディスパッチするため、
    // 内部値の除去では bounds が変わらず potentials も不変。
    return true;
}

// ============================================================================

}  // namespace sabori_csp
