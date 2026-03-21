#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>

namespace sabori_csp {

// ============================================================================
// NValueConstraint implementation
// ============================================================================

NValueConstraint::NValueConstraint(VariablePtr n_var, std::vector<VariablePtr> x_vars)
    : Constraint()
    , num_x_(x_vars.size())
    , definite_count_(0)
    , possible_count_(0) {
    // var_ids_ = [x[0], x[1], ..., x[num_x_-1], n]
    std::vector<VariablePtr> all_vars;
    all_vars.reserve(num_x_ + 1);

    // Collect all domain values (union of all x domains)
    for (auto& v : x_vars) {
        std::vector<Domain::value_type> vals;
        v->domain().copy_values_to(vals);
        sorted_values_.insert(sorted_values_.end(), vals.begin(), vals.end());
        all_vars.push_back(std::move(v));
    }
    all_vars.push_back(std::move(n_var));

    // Deduplicate and sort
    std::sort(sorted_values_.begin(), sorted_values_.end());
    sorted_values_.erase(std::unique(sorted_values_.begin(), sorted_values_.end()),
                         sorted_values_.end());
    num_values_ = sorted_values_.size();
    value_index_.reserve(num_values_);
    for (size_t i = 0; i < num_values_; ++i) {
        value_index_[sorted_values_[i]] = i;
    }

    // Initialize data structures
    support_count_.resize(num_values_, 0);
    definite_.resize(num_values_, false);
    var_support_bitmap_.resize(num_x_ * num_values_, false);

    // Build initial support counts and bitmap
    for (size_t i = 0; i < num_x_; ++i) {
        std::vector<Domain::value_type> vals;
        all_vars[i]->domain().copy_values_to(vals);
        for (auto v : vals) {
            auto it = value_index_.find(v);
            if (it != value_index_.end()) {
                size_t vi = it->second;
                var_support_bitmap_[i * num_values_ + vi] = true;
                support_count_[vi]++;
            }
        }
    }

    // Count possible (values with support > 0) and definite
    possible_count_ = 0;
    for (size_t vi = 0; vi < num_values_; ++vi) {
        if (support_count_[vi] > 0) {
            possible_count_++;
        }
    }

    var_ids_ = extract_var_ids(all_vars);
    n_id_ = all_vars[num_x_]->id();
}

std::string NValueConstraint::name() const {
    return "nvalue";
}

PresolveResult NValueConstraint::presolve(Model& model) {
    bool changed = false;

    // Rebuild support counts from current domains
    std::fill(support_count_.begin(), support_count_.end(), 0);
    std::fill(definite_.begin(), definite_.end(), false);
    std::fill(var_support_bitmap_.begin(), var_support_bitmap_.end(), false);
    definite_count_ = 0;
    possible_count_ = 0;

    for (size_t i = 0; i < num_x_; ++i) {
        auto var = model.variable(var_ids_[i]);
        std::vector<Domain::value_type> vals;
        var->domain().copy_values_to(vals);
        for (auto v : vals) {
            auto it = value_index_.find(v);
            if (it != value_index_.end()) {
                size_t vi = it->second;
                var_support_bitmap_[i * num_values_ + vi] = true;
                support_count_[vi]++;
            }
        }
        if (var->is_assigned()) {
            auto v = var->assigned_value().value();
            auto it = value_index_.find(v);
            if (it != value_index_.end()) {
                if (!definite_[it->second]) {
                    definite_[it->second] = true;
                    definite_count_++;
                }
            }
        }
    }

    for (size_t vi = 0; vi < num_values_; ++vi) {
        if (support_count_[vi] > 0) {
            possible_count_++;
        }
    }

    // Bounds on n
    auto n_var = model.variable(var_ids_[num_x_]);
    auto n_min = n_var->min();
    auto n_max = n_var->max();

    auto def = static_cast<Domain::value_type>(definite_count_);
    auto poss = static_cast<Domain::value_type>(possible_count_);

    if (def > n_max || poss < n_min) {
        return PresolveResult::Contradiction;
    }

    // n >= 1 if x is non-empty
    if (num_x_ > 0 && n_min < 1) {
        if (!n_var->remove_below(1)) return PresolveResult::Contradiction;
        changed = true;
    }
    // n <= |x|
    auto x_size = static_cast<Domain::value_type>(num_x_);
    if (n_max > x_size) {
        if (!n_var->remove_above(x_size)) return PresolveResult::Contradiction;
        changed = true;
    }

    if (def > n_min) {
        if (!n_var->remove_below(def)) return PresolveResult::Contradiction;
        changed = true;
    }
    if (poss < n_max) {
        if (!n_var->remove_above(poss)) return PresolveResult::Contradiction;
        changed = true;
    }

    // n.max == definite_count: remove non-definite values from unassigned vars
    n_max = n_var->max();
    if (n_max == def) {
        for (size_t i = 0; i < num_x_; ++i) {
            auto xi = model.variable(var_ids_[i]);
            if (xi->is_assigned()) continue;
            std::vector<Domain::value_type> vals;
            xi->domain().copy_values_to(vals);
            for (auto v : vals) {
                auto it = value_index_.find(v);
                if (it != value_index_.end() && !definite_[it->second]) {
                    if (!xi->remove(v)) return PresolveResult::Contradiction;
                    changed = true;
                }
            }
        }
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool NValueConstraint::prepare_propagation(Model& model) {
    // Rebuild from current model state
    std::fill(support_count_.begin(), support_count_.end(), 0);
    std::fill(definite_.begin(), definite_.end(), false);
    std::fill(var_support_bitmap_.begin(), var_support_bitmap_.end(), false);
    definite_count_ = 0;
    possible_count_ = 0;

    for (size_t i = 0; i < num_x_; ++i) {
        for (size_t vi = 0; vi < num_values_; ++vi) {
            auto v = sorted_values_[vi];
            if (model.contains(var_ids_[i], v)) {
                var_support_bitmap_[i * num_values_ + vi] = true;
                support_count_[vi]++;
            }
        }
        if (model.is_instantiated(var_ids_[i])) {
            auto v = model.value(var_ids_[i]);
            auto it = value_index_.find(v);
            if (it != value_index_.end()) {
                if (!definite_[it->second]) {
                    definite_[it->second] = true;
                    definite_count_++;
                }
            }
        }
    }

    for (size_t vi = 0; vi < num_values_; ++vi) {
        if (support_count_[vi] > 0) {
            possible_count_++;
        }
    }

    init_watches();
    trail_.clear();

    // Consistency check
    auto n_min = model.var_min(n_id_);
    auto n_max = model.var_max(n_id_);
    auto def = static_cast<Domain::value_type>(definite_count_);
    auto poss = static_cast<Domain::value_type>(possible_count_);

    if (def > n_max || poss < n_min) {
        return false;
    }

    return true;
}

bool NValueConstraint::on_instantiate(Model& model, int save_point,
                                       size_t var_idx, size_t internal_var_idx,
                                       Domain::value_type value,
                                       Domain::value_type prev_min,
                                       Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx,
                                     value, prev_min, prev_max)) {
        return false;
    }

    save_trail_if_needed(model, save_point);

    if (internal_var_idx < num_x_) {
        size_t var_i = internal_var_idx;

        // Mark value as definite if new
        auto it = value_index_.find(value);
        if (it != value_index_.end() && !definite_[it->second]) {
            trail_.back().second.definite_changes.push_back({it->second, false});
            definite_[it->second] = true;
            definite_count_++;
        }

        // Remove support for all other values in [prev_min, prev_max] range
        // Use sorted_values_ to find values in range
        auto lo = std::lower_bound(sorted_values_.begin(), sorted_values_.end(), prev_min);
        auto hi = std::upper_bound(sorted_values_.begin(), sorted_values_.end(), prev_max);
        for (auto sit = lo; sit != hi; ++sit) {
            if (*sit == value) continue;
            size_t vi = static_cast<size_t>(sit - sorted_values_.begin());
            size_t flat_idx = var_i * num_values_ + vi;
            if (var_support_bitmap_[flat_idx]) {
                trail_.back().second.bitmap_changes.push_back(flat_idx);
                var_support_bitmap_[flat_idx] = false;
                int old_count = support_count_[vi];
                trail_.back().second.support_count_changes.push_back({vi, old_count});
                support_count_[vi]--;
                if (support_count_[vi] == 0) {
                    possible_count_--;
                }
            }
        }
    }
    // else: n variable instantiated → propagate handles it

    if (has_uninstantiated(model)) {
        size_t last_idx = find_last_uninstantiated(model);
        if (last_idx != SIZE_MAX) {
            return on_last_uninstantiated(model, save_point, last_idx);
        }
    } else {
        return on_final_instantiate(model);
    }

    return propagate(model);
}

bool NValueConstraint::on_final_instantiate(const Model& model) {
    return static_cast<Domain::value_type>(definite_count_) == model.value(var_ids_[num_x_]);
}

bool NValueConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                                size_t last_var_internal_idx) {
    if (last_var_internal_idx == num_x_) {
        // Last uninstantiated is n
        // All x are assigned, count distinct
        auto def = static_cast<Domain::value_type>(definite_count_);
        if (model.is_instantiated(n_id_)) {
            return model.value(n_id_) == def;
        }
        if (model.contains(n_id_, def)) {
            model.enqueue_instantiate(n_id_, def);
        } else {
            return false;
        }
    } else {
        // Last uninstantiated is x[j] — just propagate
        return propagate(model);
    }
    return true;
}

bool NValueConstraint::on_remove_value(Model& model, int save_point,
                                        size_t var_idx, size_t internal_var_idx,
                                        Domain::value_type removed_value) {
    if (internal_var_idx >= num_x_) {
        // n variable domain changed
        return true;
    }

    auto it = value_index_.find(removed_value);
    if (it == value_index_.end()) return true;

    size_t vi = it->second;
    size_t flat_idx = internal_var_idx * num_values_ + vi;

    if (var_support_bitmap_[flat_idx]) {
        save_trail_if_needed(model, save_point);
        trail_.back().second.bitmap_changes.push_back(flat_idx);
        var_support_bitmap_[flat_idx] = false;
        int old_count = support_count_[vi];
        trail_.back().second.support_count_changes.push_back({vi, old_count});
        support_count_[vi]--;
        if (support_count_[vi] == 0) {
            possible_count_--;
            return propagate(model);
        }
    }
    return true;
}

bool NValueConstraint::on_set_min(Model& model, int save_point,
                                   size_t var_idx, size_t internal_var_idx,
                                   Domain::value_type new_min,
                                   Domain::value_type old_min) {
    if (internal_var_idx >= num_x_) {
        // n's bounds changed — no internal state mutation, just re-check
        return propagate(model);
    }

    // Values in [old_min, new_min) are removed
    bool any_change = false;
    auto lo = std::lower_bound(sorted_values_.begin(), sorted_values_.end(), old_min);
    auto hi = std::lower_bound(sorted_values_.begin(), sorted_values_.end(), new_min);
    for (auto sit = lo; sit != hi; ++sit) {
        size_t vi = static_cast<size_t>(sit - sorted_values_.begin());
        size_t flat_idx = internal_var_idx * num_values_ + vi;
        if (var_support_bitmap_[flat_idx]) {
            if (!any_change) {
                save_trail_if_needed(model, save_point);
                any_change = true;
            }
            trail_.back().second.bitmap_changes.push_back(flat_idx);
            var_support_bitmap_[flat_idx] = false;
            int old_count = support_count_[vi];
            trail_.back().second.support_count_changes.push_back({vi, old_count});
            support_count_[vi]--;
            if (support_count_[vi] == 0) {
                possible_count_--;
            }
        }
    }

    if (any_change) {
        return propagate(model);
    }
    return true;
}

bool NValueConstraint::on_set_max(Model& model, int save_point,
                                   size_t var_idx, size_t internal_var_idx,
                                   Domain::value_type new_max,
                                   Domain::value_type old_max) {
    if (internal_var_idx >= num_x_) {
        // n's bounds changed — no internal state mutation, just re-check
        return propagate(model);
    }

    // Values in (new_max, old_max] are removed
    bool any_change = false;
    auto lo = std::upper_bound(sorted_values_.begin(), sorted_values_.end(), new_max);
    auto hi = std::upper_bound(sorted_values_.begin(), sorted_values_.end(), old_max);
    for (auto sit = lo; sit != hi; ++sit) {
        size_t vi = static_cast<size_t>(sit - sorted_values_.begin());
        size_t flat_idx = internal_var_idx * num_values_ + vi;
        if (var_support_bitmap_[flat_idx]) {
            if (!any_change) {
                save_trail_if_needed(model, save_point);
                any_change = true;
            }
            trail_.back().second.bitmap_changes.push_back(flat_idx);
            var_support_bitmap_[flat_idx] = false;
            int old_count = support_count_[vi];
            trail_.back().second.support_count_changes.push_back({vi, old_count});
            support_count_[vi]--;
            if (support_count_[vi] == 0) {
                possible_count_--;
            }
        }
    }

    if (any_change) {
        return propagate(model);
    }
    return true;
}

void NValueConstraint::rewind_to(int save_point) {
    while (!trail_.empty() && trail_.back().first > save_point) {
        const auto& entry = trail_.back().second;

        // Restore bitmap (true→false changes, rewind by setting true)
        for (auto it = entry.bitmap_changes.rbegin();
             it != entry.bitmap_changes.rend(); ++it) {
            var_support_bitmap_[*it] = true;
        }

        // Restore support counts
        for (auto it = entry.support_count_changes.rbegin();
             it != entry.support_count_changes.rend(); ++it) {
            support_count_[it->first] = it->second;
        }

        // Restore definite flags
        for (auto it = entry.definite_changes.rbegin();
             it != entry.definite_changes.rend(); ++it) {
            definite_[it->first] = it->second;
        }

        definite_count_ = entry.definite_count;
        possible_count_ = entry.possible_count;
        trail_.pop_back();
    }
}

void NValueConstraint::save_trail_if_needed(Model& model, int save_point) {
    if (trail_.empty() || trail_.back().first != save_point) {
        TrailEntry entry;
        entry.definite_count = definite_count_;
        entry.possible_count = possible_count_;
        trail_.push_back({save_point, std::move(entry)});
        model.mark_constraint_dirty(model_index(), save_point);
    }
}

bool NValueConstraint::propagate(Model& model) {
    auto n_min = model.var_min(n_id_);
    auto n_max = model.var_max(n_id_);
    auto def = static_cast<Domain::value_type>(definite_count_);
    auto poss = static_cast<Domain::value_type>(possible_count_);

    // Contradiction checks
    if (def > n_max || poss < n_min) {
        return false;
    }

    // Bounds propagation on n
    if (def > n_min) {
        model.enqueue_set_min(n_id_, def);
    }
    if (poss < n_max) {
        model.enqueue_set_max(n_id_, poss);
    }

    // n.max == definite_count: unassigned vars can only take definite values
    if (n_max == def) {
        for (size_t i = 0; i < num_x_; ++i) {
            if (model.is_instantiated(var_ids_[i])) continue;
            // Remove all non-definite values
            for (size_t vi = 0; vi < num_values_; ++vi) {
                size_t flat_idx = i * num_values_ + vi;
                if (var_support_bitmap_[flat_idx] && !definite_[vi]) {
                    model.enqueue_remove_value(var_ids_[i], sorted_values_[vi]);
                }
            }
        }
    }

    return true;
}

}  // namespace sabori_csp
