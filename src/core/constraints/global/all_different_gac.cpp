#include "sabori_csp/constraints/all_different_gac.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <set>
#include <queue>


namespace sabori_csp {

// ============================================================================
// AllDifferentGACConstraint implementation
// ============================================================================

AllDifferentGACConstraint::AllDifferentGACConstraint(std::vector<VariablePtr> vars)
    : AllDifferentConstraint(std::move(vars)) {
    // GAC 用の安定マッピングを構築（pool 構築後のスナップショット）
    total_values_ = pool_values_.size();
    gac_idx_to_val_ = pool_values_;
    for (size_t i = 0; i < total_values_; ++i) {
        gac_val_to_idx_[gac_idx_to_val_[i]] = static_cast<int>(i);
    }
}

std::string AllDifferentGACConstraint::name() const {
    return "all_different_gac";
}

bool AllDifferentGACConstraint::prepare_propagation(Model& model) {
    if (!AllDifferentConstraint::prepare_propagation(model)) {
        return false;
    }

    // GAC 有効判定: unfixed >= 4 かつ全て sparse set ドメイン
    gac_enabled_ = false;
    if (unfixed_count_ >= 4) {
        bool all_sparse = true;
        for (size_t i = 0; i < var_ids_.size(); ++i) {
            auto vid = var_ids_[i];
            if (!model.is_instantiated(vid)) {
                if (model.variable(vid)->domain().is_bounds_only()) {
                    all_sparse = false;
                    break;
                }
            }
        }
        gac_enabled_ = all_sparse;
    }

    // ハイブリッド方式: bounds イベントは親の bounds(Z) で処理するため有効のまま。
    // フル GAC は instantiate / remove_value イベントでのみ実行する
    clear_pending_echoes();

    if (gac_enabled_) {
        size_t n = var_ids_.size();
        size_t m = total_values_;
        match_var_.assign(n, -1);
        match_val_.assign(m, -1);
        matching_valid_ = false;

        hk_dist_.resize(n);
        hk_iter_.resize(n);
        hk_buf_pool_.resize(n);
        bfs_queue_.resize(n);
        size_t total_nodes = n + m;
        reachable_.resize(total_nodes);
        scc_id_.resize(total_nodes);
        scc_stack_.reserve(total_nodes);
        scc_low_.resize(total_nodes);
        scc_num_.resize(total_nodes);
        on_stack_.resize(total_nodes);
    }

    return true;
}

bool AllDifferentGACConstraint::on_instantiate(Model& model, int save_point,
                                                size_t var_idx, size_t internal_var_idx,
                                                Domain::value_type value,
                                                Domain::value_type prev_min,
                                                Domain::value_type prev_max) {
    if (!AllDifferentConstraint::on_instantiate(model, save_point, var_idx, internal_var_idx,
                                                value, prev_min, prev_max)) {
        return false;
    }
    matching_valid_ = false;

    // 親の forward checking が enqueue した値除去をエコーとして登録し、
    // バッチ末尾の除去を処理した後に GAC を1回だけ実行する
    // （登録しないと除去1件ごとに on_remove_value で GAC がフル実行される）
    if (gac_enabled_ && unfixed_count_ >= 2) {
        size_t first = pending_echoes_.size();
        for (size_t k = 0; k < var_ids_.size(); ++k) {
            if (k == internal_var_idx) continue;
            size_t vid = var_ids_[k];
            if (model.is_instantiated(vid)) continue;
            if (model.contains(vid, value)) {
                pending_echoes_.push_back({vid, value, false});
            }
        }
        if (pending_echoes_.size() > first) {
            pending_echoes_.back().run_after = true;
        }
    }
    return true;
}

bool AllDifferentGACConstraint::on_remove_value(Model& model, int save_point,
                                                 size_t var_idx, size_t internal_var_idx,
                                                 Domain::value_type removed_value) {
    if (!gac_enabled_) {
        return AllDifferentConstraint::on_remove_value(model, save_point, var_idx,
                                                       internal_var_idx, removed_value);
    }
    matching_valid_ = false;

    // 自分(または親)が enqueue した除去のエコーなら GAC 再実行をスキップ。
    // バッチ末尾のエコー消費時のみ1回実行して fixpoint を回復する
    if (pending_echo_head_ < pending_echoes_.size()) {
        const auto& e = pending_echoes_[pending_echo_head_];
        if (e.var_id == var_idx && e.value == removed_value) {
            bool run_after = e.run_after;
            ++pending_echo_head_;
            if (pending_echo_head_ == pending_echoes_.size()) {
                clear_pending_echoes();
            }
            return run_after ? run_gac_filtering(model) : true;
        }
    }
    return run_gac_filtering(model);
}

bool AllDifferentGACConstraint::on_set_min(Model& model, int save_point,
                                            size_t var_idx, size_t internal_var_idx,
                                            Domain::value_type new_min,
                                            Domain::value_type old_min) {
    if (!gac_enabled_) {
        return AllDifferentConstraint::on_set_min(model, save_point, var_idx,
                                                   internal_var_idx, new_min, old_min);
    }
    // bounds イベントは高頻度（他制約の bounds 伝播で毎ノード大量に発生）なので
    // フル GAC は走らせず、軽量な親処理（Hall ペア + bounds(Z)）に委譲する。
    // フル GAC は instantiate / remove_value イベントでのみ実行
    matching_valid_ = false;
    return AllDifferentConstraint::on_set_min(model, save_point, var_idx,
                                               internal_var_idx, new_min, old_min);
}

bool AllDifferentGACConstraint::on_set_max(Model& model, int save_point,
                                            size_t var_idx, size_t internal_var_idx,
                                            Domain::value_type new_max,
                                            Domain::value_type old_max) {
    if (!gac_enabled_) {
        return AllDifferentConstraint::on_set_max(model, save_point, var_idx,
                                                   internal_var_idx, new_max, old_max);
    }
    matching_valid_ = false;
    return AllDifferentConstraint::on_set_max(model, save_point, var_idx,
                                               internal_var_idx, new_max, old_max);
}

void AllDifferentGACConstraint::rewind_to(int save_point) {
    AllDifferentConstraint::rewind_to(save_point);
    matching_valid_ = false;
    // バックトラックで破棄された enqueue のエコーは届かないため無効化
    clear_pending_echoes();
}

// ============================================================================
// GAC (Régin's algorithm) core
// ============================================================================

bool AllDifferentGACConstraint::run_gac_filtering(Model& model) {
    if (unfixed_count_ <= 1) return true;
    if (unfixed_count_ > pool_n_) return false;

    // fixpoint を再計算するので、未消費の古いエコーは破棄して
    // ここで enqueue する除去のエコーを登録し直す
    clear_pending_echoes();

    if (!find_maximum_matching(model)) return false;
    matching_valid_ = true;
    compute_sccs_and_filter(model);
    return true;
}

bool AllDifferentGACConstraint::find_maximum_matching(Model& model) {
    size_t n = var_ids_.size();

    // 1. 既存マッチングの検証 (warm start)
    for (size_t i = 0; i < n; ++i) {
        if (model.is_instantiated(var_ids_[i])) {
            if (match_var_[i] != -1) {
                match_val_[match_var_[i]] = -1;
                match_var_[i] = -1;
            }
        } else {
            if (match_var_[i] != -1) {
                int j = match_var_[i];
                auto val = gac_idx_to_val_[j];
                if (!model.contains(var_ids_[i], val) || !is_val_in_pool(j)) {
                    match_val_[j] = -1;
                    match_var_[i] = -1;
                }
            }
        }
    }

    // 2. Hopcroft-Karp
    while (hk_bfs(model)) {
        for (size_t i = 0; i < n; ++i) {
            if (!model.is_instantiated(var_ids_[i]) && match_var_[i] == -1) {
                std::fill(hk_iter_.begin(), hk_iter_.end(), 0);
                hk_dfs(model, static_cast<int>(i), 0);
            }
        }
    }

    // 3. マッチングサイズ検証
    size_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!model.is_instantiated(var_ids_[i]) && match_var_[i] != -1) {
            ++count;
        }
    }
    return count == unfixed_count_;
}

bool AllDifferentGACConstraint::hk_bfs(Model& model) {
    size_t n = var_ids_.size();
    int queue_head = 0, queue_tail = 0;

    for (size_t i = 0; i < n; ++i) {
        if (!model.is_instantiated(var_ids_[i]) && match_var_[i] == -1) {
            hk_dist_[i] = 0;
            bfs_queue_[queue_tail++] = static_cast<int>(i);
        } else {
            hk_dist_[i] = -1;
        }
    }

    bool found = false;
    while (queue_head < queue_tail) {
        int u = bfs_queue_[queue_head++];
        auto vid = var_ids_[u];

        model.variable(vid)->domain().for_each_value([&](Domain::value_type val) {
            auto it = gac_val_to_idx_.find(val);
            if (it == gac_val_to_idx_.end()) return;
            int j = it->second;
            if (!is_val_in_pool(j)) return;
            int v = match_val_[j];
            if (v == -1) {
                found = true;
            } else if (hk_dist_[v] == -1) {
                hk_dist_[v] = hk_dist_[u] + 1;
                bfs_queue_[queue_tail++] = v;
            }
        });
    }
    return found;
}

bool AllDifferentGACConstraint::hk_dfs(Model& model, int u, size_t depth) {
    auto vid = var_ids_[u];

    // 深さ別の再利用バッファ（毎回のヒープ確保を回避）
    if (depth >= hk_buf_pool_.size()) hk_buf_pool_.resize(depth + 1);
    auto& local_buf = hk_buf_pool_[depth];
    local_buf.clear();
    model.variable(vid)->domain().copy_values_to(local_buf);

    for (size_t idx = hk_iter_[u]; idx < local_buf.size(); ++idx) {
        hk_iter_[u] = static_cast<int>(idx);
        auto val = local_buf[idx];
        auto it = gac_val_to_idx_.find(val);
        if (it == gac_val_to_idx_.end()) continue;
        int j = it->second;
        if (!is_val_in_pool(j)) continue;
        int v = match_val_[j];

        if (v == -1 || (hk_dist_[v] == hk_dist_[u] + 1 && hk_dfs(model, v, depth + 1))) {
            match_var_[u] = j;
            match_val_[j] = u;
            return true;
        }
    }
    hk_dist_[u] = -1;
    return false;
}

void AllDifferentGACConstraint::compute_sccs_and_filter(Model& model) {
    size_t n = var_ids_.size();
    size_t m = total_values_;
    size_t total_nodes = n + m;

    // 0. val_to_vars 逆引きインデックスを1回構築: O(n * avg_domain_size)
    val_to_vars_.resize(m);
    for (size_t j = 0; j < m; ++j) val_to_vars_[j].clear();
    for (size_t i = 0; i < n; ++i) {
        if (model.is_instantiated(var_ids_[i])) continue;
        model.variable(var_ids_[i])->domain().for_each_value([&](Domain::value_type val) {
            auto it = gac_val_to_idx_.find(val);
            if (it == gac_val_to_idx_.end()) return;
            int j = it->second;
            if (!is_val_in_pool(j)) return;
            val_to_vars_[j].push_back(static_cast<int>(i));
        });
    }

    // 1. 自由値ノードからの到達可能性 (BFS on residual graph)
    std::fill(reachable_.begin(), reachable_.begin() + total_nodes, false);
    int queue_head = 0, queue_tail = 0;
    bfs_queue_.resize(total_nodes);

    // free value 初期化: val_to_vars で O(1) 判定
    for (size_t j = 0; j < m; ++j) {
        if (!is_val_in_pool(j)) continue;
        if (match_val_[j] == -1 && !val_to_vars_[j].empty()) {
            reachable_[n + j] = true;
            bfs_queue_[queue_tail++] = static_cast<int>(n + j);
        }
    }

    while (queue_head < queue_tail) {
        int node = bfs_queue_[queue_head++];
        if (node >= static_cast<int>(n)) {
            // 値ノード j: 非マッチングエッジで j をドメインに持つ変数へ
            int j = node - static_cast<int>(n);
            for (int i : val_to_vars_[j]) {
                if (reachable_[i]) continue;
                if (match_var_[i] == j) continue;  // マッチングエッジはスキップ
                reachable_[i] = true;
                bfs_queue_[queue_tail++] = i;
            }
        } else {
            // 変数ノード i: マッチング相手の値へ
            int i = node;
            if (match_var_[i] != -1) {
                int j = match_var_[i];
                if (!reachable_[n + j]) {
                    reachable_[n + j] = true;
                    bfs_queue_[queue_tail++] = static_cast<int>(n + j);
                }
            }
        }
    }

    // 2. Tarjan 用隣接リストを1回構築 (val_to_vars を活用)
    adj_.resize(total_nodes);
    for (size_t k = 0; k < total_nodes; ++k) adj_[k].clear();
    for (size_t j = 0; j < m; ++j) {
        if (!is_val_in_pool(j)) continue;
        // 値ノード → マッチング相手の変数（マッチングエッジ）
        if (match_val_[j] != -1) {
            adj_[n + j].push_back(match_val_[j]);
        }
        // 変数 → 値ノード（非マッチングエッジ）
        for (int i : val_to_vars_[j]) {
            if (match_var_[i] != static_cast<int>(j)) {
                adj_[i].push_back(static_cast<int>(n + j));
            }
        }
    }

    // Tarjan SCC
    std::fill(scc_num_.begin(), scc_num_.begin() + total_nodes, -1);
    std::fill(scc_id_.begin(), scc_id_.begin() + total_nodes, -1);
    std::fill(on_stack_.begin(), on_stack_.begin() + total_nodes, false);
    scc_stack_.clear();
    tarjan_counter_ = 0;
    scc_count_ = 0;

    for (size_t i = 0; i < n; ++i) {
        if (!model.is_instantiated(var_ids_[i]) && scc_num_[i] == -1) {
            tarjan_dfs(static_cast<int>(i), adj_);
        }
    }
    for (size_t j = 0; j < m; ++j) {
        if (is_val_in_pool(j) && match_val_[j] != -1 && scc_num_[n + j] == -1) {
            tarjan_dfs(static_cast<int>(n + j), adj_);
        }
    }

    // 3. フィルタリング
    for (size_t i = 0; i < n; ++i) {
        if (model.is_instantiated(var_ids_[i])) continue;

        domain_buf_.clear();
        model.variable(var_ids_[i])->domain().copy_values_to(domain_buf_);

        for (auto val : domain_buf_) {
            auto it = gac_val_to_idx_.find(val);
            if (it == gac_val_to_idx_.end()) continue;
            int j = it->second;
            if (!is_val_in_pool(j)) continue;

            if (match_var_[i] == j) continue;
            if (reachable_[n + j]) continue;
            if (scc_id_[i] != -1 && scc_id_[i] == scc_id_[n + j]) continue;

            // 自己除去: エコーが届いても再実行不要（GAC は1パスで fixpoint）
            pending_echoes_.push_back({var_ids_[i], val, false});
            model.enqueue_remove_value(var_ids_[i], val);
        }
    }
}

void AllDifferentGACConstraint::tarjan_dfs(int u, const std::vector<std::vector<int>>& adj) {
    // Iterative Tarjan
    struct Frame {
        int node;
        int next_idx;
    };

    std::vector<Frame> stack;
    stack.push_back({u, 0});
    scc_num_[u] = scc_low_[u] = tarjan_counter_++;
    scc_stack_.push_back(u);
    on_stack_[u] = true;

    while (!stack.empty()) {
        auto& [cur, next] = stack.back();

        if (next < static_cast<int>(adj[cur].size())) {
            int w = adj[cur][next];
            next++;

            if (scc_num_[w] == -1) {
                scc_num_[w] = scc_low_[w] = tarjan_counter_++;
                scc_stack_.push_back(w);
                on_stack_[w] = true;
                stack.push_back({w, 0});
            } else if (on_stack_[w]) {
                scc_low_[cur] = std::min(scc_low_[cur], scc_num_[w]);
            }
        } else {
            if (scc_low_[cur] == scc_num_[cur]) {
                int w;
                do {
                    w = scc_stack_.back();
                    scc_stack_.pop_back();
                    on_stack_[w] = false;
                    scc_id_[w] = scc_count_;
                } while (w != cur);
                scc_count_++;
            }

            int finished = cur;
            stack.pop_back();

            if (!stack.empty()) {
                auto& parent = stack.back();
                scc_low_[parent.node] = std::min(scc_low_[parent.node], scc_low_[finished]);
            }
        }
    }
}

}  // namespace sabori_csp
