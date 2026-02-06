#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <set>
#include <limits>

namespace sabori_csp {

// ============================================================================
// ArrayIntMinimumConstraint implementation
// ============================================================================

ArrayIntMinimumConstraint::ArrayIntMinimumConstraint(VariablePtr m, std::vector<VariablePtr> vars)
    : Constraint([&]() {
        std::vector<VariablePtr> all_vars = {m};
        all_vars.insert(all_vars.end(), vars.begin(), vars.end());
        return all_vars;
    }())
    , m_(std::move(m))
    , x_(std::move(vars))
    , n_(x_.size()) {

    var_ptr_to_idx_[m_.get()] = 0;
    for (size_t i = 0; i < n_; ++i) {
        var_ptr_to_idx_[x_[i].get()] = i + 1;
    }

    check_initial_consistency();
}

std::string ArrayIntMinimumConstraint::name() const {
    return "array_int_minimum";
}

std::vector<VariablePtr> ArrayIntMinimumConstraint::variables() const {
    std::vector<VariablePtr> result = {m_};
    result.insert(result.end(), x_.begin(), x_.end());
    return result;
}

std::optional<bool> ArrayIntMinimumConstraint::is_satisfied() const {
    if (!m_->is_assigned()) return std::nullopt;
    for (const auto& var : x_) {
        if (!var->is_assigned()) return std::nullopt;
    }

    auto m_val = m_->assigned_value().value();
    Domain::value_type min_val = x_[0]->assigned_value().value();
    for (size_t i = 1; i < n_; ++i) {
        min_val = std::min(min_val, x_[i]->assigned_value().value());
    }
    return m_val == min_val;
}

bool ArrayIntMinimumConstraint::propagate(Model& model) {
    if (n_ == 0) {
        return false;
    }

    // 1. 全 x[i] の最小値の最小値を計算 -> m.min
    Domain::value_type min_of_min = model.var_min(x_[0]->id());
    for (size_t i = 1; i < n_; ++i) {
        min_of_min = std::min(min_of_min, model.var_min(x_[i]->id()));
    }

    // 2. 全 x[i] の最大値の最小値を計算 -> m.max
    Domain::value_type min_of_max = model.var_max(x_[0]->id());
    for (size_t i = 1; i < n_; ++i) {
        min_of_max = std::min(min_of_max, model.var_max(x_[i]->id()));
    }

    // 3. m のドメインを絞る: min_of_min <= m <= min_of_max
    auto& m_domain = m_->domain();
    auto m_values = m_domain.values();
    for (auto v : m_values) {
        if (v < min_of_min || v > min_of_max) {
            if (!m_domain.remove(v)) {
                return false;
            }
        }
    }

    // 4. 各 x[i].min を m.min 以上に絞る
    auto m_min = model.var_min(m_->id());
    for (auto& var : x_) {
        auto x_values = var->domain().values();
        for (auto v : x_values) {
            if (v < m_min) {
                if (!var->remove(v)) {
                    return false;
                }
            }
        }
    }

    // 5. m が確定している場合: 少なくとも1つの x[i] が m に等しくなれる必要がある
    if (m_->is_assigned()) {
        auto m_val = m_->assigned_value().value();
        bool can_achieve = false;
        for (const auto& var : x_) {
            if (var->domain().contains(m_val)) {
                can_achieve = true;
                break;
            }
        }
        if (!can_achieve) return false;
    }

    return true;
}

bool ArrayIntMinimumConstraint::on_instantiate(Model& model, int save_point,
                                                 size_t var_idx, Domain::value_type value,
                                                 Domain::value_type /*prev_min*/,
                                                 Domain::value_type /*prev_max*/) {
    VariablePtr assigned_var = model.variable(var_idx);
    auto it = var_ptr_to_idx_.find(assigned_var.get());
    if (it == var_ptr_to_idx_.end()) {
        return true;
    }

    size_t internal_idx = it->second;

    if (internal_idx == 0) {
        // m が確定: 全 x[i].min を m 以上に制限し、少なくとも1つは m になれる必要あり
        bool can_achieve = false;
        for (auto& var : x_) {
            if (!var->is_assigned()) {
                auto var_min = model.var_min(var->id());
                if (var_min < value) {
                    model.enqueue_set_min(var->id(), value);
                }
                if (var->domain().contains(value)) {
                    can_achieve = true;
                }
            } else {
                if (var->assigned_value().value() < value) {
                    return false;  // x[i] < m は矛盾
                }
                if (var->assigned_value().value() == value) {
                    can_achieve = true;
                }
            }
        }
        if (!can_achieve) {
            return false;
        }
    } else {
        // x[i] が確定: m <= value を確認し、m.max を更新
        if (m_->is_assigned()) {
            if (m_->assigned_value().value() > value) {
                return false;
            }
        } else {
            auto m_max = model.var_max(m_->id());
            if (m_max > value) {
                model.enqueue_set_max(m_->id(), value);
            }
        }
    }

    size_t uninstantiated_count = count_uninstantiated();
    if (uninstantiated_count == 1) {
        size_t last_idx = find_last_uninstantiated();
        if (last_idx != SIZE_MAX) {
            if (!on_last_uninstantiated(model, save_point, last_idx)) {
                return false;
            }
        }
    } else if (uninstantiated_count == 0) {
        return on_final_instantiate();
    }

    return true;
}

bool ArrayIntMinimumConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                                         size_t last_var_internal_idx) {
    auto& last_var = vars_[last_var_internal_idx];

    if (last_var.get() == m_.get()) {
        // m が最後: 全 x[i] の最小値を計算して m を確定
        Domain::value_type min_val = x_[0]->assigned_value().value();
        for (size_t i = 1; i < n_; ++i) {
            min_val = std::min(min_val, x_[i]->assigned_value().value());
        }

        if (m_->is_assigned()) {
            return m_->assigned_value().value() == min_val;
        }

        if (m_->domain().contains(min_val)) {
            model.enqueue_instantiate(m_->id(), min_val);
            return true;
        }
        return false;
    } else {
        // x[i] が最後
        auto m_val = m_->assigned_value().value();

        Domain::value_type other_min = std::numeric_limits<Domain::value_type>::max();
        for (const auto& var : x_) {
            if (var.get() != last_var.get()) {
                other_min = std::min(other_min, var->assigned_value().value());
            }
        }

        if (last_var->is_assigned()) {
            auto val = last_var->assigned_value().value();
            return std::min(other_min, val) == m_val;
        }

        if (other_min > m_val) {
            // last_var == m_val でなければならない
            if (last_var->domain().contains(m_val)) {
                model.enqueue_instantiate(last_var->id(), m_val);
                return true;
            }
            return false;
        } else if (other_min == m_val) {
            // last_var >= m_val であれば OK
            auto var_min = model.var_min(last_var->id());
            if (var_min < m_val) {
                model.enqueue_set_min(last_var->id(), m_val);
            }
            return !last_var->domain().empty();
        } else {
            return false;
        }
    }
}

bool ArrayIntMinimumConstraint::on_final_instantiate() {
    auto m_val = m_->assigned_value().value();
    Domain::value_type min_val = x_[0]->assigned_value().value();
    for (size_t i = 1; i < n_; ++i) {
        min_val = std::min(min_val, x_[i]->assigned_value().value());
    }
    return m_val == min_val;
}

void ArrayIntMinimumConstraint::rewind_to(int /*save_point*/) {
    // 状態を持たないので何もしない
}

void ArrayIntMinimumConstraint::check_initial_consistency() {
    if (n_ == 0) {
        set_initially_inconsistent(true);
        return;
    }

    if (m_->is_assigned()) {
        auto m_val = m_->assigned_value().value();
        bool all_fixed = true;
        Domain::value_type min_val = std::numeric_limits<Domain::value_type>::max();

        for (const auto& var : x_) {
            if (!var->is_assigned()) {
                all_fixed = false;
                break;
            }
            min_val = std::min(min_val, var->assigned_value().value());
        }

        if (all_fixed && m_val != min_val) {
            set_initially_inconsistent(true);
            return;
        }
    }
}

// ============================================================================

}  // namespace sabori_csp

