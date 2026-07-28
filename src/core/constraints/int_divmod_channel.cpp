/**
 * @file int_divmod_channel.cpp
 * @brief IntDivModChannelConstraint: x = c*q + r かつ 0 <= r < c
 */
#include "sabori_csp/constraints/arithmetic.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <vector>

namespace sabori_csp {

namespace {

std::vector<size_t> build_var_ids(const VariablePtr& x, const VariablePtr& q,
                                  const VariablePtr& r) {
    return {x->id(), q->id(), r->id()};
}

} // namespace

IntDivModChannelConstraint::IntDivModChannelConstraint(VariablePtr x,
                                                       Domain::value_type divisor,
                                                       VariablePtr q, VariablePtr r)
    : Constraint(build_var_ids(x, q, r))
    , x_id_(x->id())
    , q_id_(q->id())
    , r_id_(r->id())
    , c_(divisor) {}

std::string IntDivModChannelConstraint::name() const {
    return "int_divmod_channel";
}

bool IntDivModChannelConstraint::propagate_bounds(Model& model) {
    if (c_ <= 0) return false;

    auto x_min = model.var_min(x_id_);
    auto x_max = model.var_max(x_id_);

    // r は必ず [0, c-1]
    auto r_min = std::max<Domain::value_type>(0, model.var_min(r_id_));
    auto r_max = std::min<Domain::value_type>(c_ - 1, model.var_max(r_id_));
    if (r_min > r_max) return false;

    auto q_min = model.var_min(q_id_);
    auto q_max = model.var_max(q_id_);

    // q ← x（x >= 0 前提なので truncated division = floor division）
    if (x_min < 0) return false;
    q_min = std::max(q_min, x_min / c_);
    q_max = std::min(q_max, x_max / c_);
    if (q_min > q_max) return false;

    // x ← q, r
    auto x_lo = std::max(x_min, c_ * q_min + r_min);
    auto x_hi = std::min(x_max, c_ * q_max + r_max);
    if (x_lo > x_hi) return false;

    // q が確定していれば r は x の残差そのもの
    if (q_min == q_max) {
        r_min = std::max(r_min, x_lo - c_ * q_min);
        r_max = std::min(r_max, x_hi - c_ * q_min);
        if (r_min > r_max) return false;
    }

    model.enqueue_set_min(r_id_, r_min);
    model.enqueue_set_max(r_id_, r_max);
    model.enqueue_set_min(q_id_, q_min);
    model.enqueue_set_max(q_id_, q_max);
    model.enqueue_set_min(x_id_, x_lo);
    model.enqueue_set_max(x_id_, x_hi);

    return true;
}

bool IntDivModChannelConstraint::propagate_domain(Model& model) {
    const auto& x_dom = model.variable(x_id_)->domain();
    if (x_dom.size() > kScanLimit) return true;

    auto q_lo = model.var_min(q_id_);
    auto q_hi = model.var_max(q_id_);
    if (q_lo > q_hi) return false;
    const auto q_span = static_cast<size_t>(q_hi - q_lo) + 1;
    if (q_span > kScanLimit || static_cast<size_t>(c_) > kScanLimit) return true;

    // D(x) を走査し、q/r 側の支持を集めつつ支持のない x の値を落とす。
    // enqueue_* は遅延適用なので、走査中にドメインが変わることはない。
    std::vector<char> q_sup(q_span, 0);
    std::vector<char> r_sup(static_cast<size_t>(c_), 0);

    for (auto it = x_dom.begin(); it != x_dom.end(); ++it) {
        auto v = *it;
        if (v < 0) {  // x >= 0 の前提が崩れている
            model.enqueue_remove_value(x_id_, v);
            continue;
        }
        auto qq = v / c_;
        auto rr = v % c_;
        if (qq < q_lo || qq > q_hi ||
            !model.contains(q_id_, qq) || !model.contains(r_id_, rr)) {
            model.enqueue_remove_value(x_id_, v);
            continue;
        }
        q_sup[static_cast<size_t>(qq - q_lo)] = 1;
        r_sup[static_cast<size_t>(rr)] = 1;
    }

    // 支持のない q / r を落とす
    const auto& q_dom = model.variable(q_id_)->domain();
    for (auto it = q_dom.begin(); it != q_dom.end(); ++it) {
        auto qq = *it;
        if (qq < q_lo || qq > q_hi || !q_sup[static_cast<size_t>(qq - q_lo)]) {
            model.enqueue_remove_value(q_id_, qq);
        }
    }
    const auto& r_dom = model.variable(r_id_)->domain();
    for (auto it = r_dom.begin(); it != r_dom.end(); ++it) {
        auto rr = *it;
        if (rr < 0 || rr >= c_ || !r_sup[static_cast<size_t>(rr)]) {
            model.enqueue_remove_value(r_id_, rr);
        }
    }

    return true;
}

bool IntDivModChannelConstraint::propagate(Model& model) {
    if (!propagate_bounds(model)) return false;
    return propagate_domain(model);
}

PresolveResult IntDivModChannelConstraint::presolve(Model& model) {
    if (c_ <= 0) return PresolveResult::Contradiction;

    bool changed = false;
    auto* x_var = model.variable(x_id_);
    auto* q_var = model.variable(q_id_);
    auto* r_var = model.variable(r_id_);

    if (x_var->min() < 0) return PresolveResult::Contradiction;

    // r ∈ [0, c-1]
    if (r_var->min() < 0) {
        if (!r_var->remove_below(0)) return PresolveResult::Contradiction;
        changed = true;
    }
    if (r_var->max() > c_ - 1) {
        if (!r_var->remove_above(c_ - 1)) return PresolveResult::Contradiction;
        changed = true;
    }

    // q ← x
    auto q_lo = x_var->min() / c_;
    auto q_hi = x_var->max() / c_;
    if (q_var->min() < q_lo) {
        if (!q_var->remove_below(q_lo)) return PresolveResult::Contradiction;
        changed = true;
    }
    if (q_var->max() > q_hi) {
        if (!q_var->remove_above(q_hi)) return PresolveResult::Contradiction;
        changed = true;
    }

    // x ← q, r
    auto x_lo = c_ * q_var->min() + r_var->min();
    auto x_hi = c_ * q_var->max() + r_var->max();
    if (x_var->min() < x_lo) {
        if (!x_var->remove_below(x_lo)) return PresolveResult::Contradiction;
        changed = true;
    }
    if (x_var->max() > x_hi) {
        if (!x_var->remove_above(x_hi)) return PresolveResult::Contradiction;
        changed = true;
    }

    // 値走査によるドメイン一貫フィルタ（presolve では直接ドメインを操作する）
    if (x_var->domain().size() <= kScanLimit && static_cast<size_t>(c_) <= kScanLimit) {
        std::vector<Domain::value_type> drop;
        for (auto it = x_var->domain().begin(); it != x_var->domain().end(); ++it) {
            auto v = *it;
            if (!q_var->domain().contains(v / c_) || !r_var->domain().contains(v % c_)) {
                drop.push_back(v);
            }
        }
        for (auto v : drop) {
            if (!x_var->remove(v)) return PresolveResult::Contradiction;
            changed = true;
        }
        if (x_var->domain().size() == 0) return PresolveResult::Contradiction;

        // 支持のない q / r を落とす
        std::vector<char> q_sup(static_cast<size_t>(q_var->max() - q_var->min()) + 1, 0);
        std::vector<char> r_sup(static_cast<size_t>(c_), 0);
        auto q_base = q_var->min();
        for (auto it = x_var->domain().begin(); it != x_var->domain().end(); ++it) {
            auto qq = *it / c_;
            if (qq >= q_base && qq <= q_var->max()) {
                q_sup[static_cast<size_t>(qq - q_base)] = 1;
            }
            r_sup[static_cast<size_t>(*it % c_)] = 1;
        }
        drop.clear();
        for (auto it = q_var->domain().begin(); it != q_var->domain().end(); ++it) {
            auto qq = *it;
            if (qq < q_base || qq > q_var->max() || !q_sup[static_cast<size_t>(qq - q_base)]) {
                drop.push_back(qq);
            }
        }
        for (auto v : drop) {
            if (!q_var->remove(v)) return PresolveResult::Contradiction;
            changed = true;
        }
        drop.clear();
        for (auto it = r_var->domain().begin(); it != r_var->domain().end(); ++it) {
            auto rr = *it;
            if (rr < 0 || rr >= c_ || !r_sup[static_cast<size_t>(rr)]) {
                drop.push_back(rr);
            }
        }
        for (auto v : drop) {
            if (!r_var->remove(v)) return PresolveResult::Contradiction;
            changed = true;
        }
        if (q_var->domain().size() == 0 || r_var->domain().size() == 0) {
            return PresolveResult::Contradiction;
        }
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool IntDivModChannelConstraint::prepare_propagation(Model& /*model*/) {
    init_watches();
    return true;
}

bool IntDivModChannelConstraint::on_instantiate(Model& model, int save_point,
                                                 size_t internal_var_idx,
                                                 Domain::value_type value,
                                                 Domain::value_type prev_min,
                                                 Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, internal_var_idx, value,
                                    prev_min, prev_max)) {
        return false;
    }

    const size_t var_idx = var_id(internal_var_idx);

    // x 確定 → q, r は一意に決まる
    if (var_idx == x_id_) {
        if (value < 0) return false;
        auto qq = value / c_;
        auto rr = value % c_;
        if (!model.contains(q_id_, qq) || !model.contains(r_id_, rr)) return false;
        if (model.is_instantiated(q_id_)) {
            if (model.value(q_id_) != qq) return false;
        } else {
            model.enqueue_instantiate(q_id_, qq);
        }
        if (model.is_instantiated(r_id_)) {
            if (model.value(r_id_) != rr) return false;
        } else {
            model.enqueue_instantiate(r_id_, rr);
        }
        return true;
    }

    // q, r ともに確定 → x も一意
    if (model.is_instantiated(q_id_) && model.is_instantiated(r_id_)) {
        auto xv = c_ * model.value(q_id_) + model.value(r_id_);
        if (!model.contains(x_id_, xv)) return false;
        if (model.is_instantiated(x_id_)) {
            if (model.value(x_id_) != xv) return false;
        } else {
            model.enqueue_instantiate(x_id_, xv);
        }
        return true;
    }

    return propagate(model);
}

// 境界変更では穴が増えないので bounds のみ。値走査は穴が生じる
// on_remove_value / on_instantiate に限定する（走査は O(|D(x)|) で、
// 全イベントで回すとカスケード中に O(|D(x)|^2) になる）。
bool IntDivModChannelConstraint::on_set_min(Model& model, int /*save_point*/,
                                             size_t /*internal_var_idx*/,
                                             Domain::value_type /*new_min*/,
                                             Domain::value_type /*old_min*/) {
    return sweep_on_bounds_ ? propagate(model) : propagate_bounds(model);
}

bool IntDivModChannelConstraint::on_set_max(Model& model, int /*save_point*/,
                                             size_t /*internal_var_idx*/,
                                             Domain::value_type /*new_max*/,
                                             Domain::value_type /*old_max*/) {
    return sweep_on_bounds_ ? propagate(model) : propagate_bounds(model);
}

bool IntDivModChannelConstraint::on_remove_value(Model& model, int /*save_point*/,
                                                  size_t /*internal_var_idx*/,
                                                  Domain::value_type /*removed_value*/) {
    return propagate(model);
}

std::optional<bool> IntDivModChannelConstraint::is_satisfied(const Model& model) const {
    if (!model.is_instantiated(x_id_) || !model.is_instantiated(q_id_) ||
        !model.is_instantiated(r_id_)) {
        return std::nullopt;
    }
    auto xv = model.value(x_id_);
    if (xv < 0 || c_ <= 0) return false;
    return xv / c_ == model.value(q_id_) && xv % c_ == model.value(r_id_);
}

bool IntDivModChannelConstraint::on_final_instantiate(const Model& model) {
    auto xv = model.value(x_id_);
    if (xv < 0 || c_ <= 0) return false;
    return xv / c_ == model.value(q_id_) && xv % c_ == model.value(r_id_);
}

} // namespace sabori_csp
