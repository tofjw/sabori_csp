#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <cmath>

namespace sabori_csp {

// ============================================================================
// IntLinEqNeReifBase implementation
//
// Shared base for int_lin_eq_reif and int_lin_ne_reif. Both maintain an
// identical incremental state (fixed sum + min/max remaining potentials +
// unfixed count + trail) and differ only in the polarity of the predicate
// P = (sum == target): eq enforces P <-> b, ne enforces (!P) <-> b.
//
// `negated_` selects the polarity. Define b_true_means_P = !negated_, so the
// constraint is P <-> (b == (b_true_means_P ? 1 : 0)).
// ============================================================================

IntLinEqNeReifBase::IntLinEqNeReifBase(std::vector<int64_t> coeffs,
                                       std::vector<VariablePtr> vars,
                                       int64_t target,
                                       VariablePtr b,
                                       bool negated)
    : LinearConstraintBase()
    , negated_(negated)
    , target_(target)
    , current_fixed_sum_(0)
    , min_rem_potential_(0)
    , max_rem_potential_(0)
    , unfixed_count_(0) {
    b_id_ = b->id();

    // 線形項を集約（同一変数の係数を合算、係数0を除外）。b は末尾に追加する。
    // 係数が全て0でも var_ids_ には b だけが残り、presolve で b を確定させる。
    auto unique_vars = aggregate_terms(coeffs, vars);
    unique_vars.push_back(b);
    var_ids_ = extract_var_ids(unique_vars);

    // 注意: 内部状態は prepare_propagation() / presolve() で初期化する。
}

PresolveResult IntLinEqNeReifBase::presolve(Model& model) {
    bool changed = false;
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
    const bool P_true = (min_sum == target_ && max_sum == target_);
    const bool P_false = (target_ < min_sum || target_ > max_sum);

    if (bvar->is_assigned()) {
        const bool want_P = (bvar->assigned_value().value() == 1) == !negated_;
        if (want_P && P_false) return PresolveResult::Contradiction;
        if (!want_P && P_true) return PresolveResult::Contradiction;
    } else {
        // bounds から b を推論
        if (P_true) {
            int target_b = negated_ ? 0 : 1;  // P 確定真 → b は P と同値
            if (!bvar->domain().contains(target_b)) return PresolveResult::Contradiction;
            bvar->assign(target_b);
            changed = true;
        } else if (P_false) {
            int target_b = negated_ ? 1 : 0;
            if (!bvar->domain().contains(target_b)) return PresolveResult::Contradiction;
            bvar->assign(target_b);
            changed = true;
        }
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool IntLinEqNeReifBase::reconcile_b(Model& model, int64_t min_sum, int64_t max_sum) {
    const bool P_true = (min_sum == target_ && max_sum == target_);
    const bool P_false = (target_ < min_sum || target_ > max_sum);

    if (model.is_instantiated(b_id_)) {
        const bool want_P = (model.value(b_id_) == 1) == !negated_;
        if (want_P && P_false) return false;
        if (!want_P && P_true) return false;
    } else {
        if (P_true) {
            model.enqueue_instantiate(b_id_, negated_ ? 0 : 1);
        } else if (P_false) {
            model.enqueue_instantiate(b_id_, negated_ ? 1 : 0);
        }
    }
    return true;
}

bool IntLinEqNeReifBase::on_instantiate(Model& model, int save_point,
                                        size_t var_idx, size_t internal_var_idx,
                                        Domain::value_type value,
                                        Domain::value_type prev_min,
                                        Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx,
                                    value, prev_min, prev_max)) {
        return false;
    }

    // b が確定した場合
    if (var_idx == b_id_) {
        int64_t min_sum = current_fixed_sum_ + min_rem_potential_;
        int64_t max_sum = current_fixed_sum_ + max_rem_potential_;

        const bool P_true = (min_sum == target_ && max_sum == target_);
        const bool P_false = (target_ < min_sum || target_ > max_sum);
        const bool want_P = (value == 1) == !negated_;
        if (want_P && P_false) return false;
        if (!want_P && P_true) return false;

        // 全線形変数が既に確定している場合は最終チェック
        if (unfixed_count_ == 0) {
            return on_final_instantiate(model);
        }
        return true;
    }

    // 線形変数が確定した場合
    save_trail_if_needed(model, save_point);

    // 差分更新
    int64_t c = coeffs_[internal_var_idx];
    current_fixed_sum_ += c * value;
    if (c >= 0) {
        min_rem_potential_ -= c * prev_min;
        max_rem_potential_ -= c * prev_max;
    } else {
        min_rem_potential_ -= c * prev_max;
        max_rem_potential_ -= c * prev_min;
    }
    --unfixed_count_;

    int64_t min_sum = current_fixed_sum_ + min_rem_potential_;
    int64_t max_sum = current_fixed_sum_ + max_rem_potential_;

    if (!reconcile_b(model, min_sum, max_sum)) return false;

    // 全線形変数が確定し、かつ b も確定している場合は最終チェック
    if (unfixed_count_ == 0 && model.is_instantiated(b_id_)) {
        return on_final_instantiate(model);
    }

    return true;
}

bool IntLinEqNeReifBase::on_final_instantiate(const Model& model) {
    int64_t sum = 0;
    size_t n_linear = coeffs_.size();
    for (size_t i = 0; i < n_linear; ++i) {
        if (!model.is_instantiated(var_ids_[i])) {
            return true;  // Not ready yet
        }
        sum += coeffs_[i] * model.value(var_ids_[i]);
    }

    if (!model.is_instantiated(b_id_)) {
        return true;  // Not ready yet
    }

    const bool P = (sum == target_);
    const bool b1 = (model.value(b_id_) == 1);
    return P == (b1 == !negated_);
}

void IntLinEqNeReifBase::rewind_to(int save_point) {
    trail_.rewind_to(save_point, [&](const TrailEntry& entry) {
        current_fixed_sum_ = entry.fixed_sum;
        min_rem_potential_ = entry.min_pot;
        max_rem_potential_ = entry.max_pot;
        unfixed_count_ = entry.unfixed_count;
    });
}

bool IntLinEqNeReifBase::prepare_propagation(Model& model) {
    // 全ての係数が0の場合の特別処理
    if (coeffs_.empty()) {
        auto* bvar = model.variable(b_id_);
        // sum=0 のとき P=(0==target)。enforced な b 値は eq=P, ne=!P。
        const bool desired_b1 = negated_ ? (target_ != 0) : (target_ == 0);
        if (bvar->is_assigned()) {
            bool b_val = (bvar->assigned_value().value() == 1);
            if (b_val != desired_b1) {
                return false;  // 矛盾
            }
        } else if (negated_) {
            // 既存仕様: ne は b を確定させ、eq は確定させない（presolve で確定済みのため
            // 実際には到達しない防御的経路。挙動を変えないため極性差を保存する）。
            bvar->assign(desired_b1 ? 1 : 0);
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

        if (model.is_instantiated(var_ids_[i])) {
            current_fixed_sum_ += c * model.value(var_ids_[i]);
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
    int64_t min_sum = current_fixed_sum_ + min_rem_potential_;
    int64_t max_sum = current_fixed_sum_ + max_rem_potential_;

    if (model.is_instantiated(b_id_)) {
        const bool P_true = (min_sum == target_ && max_sum == target_);
        const bool P_false = (target_ < min_sum || target_ > max_sum);
        const bool want_P = (model.value(b_id_) == 1) == !negated_;
        if (want_P && P_false) return false;
        if (!want_P && P_true) return false;
    }

    return true;
}

void IntLinEqNeReifBase::save_trail_if_needed(Model& model, int save_point) {
    if (trail_.save_if_needed(save_point,
            {current_fixed_sum_, min_rem_potential_, max_rem_potential_, unfixed_count_})) {
        model.mark_constraint_dirty(model_index(), save_point);
    }
}

bool IntLinEqNeReifBase::on_set_min(Model& model, int save_point,
                                    size_t var_idx, size_t internal_var_idx,
                                    Domain::value_type new_min,
                                    Domain::value_type old_min) {
    if (var_idx == b_id_) return true;  // b の変更は無視
    int64_t c = coeffs_[internal_var_idx];

    save_trail_if_needed(model, save_point);
    if (c >= 0) {
        min_rem_potential_ += c * (new_min - old_min);
    } else {
        max_rem_potential_ += c * (new_min - old_min);
    }

    int64_t min_sum = current_fixed_sum_ + min_rem_potential_;
    int64_t max_sum = current_fixed_sum_ + max_rem_potential_;
    return reconcile_b(model, min_sum, max_sum);
}

bool IntLinEqNeReifBase::on_set_max(Model& model, int save_point,
                                    size_t var_idx, size_t internal_var_idx,
                                    Domain::value_type new_max,
                                    Domain::value_type old_max) {
    if (var_idx == b_id_) return true;  // b の変更は無視
    int64_t c = coeffs_[internal_var_idx];

    save_trail_if_needed(model, save_point);
    if (c >= 0) {
        max_rem_potential_ += c * (new_max - old_max);
    } else {
        min_rem_potential_ += c * (new_max - old_max);
    }

    int64_t min_sum = current_fixed_sum_ + min_rem_potential_;
    int64_t max_sum = current_fixed_sum_ + max_rem_potential_;
    return reconcile_b(model, min_sum, max_sum);
}

bool IntLinEqNeReifBase::on_remove_value(Model& /*model*/, int /*save_point*/,
                                         size_t /*var_idx*/, size_t /*internal_var_idx*/,
                                         Domain::value_type /*removed_value*/) {
    // 境界変化は solver が on_set_min/on_set_max をディスパッチするため、
    // 内部値の除去では bounds が変わらず potentials も不変。
    return true;
}

void IntLinEqNeReifBase::init_activity(const Model& model, double* activity) const {
    int64_t max_abs = 0;
    for (auto c : coeffs_) {
        int64_t a = c < 0 ? -c : c;
        if (a > max_abs) max_abs = a;
    }
    if (max_abs <= 100) return;

    double sum_abs = 0.0;
    for (auto c : coeffs_) {
        sum_abs += std::abs(static_cast<double>(c));
    }

    for (size_t i = 0; i < coeffs_.size(); ++i) {
        size_t vid = var_ids_[i];
        if (!model.is_instantiated(vid)) {
            activity[vid] += std::abs(static_cast<double>(coeffs_[i])) / sum_abs;
        }
    }
}

}  // namespace sabori_csp
