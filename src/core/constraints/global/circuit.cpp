#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <limits>

namespace sabori_csp {

// ============================================================================
// CircuitConstraint implementation
// ============================================================================

CircuitConstraint::CircuitConstraint(std::vector<VariablePtr> vars)
    : Constraint(extract_var_ids(vars))
    , n_(var_ids_.size())
    , base_offset_(0)
    , partner_(n_)
    , size_(n_, 1)
    , occupier_(n_, SIZE_MAX)
    , unfixed_count_(0)
    , pool_n_(n_) {
    // ベースオフセットを検出（全変数の値域のグローバル最小値から）
    // FlatZinc の circuit は通常 1-based（値の範囲は 1..n）
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

    // 初期状態: 各ノードは長さ1のパス（端点は自分自身）
    for (size_t i = 0; i < n_; ++i) {
        partner_[i] = i;
    }

    // プール初期化（0 から n-1 の内部インデックス）
    pool_.resize(n_);
    pool_idx_.resize(n_);
    for (size_t i = 0; i < n_; ++i) {
        pool_[i] = static_cast<Domain::value_type>(i);
        pool_idx_[i] = i;
    }

    // 既に確定している変数のパス結合と入次数を設定 + 未確定カウント
    for (size_t i = 0; i < n_; ++i) {
        if (vars[i]->is_assigned()) {
            auto val = vars[i]->assigned_value().value();
            // 値を内部インデックス（0-based）に変換
            size_t j = static_cast<size_t>(val - base_offset_);

            // 範囲チェック / 入次数チェック / サブサーキット検出は
            // prepare_propagation() でも同等のチェックを行うためここでは早期 return のみ
            if (j >= n_) return;
            if (occupier_[j] != SIZE_MAX) return;

            // i は自パスの tail、j は自パスの head
            size_t h1 = partner_[i];
            size_t t2 = partner_[j];

            if (h1 == j) {
                // 閉路形成
                if (size_[h1] < n_) {
                    // サブサーキット
                    return;
                }
            } else {
                // パス結合: h1 → ... → i → j → ... → t2
                partner_[h1] = t2;
                partner_[t2] = h1;
                size_[h1] += size_[j];
            }

            occupier_[j] = i;
            remove_from_pool(j);
        } else {
            ++unfixed_count_;
        }
    }

}

std::string CircuitConstraint::name() const {
    return "circuit";
}

bool CircuitConstraint::prepare_propagation(Model& model) {
    // presolve 後の変数状態に基づいて内部状態を完全に再構築
    for (size_t i = 0; i < n_; ++i) {
        partner_[i] = i;
        size_[i] = 1;
        occupier_[i] = SIZE_MAX;
    }
    unfixed_count_ = 0;
    pool_n_ = n_;
    for (size_t i = 0; i < n_; ++i) {
        pool_[i] = static_cast<Domain::value_type>(i);
        pool_idx_[i] = i;
    }
    trail_.clear();

    for (size_t i = 0; i < n_; ++i) {
        if (model.is_instantiated(var_ids_[i])) {
            auto val = model.value(var_ids_[i]);
            size_t j = static_cast<size_t>(val - base_offset_);
            if (j >= n_) return false;
            if (occupier_[j] != SIZE_MAX) return false;

            size_t h1 = partner_[i];
            size_t t2 = partner_[j];
            if (h1 == j) {
                if (size_[h1] < n_) return false;
            } else {
                partner_[h1] = t2;
                partner_[t2] = h1;
                size_[h1] += size_[j];
            }
            occupier_[j] = i;
            remove_from_pool(j);
        } else {
            ++unfixed_count_;
        }
    }

    if (unfixed_count_ > pool_n_) return false;

    return true;
}

PresolveResult CircuitConstraint::presolve(Model& model) {
    // 確定済みエッジが作る各パス h → ... → t (size < n) について、
    // tail t が head h へ戻るとサブサーキットになるため値 h を除去する。
    // 長さ1のパス（未確定ノード）では自己ループ除去 x[i] != i になる。
    // 構築後に他制約の presolve でパスが伸びていても、t → h は
    // サブサーキットか入次数2のどちらかで必ず矛盾なので除去は健全。
    if (n_ <= 1) return PresolveResult::Unchanged;

    bool changed = false;
    for (size_t h = 0; h < n_; ++h) {
        if (occupier_[h] != SIZE_MAX) continue;  // head でないノード
        if (size_[h] >= n_) continue;            // 全ノードを含むパスは閉じる必要がある
        size_t t = partner_[h];
        Variable* v = model.variable(var_ids_[t]);
        if (v->is_assigned()) continue;
        auto forbidden = static_cast<Domain::value_type>(h) + base_offset_;
        if (v->domain().contains(forbidden)) {
            if (!v->remove(forbidden)) return PresolveResult::Contradiction;
            changed = true;
        }
    }

    // AllDifferent の forward checking: 確定済みの値を未確定変数のドメインから除去
    std::vector<Domain::value_type> used_values;
    for (size_t k = 0; k < n_; ++k) {
        Variable* v = model.variable(var_ids_[k]);
        if (v->is_assigned()) {
            used_values.push_back(v->min());
        }
    }
    if (!used_values.empty()) {
        for (size_t k = 0; k < n_; ++k) {
            Variable* v = model.variable(var_ids_[k]);
            if (v->is_assigned()) continue;
            for (auto val : used_values) {
                if (v->domain().contains(val)) {
                    if (!v->remove(val)) return PresolveResult::Contradiction;
                    changed = true;
                }
            }
        }
    }

    // SCC フィルタリング: ドメイングラフが強連結でなければハミルトン閉路は不可能。
    // 入次数 0 のノードは矛盾、入次数 1 のノードは唯一の前任エッジを確定させる。
    if (n_ >= 3) {
        if (!build_scc_graph(model, /*use_path_state=*/false)) {
            return PresolveResult::Contradiction;
        }
        if (!scc_single_component()) {
            return PresolveResult::Contradiction;
        }
        for (size_t j = 0; j < n_; ++j) {
            if (scc_indeg_[j] == 0) return PresolveResult::Contradiction;
            if (scc_indeg_[j] == 1) {
                size_t u = static_cast<size_t>(scc_pred_[j]);
                Variable* uv = model.variable(var_ids_[u]);
                if (!uv->is_assigned()) {
                    auto val = static_cast<Domain::value_type>(j) + base_offset_;
                    if (!uv->assign(val)) return PresolveResult::Contradiction;
                    changed = true;
                }
            }
        }
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool CircuitConstraint::build_scc_graph(Model& model, bool use_path_state) {
    if (scc_adj_head_.size() != n_ + 1) {
        scc_adj_head_.resize(n_ + 1);
        scc_index_.resize(n_);
        scc_low_.resize(n_);
        scc_edge_pos_.resize(n_);
        scc_onstack_.resize(n_);
        scc_indeg_.resize(n_);
        scc_pred_.resize(n_);
        scc_occupied_.resize(n_);
        scc_stack_.reserve(n_);
        scc_dfs_stack_.reserve(n_);
    }
    scc_adj_.clear();
    std::fill(scc_indeg_.begin(), scc_indeg_.end(), 0);

    // presolve では occupier_ が古い可能性があるため model から再計算する
    if (!use_path_state) {
        std::fill(scc_occupied_.begin(), scc_occupied_.end(), 0);
        for (size_t i = 0; i < n_; ++i) {
            if (model.is_instantiated(var_ids_[i])) {
                size_t j = static_cast<size_t>(model.value(var_ids_[i]) - base_offset_);
                if (j >= n_) return false;
                scc_occupied_[j] = 1;
            }
        }
    }

    for (size_t i = 0; i < n_; ++i) {
        scc_adj_head_[i] = static_cast<int>(scc_adj_.size());
        size_t vid = var_ids_[i];

        if (model.is_instantiated(vid)) {
            size_t j = static_cast<size_t>(model.value(vid) - base_offset_);
            if (j >= n_) return false;
            scc_adj_.push_back(static_cast<int>(j));
            ++scc_indeg_[j];
            scc_pred_[j] = static_cast<int>(i);
            continue;
        }

        // サブサーキットを閉じるエッジ（tail → 自パスの head）は使用不能。
        // 探索中は partner_/size_ が trail で最新に保たれている。
        size_t block = SIZE_MAX;
        if (use_path_state) {
            size_t h = partner_[i];
            if (size_[h] < n_) block = h;
        }

        auto lo = std::max(model.var_min(vid), base_offset_);
        auto hi = std::min(model.var_max(vid),
                           base_offset_ + static_cast<Domain::value_type>(n_) - 1);
        size_t out_degree = 0;
        for (auto val = lo; val <= hi; ++val) {
            if (!model.contains(vid, val)) continue;
            size_t j = static_cast<size_t>(val - base_offset_);
            if (j == i) continue;  // 自己ループは閉路に使えない (n>=2)
            if (use_path_state ? (occupier_[j] != SIZE_MAX) : (scc_occupied_[j] != 0)) {
                continue;  // j の前任は確定済み
            }
            if (j == block) continue;
            scc_adj_.push_back(static_cast<int>(j));
            ++scc_indeg_[j];
            scc_pred_[j] = static_cast<int>(i);
            ++out_degree;
        }
        if (out_degree == 0) return false;
    }
    scc_adj_head_[n_] = static_cast<int>(scc_adj_.size());
    return true;
}

bool CircuitConstraint::scc_single_component() {
    std::fill(scc_index_.begin(), scc_index_.end(), -1);
    scc_stack_.clear();
    scc_dfs_stack_.clear();
    std::fill(scc_onstack_.begin(), scc_onstack_.end(), 0);

    int counter = 0;
    int scc_count = 0;

    scc_dfs_stack_.push_back(0);
    while (!scc_dfs_stack_.empty()) {
        int v = scc_dfs_stack_.back();
        if (scc_index_[v] < 0) {
            scc_index_[v] = scc_low_[v] = counter++;
            scc_stack_.push_back(v);
            scc_onstack_[v] = 1;
            scc_edge_pos_[v] = scc_adj_head_[v];
        }
        if (scc_edge_pos_[v] < scc_adj_head_[v + 1]) {
            int w = scc_adj_[scc_edge_pos_[v]++];
            if (scc_index_[w] < 0) {
                scc_dfs_stack_.push_back(w);
            } else if (scc_onstack_[w]) {
                scc_low_[v] = std::min(scc_low_[v], scc_index_[w]);
            }
        } else {
            scc_dfs_stack_.pop_back();
            if (!scc_dfs_stack_.empty()) {
                int parent = scc_dfs_stack_.back();
                scc_low_[parent] = std::min(scc_low_[parent], scc_low_[v]);
            }
            if (scc_low_[v] == scc_index_[v]) {
                if (++scc_count > 1) return false;  // 2つ目の SCC = 強連結でない
                int w;
                do {
                    w = scc_stack_.back();
                    scc_stack_.pop_back();
                    scc_onstack_[w] = 0;
                } while (w != v);
            }
        }
    }

    // ノード 0 から到達できないノードがある場合も閉路は不可能
    return static_cast<size_t>(counter) == n_;
}

void CircuitConstraint::remove_from_pool(size_t value) {
    size_t idx = pool_idx_[value];
    if (idx >= pool_n_) {
        return;  // 既にプールにない
    }

    // Sparse Set から削除（スワップ）
    size_t last_idx = pool_n_ - 1;
    size_t last_value = static_cast<size_t>(pool_[last_idx]);

    pool_[idx] = static_cast<Domain::value_type>(last_value);
    pool_[last_idx] = static_cast<Domain::value_type>(value);
    pool_idx_[last_value] = idx;
    pool_idx_[value] = last_idx;
    --pool_n_;
}

bool CircuitConstraint::on_instantiate(Model& model, int save_point,
                                        size_t internal_var_idx,
                                        Domain::value_type value,
                                        Domain::value_type /*prev_min*/,
                                        Domain::value_type /*prev_max*/) {
    const size_t var_idx = var_id(internal_var_idx);
    (void)var_idx;
    size_t i = internal_var_idx;
    // 値を内部インデックス（0-based）に変換
    size_t j = static_cast<size_t>(value - base_offset_);

    // 範囲チェック
    if (j >= n_) {
        return false;
    }

    // AllDifferent チェック: ノード j に既に確定済みエッジがあれば重複
    if (occupier_[j] != SIZE_MAX) {
        return false;
    }

    // i は自パスの tail（未確定だったので出エッジなし）、
    // j は自パスの head（入次数 0）
    size_t h1 = partner_[i];
    size_t t2 = partner_[j];

    // サブサーキット検出: 同じパス内なら閉路形成
    if (h1 == j) {
        // 閉路が形成される
        if (size_[h1] < n_) {
            // サブサーキット（全ノードを含まない閉路）
            return false;
        }
        // size == n なら正当な完全閉路
        // occupier とプールを更新して trail に記録
        TrailEntry entry{i, j, 0, 0, 0, pool_n_, unfixed_count_, false};
        trail_.push_back({save_point, entry});
        model.mark_constraint_dirty(model_index(), save_point);

        occupier_[j] = i;
        remove_from_pool(j);
        --unfixed_count_;

        // 残り1変数チェック（O(1)）
        if (unfixed_count_ == 1) {
            size_t last_idx = find_last_uninstantiated(model);
            if (last_idx != SIZE_MAX) {
                if (!on_last_uninstantiated(model, save_point, last_idx)) {
                    return false;
                }
            }
        } else if (unfixed_count_ == 0) {
            return on_final_instantiate(model);
        }

        return true;
    }

    // パス結合: h1 → ... → i → j → ... → t2
    TrailEntry entry{i, j, h1, t2, size_[h1], pool_n_, unfixed_count_, true};
    trail_.push_back({save_point, entry});
    model.mark_constraint_dirty(model_index(), save_point);

    // 更新: 新しいパスの端点は h1 と t2
    partner_[h1] = t2;
    partner_[t2] = h1;
    size_[h1] += size_[j];
    occupier_[j] = i;
    remove_from_pool(j);  // 内部インデックス j をプールから削除
    --unfixed_count_;

    // 鳩の巣原理: 未確定変数 > 利用可能な値 なら矛盾
    if (unfixed_count_ > pool_n_) {
        return false;
    }

    // サブサーキット枝刈り: パスが全ノードを含まないなら
    // tail t2 が head h1 へ戻る値を除去
    if (size_[h1] < n_) {
        model.enqueue_remove_value(var_ids_[t2],
                                   static_cast<Domain::value_type>(h1) + base_offset_);
    }

    // AllDifferent の forward checking: 確定値 value を他の未確定変数から除去
    for (size_t k = 0; k < n_; ++k) {
        if (k == i) continue;
        size_t vid = var_ids_[k];
        if (model.is_instantiated(vid)) continue;
        if (model.contains(vid, value)) {
            model.enqueue_remove_value(vid, value);
        }
    }

    // SCC フィルタリングはバッチ登録（イベントキューが空になった時点で1回実行）
    if (unfixed_count_ >= 2 && n_ >= 3) {
        model.schedule_constraint_batch(model_index());
    }

    // 残り1変数チェック（O(1)）
    if (unfixed_count_ == 1) {
        size_t last_idx = find_last_uninstantiated(model);
        if (last_idx != SIZE_MAX) {
            if (!on_last_uninstantiated(model, save_point, last_idx)) {
                return false;
            }
        }
    } else if (unfixed_count_ == 0) {
        return on_final_instantiate(model);
    }

    return true;
}

bool CircuitConstraint::propagate_batch(Model& model, int /*save_point*/) {
    // SCC フィルタリング: 強連結でなければ矛盾、
    // 入次数 0 は矛盾、入次数 1 は唯一の前任エッジを確定。
    // occupier_/partner_/size_ は trail で常に最新なのでバッチ時点の状態で正しい
    if (unfixed_count_ < 2 || n_ < 3) return true;
    if (!build_scc_graph(model, /*use_path_state=*/true)) return false;
    if (!scc_single_component()) return false;
    for (size_t k = 0; k < n_; ++k) {
        if (scc_indeg_[k] == 0) return false;
        if (scc_indeg_[k] == 1) {
            size_t u = static_cast<size_t>(scc_pred_[k]);
            size_t uvid = var_ids_[u];
            if (!model.is_instantiated(uvid)) {
                model.enqueue_instantiate(
                    uvid, static_cast<Domain::value_type>(k) + base_offset_);
            }
        }
    }
    return true;
}

bool CircuitConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                                  size_t last_var_internal_idx) {
    auto last_var_id = var_ids_[last_var_internal_idx];

    // 既に確定している場合は整合性チェックのみ
    if (model.is_instantiated(last_var_id)) {
        auto val = model.value(last_var_id);
        // 値を内部インデックスに変換
        size_t j = static_cast<size_t>(val - base_offset_);
        if (j >= n_) {
            return false;
        }
        // その値がプールに残っているか（他の変数と重複していないか）
        return pool_idx_[j] < pool_n_ && occupier_[j] == SIZE_MAX;
    }

    // 利用可能な値が1つだけなら自動決定
    if (pool_n_ == 1) {
        // プールには内部インデックスが格納されているので、元の値に戻す
        Domain::value_type remaining_value = pool_[0] + base_offset_;
        model.enqueue_instantiate(last_var_id, remaining_value);
    }
    // 利用可能な値が0なら矛盾
    else if (pool_n_ == 0) {
        return false;
    }

    return true;
}

bool CircuitConstraint::on_final_instantiate(const Model& model) {
    // 全ての変数が確定したときの最終確認: 単一のハミルトン閉路を形成しているか
    // 内部状態に依存せず、モデルの値から閉路を直接たどって検証する
    if (n_ == 0) return true;
    std::vector<bool> visited(n_, false);
    size_t cur = 0;
    for (size_t steps = 0; steps < n_; ++steps) {
        if (visited[cur]) return false;
        visited[cur] = true;
        if (!model.is_instantiated(var_ids_[cur])) return false;
        size_t next = static_cast<size_t>(model.value(var_ids_[cur]) - base_offset_);
        if (next >= n_) return false;
        cur = next;
    }
    return cur == 0;
}

void CircuitConstraint::rewind_to(int save_point) {
    while (!trail_.empty() && trail_.back().first > save_point) {
        const auto& entry = trail_.back().second;

        // occupier を戻す
        occupier_[entry.j] = SIZE_MAX;

        // プールを戻す
        pool_n_ = entry.old_pool_n;

        // 未確定カウントを戻す
        unfixed_count_ = entry.old_unfixed_count;

        // パス結合の場合のみ端点とサイズを戻す
        if (entry.is_merge) {
            partner_[entry.h1] = entry.i;  // h1 のパスは h1 → ... → i に戻る
            partner_[entry.t2] = entry.j;  // t2 のパスは j → ... → t2 に戻る
            size_[entry.h1] = entry.old_size_h1;
        }

        trail_.pop_back();
    }
}

void CircuitConstraint::bump_activity(const Model& model, size_t trigger_var_idx,
                                       double* activity, double activity_inc,
                                       bool& need_rescale, std::mt19937& rng) const {
    if (!model.is_instantiated(trigger_var_idx)) return;

    auto trigger_val = model.value(trigger_var_idx);

    // 原因となる変数と衝突している変数の 2つがあるはず
    const double inc = 0.5 * activity_inc;

    bump_variable_activity(activity, trigger_var_idx, inc, need_rescale, rng);

    // trigger と同じ値に確定済みの変数は occupier で O(1) 特定できる
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

// ============================================================================

}  // namespace sabori_csp
