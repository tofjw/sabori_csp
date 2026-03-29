#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>

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
    if (n_ == 0 || Q_ <= 0 || S_ <= 0) {
        set_initially_inconsistent(true);
        return;
    }

    if (q0_ < 1 || q0_ > Q_) {
        set_initially_inconsistent(true);
        return;
    }

    if (transition_flat.size() != static_cast<size_t>(Q_) * S_) {
        set_initially_inconsistent(true);
        return;
    }

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
        set_initially_inconsistent(true);
        return;
    }

    // Initialize reachable arrays
    reachable_from_.assign(n_ + 1, std::vector<bool>(Q_ + 1, false));
    reachable_to_.assign(n_ + 1, std::vector<bool>(Q_ + 1, false));
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

    // Forward pass
    for (auto& v : reachable_from_) std::fill(v.begin(), v.end(), false);
    reachable_from_[0][q0_] = true;

    for (size_t i = 0; i < n_; ++i) {
        auto* var = model.variable(var_ids_[i]);
        for (auto s = var->min(); s <= var->max(); ++s) {
            if (!var->domain().contains(s)) continue;
            if (s < 1 || s > S_) continue;
            for (int q = 1; q <= Q_; ++q) {
                if (!reachable_from_[i][q]) continue;
                int next = transition(q, (int)s);
                if (next != 0) {
                    reachable_from_[i + 1][next] = true;
                }
            }
        }
        // Check if any state reachable at i+1
        bool any = false;
        for (int q = 1; q <= Q_; ++q) {
            if (reachable_from_[i + 1][q]) { any = true; break; }
        }
        if (!any) return PresolveResult::Contradiction;
    }

    // Backward pass
    for (auto& v : reachable_to_) std::fill(v.begin(), v.end(), false);
    for (int q = 1; q <= Q_; ++q) {
        if (accepting_[q] && reachable_from_[n_][q]) {
            reachable_to_[n_][q] = true;
        }
    }

    // Check if any accepting state is reachable
    {
        bool any = false;
        for (int q = 1; q <= Q_; ++q) {
            if (reachable_to_[n_][q]) { any = true; break; }
        }
        if (!any) return PresolveResult::Contradiction;
    }

    for (size_t i = n_; i > 0; --i) {
        auto* var = model.variable(var_ids_[i - 1]);
        for (int q = 1; q <= Q_; ++q) {
            if (!reachable_from_[i - 1][q]) continue;
            for (auto s = var->min(); s <= var->max(); ++s) {
                if (!var->domain().contains(s)) continue;
                if (s < 1 || s > S_) continue;
                int next = transition(q, (int)s);
                if (next != 0 && reachable_to_[i][next]) {
                    reachable_to_[i - 1][q] = true;
                }
            }
        }
    }

    // Filter domains
    for (size_t i = 0; i < n_; ++i) {
        auto* var = model.variable(var_ids_[i]);
        std::vector<Domain::value_type> to_remove;
        for (auto s = var->min(); s <= var->max(); ++s) {
            if (!var->domain().contains(s)) continue;
            bool valid = false;
            if (s >= 1 && s <= S_) {
                for (int q = 1; q <= Q_; ++q) {
                    if (!reachable_from_[i][q]) continue;
                    int next = transition(q, (int)s);
                    if (next != 0 && reachable_to_[i + 1][next]) {
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
                                        size_t var_idx, size_t internal_var_idx,
                                        Domain::value_type value,
                                        Domain::value_type prev_min,
                                        Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value, prev_min, prev_max)) {
        return false;
    }

    // Mark dirty for backtracking (reachable sets recomputed from model state)
    if (trail_save_points_.empty() || trail_save_points_.back() != save_point) {
        trail_save_points_.push_back(save_point);
        model.mark_constraint_dirty(model_index(), save_point);
    }

    return compute_and_filter(model);
}

bool RegularConstraint::on_remove_value(Model& model, int save_point,
                                         size_t /*var_idx*/, size_t /*internal_var_idx*/,
                                         Domain::value_type /*removed_value*/) {
    if (trail_save_points_.empty() || trail_save_points_.back() != save_point) {
        trail_save_points_.push_back(save_point);
        model.mark_constraint_dirty(model_index(), save_point);
    }
    return compute_and_filter(model);
}

bool RegularConstraint::on_set_min(Model& model, int save_point,
                                    size_t /*var_idx*/, size_t /*internal_var_idx*/,
                                    Domain::value_type /*new_min*/, Domain::value_type /*old_min*/) {
    if (trail_save_points_.empty() || trail_save_points_.back() != save_point) {
        trail_save_points_.push_back(save_point);
        model.mark_constraint_dirty(model_index(), save_point);
    }
    return compute_and_filter(model);
}

bool RegularConstraint::on_set_max(Model& model, int save_point,
                                    size_t /*var_idx*/, size_t /*internal_var_idx*/,
                                    Domain::value_type /*new_max*/, Domain::value_type /*old_max*/) {
    if (trail_save_points_.empty() || trail_save_points_.back() != save_point) {
        trail_save_points_.push_back(save_point);
        model.mark_constraint_dirty(model_index(), save_point);
    }
    return compute_and_filter(model);
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

    // 前後はフル bump、それ以外は半分
    size_t n = n_ > 0 ? n_ : 1;
    double inc_full = activity_inc / n;
    double inc_half = inc_full * 0.5;
    for (size_t i = 0; i < n_; ++i) {
        if (!model.is_instantiated(var_ids_[i])) continue;
        bool neighbor = (i + 1 == pos || i == pos + 1);
        bump_variable_activity(activity, var_ids_[i], neighbor ? inc_full : inc_half, need_rescale, rng);
    }
}

bool RegularConstraint::compute_and_filter(Model& model) {
    const int S = S_;
    const int Q = Q_;

    // Forward pass
    for (auto& v : reachable_from_) std::fill(v.begin(), v.end(), false);
    reachable_from_[0][q0_] = true;

    for (size_t i = 0; i < n_; ++i) {
        auto v_id = var_ids_[i];
        if (model.is_instantiated(v_id)) {
            auto s = model.value(v_id);
            if (s >= 1 && s <= S) {
                for (int q = 1; q <= Q; ++q) {
                    if (!reachable_from_[i][q]) continue;
                    int next = transition(q, (int)s);
                    if (next != 0) reachable_from_[i + 1][next] = true;
                }
            }
        } else {
            auto& dom = model.variable(v_id)->domain();
            if (dom.is_bounds_only()) {
                auto lo = std::max(model.var_min(v_id), (Domain::value_type)1);
                auto hi = std::min(model.var_max(v_id), (Domain::value_type)S);
                for (auto s = lo; s <= hi; ++s) {
                    if (!dom.contains(s)) continue;
                    for (int q = 1; q <= Q; ++q) {
                        if (!reachable_from_[i][q]) continue;
                        int next = transition(q, (int)s);
                        if (next != 0) reachable_from_[i + 1][next] = true;
                    }
                }
            } else {
                const auto& vals = dom.values_ref();
                size_t dom_n = dom.size();
                for (size_t vi = 0; vi < dom_n; ++vi) {
                    auto s = vals[vi];
                    if (s < 1 || s > S) continue;
                    for (int q = 1; q <= Q; ++q) {
                        if (!reachable_from_[i][q]) continue;
                        int next = transition(q, (int)s);
                        if (next != 0) reachable_from_[i + 1][next] = true;
                    }
                }
            }
        }
        bool any = false;
        for (int q = 1; q <= Q; ++q) {
            if (reachable_from_[i + 1][q]) { any = true; break; }
        }
        if (!any) return false;
    }

    // Backward pass
    for (auto& v : reachable_to_) std::fill(v.begin(), v.end(), false);
    for (int q = 1; q <= Q; ++q) {
        if (accepting_[q] && reachable_from_[n_][q]) {
            reachable_to_[n_][q] = true;
        }
    }
    {
        bool any = false;
        for (int q = 1; q <= Q; ++q) {
            if (reachable_to_[n_][q]) { any = true; break; }
        }
        if (!any) return false;
    }

    for (size_t i = n_; i > 0; --i) {
        auto v_id = var_ids_[i - 1];
        if (model.is_instantiated(v_id)) {
            auto s = model.value(v_id);
            if (s >= 1 && s <= S) {
                for (int q = 1; q <= Q; ++q) {
                    if (!reachable_from_[i - 1][q]) continue;
                    int next = transition(q, (int)s);
                    if (next != 0 && reachable_to_[i][next]) {
                        reachable_to_[i - 1][q] = true;
                    }
                }
            }
        } else {
            auto& dom = model.variable(v_id)->domain();
            if (dom.is_bounds_only()) {
                auto lo = std::max(model.var_min(v_id), (Domain::value_type)1);
                auto hi = std::min(model.var_max(v_id), (Domain::value_type)S);
                for (auto s = lo; s <= hi; ++s) {
                    if (!dom.contains(s)) continue;
                    for (int q = 1; q <= Q; ++q) {
                        if (!reachable_from_[i - 1][q]) continue;
                        int next = transition(q, (int)s);
                        if (next != 0 && reachable_to_[i][next]) {
                            reachable_to_[i - 1][q] = true;
                        }
                    }
                }
            } else {
                const auto& vals = dom.values_ref();
                size_t dom_n = dom.size();
                for (size_t vi = 0; vi < dom_n; ++vi) {
                    auto s = vals[vi];
                    if (s < 1 || s > S) continue;
                    for (int q = 1; q <= Q; ++q) {
                        if (!reachable_from_[i - 1][q]) continue;
                        int next = transition(q, (int)s);
                        if (next != 0 && reachable_to_[i][next]) {
                            reachable_to_[i - 1][q] = true;
                        }
                    }
                }
            }
        }
    }

    // Filter domains
    for (size_t i = 0; i < n_; ++i) {
        auto v_id = var_ids_[i];
        if (model.is_instantiated(v_id)) continue;

        auto& dom = model.variable(v_id)->domain();
        if (dom.is_bounds_only()) {
            auto lo = std::max(model.var_min(v_id), (Domain::value_type)1);
            auto hi = std::min(model.var_max(v_id), (Domain::value_type)S);
            for (auto s = lo; s <= hi; ++s) {
                if (!dom.contains(s)) continue;
                bool valid = false;
                for (int q = 1; q <= Q; ++q) {
                    if (!reachable_from_[i][q]) continue;
                    int next = transition(q, (int)s);
                    if (next != 0 && reachable_to_[i + 1][next]) {
                        valid = true;
                        break;
                    }
                }
                if (!valid) model.enqueue_remove_value(v_id, s);
            }
        } else {
            const auto& vals = dom.values_ref();
            size_t dom_n = dom.size();
            for (size_t vi = 0; vi < dom_n; ++vi) {
                auto s = vals[vi];
                if (s < 1 || s > S) continue;
                bool valid = false;
                for (int q = 1; q <= Q; ++q) {
                    if (!reachable_from_[i][q]) continue;
                    int next = transition(q, (int)s);
                    if (next != 0 && reachable_to_[i + 1][next]) {
                        valid = true;
                        break;
                    }
                }
                if (!valid) model.enqueue_remove_value(v_id, s);
            }
        }
    }

    return true;
}

}  // namespace sabori_csp
