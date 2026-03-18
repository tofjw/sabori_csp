#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"

namespace sabori_csp {

// ============================================================================
// InverseConstraint implementation
// ============================================================================

InverseConstraint::InverseConstraint(std::vector<VariablePtr> f, std::vector<VariablePtr> invf)
    : Constraint()
    , n_(f.size())
    , offset_(1)  // FlatZinc inverse is 1-indexed
{
    if (f.size() != invf.size()) {
        set_initially_inconsistent(true);
        return;
    }

    std::vector<VariablePtr> all;
    all.reserve(2 * n_);
    for (auto& v : f) all.push_back(std::move(v));
    for (auto& v : invf) all.push_back(std::move(v));
    var_ids_ = extract_var_ids(all);
}

std::string InverseConstraint::name() const {
    return "inverse";
}

PresolveResult InverseConstraint::presolve(Model& model) {
    bool changed = false;
    auto val_min = static_cast<Domain::value_type>(offset_);
    auto val_max = static_cast<Domain::value_type>(offset_ + static_cast<int64_t>(n_) - 1);

    // 1. Restrict all domains to [offset_, offset_ + n_ - 1]
    for (size_t k = 0; k < 2 * n_; ++k) {
        auto var_id = var_ids_[k];
        auto* var = model.variable(var_id);
        if (var->min() < val_min) {
            var->remove_below(val_min);
            changed = true;
        }
        if (var->max() > val_max) {
            var->remove_above(val_max);
            changed = true;
        }
        if (var->domain().empty()) return PresolveResult::Contradiction;
    }

    // 2. Fixed-point loop for support propagation
    bool progress = true;
    while (progress) {
        progress = false;

        for (size_t i = 0; i < n_; ++i) {
            auto f_id = var_ids_[i];
            auto invf_i_id = var_ids_[n_ + i];
            auto* f_var = model.variable(f_id);
            auto* invf_var = model.variable(invf_i_id);

            // f[i] assigned to v → invf[v-offset] = i+offset
            if (f_var->is_assigned()) {
                auto v = f_var->assigned_value().value();
                size_t j = static_cast<size_t>(v - offset_);
                if (j >= n_) return PresolveResult::Contradiction;
                auto* target = model.variable(var_ids_[n_ + j]);
                auto i_val = static_cast<Domain::value_type>(i) + static_cast<Domain::value_type>(offset_);
                if (!target->is_assigned()) {
                    target->assign(i_val);
                    if (target->domain().empty()) return PresolveResult::Contradiction;
                    changed = true;
                    progress = true;
                } else if (target->assigned_value().value() != i_val) {
                    return PresolveResult::Contradiction;
                }
            }

            // invf[i] assigned to v → f[v-offset] = i+offset
            if (invf_var->is_assigned()) {
                auto v = invf_var->assigned_value().value();
                size_t j = static_cast<size_t>(v - offset_);
                if (j >= n_) return PresolveResult::Contradiction;
                auto* target = model.variable(var_ids_[j]);
                auto i_val = static_cast<Domain::value_type>(i) + static_cast<Domain::value_type>(offset_);
                if (!target->is_assigned()) {
                    target->assign(i_val);
                    if (target->domain().empty()) return PresolveResult::Contradiction;
                    changed = true;
                    progress = true;
                } else if (target->assigned_value().value() != i_val) {
                    return PresolveResult::Contradiction;
                }
            }
        }

        // Domain support: if v not in dom(invf[v-offset]) for value in f[i], remove from f[i]
        for (size_t i = 0; i < n_; ++i) {
            auto f_id = var_ids_[i];
            auto* f_var = model.variable(f_id);
            if (f_var->is_assigned()) continue;

            auto i_val = static_cast<Domain::value_type>(i) + static_cast<Domain::value_type>(offset_);
            std::vector<Domain::value_type> to_remove;
            for (auto v = f_var->min(); v <= f_var->max(); ++v) {
                if (!f_var->domain().contains(v)) continue;
                size_t j = static_cast<size_t>(v - offset_);
                if (j >= n_) {
                    to_remove.push_back(v);
                    continue;
                }
                auto* invf_j = model.variable(var_ids_[n_ + j]);
                if (!invf_j->domain().contains(i_val)) {
                    to_remove.push_back(v);
                }
            }
            for (auto v : to_remove) {
                f_var->remove(v);
                changed = true;
                progress = true;
            }
            if (f_var->domain().empty()) return PresolveResult::Contradiction;
        }

        // Same for invf side
        for (size_t i = 0; i < n_; ++i) {
            auto invf_id = var_ids_[n_ + i];
            auto* invf_var = model.variable(invf_id);
            if (invf_var->is_assigned()) continue;

            auto i_val = static_cast<Domain::value_type>(i) + static_cast<Domain::value_type>(offset_);
            std::vector<Domain::value_type> to_remove;
            for (auto v = invf_var->min(); v <= invf_var->max(); ++v) {
                if (!invf_var->domain().contains(v)) continue;
                size_t j = static_cast<size_t>(v - offset_);
                if (j >= n_) {
                    to_remove.push_back(v);
                    continue;
                }
                auto* f_j = model.variable(var_ids_[j]);
                if (!f_j->domain().contains(i_val)) {
                    to_remove.push_back(v);
                }
            }
            for (auto v : to_remove) {
                invf_var->remove(v);
                changed = true;
                progress = true;
            }
            if (invf_var->domain().empty()) return PresolveResult::Contradiction;
        }
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool InverseConstraint::prepare_propagation(Model& model) {
    // No internal state to rebuild; all propagation is via enqueue
    (void)model;
    return true;
}

bool InverseConstraint::on_instantiate(Model& model, int /*save_point*/,
                                        size_t /*var_idx*/, size_t internal_var_idx,
                                        Domain::value_type value,
                                        Domain::value_type /*prev_min*/,
                                        Domain::value_type /*prev_max*/) {
    size_t j = static_cast<size_t>(value - offset_);
    if (j >= n_) return false;

    if (internal_var_idx < n_) {
        // f[i] = value → invf[value-offset] = i+offset
        size_t i = internal_var_idx;
        auto i_val = static_cast<Domain::value_type>(i) + static_cast<Domain::value_type>(offset_);

        model.enqueue_instantiate(var_ids_[n_ + j], i_val);

        // all_different on f: remove value from other f[k]
        for (size_t k = 0; k < n_; ++k) {
            if (k == i) continue;
            if (!model.is_instantiated(var_ids_[k])) {
                model.enqueue_remove_value(var_ids_[k], value);
            }
        }

        // all_different on invf: remove i_val from other invf[k]
        for (size_t k = 0; k < n_; ++k) {
            if (k == j) continue;
            if (!model.is_instantiated(var_ids_[n_ + k])) {
                model.enqueue_remove_value(var_ids_[n_ + k], i_val);
            }
        }
    } else {
        // invf[i] = value → f[value-offset] = i+offset
        size_t i = internal_var_idx - n_;
        auto i_val = static_cast<Domain::value_type>(i) + static_cast<Domain::value_type>(offset_);

        model.enqueue_instantiate(var_ids_[j], i_val);

        // all_different on invf: remove value from other invf[k]
        for (size_t k = 0; k < n_; ++k) {
            if (k == i) continue;
            if (!model.is_instantiated(var_ids_[n_ + k])) {
                model.enqueue_remove_value(var_ids_[n_ + k], value);
            }
        }

        // all_different on f: remove i_val from other f[k]
        for (size_t k = 0; k < n_; ++k) {
            if (k == j) continue;
            if (!model.is_instantiated(var_ids_[k])) {
                model.enqueue_remove_value(var_ids_[k], i_val);
            }
        }
    }

    return true;
}

bool InverseConstraint::on_final_instantiate(const Model& model) {
    // Verify: f[i] = j <-> invf[j] = i for all i
    for (size_t i = 0; i < n_; ++i) {
        auto f_val = model.value(var_ids_[i]);
        size_t j = static_cast<size_t>(f_val - offset_);
        if (j >= n_) return false;

        auto invf_val = model.value(var_ids_[n_ + j]);
        auto expected = static_cast<Domain::value_type>(i) + static_cast<Domain::value_type>(offset_);
        if (invf_val != expected) return false;
    }
    return true;
}

bool InverseConstraint::on_remove_value(Model& model, int /*save_point*/,
                                         size_t /*var_idx*/, size_t internal_var_idx,
                                         Domain::value_type removed_value) {
    size_t j = static_cast<size_t>(removed_value - offset_);
    if (j >= n_) return true;  // out of range, already handled by domain restriction

    if (internal_var_idx < n_) {
        // f[i] lost value v → invf[v-offset] loses i+offset
        size_t i = internal_var_idx;
        auto i_val = static_cast<Domain::value_type>(i) + static_cast<Domain::value_type>(offset_);
        model.enqueue_remove_value(var_ids_[n_ + j], i_val);
    } else {
        // invf[i] lost value v → f[v-offset] loses i+offset
        size_t i = internal_var_idx - n_;
        auto i_val = static_cast<Domain::value_type>(i) + static_cast<Domain::value_type>(offset_);
        model.enqueue_remove_value(var_ids_[j], i_val);
    }

    return true;
}

bool InverseConstraint::on_set_min(Model& model, int /*save_point*/,
                                    size_t /*var_idx*/, size_t internal_var_idx,
                                    Domain::value_type new_min, Domain::value_type old_min) {
    // Values old_min..new_min-1 have been removed
    for (auto v = old_min; v < new_min; ++v) {
        size_t j = static_cast<size_t>(v - offset_);
        if (j >= n_) continue;

        if (internal_var_idx < n_) {
            size_t i = internal_var_idx;
            auto i_val = static_cast<Domain::value_type>(i) + static_cast<Domain::value_type>(offset_);
            model.enqueue_remove_value(var_ids_[n_ + j], i_val);
        } else {
            size_t i = internal_var_idx - n_;
            auto i_val = static_cast<Domain::value_type>(i) + static_cast<Domain::value_type>(offset_);
            model.enqueue_remove_value(var_ids_[j], i_val);
        }
    }
    return true;
}

bool InverseConstraint::on_set_max(Model& model, int /*save_point*/,
                                    size_t /*var_idx*/, size_t internal_var_idx,
                                    Domain::value_type new_max, Domain::value_type old_max) {
    // Values new_max+1..old_max have been removed
    for (auto v = new_max + 1; v <= old_max; ++v) {
        size_t j = static_cast<size_t>(v - offset_);
        if (j >= n_) continue;

        if (internal_var_idx < n_) {
            size_t i = internal_var_idx;
            auto i_val = static_cast<Domain::value_type>(i) + static_cast<Domain::value_type>(offset_);
            model.enqueue_remove_value(var_ids_[n_ + j], i_val);
        } else {
            size_t i = internal_var_idx - n_;
            auto i_val = static_cast<Domain::value_type>(i) + static_cast<Domain::value_type>(offset_);
            model.enqueue_remove_value(var_ids_[j], i_val);
        }
    }
    return true;
}

void InverseConstraint::rewind_to(int /*save_point*/) {
    // No internal state to rewind
}

}  // namespace sabori_csp
