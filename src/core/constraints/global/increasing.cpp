#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <limits>

namespace sabori_csp {

// ============================================================================
// IncreasingConstraint implementation
// ============================================================================
//
// x[0] <= x[1] <= ... <= x[n-1] （strict なら <）
//
// 状態なし。
//   - 前方スイープ: x[i].min >= x[i-1].min (+1 if strict)
//   - 後方スイープ: x[i].max <= x[i+1].max (-1 if strict)
// イベントハンドラでは隣接2変数の局所伝搬のみ行い、process_queue を介して
// 連鎖的に伝播させる。
// ============================================================================

IncreasingConstraint::IncreasingConstraint(std::vector<VariablePtr> vars, bool strict)
    : Constraint(extract_var_ids(vars))
    , n_(vars.size())
    , strict_(strict) {}

std::string IncreasingConstraint::name() const {
    return strict_ ? "strictly_increasing" : "increasing";
}

bool IncreasingConstraint::sweep(Model& model) {
    if (n_ <= 1) return true;
    Domain::value_type offset = strict_ ? 1 : 0;

    // 前方: x[i].min を x[i-1].min + offset 以上に引き上げる
    Domain::value_type prev_min = model.var_min(var_ids_[0]);
    for (size_t i = 1; i < n_; ++i) {
        Domain::value_type lo = prev_min + offset;
        auto cur_min = model.var_min(var_ids_[i]);
        if (lo > model.var_max(var_ids_[i])) return false;
        if (lo > cur_min) {
            model.enqueue_set_min(var_ids_[i], lo);
            prev_min = lo;
        } else {
            prev_min = cur_min;
        }
    }

    // 後方: x[i].max を x[i+1].max - offset 以下に引き下げる
    Domain::value_type next_max = model.var_max(var_ids_[n_ - 1]);
    for (size_t i = n_ - 1; i > 0; --i) {
        Domain::value_type hi = next_max - offset;
        size_t prev = i - 1;
        auto cur_max = model.var_max(var_ids_[prev]);
        if (hi < model.var_min(var_ids_[prev])) return false;
        if (hi < cur_max) {
            model.enqueue_set_max(var_ids_[prev], hi);
            next_max = hi;
        } else {
            next_max = cur_max;
        }
    }

    return true;
}

PresolveResult IncreasingConstraint::presolve(Model& model) {
    if (n_ <= 1) return PresolveResult::Unchanged;
    Domain::value_type offset = strict_ ? 1 : 0;
    bool changed = false;

    // 前方
    for (size_t i = 1; i < n_; ++i) {
        auto prev = model.variable(var_ids_[i - 1]);
        auto cur = model.variable(var_ids_[i]);
        Domain::value_type lo = prev->min() + offset;
        if (lo > cur->max()) return PresolveResult::Contradiction;
        if (lo > cur->min()) {
            if (!cur->remove_below(lo)) return PresolveResult::Contradiction;
            changed = true;
        }
    }
    // 後方
    for (size_t i = n_ - 1; i > 0; --i) {
        auto next = model.variable(var_ids_[i]);
        auto cur = model.variable(var_ids_[i - 1]);
        Domain::value_type hi = next->max() - offset;
        if (hi < cur->min()) return PresolveResult::Contradiction;
        if (hi < cur->max()) {
            if (!cur->remove_above(hi)) return PresolveResult::Contradiction;
            changed = true;
        }
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool IncreasingConstraint::prepare_propagation(Model& model) {
    init_watches();
    return sweep(model);
}

bool IncreasingConstraint::on_instantiate(Model& model, int save_point,
                                            size_t internal_var_idx,
                                            Domain::value_type value,
                                            Domain::value_type prev_min,
                                            Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, internal_var_idx,
                                     value, prev_min, prev_max)) {
        return false;
    }
    Domain::value_type offset = strict_ ? 1 : 0;
    size_t i = internal_var_idx;

    // 右隣: x[i+1].min >= value + offset
    if (i + 1 < n_) {
        auto rmin = value + offset;
        if (rmin > model.var_max(var_ids_[i + 1])) return false;
        model.enqueue_set_min(var_ids_[i + 1], rmin);
    }
    // 左隣: x[i-1].max <= value - offset
    if (i > 0) {
        auto lmax = value - offset;
        if (lmax < model.var_min(var_ids_[i - 1])) return false;
        model.enqueue_set_max(var_ids_[i - 1], lmax);
    }
    return true;
}

bool IncreasingConstraint::on_final_instantiate(const Model& model) {
    if (n_ <= 1) return true;
    Domain::value_type offset = strict_ ? 1 : 0;
    for (size_t i = 1; i < n_; ++i) {
        if (model.value(var_ids_[i - 1]) + offset > model.value(var_ids_[i])) {
            return false;
        }
    }
    return true;
}

bool IncreasingConstraint::on_set_min(Model& model, int /*save_point*/,
                                        size_t internal_var_idx,
                                        Domain::value_type new_min,
                                        Domain::value_type /*old_min*/) {
    Domain::value_type offset = strict_ ? 1 : 0;
    size_t i = internal_var_idx;
    if (i + 1 < n_) {
        auto rmin = new_min + offset;
        if (rmin > model.var_max(var_ids_[i + 1])) return false;
        model.enqueue_set_min(var_ids_[i + 1], rmin);
    }
    return true;
}

bool IncreasingConstraint::on_set_max(Model& model, int /*save_point*/,
                                        size_t internal_var_idx,
                                        Domain::value_type new_max,
                                        Domain::value_type /*old_max*/) {
    Domain::value_type offset = strict_ ? 1 : 0;
    size_t i = internal_var_idx;
    if (i > 0) {
        auto lmax = new_max - offset;
        if (lmax < model.var_min(var_ids_[i - 1])) return false;
        model.enqueue_set_max(var_ids_[i - 1], lmax);
    }
    return true;
}

void IncreasingConstraint::init_activity(const Model& model, double* activity) const {
    if (n_ <= 2)
        return;

    if (strict_)
        return;

    // 他の探索への影響がでないように小さな値
    constexpr double base = 1e-6;

    if (0) {
        size_t i = n_ / 2;
        size_t vid = var_ids_[i];
        if (!model.is_instantiated(vid))
            activity[vid] += base;
    }
    if (0) {
        size_t i = n_ / 2;
        size_t vid = var_ids_[i];
        if (!model.is_instantiated(vid))
            activity[vid] += base;
    }

    
    // 20260525_increase_step4b
    // 経験的な値を使用。
    // きれいに 2分探索のなる重みづけは局所解に落ちやすいようだ
#if 0
    const size_t step = 2;

    for (size_t i = 0; i < n_; i += step) {
        size_t vid = var_ids_[i];
        if (model.is_instantiated(vid))
            continue;

        activity[vid] += (i % (step * 2) == 0) ? (0.1 * base) : base;
        // activity[vid] += base;
    }
#endif
#if 1
    const size_t step = 2;
    bool weak = true;

    for (size_t i = 0; i < n_; i += step) {
        size_t vid = var_ids_[i];
        if (!model.is_instantiated(vid)) {
            activity[vid] += weak ? (0.1 * base) : base;
        }
        weak = !weak;
    }
#endif
}

}  // namespace sabori_csp
