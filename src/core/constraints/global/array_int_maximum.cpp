#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <set>
#include <limits>

namespace sabori_csp {

// ============================================================================
// ArrayIntMaximumConstraint implementation
// ============================================================================

ArrayIntMaximumConstraint::ArrayIntMaximumConstraint(VariablePtr m, std::vector<VariablePtr> vars)
    : Constraint([&]() {
        std::vector<VariablePtr> all_vars = {m};
        all_vars.insert(all_vars.end(), vars.begin(), vars.end());
        return all_vars;
    }())
    , m_(std::move(m))
    , x_(std::move(vars))
    , n_(x_.size()) {

    // var_ptr_to_idx 構築: 0 = m, 1..n = x[0]..x[n-1]
    var_ptr_to_idx_[m_.get()] = 0;
    for (size_t i = 0; i < n_; ++i) {
        var_ptr_to_idx_[x_[i].get()] = i + 1;
    }

    check_initial_consistency();
}

std::string ArrayIntMaximumConstraint::name() const {
    return "array_int_maximum";
}

std::vector<VariablePtr> ArrayIntMaximumConstraint::variables() const {
    std::vector<VariablePtr> result = {m_};
    result.insert(result.end(), x_.begin(), x_.end());
    return result;
}

std::optional<bool> ArrayIntMaximumConstraint::is_satisfied() const {
    // 全変数が確定しているか
    if (!m_->is_assigned()) return std::nullopt;
    for (const auto& var : x_) {
        if (!var->is_assigned()) return std::nullopt;
    }

    // m が全ての x[i] の最大値と一致するか
    auto m_val = m_->assigned_value().value();
    Domain::value_type max_val = x_[0]->assigned_value().value();
    for (size_t i = 1; i < n_; ++i) {
        max_val = std::max(max_val, x_[i]->assigned_value().value());
    }
    return m_val == max_val;
}

bool ArrayIntMaximumConstraint::propagate(Model& /*model*/) {
    if (n_ == 0) {
        return false;  // 空配列は不正
    }

    // 1. 全 x[i] の最大値の最大値を計算 -> m.max
    Domain::value_type max_of_max = x_[0]->domain().max().value();
    for (size_t i = 1; i < n_; ++i) {
        max_of_max = std::max(max_of_max, x_[i]->domain().max().value());
    }

    // 2. 全 x[i] の最小値の最大値を計算 -> m.min
    Domain::value_type max_of_min = x_[0]->domain().min().value();
    for (size_t i = 1; i < n_; ++i) {
        max_of_min = std::max(max_of_min, x_[i]->domain().min().value());
    }

    // 3. m のドメインを絞る: max_of_min <= m <= max_of_max
    auto& m_domain = m_->domain();
    auto m_values = m_domain.values();
    for (auto v : m_values) {
        if (v < max_of_min || v > max_of_max) {
            if (!m_domain.remove(v)) {
                return false;
            }
        }
    }

    // 4. 各 x[i].max を m.max 以下に絞る
    auto m_max = m_domain.max().value();
    for (auto& var : x_) {
        auto x_values = var->domain().values();
        for (auto v : x_values) {
            if (v > m_max) {
                if (!var->domain().remove(v)) {
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

bool ArrayIntMaximumConstraint::on_instantiate(Model& model, int save_point,
                                                 size_t var_idx, Domain::value_type value,
                                                 Domain::value_type /*prev_min*/,
                                                 Domain::value_type /*prev_max*/) {
    // 確定した変数を特定
    VariablePtr assigned_var = model.variable(var_idx);
    auto it = var_ptr_to_idx_.find(assigned_var.get());
    if (it == var_ptr_to_idx_.end()) {
        return true;  // この制約に関係ない変数
    }

    size_t internal_idx = it->second;

    if (internal_idx == 0) {
        // m が確定: 全 x[i].max を m 以下に制限し、少なくとも1つは m になれる必要あり
        bool can_achieve = false;
        for (auto& var : x_) {
            if (!var->is_assigned()) {
                // x[i].max を m 以下に
                auto var_max = var->domain().max().value();
                if (var_max > value) {
                    model.enqueue_set_max(var->id(), value);
                }
                // m に等しくなれるか
                if (var->domain().contains(value)) {
                    can_achieve = true;
                }
            } else {
                // x[i] が確定済み
                if (var->assigned_value().value() > value) {
                    return false;  // x[i] > m は矛盾
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
        // x[i] が確定: m >= value を確認し、m.min を更新
        if (m_->is_assigned()) {
            if (m_->assigned_value().value() < value) {
                return false;  // x[i] > m は矛盾
            }
        } else {
            // m.min を更新
            auto m_min = m_->domain().min().value();
            if (m_min < value) {
                model.enqueue_set_min(m_->id(), value);
            }
        }
    }

    // 残り1変数チェック
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

bool ArrayIntMaximumConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                                         size_t last_var_internal_idx) {
    auto& last_var = vars_[last_var_internal_idx];

    if (last_var.get() == m_.get()) {
        // m が最後: 全 x[i] の最大値を計算して m を確定
        Domain::value_type max_val = x_[0]->assigned_value().value();
        for (size_t i = 1; i < n_; ++i) {
            max_val = std::max(max_val, x_[i]->assigned_value().value());
        }

        if (m_->is_assigned()) {
            return m_->assigned_value().value() == max_val;
        }

        if (m_->domain().contains(max_val)) {
            model.enqueue_instantiate(m_->id(), max_val);
            return true;
        }
        return false;
    } else {
        // x[i] が最後: m は確定済み
        auto m_val = m_->assigned_value().value();

        // 他の x[j] の最大値を計算
        Domain::value_type other_max = std::numeric_limits<Domain::value_type>::min();
        for (const auto& var : x_) {
            if (var.get() != last_var.get()) {
                other_max = std::max(other_max, var->assigned_value().value());
            }
        }

        if (last_var->is_assigned()) {
            auto val = last_var->assigned_value().value();
            return std::max(other_max, val) == m_val;
        }

        // last_var のドメインを絞る
        // 1. last_var <= m_val
        // 2. max(other_max, last_var) == m_val
        //    -> last_var == m_val (if other_max < m_val)
        //    -> last_var <= m_val (if other_max == m_val)

        if (other_max < m_val) {
            // last_var == m_val でなければならない
            if (last_var->domain().contains(m_val)) {
                model.enqueue_instantiate(last_var->id(), m_val);
                return true;
            }
            return false;
        } else if (other_max == m_val) {
            // last_var <= m_val であれば OK
            auto var_max = last_var->domain().max().value();
            if (var_max > m_val) {
                model.enqueue_set_max(last_var->id(), m_val);
            }
            return !last_var->domain().empty();
        } else {
            // other_max > m_val は既にエラーのはず
            return false;
        }
    }
}

bool ArrayIntMaximumConstraint::on_final_instantiate() {
    auto m_val = m_->assigned_value().value();
    Domain::value_type max_val = x_[0]->assigned_value().value();
    for (size_t i = 1; i < n_; ++i) {
        max_val = std::max(max_val, x_[i]->assigned_value().value());
    }
    return m_val == max_val;
}

void ArrayIntMaximumConstraint::rewind_to(int /*save_point*/) {
    // 状態を持たないので何もしない
}

void ArrayIntMaximumConstraint::check_initial_consistency() {
    if (n_ == 0) {
        set_initially_inconsistent(true);
        return;
    }

    // 全変数が確定している場合
    if (m_->is_assigned()) {
        auto m_val = m_->assigned_value().value();
        bool all_fixed = true;
        Domain::value_type max_val = std::numeric_limits<Domain::value_type>::min();

        for (const auto& var : x_) {
            if (!var->is_assigned()) {
                all_fixed = false;
                break;
            }
            max_val = std::max(max_val, var->assigned_value().value());
        }

        if (all_fixed && m_val != max_val) {
            set_initially_inconsistent(true);
            return;
        }
    }
}

// ============================================================================

}  // namespace sabori_csp

