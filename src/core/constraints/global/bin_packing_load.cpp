#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <vector>

namespace sabori_csp {

// ============================================================================
// BinPackingLoadConstraint
//
// bin_packing_load(load, bin, w):
//   各 item i は重み w[i]（定数）を持ち、bin[i]（変数）が示すビンに入る。
//   各ビン b について load[b] = sum(i : bin[i] == b) w[i] を保証する。
//   bin 値 v は load 内部 index v - index_offset_ に対応（既定 offset=0=0-based、
//   FlatZinc の 1-indexed では offset=1）。有効ビン値は [offset, offset + B - 1]。
//   var_ids_ = [load[0..B-1], bin[0..M-1]]。
//
// ステートレスな全再計算 propagator（cumulative と同方式）。
// ============================================================================

BinPackingLoadConstraint::BinPackingLoadConstraint(
    std::vector<VariablePtr> loads,
    std::vector<VariablePtr> bins,
    std::vector<int64_t> weights,
    int64_t index_offset)
    : Constraint()
    , n_load_(loads.size())
    , n_bin_(bins.size())
    , w_(std::move(weights))
    , index_offset_(index_offset) {
    std::vector<VariablePtr> all_vars;
    all_vars.reserve(n_load_ + n_bin_);
    for (auto& v : loads) all_vars.push_back(std::move(v));
    for (auto& v : bins)  all_vars.push_back(std::move(v));
    var_ids_ = extract_var_ids(all_vars);
}

std::string BinPackingLoadConstraint::name() const {
    return "sabori_bin_packing_load";
}

bool BinPackingLoadConstraint::propagate_impl(Model& model, bool direct, bool& changed) {
    const size_t B = n_load_;
    const size_t M = n_bin_;
    if (B == 0) {
        // ビンが無い: item が存在すれば充足不能
        return M == 0;
    }

    // direct(presolve) は Variable を直接操作、search は enqueue を使う
    auto vmin = [&](size_t id) -> int64_t {
        return direct ? static_cast<int64_t>(model.variable(id)->min())
                      : static_cast<int64_t>(model.var_min(id));
    };
    auto vmax = [&](size_t id) -> int64_t {
        return direct ? static_cast<int64_t>(model.variable(id)->max())
                      : static_cast<int64_t>(model.var_max(id));
    };
    auto vassigned = [&](size_t id) -> bool {
        return direct ? model.variable(id)->is_assigned() : model.is_instantiated(id);
    };
    auto vcontains = [&](size_t id, int64_t v) -> bool {
        return direct ? model.variable(id)->domain().contains(static_cast<Domain::value_type>(v))
                      : model.contains(id, static_cast<Domain::value_type>(v));
    };
    auto set_min = [&](size_t id, int64_t v) -> bool {
        if (direct) return model.variable(id)->remove_below(static_cast<Domain::value_type>(v));
        model.enqueue_set_min(id, static_cast<Domain::value_type>(v));
        return true;
    };
    auto set_max = [&](size_t id, int64_t v) -> bool {
        if (direct) return model.variable(id)->remove_above(static_cast<Domain::value_type>(v));
        model.enqueue_set_max(id, static_cast<Domain::value_type>(v));
        return true;
    };
    auto rem_val = [&](size_t id, int64_t v) -> bool {
        if (direct) return model.variable(id)->remove(static_cast<Domain::value_type>(v));
        model.enqueue_remove_value(id, static_cast<Domain::value_type>(v));
        return true;
    };
    auto assign_val = [&](size_t id, int64_t v) -> bool {
        if (direct) return model.variable(id)->assign(static_cast<Domain::value_type>(v));
        model.enqueue_instantiate(id, static_cast<Domain::value_type>(v));
        return true;
    };

    // 有効ビン値の範囲 [off, off + B - 1]。bin 値 v → load index v - off。
    const int64_t off = index_offset_;
    const int64_t bin_lo = off;
    const int64_t bin_hi = off + static_cast<int64_t>(B) - 1;

    // --- Step 0: bin[i] ∈ [bin_lo, bin_hi] を強制し、ビンごとの重み集計を作る ---
    // req[b]     : bin[i] が確定して b に入る item の重み合計（下限寄与）
    // pos_add[b] : b に入り得る未確定 item の max(w,0) 合計（上限寄与）
    // neg_sum[b] : b に入り得る未確定 item の min(w,0) 合計（下限寄与、負重み対応）
    std::vector<int64_t> req(B, 0), pos_add(B, 0), neg_sum(B, 0);
    for (size_t i = 0; i < M; ++i) {
        size_t bid = var_ids_[B + i];
        int64_t w = w_[i];

        if (vmin(bid) < bin_lo) {
            if (!set_min(bid, bin_lo)) return false;
            changed = true;
        }
        if (vmax(bid) > bin_hi) {
            if (!set_max(bid, bin_hi)) return false;
            changed = true;
        }

        if (vassigned(bid)) {
            int64_t v = vmin(bid);
            if (v < bin_lo || v > bin_hi) return false;
            req[v - off] += w;
        } else {
            int64_t lo = std::max<int64_t>(bin_lo, vmin(bid));
            int64_t hi = std::min<int64_t>(bin_hi, vmax(bid));
            for (int64_t v = lo; v <= hi; ++v) {
                if (vcontains(bid, v)) {
                    pos_add[v - off] += std::max<int64_t>(w, 0);
                    neg_sum[v - off] += std::min<int64_t>(w, 0);
                }
            }
        }
    }

    // --- Step 1: load[b] の bounds を [req+neg_sum, req+pos_add] に絞る ---
    std::vector<int64_t> load_lo(B), load_hi(B);
    for (size_t b = 0; b < B; ++b) {
        size_t lid = var_ids_[b];
        int64_t lmin = req[b] + neg_sum[b];
        int64_t lmax = req[b] + pos_add[b];
        int64_t cur_min = vmin(lid), cur_max = vmax(lid);

        if (cur_min > lmax || cur_max < lmin) return false;
        if (lmin > cur_min) { if (!set_min(lid, lmin)) return false; changed = true; cur_min = lmin; }
        if (lmax < cur_max) { if (!set_max(lid, lmax)) return false; changed = true; cur_max = lmax; }

        load_lo[b] = cur_min;
        load_hi[b] = cur_max;
    }

    // --- Step 2: load 制約から bin へ逆伝播（include / exclude）---
    for (size_t i = 0; i < M; ++i) {
        size_t bid = var_ids_[B + i];
        if (vassigned(bid)) continue;
        int64_t w = w_[i];
        int64_t lo = std::max<int64_t>(bin_lo, vmin(bid));
        int64_t hi = std::min<int64_t>(bin_hi, vmax(bid));

        for (int64_t v = lo; v <= hi; ++v) {
            if (!vcontains(bid, v)) continue;
            size_t b = static_cast<size_t>(v - off);

            // exclude: item i を b に入れると load[b] が最小でも上限を超える
            int64_t min_with_i = req[b] + w + (neg_sum[b] - std::min<int64_t>(w, 0));
            if (min_with_i > load_hi[b]) {
                if (!rem_val(bid, v)) return false;
                changed = true;
                continue;
            }

            // include: item i を b に入れないと load[b] が最大でも下限に届かない
            int64_t max_without_i = req[b] + (pos_add[b] - std::max<int64_t>(w, 0));
            if (max_without_i < load_lo[b]) {
                if (!assign_val(bid, v)) return false;
                changed = true;
                break;  // item i は確定
            }
        }
    }

    return true;
}

PresolveResult BinPackingLoadConstraint::presolve(Model& model) {
    bool changed = false;
    if (!propagate_impl(model, /*direct=*/true, changed)) {
        return PresolveResult::Contradiction;
    }
    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool BinPackingLoadConstraint::prepare_propagation(Model& model) {
    init_watches();
    return true;
}

bool BinPackingLoadConstraint::on_instantiate(
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

bool BinPackingLoadConstraint::on_set_min(
    Model& model, int /*save_point*/, size_t /*internal_var_idx*/,
    Domain::value_type /*new_min*/, Domain::value_type /*old_min*/) {
    model.schedule_constraint_batch(model_index());
    return true;
}

bool BinPackingLoadConstraint::on_set_max(
    Model& model, int /*save_point*/, size_t /*internal_var_idx*/,
    Domain::value_type /*new_max*/, Domain::value_type /*old_max*/) {
    model.schedule_constraint_batch(model_index());
    return true;
}

bool BinPackingLoadConstraint::on_remove_value(
    Model& model, int /*save_point*/, size_t /*internal_var_idx*/,
    Domain::value_type /*removed_value*/) {
    model.schedule_constraint_batch(model_index());
    return true;
}

bool BinPackingLoadConstraint::propagate_batch(Model& model, int /*save_point*/) {
    bool changed = false;
    return propagate_impl(model, /*direct=*/false, changed);
}

bool BinPackingLoadConstraint::on_final_instantiate(const Model& model) {
    std::vector<int64_t> actual(n_load_, 0);
    const int64_t off = index_offset_;
    for (size_t i = 0; i < n_bin_; ++i) {
        int64_t v = model.value(var_ids_[n_load_ + i]);
        if (v < off || v >= off + static_cast<int64_t>(n_load_)) return false;
        actual[v - off] += w_[i];
    }
    for (size_t b = 0; b < n_load_; ++b) {
        if (static_cast<int64_t>(model.value(var_ids_[b])) != actual[b]) return false;
    }
    return true;
}

void BinPackingLoadConstraint::rewind_to(int /*save_point*/) {
    // ステートレス — 復元不要
}

}  // namespace sabori_csp
