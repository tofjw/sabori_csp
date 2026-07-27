#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <cstdlib>
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

bool SubcircuitConstraint::filter_reachability(Model& model, bool in_presolve, bool* changed) {
    // kill switch（退行時の切り分け用）
    static const bool disabled = std::getenv("SABORI_NO_SUBCIRCUIT_REACH") != nullptr;
    if (disabled) return true;
    if (n_ < 3) return true;

    // ------------------------------------------------------------------
    // 断片グラフによる到達可能性フィルタ
    //
    // 確定済みの非自己ループ弧は「パス断片」を成す。閉路が閉じるには、全断片が
    // 未使用ノードだけを通って1つの輪に繋がらなければならない。そこで各断片を
    // 1ノードに縮約し（head で入り tail から出る）、その縮約グラフ上で
    // 全断片が相互到達可能かを検査する。
    //
    // 縮約グラフ上で基準断片と相互到達できないノードは閉路に入りえないので、
    // 自己ループ（out）に強制できる。必須ノードがそこに落ちたら矛盾。
    //
    // 内部状態には依存せずモデルから毎回再構築する（backtrack 安全）。
    // ------------------------------------------------------------------
    const auto self_of = [&](size_t i) {
        return static_cast<Domain::value_type>(i) + base_offset_;
    };

    // --- ノード分類 ---
    // occupied[j]: 確定弧が j に入っている（自己ループ含む）→ もう入れない
    // succ[i]:     確定した非自己ループの後続（無ければ SIZE_MAX）
    frag_occupied_.assign(n_, 0);
    frag_succ_.assign(n_, SIZE_MAX);
    size_t mandatory_anchor = SIZE_MAX;
    for (size_t i = 0; i < n_; ++i) {
        size_t vid = var_ids_[i];
        if (!model.contains(vid, self_of(i)) && mandatory_anchor == SIZE_MAX) {
            mandatory_anchor = i;
        }
        if (!model.is_instantiated(vid)) continue;
        auto j = static_cast<size_t>(model.value(vid) - base_offset_);
        if (j >= n_) return false;
        if (frag_occupied_[j]) return false;  // alldifferent 違反
        frag_occupied_[j] = 1;
        if (j != i) frag_succ_[i] = j;
    }

    // --- 断片の抽出（head = 入次数0 かつ 出弧確定、tail = 入次数1 かつ 出弧未確定）---
    // 縮約ノード ID: 0..n_-1 は素のノード、n_+f は断片 f
    frag_id_.assign(n_, SIZE_MAX);   // ノード -> 所属断片
    frag_tail_.clear();
    frag_head_.clear();
    for (size_t h = 0; h < n_; ++h) {
        if (frag_occupied_[h]) continue;             // head は入次数0
        if (frag_succ_[h] == SIZE_MAX) continue;     // 出弧が確定していない
        size_t f = frag_head_.size();
        size_t cur = h, steps = 0;
        while (true) {
            frag_id_[cur] = f;
            size_t nx = frag_succ_[cur];
            if (nx == SIZE_MAX) break;               // ここが tail
            cur = nx;
            if (++steps > n_) return false;          // 確定弧だけで閉路 → 別ルートで検出済みのはず
        }
        frag_head_.push_back(h);
        frag_tail_.push_back(cur);
    }
    const size_t m = frag_head_.size();

    // --- 走査の起点を決める ---
    // 断片があればその0番、無ければ必須ノード。どちらも無ければ「全て out」が
    // 妥当解なので何も強制できない。
    size_t anchor;
    if (m > 0) {
        anchor = n_ + 0;
    } else if (mandatory_anchor != SIZE_MAX) {
        anchor = mandatory_anchor;
    } else {
        return true;
    }

    const size_t total = n_ + m;
    // 縮約ノード v の「出口となる素ノード」
    const auto exit_node = [&](size_t v) { return v < n_ ? v : frag_tail_[v - n_]; };
    // 素ノード j を縮約ノードへ写す（進入可能なら）
    const auto entry_of = [&](size_t j) {
        size_t f = frag_id_[j];
        if (f == SIZE_MAX) return j;                     // 自由ノード
        return (frag_head_[f] == j) ? n_ + f : SIZE_MAX; // 断片へは head からのみ進入可
    };

    // --- 縮約グラフを CSR で構築 ---
    succ_start_.assign(total + 1, 0);
    pred_start_.assign(total + 1, 0);
    size_t arc_count = 0;
    for (size_t v = 0; v < total; ++v) {
        size_t u = exit_node(v);
        if (v < n_ && (frag_occupied_[u] || frag_id_[u] != SIZE_MAX)) continue;  // out/断片内部は起点にしない
        const auto& dom = model.variable(var_ids_[u])->domain();
        size_t deg = 0;
        dom.for_each_value([&](Domain::value_type val) {
            auto j = static_cast<size_t>(val - base_offset_);
            if (j >= n_ || j == u) return;
            if (frag_occupied_[j]) return;               // 既に入次数が埋まっている
            size_t w = entry_of(j);
            // 縮約後の自己ループ (w == v) は「断片の tail が自分の head へ戻る」
            // = 閉路を閉じる弧。除外してはいけない。
            if (w == SIZE_MAX) return;
            ++deg;
            ++pred_start_[w + 1];
        });
        succ_start_[v + 1] = deg;
        arc_count += deg;
    }
    for (size_t v = 0; v < total; ++v) {
        succ_start_[v + 1] += succ_start_[v];
        pred_start_[v + 1] += pred_start_[v];
    }
    succ_list_.assign(arc_count, 0);
    pred_list_.assign(arc_count, 0);
    {
        std::vector<size_t> spos(succ_start_.begin(), succ_start_.end() - 1);
        std::vector<size_t> ppos(pred_start_.begin(), pred_start_.end() - 1);
        for (size_t v = 0; v < total; ++v) {
            size_t u = exit_node(v);
            if (v < n_ && (frag_occupied_[u] || frag_id_[u] != SIZE_MAX)) continue;
            const auto& dom = model.variable(var_ids_[u])->domain();
            dom.for_each_value([&](Domain::value_type val) {
                auto j = static_cast<size_t>(val - base_offset_);
                if (j >= n_ || j == u) return;
                if (frag_occupied_[j]) return;
                size_t w = entry_of(j);
                if (w == SIZE_MAX) return;
                succ_list_[spos[v]++] = w;
                pred_list_[ppos[w]++] = v;
            });
        }
    }

    // --- anchor からの前向き / 後ろ向き到達 ---
    auto traverse = [&](std::vector<uint8_t>& mark, const std::vector<size_t>& start,
                        const std::vector<size_t>& list) {
        mark.assign(total, 0);
        reach_stack_.clear();
        mark[anchor] = 1;
        reach_stack_.push_back(anchor);
        while (!reach_stack_.empty()) {
            size_t u = reach_stack_.back();
            reach_stack_.pop_back();
            for (size_t p = start[u]; p < start[u + 1]; ++p) {
                size_t w = list[p];
                if (!mark[w]) { mark[w] = 1; reach_stack_.push_back(w); }
            }
        }
    };
    traverse(reach_fwd_, succ_start_, succ_list_);
    traverse(reach_bwd_, pred_start_, pred_list_);

    // --- 規則1: 全断片が anchor と相互到達できなければ閉路は閉じない ---
    for (size_t f = 0; f < m; ++f) {
        if (!reach_fwd_[n_ + f] || !reach_bwd_[n_ + f]) return false;
    }

    // --- 規則2: 相互到達できない素ノードは閉路に入れない ---
    for (size_t k = 0; k < n_; ++k) {
        if (frag_id_[k] != SIZE_MAX) continue;          // 断片所属は規則1で判定済み
        size_t vid = var_ids_[k];
        if (model.is_instantiated(vid)) continue;       // 既に out 確定
        if (reach_fwd_[k] && reach_bwd_[k]) continue;
        if (!model.contains(vid, self_of(k))) return false;  // 必須なのに閉路外
        if (in_presolve) {
            if (!model.variable(vid)->assign(self_of(k))) return false;
        } else {
            model.enqueue_instantiate(vid, self_of(k));
        }
        if (changed) *changed = true;
    }

    // --- 規則3: 必須ノード・断片 head の入次数ルール ---
    // 閉路上のノードには非自己ループの前任がちょうど1つ。候補0なら矛盾、
    // 1つならその弧を強制できる（ドメインは縮む一方なので安全）。
    for (size_t k = 0; k < n_; ++k) {
        size_t vid = var_ids_[k];
        bool must_be_in = !model.contains(vid, self_of(k));
        size_t f = frag_id_[k];
        if (!must_be_in && !(f != SIZE_MAX && frag_head_[f] == k)) continue;
        if (frag_occupied_[k]) continue;                // 既に前任が確定
        size_t w = (f != SIZE_MAX) ? n_ + f : k;
        size_t cand = SIZE_MAX, cnt = 0;
        for (size_t p = pred_start_[w]; p < pred_start_[w + 1]; ++p) {
            cand = pred_list_[p];
            if (++cnt > 1) break;
        }
        if (cnt == 0) return false;
        if (cnt > 1) continue;
        size_t u = exit_node(cand);
        size_t uvid = var_ids_[u];
        if (model.is_instantiated(uvid)) continue;
        auto arc_val = self_of(k);
        if (in_presolve) {
            if (!model.variable(uvid)->assign(arc_val)) return false;
        } else {
            model.enqueue_instantiate(uvid, arc_val);
        }
        if (changed) *changed = true;
    }

    return true;
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

    // 到達可能性フィルタ（必須ノードの相互到達性）
    {
        bool f_changed = false;
        if (!filter_reachability(model, /*in_presolve=*/true, &f_changed)) {
            return PresolveResult::Contradiction;
        }
        if (f_changed) changed = true;
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool SubcircuitConstraint::propagate_batch(Model& model, int /*save_point*/) {
    // 未確定が少なければ既存の O(1) ルールで十分。グラフ再構築のコストを避ける。
    if (unfixed_count_ < 2 || n_ < 3) return true;
    return filter_reachability(model, /*in_presolve=*/false, nullptr);
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

        if (unfixed_count_ >= 2 && n_ >= 3) model.schedule_constraint_batch(model_index());

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

    if (unfixed_count_ >= 2 && n_ >= 3) model.schedule_constraint_batch(model_index());

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
