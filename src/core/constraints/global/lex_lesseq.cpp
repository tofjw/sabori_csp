#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <vector>

namespace sabori_csp {

// ============================================================================
// LexLessEqConstraint
//
// lex_lesseq(x, y): x が y より辞書順で小さいか等しい（strict_ なら厳密に小さい）。
// 配列長は異なってよい: 等値 prefix なら短い方が辞書順で先。
// ステートレスな全再計算 batch propagator（bin_packing_load と同方式）。
//
// 走査: 先頭から「両者確定かつ等値」の間だけ進み、最初の非強制等値位置 i で
//   - x[i] ≤ y[i] の bounds を強制（prefix 等値強制下で x[i] > y[i] は違反）
//   - 最終比較位置かつ厳密性が必要（strict_ または len(x) > len(y)）なら
//     x[i] < y[i] を強制
//   - x[i] < y[i] が保証されたら constraint は充足済み（suffix 無拘束）で停止
// ============================================================================

namespace {
std::vector<VariablePtr> concat_vars(const std::vector<VariablePtr>& a,
                                     const std::vector<VariablePtr>& b) {
    std::vector<VariablePtr> all;
    all.reserve(a.size() + b.size());
    all.insert(all.end(), a.begin(), a.end());
    all.insert(all.end(), b.begin(), b.end());
    return all;
}
}  // namespace

LexLessEqConstraint::LexLessEqConstraint(std::vector<VariablePtr> xs,
                                         std::vector<VariablePtr> ys,
                                         bool strict)
    : Constraint(extract_var_ids(concat_vars(xs, ys)))
    , nx_(xs.size())
    , ny_(ys.size())
    , strict_(strict) {
}

std::string LexLessEqConstraint::name() const {
    return strict_ ? "sabori_lex_less" : "sabori_lex_lesseq";
}

bool LexLessEqConstraint::propagate_impl(Model& model, bool direct, bool& changed) {
    // direct(presolve) は Variable を直接操作、search は enqueue を使う
    auto vmin = [&](size_t id) -> Domain::value_type {
        return direct ? model.variable(id)->min() : model.var_min(id);
    };
    auto vmax = [&](size_t id) -> Domain::value_type {
        return direct ? model.variable(id)->max() : model.var_max(id);
    };
    auto set_min = [&](size_t id, Domain::value_type v) -> bool {
        if (direct) return model.variable(id)->remove_below(v);
        model.enqueue_set_min(id, v);
        return true;
    };
    auto set_max = [&](size_t id, Domain::value_type v) -> bool {
        if (direct) return model.variable(id)->remove_above(v);
        model.enqueue_set_max(id, v);
        return true;
    };

    const size_t nmin = std::min(nx_, ny_);
    // 最終比較位置で厳密性が必要か:
    // - nx > ny: prefix が全て等値だと長い方（x）が辞書順で後になり違反
    //   → lesseq でも厳密不等号が必要
    // - nx == ny かつ strict_: 全位置等値は違反
    // - nx < ny: 等値 prefix でも x は真の prefix = 厳密に小さいので不要
    const bool need_strict_tail = (nx_ > ny_) || (strict_ && nx_ == ny_);

    size_t i = 0;
    while (true) {
        if (i == nmin) {
            // 比較位置を使い切った = prefix 全域が等値強制
            if (nx_ < ny_) return true;   // x が真の prefix → 厳密に小さい
            if (nx_ > ny_) return false;  // x が長い → 辞書順で後
            return !strict_;              // 同一列: lesseq は真、less は偽
        }
        const size_t xid = var_ids_[i];
        const size_t yid = var_ids_[nx_ + i];
        Domain::value_type x_min = vmin(xid), x_max = vmax(xid);
        Domain::value_type y_min = vmin(yid), y_max = vmax(yid);
        const bool last = (i + 1 == nmin);
        const bool strict_here = last && need_strict_tail;

        // x[i] ≤ y[i]（strict_here なら x[i] < y[i]）の bounds を強制
        const Domain::value_type slack = strict_here ? 1 : 0;
        if (x_min > y_max - slack) return false;
        if (x_max > y_max - slack) {
            if (!set_max(xid, y_max - slack)) return false;
            changed = true;
            x_max = y_max - slack;
        }
        if (y_min < x_min + slack) {
            if (!set_min(yid, x_min + slack)) return false;
            changed = true;
            y_min = x_min + slack;
        }

        // x[i] < y[i] が保証された → constraint は充足済み（suffix 無拘束）
        if (x_max < y_min) return true;

        // 両者確定かつ等値なら次の位置へ（等値 prefix の延長）
        if (x_min == x_max && y_min == y_max && x_min == y_min) {
            ++i;
            continue;
        }

        // x[i] ≤ y[i] は強制済みだが等/小のどちらか未確定 → ここで停止
        return true;
    }
}

PresolveResult LexLessEqConstraint::presolve(Model& model) {
    bool changed = false;
    if (!propagate_impl(model, /*direct=*/true, changed)) {
        return PresolveResult::Contradiction;
    }
    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool LexLessEqConstraint::prepare_propagation(Model& model) {
    init_watches();
    return true;
}

bool LexLessEqConstraint::on_instantiate(
    Model& model, int save_point,
    size_t internal_var_idx, Domain::value_type value,
    Domain::value_type prev_min, Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, internal_var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }
    if (!has_uninstantiated(model)) {
        return on_final_instantiate(model);
    }
    model.schedule_constraint_batch(model_index());
    return true;
}

bool LexLessEqConstraint::on_set_min(
    Model& model, int /*save_point*/, size_t /*internal_var_idx*/,
    Domain::value_type /*new_min*/, Domain::value_type /*old_min*/) {
    model.schedule_constraint_batch(model_index());
    return true;
}

bool LexLessEqConstraint::on_set_max(
    Model& model, int /*save_point*/, size_t /*internal_var_idx*/,
    Domain::value_type /*new_max*/, Domain::value_type /*old_max*/) {
    model.schedule_constraint_batch(model_index());
    return true;
}

bool LexLessEqConstraint::on_remove_value(
    Model& model, int /*save_point*/, size_t /*internal_var_idx*/,
    Domain::value_type /*removed_value*/) {
    // bounds ベースの伝播なので内部の穴には反応しなくてよいが、
    // 等値 prefix の延長判定は確定に依存するためスケジュールしておく
    model.schedule_constraint_batch(model_index());
    return true;
}

bool LexLessEqConstraint::propagate_batch(Model& model, int /*save_point*/) {
    bool changed = false;
    return propagate_impl(model, /*direct=*/false, changed);
}

bool LexLessEqConstraint::on_final_instantiate(const Model& model) {
    const size_t nmin = std::min(nx_, ny_);
    for (size_t i = 0; i < nmin; ++i) {
        auto xv = model.value(var_ids_[i]);
        auto yv = model.value(var_ids_[nx_ + i]);
        if (xv < yv) return true;
        if (xv > yv) return false;
    }
    // 等値 prefix: 長さで決着
    if (nx_ < ny_) return true;
    if (nx_ > ny_) return false;
    return !strict_;
}

void LexLessEqConstraint::rewind_to(int /*save_point*/) {
    // ステートレス — 復元不要
}

}  // namespace sabori_csp
