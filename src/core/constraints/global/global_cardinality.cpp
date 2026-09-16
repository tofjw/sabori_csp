#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <unordered_map>
#include <vector>

namespace sabori_csp {

// ============================================================================
// GlobalCardinalityConstraint
//
// global_cardinality(x, cover, counts):
//   counts[j] = |{i : x[i] == cover[j]}| （open 意味論: x は cover 外の値も可）。
//   var_ids_ = [counts[0..V-1], x[0..M-1]]。
//
// ステートレスな全再計算 propagator（bin_packing_load と同方式）。
//   Step 0: 各 distinct cover 値の definite/possible 出現数を集計
//   Step 1: counts[j] の bounds を [definite, definite+possible] に絞る
//   Step 1.5: cover が distinct なら Σcounts と cover 値を取り得る変数数の
//             突き合わせ（sum reasoning）
//   Step 2: counts から x へ逆伝播（exclude: 上限到達で値除去 /
//           include: 下限が全 possible を要求したら値を強制）
// ============================================================================

GlobalCardinalityConstraint::GlobalCardinalityConstraint(
    std::vector<VariablePtr> xs,
    std::vector<int64_t> cover,
    std::vector<VariablePtr> counts)
    : Constraint()
    , n_count_(counts.size())
    , n_x_(xs.size())
    , cover_(std::move(cover)) {
    // distinct 値インデックスの構築（エントリ順を保存し重複はまとめる）
    std::unordered_map<int64_t, size_t> value_index;
    for (size_t j = 0; j < n_count_; ++j) {
        auto it = value_index.find(cover_[j]);
        if (it == value_index.end()) {
            value_index.emplace(cover_[j], distinct_values_.size());
            distinct_values_.push_back(cover_[j]);
            entries_of_value_.push_back({j});
        } else {
            entries_of_value_[it->second].push_back(j);
        }
    }
    cover_distinct_ = (distinct_values_.size() == n_count_);

    std::vector<VariablePtr> all_vars;
    all_vars.reserve(n_count_ + n_x_);
    for (auto& v : counts) all_vars.push_back(std::move(v));
    for (auto& v : xs)     all_vars.push_back(std::move(v));
    var_ids_ = extract_var_ids(all_vars);
}

std::string GlobalCardinalityConstraint::name() const {
    return "sabori_global_cardinality";
}

bool GlobalCardinalityConstraint::propagate_impl(Model& model, bool direct, bool& changed) {
    const size_t V = n_count_;
    const size_t M = n_x_;
    const size_t K = distinct_values_.size();
    if (V == 0) return true;

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
    auto vsize = [&](size_t id) -> size_t {
        return direct ? model.variable(id)->domain().size() : model.var_size(id);
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

    // --- Step 0: distinct cover 値ごとの definite/possible 出現数を集計 ---
    // occ_def[k]  : x[i] が確定して distinct_values_[k] を取る数
    // occ_pos[k]  : 未確定で distinct_values_[k] を domain に含む x[i] の数
    // assigned_in : cover 内の値に確定した x の数
    // can_in      : 未確定で cover 値を1つ以上 domain に含む x の数
    // must_in     : 未確定で domain ⊆ cover の x の数（cover 値を取らざるを得ない）
    std::vector<int64_t> occ_def(K, 0), occ_pos(K, 0);
    int64_t assigned_in = 0, can_in = 0, must_in = 0;
    for (size_t i = 0; i < M; ++i) {
        size_t xid = var_ids_[V + i];
        if (vassigned(xid)) {
            int64_t v = vmin(xid);
            for (size_t k = 0; k < K; ++k) {
                if (distinct_values_[k] == v) {
                    ++occ_def[k];
                    ++assigned_in;
                    break;
                }
            }
        } else {
            int64_t lo = vmin(xid), hi = vmax(xid);
            size_t hits = 0;
            for (size_t k = 0; k < K; ++k) {
                int64_t v = distinct_values_[k];
                if (v < lo || v > hi) continue;
                if (vcontains(xid, v)) {
                    ++occ_pos[k];
                    ++hits;
                }
            }
            if (hits > 0) ++can_in;
            if (hits == vsize(xid)) ++must_in;
        }
    }

    // --- Step 1: counts[j] の bounds を [occ_def, occ_def+occ_pos] に絞る ---
    std::vector<int64_t> cmin(V), cmax(V);
    for (size_t k = 0; k < K; ++k) {
        int64_t lb = occ_def[k];
        int64_t ub = occ_def[k] + occ_pos[k];
        for (size_t j : entries_of_value_[k]) {
            size_t cid = var_ids_[j];
            int64_t cur_min = vmin(cid), cur_max = vmax(cid);
            if (cur_min > ub || cur_max < lb) return false;
            if (lb > cur_min) { if (!set_min(cid, lb)) return false; changed = true; cur_min = lb; }
            if (ub < cur_max) { if (!set_max(cid, ub)) return false; changed = true; cur_max = ub; }
            cmin[j] = cur_min;
            cmax[j] = cur_max;
        }
    }

    // --- Step 1.5: sum reasoning（cover が distinct のときのみ健全）---
    // cover 内の値を取る x の総数 T は total_min <= T <= total_max。
    // cover が distinct なら T = Σ counts なので相互に絞れる。
    if (cover_distinct_) {
        int64_t total_min = assigned_in + must_in;
        int64_t total_max = assigned_in + can_in;
        int64_t sum_cmin = 0, sum_cmax = 0;
        for (size_t j = 0; j < V; ++j) { sum_cmin += cmin[j]; sum_cmax += cmax[j]; }
        if (sum_cmin > total_max || sum_cmax < total_min) return false;
        for (size_t j = 0; j < V; ++j) {
            size_t cid = var_ids_[j];
            int64_t new_max = total_max - (sum_cmin - cmin[j]);
            if (new_max < cmax[j]) {
                if (new_max < cmin[j]) return false;
                if (!set_max(cid, new_max)) return false;
                changed = true;
                cmax[j] = new_max;
            }
            int64_t new_min = total_min - (sum_cmax - cmax[j]);
            if (new_min > cmin[j]) {
                if (new_min > cmax[j]) return false;
                if (!set_min(cid, new_min)) return false;
                changed = true;
                cmin[j] = new_min;
            }
        }
    }

    // --- Step 2: counts から x へ逆伝播（exclude / include）---
    for (size_t k = 0; k < K; ++k) {
        if (occ_pos[k] == 0) continue;
        int64_t v = distinct_values_[k];

        // 同値エントリ群の実効 bounds（重複 cover は全エントリが同じ出現数を拘束）
        int64_t eff_min = cmin[entries_of_value_[k][0]];
        int64_t eff_max = cmax[entries_of_value_[k][0]];
        for (size_t j : entries_of_value_[k]) {
            eff_min = std::max(eff_min, cmin[j]);
            eff_max = std::min(eff_max, cmax[j]);
        }
        if (eff_min > eff_max) return false;

        // exclude: もう v を取れる余地がない → 未確定の x から v を除去
        if (eff_max == occ_def[k]) {
            for (size_t i = 0; i < M; ++i) {
                size_t xid = var_ids_[V + i];
                if (vassigned(xid) || !vcontains(xid, v)) continue;
                if (!rem_val(xid, v)) return false;
                changed = true;
            }
        }
        // include: possible 全員が v を取らないと下限に届かない → v を強制
        else if (eff_min == occ_def[k] + occ_pos[k]) {
            for (size_t i = 0; i < M; ++i) {
                size_t xid = var_ids_[V + i];
                if (vassigned(xid) || !vcontains(xid, v)) continue;
                if (!assign_val(xid, v)) return false;
                changed = true;
            }
        }
    }

    return true;
}

PresolveResult GlobalCardinalityConstraint::presolve(Model& model) {
    bool changed = false;
    if (!propagate_impl(model, /*direct=*/true, changed)) {
        return PresolveResult::Contradiction;
    }
    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool GlobalCardinalityConstraint::prepare_propagation(Model& model) {
    init_watches();
    return true;
}

bool GlobalCardinalityConstraint::on_instantiate(
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

bool GlobalCardinalityConstraint::on_set_min(
    Model& model, int /*save_point*/, size_t /*internal_var_idx*/,
    Domain::value_type /*new_min*/, Domain::value_type /*old_min*/) {
    model.schedule_constraint_batch(model_index());
    return true;
}

bool GlobalCardinalityConstraint::on_set_max(
    Model& model, int /*save_point*/, size_t /*internal_var_idx*/,
    Domain::value_type /*new_max*/, Domain::value_type /*old_max*/) {
    model.schedule_constraint_batch(model_index());
    return true;
}

bool GlobalCardinalityConstraint::on_remove_value(
    Model& model, int /*save_point*/, size_t /*internal_var_idx*/,
    Domain::value_type /*removed_value*/) {
    model.schedule_constraint_batch(model_index());
    return true;
}

bool GlobalCardinalityConstraint::propagate_batch(Model& model, int /*save_point*/) {
    bool changed = false;
    return propagate_impl(model, /*direct=*/false, changed);
}

bool GlobalCardinalityConstraint::on_final_instantiate(const Model& model) {
    const size_t K = distinct_values_.size();
    std::vector<int64_t> occ(K, 0);
    for (size_t i = 0; i < n_x_; ++i) {
        int64_t v = model.value(var_ids_[n_count_ + i]);
        for (size_t k = 0; k < K; ++k) {
            if (distinct_values_[k] == v) {
                ++occ[k];
                break;
            }
        }
    }
    for (size_t k = 0; k < K; ++k) {
        for (size_t j : entries_of_value_[k]) {
            if (static_cast<int64_t>(model.value(var_ids_[j])) != occ[k]) return false;
        }
    }
    return true;
}

void GlobalCardinalityConstraint::rewind_to(int /*save_point*/) {
    // ステートレス — 復元不要
}

}  // namespace sabori_csp
