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

PresolveResult IntLinLeReifConstraint::presolve(Model& model) {
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

    // bounds から b を推論
    if (!bvar->is_assigned()) {
        if (max_sum <= bound_) {
            if (!bvar->domain().contains(1)) {
                return PresolveResult::Contradiction;
            }
            bvar->assign(1);
            changed = true;
        } else if (min_sum > bound_) {
            if (!bvar->domain().contains(0)) {
                return PresolveResult::Contradiction;
            }
            bvar->assign(0);
            changed = true;
        }
    }

    // b が確定している場合、線形変数の bounds を絞り込む
    if (bvar->is_assigned()) {
        if (bvar->assigned_value().value() == 1) {
            // sum <= bound
            if (min_sum > bound_) return PresolveResult::Contradiction;

            bool progress = true;
            while (progress) {
                progress = false;
                int64_t total_min = 0;
                for (size_t i = 0; i < n_linear; ++i) {
                    auto* v = model.variable(var_ids_[i]);
                    int64_t ci = coeffs_[i];
                    if (ci >= 0) total_min += ci * v->min();
                    else         total_min += ci * v->max();
                }
                if (total_min > bound_) return PresolveResult::Contradiction;

                for (size_t j = 0; j < n_linear; ++j) {
                    int64_t c = coeffs_[j];
                    auto* var = model.variable(var_ids_[j]);
                    if (var->is_assigned()) continue;

                    int64_t rest_min = (c >= 0)
                        ? total_min - c * var->min()
                        : total_min - c * var->max();
                    int64_t available = bound_ - rest_min;

                    if (c > 0) {
                        int64_t new_max = available / c;
                        if (new_max < var->max()) {
                            if (!var->remove_above(new_max)) return PresolveResult::Contradiction;
                            progress = true; changed = true;
                        }
                    } else {
                        int64_t abs_c = -c;
                        int64_t new_min;
                        if (available >= 0) new_min = -(available / abs_c);
                        else new_min = ((-available) + abs_c - 1) / abs_c;
                        if (new_min > var->min()) {
                            if (!var->remove_below(new_min)) return PresolveResult::Contradiction;
                            progress = true; changed = true;
                        }
                    }
                }
            }
        } else {
            // sum > bound → sum >= bound + 1
            if (max_sum <= bound_) return PresolveResult::Contradiction;

            bool progress = true;
            while (progress) {
                progress = false;
                int64_t total_max = 0;
                for (size_t i = 0; i < n_linear; ++i) {
                    auto* v = model.variable(var_ids_[i]);
                    int64_t ci = coeffs_[i];
                    if (ci >= 0) total_max += ci * v->max();
                    else         total_max += ci * v->min();
                }
                if (total_max <= bound_) return PresolveResult::Contradiction;

                int64_t target = bound_ + 1;
                for (size_t j = 0; j < n_linear; ++j) {
                    int64_t c = coeffs_[j];
                    auto* var = model.variable(var_ids_[j]);
                    if (var->is_assigned()) continue;

                    int64_t rest_max = (c >= 0)
                        ? total_max - c * var->max()
                        : total_max - c * var->min();
                    int64_t required = target - rest_max;

                    if (c > 0) {
                        int64_t new_min;
                        if (required >= 0) new_min = (required + c - 1) / c;
                        else new_min = -((-required) / c);
                        if (new_min > var->min()) {
                            if (!var->remove_below(new_min)) return PresolveResult::Contradiction;
                            progress = true; changed = true;
                        }
                    } else {
                        int64_t abs_c = -c;
                        int64_t new_max;
                        if (required >= 0) {
                            new_max = -(required / abs_c);
                            if (required % abs_c != 0) new_max -= 1;
                        } else {
                            new_max = (-required) / abs_c;
                        }
                        if (new_max < var->max()) {
                            if (!var->remove_above(new_max)) return PresolveResult::Contradiction;
                            progress = true; changed = true;
                        }
                    }
                }
            }
        }
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
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
            return propagate_bounds_le(model, SIZE_MAX);
        } else {
            // sum > bound を強制
            if (current_fixed_sum_ + max_rem_potential_ <= bound_) {
                return false;
            }
            return propagate_bounds_gt(model, SIZE_MAX);
        }
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

    // b が確定している場合の矛盾チェック + bounds propagation
    if (model.is_instantiated(b_id_)) {
        if (model.value(b_id_) == 1) {
            if (current_fixed_sum_ + min_rem_potential_ > bound_) {
                return false;
            }
            return propagate_bounds_le(model, internal_idx);
        } else {
            if (current_fixed_sum_ + max_rem_potential_ <= bound_) {
                return false;
            }
            return propagate_bounds_gt(model, internal_idx);
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
        if (model.value(b_id_) == 1) {
            if (min_sum > bound_) return false;
            // c >= 0 の min 増加 → min_rem_potential_ 増加 → le 方向がきつくなる
            if (c >= 0) return propagate_bounds_le(model, idx);
        } else {
            if (max_sum <= bound_) return false;
            // c < 0 の min 増加 → max_rem_potential_ 減少 → gt 方向がきつくなる
            if (c < 0) return propagate_bounds_gt(model, idx);
        }
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
        if (model.value(b_id_) == 1) {
            if (min_sum > bound_) return false;
            // c < 0 の max 減少 → min_rem_potential_ 増加 → le 方向がきつくなる
            if (c < 0) return propagate_bounds_le(model, idx);
        } else {
            if (max_sum <= bound_) return false;
            // c >= 0 の max 減少 → max_rem_potential_ 減少 → gt 方向がきつくなる
            if (c >= 0) return propagate_bounds_gt(model, idx);
        }
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

bool IntLinLeReifConstraint::propagate_bounds_le(Model& model, size_t skip_idx) {
    // b=1: sum <= bound → 各変数の境界を絞る（int_lin_le と同じロジック）
    int64_t slack = bound_ - current_fixed_sum_;

    for (size_t j = 0; j < coeffs_.size(); ++j) {
        if (j == skip_idx) continue;
        size_t vid = var_ids_[j];
        if (model.is_instantiated(vid)) continue;

        int64_t c = coeffs_[j];

        int64_t rest_min;
        if (c >= 0) {
            rest_min = min_rem_potential_ - c * model.var_min(vid);
        } else {
            rest_min = min_rem_potential_ - c * model.var_max(vid);
        }
        int64_t available = slack - rest_min;

        if (c > 0) {
            int64_t new_max = available / c;
            if (new_max < model.var_max(vid)) {
                model.enqueue_set_max(vid, new_max);
            }
        } else {
            int64_t abs_c = -c;
            int64_t new_min;
            if (available >= 0) {
                new_min = -(available / abs_c);
            } else {
                new_min = ((-available) + abs_c - 1) / abs_c;
            }
            if (new_min > model.var_min(vid)) {
                model.enqueue_set_min(vid, new_min);
            }
        }
    }
    return true;
}

bool IntLinLeReifConstraint::propagate_bounds_gt(Model& model, size_t skip_idx) {
    // b=0: sum > bound → sum >= bound + 1
    // 各変数について: c_j * x_j >= (bound + 1) - current_fixed_sum_ - rest_max_j
    int64_t target = bound_ + 1 - current_fixed_sum_;

    for (size_t j = 0; j < coeffs_.size(); ++j) {
        if (j == skip_idx) continue;
        size_t vid = var_ids_[j];
        if (model.is_instantiated(vid)) continue;

        int64_t c = coeffs_[j];

        // rest_max = max_rem_potential_ minus j's max contribution
        int64_t rest_max;
        if (c >= 0) {
            rest_max = max_rem_potential_ - c * model.var_max(vid);
        } else {
            rest_max = max_rem_potential_ - c * model.var_min(vid);
        }
        // c_j * x_j >= target - rest_max
        int64_t required = target - rest_max;

        if (c > 0) {
            // x_j >= ceil(required / c)
            int64_t new_min;
            if (required >= 0) {
                new_min = (required + c - 1) / c;
            } else {
                new_min = -((-required) / c);
            }
            if (new_min > model.var_min(vid)) {
                model.enqueue_set_min(vid, new_min);
            }
        } else {
            // c < 0: x_j <= floor(required / c) (不等号反転)
            int64_t abs_c = -c;
            int64_t new_max;
            if (required >= 0) {
                new_max = -(required / abs_c);
                // ceil division for negative: -(ceil(required / abs_c))
                // floor(required / c) = floor(-required / abs_c) ...
                // required / c where c<0: dividing by negative flips sign
                // c*x >= required → x <= required/c (flip because c<0)
                // required/c = -required/abs_c
                if (required % abs_c != 0) {
                    new_max = -(required / abs_c) - 1;
                }
            } else {
                new_max = (-required) / abs_c;
            }
            if (new_max < model.var_max(vid)) {
                model.enqueue_set_max(vid, new_max);
            }
        }
    }
    return true;
}

// ============================================================================

void IntLinLeReifConstraint::init_activity(const Model& model, double* activity) const {
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
