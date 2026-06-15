#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <cstring>

namespace sabori_csp {

// ============================================================================
// RegularConstraint implementation (DFA-based GAC propagation)
// ============================================================================

RegularConstraint::RegularConstraint(std::vector<VariablePtr> vars,
                                     int num_states, int num_symbols,
                                     std::vector<int> transition_flat,
                                     int initial_state,
                                     std::vector<int> accepting_states)
    : Constraint(extract_var_ids(vars))
    , Q_(num_states)
    , S_(num_symbols)
    , q0_(initial_state)
    , n_(var_ids_.size())
{
    // 入力 sanity チェック (空配列 / 不正な状態数 / 初期状態 / 遷移表)
    // — 矛盾自体は presolve()/prepare_propagation() で検出される
    if (n_ == 0 || Q_ <= 0 || S_ <= 0) return;
    if (q0_ < 1 || q0_ > Q_) return;
    if (transition_flat.size() != static_cast<size_t>(Q_) * S_) return;

    // Build (Q+1) x (S+1) transition table with row 0 and col 0 = 0
    transition_.assign(static_cast<size_t>(Q_ + 1) * (S_ + 1), 0);
    for (int q = 1; q <= Q_; ++q) {
        for (int s = 1; s <= S_; ++s) {
            int flat_idx = (q - 1) * S_ + (s - 1);
            int next = transition_flat[flat_idx];
            if (next < 0 || next > Q_) next = 0;
            transition_[(size_t)q * (S_ + 1) + s] = next;
        }
    }

    // Build accepting states bitvector
    accepting_.assign(Q_ + 1, false);
    bool has_accepting = false;
    for (int a : accepting_states) {
        if (a >= 1 && a <= Q_) {
            accepting_[a] = true;
            has_accepting = true;
        }
    }
    if (!has_accepting) {
        // 受理状態なし → 必ず UNSAT。presolve()/prepare_propagation() で検出される
        return;
    }

    // Initialize reachable arrays (flat layout: i * stride + q)
    reach_stride_ = static_cast<size_t>(Q_ + 1);
    reachable_from_.assign((n_ + 1) * reach_stride_, 0);
    reachable_to_.assign((n_ + 1) * reach_stride_, 0);

    // Pre-size dom value buffer offset table (vals_buf_ grows on demand)
    vals_offset_.assign(n_ + 1, 0);

    // Pre-size no-op cache
    prev_min_.assign(n_, 0);
    prev_max_.assign(n_, 0);
    prev_size_.assign(n_, 0);
}

std::string RegularConstraint::name() const {
    return "regular";
}

PresolveResult RegularConstraint::presolve(Model& model) {
    bool changed = false;

    // First restrict all variables to [1, S]
    for (size_t i = 0; i < n_; ++i) {
        auto* var = model.variable(var_ids_[i]);
        if (var->min() < 1) {
            var->remove_below(1);
            changed = true;
        }
        if (var->max() > S_) {
            var->remove_above(S_);
            changed = true;
        }
        if (var->domain().empty()) return PresolveResult::Contradiction;
    }

    const size_t stride = reach_stride_;

    // Forward pass (フラット配列、std::memset でクリア)
    std::memset(reachable_from_.data(), 0, reachable_from_.size());
    reachable_from_[0 * stride + q0_] = 1;

    for (size_t i = 0; i < n_; ++i) {
        auto* var = model.variable(var_ids_[i]);
        const uint8_t* rf_in  = &reachable_from_[i * stride];
        uint8_t*       rf_out = &reachable_from_[(i + 1) * stride];
        for (auto s = var->min(); s <= var->max(); ++s) {
            if (!var->domain().contains(s)) continue;
            if (s < 1 || s > S_) continue;
            for (int q = 1; q <= Q_; ++q) {
                if (!rf_in[q]) continue;
                int next = transition(q, (int)s);
                if (next != 0) rf_out[next] = 1;
            }
        }
        bool any = false;
        for (int q = 1; q <= Q_; ++q) {
            if (rf_out[q]) { any = true; break; }
        }
        if (!any) return PresolveResult::Contradiction;
    }

    // Backward pass
    std::memset(reachable_to_.data(), 0, reachable_to_.size());
    {
        uint8_t* rt_n = &reachable_to_[n_ * stride];
        const uint8_t* rf_n = &reachable_from_[n_ * stride];
        bool any = false;
        for (int q = 1; q <= Q_; ++q) {
            if (accepting_[q] && rf_n[q]) {
                rt_n[q] = 1;
                any = true;
            }
        }
        if (!any) return PresolveResult::Contradiction;
    }

    for (size_t i = n_; i > 0; --i) {
        auto* var = model.variable(var_ids_[i - 1]);
        const uint8_t* rf_prev = &reachable_from_[(i - 1) * stride];
        const uint8_t* rt_in   = &reachable_to_[i * stride];
        uint8_t*       rt_out  = &reachable_to_[(i - 1) * stride];
        for (int q = 1; q <= Q_; ++q) {
            if (!rf_prev[q]) continue;
            for (auto s = var->min(); s <= var->max(); ++s) {
                if (!var->domain().contains(s)) continue;
                if (s < 1 || s > S_) continue;
                int next = transition(q, (int)s);
                if (next != 0 && rt_in[next]) {
                    rt_out[q] = 1;
                }
            }
        }
    }

    // Filter domains
    for (size_t i = 0; i < n_; ++i) {
        auto* var = model.variable(var_ids_[i]);
        const uint8_t* rf_i  = &reachable_from_[i * stride];
        const uint8_t* rt_ip = &reachable_to_[(i + 1) * stride];
        std::vector<Domain::value_type> to_remove;
        for (auto s = var->min(); s <= var->max(); ++s) {
            if (!var->domain().contains(s)) continue;
            bool valid = false;
            if (s >= 1 && s <= S_) {
                for (int q = 1; q <= Q_; ++q) {
                    if (!rf_i[q]) continue;
                    int next = transition(q, (int)s);
                    if (next != 0 && rt_ip[next]) {
                        valid = true;
                        break;
                    }
                }
            }
            if (!valid) {
                to_remove.push_back(s);
            }
        }
        for (auto v : to_remove) {
            var->remove(v);
            changed = true;
        }
        if (var->domain().empty()) return PresolveResult::Contradiction;
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool RegularConstraint::prepare_propagation(Model& model) {
    trail_save_points_.clear();
    return compute_and_filter(model);
}

bool RegularConstraint::on_instantiate(Model& model, int save_point,
                                        size_t internal_var_idx,
                                        Domain::value_type value,
                                        Domain::value_type prev_min,
                                        Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, internal_var_idx, value, prev_min, prev_max)) {
        return false;
    }

    model.schedule_constraint_batch(model_index());
    return true;
}

bool RegularConstraint::propagate_batch(Model& model, int save_point) {
    // DFA フルパス（forward/backward reachability + filter）をイベント毎ではなく
    // イベントバッチ毎の1回に集約する
    if (trail_save_points_.empty() || trail_save_points_.back() != save_point) {
        trail_save_points_.push_back(save_point);
        model.mark_constraint_dirty(model_index(), save_point);
    }
    return compute_and_filter(model);
}

bool RegularConstraint::on_remove_value(Model& model, int save_point,
                                         size_t /*internal_var_idx*/,
                                         Domain::value_type /*removed_value*/) {
    model.schedule_constraint_batch(model_index());
    return true;
}

bool RegularConstraint::on_set_min(Model& model, int save_point,
                                    size_t /*internal_var_idx*/,
                                    Domain::value_type /*new_min*/, Domain::value_type /*old_min*/) {
    model.schedule_constraint_batch(model_index());
    return true;
}

bool RegularConstraint::on_set_max(Model& model, int save_point,
                                    size_t /*internal_var_idx*/,
                                    Domain::value_type /*new_max*/, Domain::value_type /*old_max*/) {
    model.schedule_constraint_batch(model_index());
    return true;
}

bool RegularConstraint::on_final_instantiate(const Model& model) {
    // Verify: simulate DFA on the assigned values
    int state = q0_;
    for (size_t i = 0; i < n_; ++i) {
        auto s = model.value(var_ids_[i]);
        if (s < 1 || s > S_) return false;
        state = transition(state, (int)s);
        if (state == 0) return false;
    }
    return accepting_[state];
}

void RegularConstraint::rewind_to(int save_point) {
    while (!trail_save_points_.empty() && trail_save_points_.back() > save_point) {
        trail_save_points_.pop_back();
    }
    // reachable sets will be recomputed on next compute_and_filter call
    // バックトラックで変数状態が巻き戻るため、no-op キャッシュも無効化
    prev_valid_ = false;
}

void RegularConstraint::bump_activity(const Model& model, size_t trigger_var_idx,
                                       double* activity, double activity_inc,
                                       bool& need_rescale, std::mt19937& rng) const {
    // trigger 変数のシーケンス内位置を特定
    size_t pos = SIZE_MAX;
    for (size_t i = 0; i < n_; ++i) {
        if (var_ids_[i] == trigger_var_idx) {
            pos = i;
            break;
        }
    }

    if (pos == SIZE_MAX) {
        Constraint::bump_activity(model, trigger_var_idx, activity, activity_inc, need_rescale, rng);
        return;
    }

    size_t n = n_ > 0 ? n_ : 1;
    double inc_full = activity_inc / n;
    for (size_t i = 0; i < n_; ++i) {
        double a = inc_full;
        if (pos != i)
            a /= model.var_size(var_ids_[i]);
        bump_variable_activity(activity, var_ids_[i], a, need_rescale, rng);
    }
}

bool RegularConstraint::compute_and_filter(Model& model) {
    const int S = S_;
    const int Q = Q_;
    const size_t stride = reach_stride_;

    // ----- B) no-op early-out: 前回完了時の (min, max, size) と一致なら結果も同じ
    if (prev_valid_) {
        bool same = true;
        for (size_t i = 0; i < n_; ++i) {
            auto v_id = var_ids_[i];
            if (model.var_min(v_id) != prev_min_[i] ||
                model.var_max(v_id) != prev_max_[i] ||
                model.var_size(v_id) != prev_size_[i]) {
                same = false;
                break;
            }
        }
        if (same) return true;
    }

    // ----- D) 各位置の有効シンボルを 1 度だけ展開（3 パスで再利用）
    vals_buf_.clear();
    vals_offset_[0] = 0;
    for (size_t i = 0; i < n_; ++i) {
        auto v_id = var_ids_[i];
        if (model.is_instantiated(v_id)) {
            auto s = model.value(v_id);
            if (s >= 1 && s <= S) vals_buf_.push_back(static_cast<int>(s));
        } else {
            auto& dom = model.variable(v_id)->domain();
            if (dom.is_bounds_only()) {
                auto lo = std::max(model.var_min(v_id), (Domain::value_type)1);
                auto hi = std::min(model.var_max(v_id), (Domain::value_type)S);
                for (auto s = lo; s <= hi; ++s) {
                    if (dom.contains(s)) vals_buf_.push_back(static_cast<int>(s));
                }
            } else {
                const auto& vals = dom.values_ref();
                size_t dom_n = dom.size();
                for (size_t vi = 0; vi < dom_n; ++vi) {
                    auto s = vals[vi];
                    if (s >= 1 && s <= S) vals_buf_.push_back(static_cast<int>(s));
                }
            }
        }
        vals_offset_[i + 1] = vals_buf_.size();
    }

    // ----- C) Forward pass (フラット配列、std::memset で一括クリア)
    std::memset(reachable_from_.data(), 0, reachable_from_.size());
    reachable_from_[0 * stride + q0_] = 1;

    for (size_t i = 0; i < n_; ++i) {
        const uint8_t* rf_in  = &reachable_from_[i * stride];
        uint8_t*       rf_out = &reachable_from_[(i + 1) * stride];
        size_t vbeg = vals_offset_[i];
        size_t vend = vals_offset_[i + 1];
        for (size_t vi = vbeg; vi < vend; ++vi) {
            int s = vals_buf_[vi];
            const int* trans_row_base = transition_.data();
            for (int q = 1; q <= Q; ++q) {
                if (!rf_in[q]) continue;
                int next = trans_row_base[q * (S + 1) + s];
                if (next != 0) rf_out[next] = 1;
            }
        }
        bool any = false;
        for (int q = 1; q <= Q; ++q) {
            if (rf_out[q]) { any = true; break; }
        }
        if (!any) {
            prev_valid_ = false;
            return false;
        }
    }

    // ----- C) Backward pass
    std::memset(reachable_to_.data(), 0, reachable_to_.size());
    {
        uint8_t* rt_n = &reachable_to_[n_ * stride];
        const uint8_t* rf_n = &reachable_from_[n_ * stride];
        bool any = false;
        for (int q = 1; q <= Q; ++q) {
            if (accepting_[q] && rf_n[q]) {
                rt_n[q] = 1;
                any = true;
            }
        }
        if (!any) {
            prev_valid_ = false;
            return false;
        }
    }

    for (size_t i = n_; i > 0; --i) {
        const uint8_t* rf_prev = &reachable_from_[(i - 1) * stride];
        const uint8_t* rt_in   = &reachable_to_[i * stride];
        uint8_t*       rt_out  = &reachable_to_[(i - 1) * stride];
        size_t vbeg = vals_offset_[i - 1];
        size_t vend = vals_offset_[i];
        const int* trans_row_base = transition_.data();
        for (size_t vi = vbeg; vi < vend; ++vi) {
            int s = vals_buf_[vi];
            for (int q = 1; q <= Q; ++q) {
                if (!rf_prev[q]) continue;
                int next = trans_row_base[q * (S + 1) + s];
                if (next != 0 && rt_in[next]) {
                    rt_out[q] = 1;
                }
            }
        }
    }

    // ----- C) Filter pass
    for (size_t i = 0; i < n_; ++i) {
        auto v_id = var_ids_[i];
        if (model.is_instantiated(v_id)) continue;

        const uint8_t* rf_i  = &reachable_from_[i * stride];
        const uint8_t* rt_ip = &reachable_to_[(i + 1) * stride];
        size_t vbeg = vals_offset_[i];
        size_t vend = vals_offset_[i + 1];
        const int* trans_row_base = transition_.data();
        for (size_t vi = vbeg; vi < vend; ++vi) {
            int s = vals_buf_[vi];
            bool valid = false;
            for (int q = 1; q <= Q; ++q) {
                if (!rf_i[q]) continue;
                int next = trans_row_base[q * (S + 1) + s];
                if (next != 0 && rt_ip[next]) { valid = true; break; }
            }
            if (!valid) model.enqueue_remove_value(v_id, s);
        }
    }

    // ----- B) キャッシュ更新（成功完了時のみ）
    for (size_t i = 0; i < n_; ++i) {
        auto v_id = var_ids_[i];
        prev_min_[i]  = model.var_min(v_id);
        prev_max_[i]  = model.var_max(v_id);
        prev_size_[i] = model.var_size(v_id);
    }
    prev_valid_ = true;

    return true;
}

}  // namespace sabori_csp
