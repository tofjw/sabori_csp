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
        std::vector<size_t> ids = {m->id()};
        for (const auto& v : vars) ids.push_back(v->id());
        return ids;
    }())
    , n_(vars.size()) {

    m_id_ = m->id();
}

std::string ArrayIntMinimumConstraint::name() const {
    return "array_int_minimum";
}

PresolveResult ArrayIntMinimumConstraint::presolve(Model& model) {
    if (n_ == 0) {
        return PresolveResult::Contradiction;
    }

    bool changed = false;
    auto* m_var = model.variable(m_id_);

    // 1. 全 x[i] の最小値の最小値を計算 -> m.min
    Domain::value_type min_of_min = model.var_min(var_ids_[1]);
    for (size_t i = 1; i < n_; ++i) {
        min_of_min = std::min(min_of_min, model.var_min(var_ids_[1 + i]));
    }

    // 2. 全 x[i] の最大値の最小値を計算 -> m.max
    Domain::value_type min_of_max = model.var_max(var_ids_[1]);
    for (size_t i = 1; i < n_; ++i) {
        min_of_max = std::min(min_of_max, model.var_max(var_ids_[1 + i]));
    }

    // 3. m のドメインを絞る: min_of_min <= m <= min_of_max
    if (min_of_min > m_var->min()) changed = true;
    if (!m_var->remove_below(min_of_min)) return PresolveResult::Contradiction;
    if (min_of_max < m_var->max()) changed = true;
    if (!m_var->remove_above(min_of_max)) return PresolveResult::Contradiction;

    // 4. 各 x[i].min を m.min 以上に絞る
    auto m_min = model.var_min(m_id_);
    for (size_t i = 0; i < n_; ++i) {
        if (m_min > model.variable(var_ids_[1 + i])->min()) changed = true;
        if (!model.variable(var_ids_[1 + i])->remove_below(m_min)) return PresolveResult::Contradiction;
    }

    // 5. m が確定している場合: 少なくとも1つの x[i] が m に等しくなれる必要がある
    if (m_var->is_assigned()) {
        auto m_val = m_var->assigned_value().value();
        bool can_achieve = false;
        for (size_t i = 0; i < n_; ++i) {
            if (model.variable(var_ids_[1 + i])->domain().contains(m_val)) {
                can_achieve = true;
                break;
            }
        }
        if (!can_achieve) return PresolveResult::Contradiction;
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool ArrayIntMinimumConstraint::on_instantiate(Model& model, int save_point,
                                                 size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type value,
                                                 Domain::value_type /*prev_min*/,
                                                 Domain::value_type /*prev_max*/) {
    // 確定した変数を特定: var_ids_ layout = [m, x[0], ..., x[n-1]]
    size_t internal_idx = SIZE_MAX;
    auto assigned_id = model.variable(var_idx)->id();
    for (size_t i = 0; i < var_ids_.size(); ++i) {
        if (var_ids_[i] == assigned_id) {
            internal_idx = i;
            break;
        }
    }
    if (internal_idx == SIZE_MAX) {
        return true;
    }

    if (internal_idx == 0) {
        // m が確定: 全 x[i].min を m 以上に制限し、少なくとも1つは m になれる必要あり
        bool can_achieve = false;
        for (size_t i = 0; i < n_; ++i) {
            auto x_id = var_ids_[1 + i];
            if (!model.is_instantiated(x_id)) {
                auto var_min = model.var_min(x_id);
                if (var_min < value) {
                    model.enqueue_set_min(x_id, value);
                }
                if (model.contains(var_ids_[1 + i], value)) {
                    can_achieve = true;
                }
            } else {
                auto x_val = model.value(x_id);
                if (x_val < value) {
                    return false;  // x[i] < m は矛盾
                }
                if (x_val == value) {
                    can_achieve = true;
                }
            }
        }
        if (!can_achieve) {
            return false;
        }
    } else {
        // x[i] が確定: m <= value を確認し、m.max を更新
        if (model.is_instantiated(m_id_)) {
            if (model.value(m_id_) > value) {
                return false;
            }
        } else {
            auto m_max = model.var_max(m_id_);
            if (m_max > value) {
                model.enqueue_set_max(m_id_, value);
            }
        }
    }

    // 残り変数が 1 or 0 の時
    if (has_uninstantiated(model)) {
        size_t last_idx = find_last_uninstantiated(model);
        if (last_idx != SIZE_MAX) {
            if (!on_last_uninstantiated(model, save_point, last_idx)) {
                return false;
            }
        }
    }
    else {
        return on_final_instantiate(model);
    }

    return true;
}

bool ArrayIntMinimumConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                                         size_t last_var_internal_idx) {
    auto last_var_id = var_ids_[last_var_internal_idx];

    if (last_var_id == m_id_) {
        // m が最後: 全 x[i] の最小値を計算して m を確定
        Domain::value_type min_val = model.value(var_ids_[1]);
        for (size_t i = 1; i < n_; ++i) {
            min_val = std::min(min_val, model.value(var_ids_[1 + i]));
        }

        if (model.is_instantiated(m_id_)) {
            return model.value(m_id_) == min_val;
        }

        if (model.contains(m_id_, min_val)) {
            model.enqueue_instantiate(m_id_, min_val);
            return true;
        }
        return false;
    } else {
        // x[i] が最後
        auto m_val = model.value(m_id_);

        Domain::value_type other_min = std::numeric_limits<Domain::value_type>::max();
        for (size_t i = 0; i < n_; ++i) {
            if (var_ids_[1 + i] != last_var_id) {
                other_min = std::min(other_min, model.value(var_ids_[1 + i]));
            }
        }

        if (model.is_instantiated(last_var_id)) {
            auto val = model.value(last_var_id);
            return std::min(other_min, val) == m_val;
        }

        if (other_min > m_val) {
            // last_var == m_val でなければならない
            if (model.contains(last_var_id, m_val)) {
                model.enqueue_instantiate(last_var_id, m_val);
                return true;
            }
            return false;
        } else if (other_min == m_val) {
            // last_var >= m_val であれば OK
            auto var_min = model.var_min(last_var_id);
            if (var_min < m_val) {
                model.enqueue_set_min(last_var_id, m_val);
            }
            return true;
        } else {
            return false;
        }
    }
}

bool ArrayIntMinimumConstraint::on_final_instantiate(const Model& model) {
    auto m_val = model.value(m_id_);
    // base var_ids_ layout: [m, x[0], ..., x[n-1]]
    Domain::value_type min_val = model.value(var_ids_[1]);
    for (size_t i = 2; i <= n_; ++i) {
        min_val = std::min(min_val, model.value(var_ids_[i]));
    }
    return m_val == min_val;
}

void ArrayIntMinimumConstraint::rewind_to(int /*save_point*/) {
    // 状態を持たないので何もしない
}

// ============================================================================

}  // namespace sabori_csp

