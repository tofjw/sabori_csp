#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <limits>

namespace sabori_csp {

// ============================================================================
// SubcircuitConstraint implementation
//
// circuit の端点リンク方式を流用しつつ、自己ループ (x[i]=i) を「out ノード」
// として扱う。閉路の妥当性は circuit の size==n ではなく size==in_count_
// （非自己ループの確定エッジ数 = in ノード総数）で判定する。
// ============================================================================

SubcircuitConstraint::SubcircuitConstraint(std::vector<VariablePtr> vars)
    : Constraint(extract_var_ids(vars))
    , n_(var_ids_.size())
    , base_offset_(0)
    , partner_(n_)
    , size_(n_, 1)
    , occupier_(n_, SIZE_MAX)
    , unfixed_count_(0)
    , in_count_(0)
    , pool_n_(n_) {
    // ベースオフセット検出（FlatZinc subcircuit は通常 1-based）
    if (!vars.empty()) {
        Domain::value_type global_min = std::numeric_limits<Domain::value_type>::max();
        for (const auto& v : vars) {
            if (!v->domain().empty()) {
                global_min = std::min(global_min, v->min());
            }
        }
        if (global_min != std::numeric_limits<Domain::value_type>::max()) {
            base_offset_ = global_min;
        }
    }

    pool_.resize(n_);
    pool_idx_.resize(n_);
    for (size_t i = 0; i < n_; ++i) {
        partner_[i] = i;
        pool_[i] = static_cast<Domain::value_type>(i);
        pool_idx_[i] = i;
    }
}

std::string SubcircuitConstraint::name() const {
    return "subcircuit";
}

void SubcircuitConstraint::remove_from_pool(size_t value) {
    size_t idx = pool_idx_[value];
    if (idx >= pool_n_) return;
    size_t last_idx = pool_n_ - 1;
    size_t last_value = static_cast<size_t>(pool_[last_idx]);
    pool_[idx] = static_cast<Domain::value_type>(last_value);
    pool_[last_idx] = static_cast<Domain::value_type>(value);
    pool_idx_[last_value] = idx;
    pool_idx_[value] = last_idx;
    --pool_n_;
}

void SubcircuitConstraint::rebuild_state(Model& model) {
    for (size_t i = 0; i < n_; ++i) {
        partner_[i] = i;
        size_[i] = 1;
        occupier_[i] = SIZE_MAX;
        pool_[i] = static_cast<Domain::value_type>(i);
        pool_idx_[i] = i;
    }
    unfixed_count_ = 0;
    in_count_ = 0;
    pool_n_ = n_;
    trail_.clear();
}

bool SubcircuitConstraint::prepare_propagation(Model& model) {
    rebuild_state(model);

    for (size_t i = 0; i < n_; ++i) {
        if (!model.is_instantiated(var_ids_[i])) {
            ++unfixed_count_;
            continue;
        }
        auto val = model.value(var_ids_[i]);
        size_t j = static_cast<size_t>(val - base_offset_);
        if (j >= n_) return false;
        if (occupier_[j] != SIZE_MAX) return false;  // alldifferent 違反

        if (j == i) {
            // 自己ループ（out ノード）
            occupier_[i] = i;
            remove_from_pool(i);
            continue;
        }

        // 非自己ループ（in エッジ）
        size_t h1 = partner_[i];
        size_t t2 = partner_[j];
        if (h1 == j) {
            // 閉路形成: 全 in ノードを含まなければ矛盾
            if (size_[h1] != in_count_ + 1) return false;
        } else {
            partner_[h1] = t2;
            partner_[t2] = h1;
            size_[h1] += size_[j];
        }
        occupier_[j] = i;
        remove_from_pool(j);
        ++in_count_;
    }

    if (unfixed_count_ > pool_n_) return false;
    return true;
}

PresolveResult SubcircuitConstraint::presolve(Model& model) {
    if (n_ <= 1) return PresolveResult::Unchanged;

    // 確定済みエッジから in ノード数とパス構造を数える（内部状態には触れず model から再計算）
    // subcircuit は自己ループを許すので circuit のような一律の x[i]!=i 除去は行わない。
    std::vector<size_t> occ(n_, SIZE_MAX);
    std::vector<size_t> head(n_);   // union-find 風の端点管理
    std::vector<size_t> tail(n_);
    std::vector<size_t> psize(n_, 1);
    for (size_t i = 0; i < n_; ++i) { head[i] = i; tail[i] = i; }
    size_t committed_in = 0;
    bool changed = false;

    // まず全確定エッジを走査（順不同でも端点リンクは O(1) 更新可能）
    for (size_t i = 0; i < n_; ++i) {
        Variable* v = model.variable(var_ids_[i]);
        if (!v->is_assigned()) continue;
        size_t j = static_cast<size_t>(v->min() - base_offset_);
        if (j >= n_) return PresolveResult::Contradiction;
        if (occ[j] != SIZE_MAX) return PresolveResult::Contradiction;
        occ[j] = i;
        if (j == i) continue;  // 自己ループは in パスに寄与しない
        ++committed_in;
    }

    // AllDifferent forward checking: 確定値を未確定変数から除去
    for (size_t i = 0; i < n_; ++i) {
        if (occ[i] == SIZE_MAX) continue;  // ノード i に入るエッジ未確定
        auto used = static_cast<Domain::value_type>(i) + base_offset_;
        for (size_t k = 0; k < n_; ++k) {
            Variable* vk = model.variable(var_ids_[k]);
            if (vk->is_assigned()) continue;
            if (vk->domain().contains(used)) {
                if (!vk->remove(used)) return PresolveResult::Contradiction;
                changed = true;
            }
        }
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool SubcircuitConstraint::on_instantiate(Model& model, int save_point,
                                           size_t internal_var_idx,
                                           Domain::value_type value,
                                           Domain::value_type /*prev_min*/,
                                           Domain::value_type /*prev_max*/) {
    size_t i = internal_var_idx;
    size_t j = static_cast<size_t>(value - base_offset_);

    if (j >= n_) return false;
    // AllDifferent: ノード j に既に確定エッジがあれば重複
    if (occupier_[j] != SIZE_MAX) return false;

    // ---- 自己ループ (out ノード) ----
    if (j == i) {
        TrailEntry entry{i, j, 0, 0, 0, pool_n_, unfixed_count_, in_count_, 0};
        trail_.push_back({save_point, entry});
        model.mark_constraint_dirty(model_index(), save_point);

        occupier_[i] = i;
        remove_from_pool(i);
        --unfixed_count_;

        if (unfixed_count_ == 1) {
            size_t last_idx = find_last_uninstantiated(model);
            if (last_idx != SIZE_MAX &&
                !on_last_uninstantiated(model, save_point, last_idx)) {
                return false;
            }
        } else if (unfixed_count_ == 0) {
            return on_final_instantiate(model);
        }
        return true;
    }

    // ---- 非自己ループ (in エッジ i -> j) ----
    size_t h1 = partner_[i];  // i のパスの head（i は tail）
    size_t t2 = partner_[j];  // j のパスの tail（j は head）

    // ---- 閉路形成 ----
    if (h1 == j) {
        // 全 in ノードを含む閉路のみ妥当（size==in_count_+1）。
        // それ未満なら他のパスに in ノードが残っており部分閉路 → 矛盾。
        if (size_[h1] != in_count_ + 1) return false;

        TrailEntry entry{i, j, 0, 0, 0, pool_n_, unfixed_count_, in_count_, 2};
        trail_.push_back({save_point, entry});
        model.mark_constraint_dirty(model_index(), save_point);

        occupier_[j] = i;
        remove_from_pool(j);
        --unfixed_count_;
        ++in_count_;

        // 妥当な閉路が閉じた: 残りの未確定ノードは全て out（自己ループ）に強制。
        for (size_t k = 0; k < n_; ++k) {
            if (k == i) continue;
            size_t vid = var_ids_[k];
            if (model.is_instantiated(vid)) continue;
            model.enqueue_instantiate(vid, static_cast<Domain::value_type>(k) + base_offset_);
        }

        if (unfixed_count_ == 0) return on_final_instantiate(model);
        return true;
    }

    // ---- パス結合: h1 -> ... -> i -> j -> ... -> t2 ----
    TrailEntry entry{i, j, h1, t2, size_[h1], pool_n_, unfixed_count_, in_count_, 1};
    trail_.push_back({save_point, entry});
    model.mark_constraint_dirty(model_index(), save_point);

    partner_[h1] = t2;
    partner_[t2] = h1;
    size_[h1] += size_[j];
    occupier_[j] = i;
    remove_from_pool(j);
    --unfixed_count_;
    ++in_count_;

    // 鳩の巣（alldifferent）
    if (unfixed_count_ > pool_n_) return false;

    // 部分閉路の早期防止: このパスがまだ全 in ノードを含まないなら、
    // tail t2 が head h1 へ戻る（閉じる）値を除去する。
    // size_[h1] == in_count_ のときは全 in ノードを含むので閉じてよい。
    if (size_[h1] < in_count_) {
        model.enqueue_remove_value(var_ids_[t2],
                                   static_cast<Domain::value_type>(h1) + base_offset_);
    }

    // AllDifferent forward checking
    for (size_t k = 0; k < n_; ++k) {
        if (k == i) continue;
        size_t vid = var_ids_[k];
        if (model.is_instantiated(vid)) continue;
        if (model.contains(vid, value)) {
            model.enqueue_remove_value(vid, value);
        }
    }

    if (unfixed_count_ == 1) {
        size_t last_idx = find_last_uninstantiated(model);
        if (last_idx != SIZE_MAX &&
            !on_last_uninstantiated(model, save_point, last_idx)) {
            return false;
        }
    } else if (unfixed_count_ == 0) {
        return on_final_instantiate(model);
    }

    return true;
}

bool SubcircuitConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                                    size_t last_var_internal_idx) {
    auto last_var_id = var_ids_[last_var_internal_idx];

    if (model.is_instantiated(last_var_id)) {
        auto val = model.value(last_var_id);
        size_t j = static_cast<size_t>(val - base_offset_);
        if (j >= n_) return false;
        return pool_idx_[j] < pool_n_ && occupier_[j] == SIZE_MAX;
    }

    // 残る値が1つならそれで確定（alldifferent）
    if (pool_n_ == 1) {
        Domain::value_type remaining_value = pool_[0] + base_offset_;
        model.enqueue_instantiate(last_var_id, remaining_value);
    } else if (pool_n_ == 0) {
        return false;
    }
    return true;
}

bool SubcircuitConstraint::on_final_instantiate(const Model& model) {
    // 全変数確定。非自己ループのノードが単一閉路を成すか検証する。
    if (n_ == 0) return true;

    size_t start = SIZE_MAX;
    size_t in_nodes = 0;
    for (size_t i = 0; i < n_; ++i) {
        if (!model.is_instantiated(var_ids_[i])) return false;
        size_t nxt = static_cast<size_t>(model.value(var_ids_[i]) - base_offset_);
        if (nxt >= n_) return false;
        if (nxt != i) {
            ++in_nodes;
            if (start == SIZE_MAX) start = i;
        }
    }

    // 全て自己ループ = 空の部分閉路（妥当）
    if (start == SIZE_MAX) return true;

    // start から閉路をたどり、全 in ノードをちょうど1周で回れるか確認
    size_t cur = start;
    size_t count = 0;
    do {
        size_t nxt = static_cast<size_t>(model.value(var_ids_[cur]) - base_offset_);
        if (nxt == cur) return false;  // 閉路内に自己ループはない
        cur = nxt;
        if (++count > in_nodes) return false;
    } while (cur != start);

    return count == in_nodes;  // 単一閉路が全 in ノードを含む
}

void SubcircuitConstraint::rewind_to(int save_point) {
    while (!trail_.empty() && trail_.back().first > save_point) {
        const auto& entry = trail_.back().second;

        occupier_[entry.j] = SIZE_MAX;
        pool_n_ = entry.old_pool_n;
        unfixed_count_ = entry.old_unfixed_count;
        in_count_ = entry.old_in_count;

        if (entry.kind == 1) {
            // merge を戻す
            partner_[entry.h1] = entry.i;
            partner_[entry.t2] = entry.j;
            size_[entry.h1] = entry.old_size_h1;
        }
        // kind 0(self-loop) / 2(closure) は partner_/size_ を変更していない

        trail_.pop_back();
    }
}

void SubcircuitConstraint::bump_activity(const Model& model, size_t trigger_var_idx,
                                          double* activity, double activity_inc,
                                          bool& need_rescale, std::mt19937& rng) const {
    if (!model.is_instantiated(trigger_var_idx)) return;
    auto trigger_val = model.value(trigger_var_idx);
    const double inc = 0.5 * activity_inc;

    bump_variable_activity(activity, trigger_var_idx, inc, need_rescale, rng);

    size_t j = static_cast<size_t>(trigger_val - base_offset_);
    if (j < n_) {
        size_t occ = occupier_[j];
        if (occ != SIZE_MAX) {
            size_t vid = var_ids_[occ];
            if (vid != trigger_var_idx && model.is_instantiated(vid) &&
                model.value(vid) == trigger_val) {
                bump_variable_activity(activity, vid, inc, need_rescale, rng);
            }
        }
    }
}

}  // namespace sabori_csp
