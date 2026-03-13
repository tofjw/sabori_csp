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

    if (gac_enabled_) {
        size_t n = var_ids_.size();
        size_t m = total_values_;
        match_var_.assign(n, -1);
        match_val_.assign(m, -1);
        matching_valid_ = false;

        hk_dist_.resize(n);
        hk_iter_.resize(n);
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
    matching_valid_ = false;
    return run_gac_filtering(model);
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
    return run_gac_filtering(model);
}

void AllDifferentGACConstraint::rewind_to(int save_point) {
    AllDifferentConstraint::rewind_to(save_point);
    matching_valid_ = false;
}

// ============================================================================
// GAC (Régin's algorithm) core
// ============================================================================

bool AllDifferentGACConstraint::run_gac_filtering(Model& model) {
    if (unfixed_count_ <= 1) return true;
    if (unfixed_count_ > pool_n_) return false;

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
                hk_dfs(model, static_cast<int>(i));
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

bool AllDifferentGACConstraint::hk_dfs(Model& model, int u) {
    auto vid = var_ids_[u];

    std::vector<Domain::value_type> local_buf;
    model.variable(vid)->domain().copy_values_to(local_buf);

    for (size_t idx = hk_iter_[u]; idx < local_buf.size(); ++idx) {
        hk_iter_[u] = static_cast<int>(idx);
        auto val = local_buf[idx];
        auto it = gac_val_to_idx_.find(val);
        if (it == gac_val_to_idx_.end()) continue;
        int j = it->second;
        if (!is_val_in_pool(j)) continue;
        int v = match_val_[j];

        if (v == -1 || (hk_dist_[v] == hk_dist_[u] + 1 && hk_dfs(model, v))) {
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

    // 1. 自由値ノードからの到達可能性 (BFS on residual graph)
    std::fill(reachable_.begin(), reachable_.begin() + total_nodes, false);
    int queue_head = 0, queue_tail = 0;
    bfs_queue_.resize(total_nodes);

    for (size_t j = 0; j < m; ++j) {
        if (!is_val_in_pool(j)) continue;
        if (match_val_[j] == -1) {
            bool exists_in_some_domain = false;
            auto val = gac_idx_to_val_[j];
            for (size_t i = 0; i < n; ++i) {
                if (!model.is_instantiated(var_ids_[i]) && model.contains(var_ids_[i], val)) {
                    exists_in_some_domain = true;
                    break;
                }
            }
            if (exists_in_some_domain) {
                reachable_[n + j] = true;
                bfs_queue_[queue_tail++] = static_cast<int>(n + j);
            }
        }
    }

    while (queue_head < queue_tail) {
        int node = bfs_queue_[queue_head++];
        if (node >= static_cast<int>(n)) {
            int j = node - static_cast<int>(n);
            int matched_var = match_val_[j];
            if (matched_var != -1 && !reachable_[matched_var]) {
                reachable_[matched_var] = true;
                bfs_queue_[queue_tail++] = matched_var;
            }
        } else {
            int i = node;
            if (model.is_instantiated(var_ids_[i])) continue;
            model.variable(var_ids_[i])->domain().for_each_value([&](Domain::value_type val) {
                auto it = gac_val_to_idx_.find(val);
                if (it == gac_val_to_idx_.end()) return;
                int j = it->second;
                if (!is_val_in_pool(j)) return;
                if (match_var_[i] == j) return;
                if (!reachable_[n + j]) {
                    reachable_[n + j] = true;
                    bfs_queue_[queue_tail++] = static_cast<int>(n + j);
                }
            });
        }
    }

    // 2. Tarjan SCC
    std::fill(scc_num_.begin(), scc_num_.begin() + total_nodes, -1);
    std::fill(scc_low_.begin(), scc_low_.begin() + total_nodes, -1);
    std::fill(scc_id_.begin(), scc_id_.begin() + total_nodes, -1);
    std::fill(on_stack_.begin(), on_stack_.begin() + total_nodes, false);
    scc_stack_.clear();
    tarjan_counter_ = 0;
    scc_count_ = 0;

    for (size_t i = 0; i < n; ++i) {
        if (!model.is_instantiated(var_ids_[i]) && scc_num_[i] == -1) {
            tarjan_dfs(model, static_cast<int>(i));
        }
    }
    for (size_t j = 0; j < m; ++j) {
        if (is_val_in_pool(j) && match_val_[j] != -1 && scc_num_[n + j] == -1) {
            tarjan_dfs(model, static_cast<int>(n + j));
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

            model.enqueue_remove_value(var_ids_[i], val);
        }
    }
}

void AllDifferentGACConstraint::tarjan_dfs(Model& model, int u) {
    size_t n = var_ids_.size();

    struct Frame {
        int node;
        int phase;
        size_t neighbor_idx;
        std::vector<int> neighbors;
    };

    std::vector<Frame> call_stack;
    call_stack.push_back({u, 0, 0, {}});

    while (!call_stack.empty()) {
        auto& frame = call_stack.back();
        int node = frame.node;

        if (frame.phase == 0) {
            scc_num_[node] = scc_low_[node] = tarjan_counter_++;
            scc_stack_.push_back(node);
            on_stack_[node] = true;

            frame.neighbors.clear();
            if (node < static_cast<int>(n)) {
                int i = node;
                if (!model.is_instantiated(var_ids_[i])) {
                    model.variable(var_ids_[i])->domain().for_each_value([&](Domain::value_type val) {
                        auto it = gac_val_to_idx_.find(val);
                        if (it == gac_val_to_idx_.end()) return;
                        int j = it->second;
                        if (!is_val_in_pool(j)) return;
                        if (match_var_[i] != j) {
                            frame.neighbors.push_back(static_cast<int>(n) + j);
                        }
                    });
                }
            } else {
                int j = node - static_cast<int>(n);
                if (match_val_[j] != -1) {
                    frame.neighbors.push_back(match_val_[j]);
                }
            }

            frame.phase = 1;
            frame.neighbor_idx = 0;
        }

        if (frame.phase == 1) {
            bool pushed_child = false;
            while (frame.neighbor_idx < frame.neighbors.size()) {
                int w = frame.neighbors[frame.neighbor_idx];
                frame.neighbor_idx++;

                if (scc_num_[w] == -1) {
                    call_stack.push_back({w, 0, 0, {}});
                    pushed_child = true;
                    break;
                } else if (on_stack_[w]) {
                    scc_low_[node] = std::min(scc_low_[node], scc_num_[w]);
                }
            }

            if (pushed_child) continue;

            if (scc_low_[node] == scc_num_[node]) {
                int w;
                do {
                    w = scc_stack_.back();
                    scc_stack_.pop_back();
                    on_stack_[w] = false;
                    scc_id_[w] = scc_count_;
                } while (w != node);
                scc_count_++;
            }

            call_stack.pop_back();

            if (!call_stack.empty()) {
                auto& parent = call_stack.back();
                scc_low_[parent.node] = std::min(scc_low_[parent.node], scc_low_[node]);
            }
        }
    }
}

}  // namespace sabori_csp
