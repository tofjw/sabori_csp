#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <queue>

namespace sabori_csp {

// ============================================================================
// ConnectedConstraint implementation
// ============================================================================

ConnectedConstraint::ConnectedConstraint(
    std::vector<Domain::value_type> from,
    std::vector<Domain::value_type> to,
    std::vector<VariablePtr> ns,
    std::vector<VariablePtr> es)
    : Constraint([&]() {
        // vars_ = ns ++ es
        std::vector<VariablePtr> all;
        all.reserve(ns.size() + es.size());
        all.insert(all.end(), ns.begin(), ns.end());
        all.insert(all.end(), es.begin(), es.end());
        return all;
    }())
    , n_nodes_(ns.size())
    , n_edges_(es.size())
    , from_(std::move(from))
    , to_(std::move(to))
    , adj_(n_nodes_)
    , uf_parent_(n_nodes_)
    , uf_rank_(n_nodes_, 0)
    , n_selected_nodes_(0)
    , n_selected_components_(0)
{
    // from/to を 0-based に変換し、隣接リストを構築
    for (size_t e = 0; e < n_edges_; ++e) {
        from_[e] -= 1;  // 1-based → 0-based
        to_[e] -= 1;
        if (from_[e] >= 0 && static_cast<size_t>(from_[e]) < n_nodes_) {
            adj_[static_cast<size_t>(from_[e])].push_back(e);
        }
        if (to_[e] >= 0 && static_cast<size_t>(to_[e]) < n_nodes_) {
            adj_[static_cast<size_t>(to_[e])].push_back(e);
        }
    }

    // UF 初期化
    for (size_t i = 0; i < n_nodes_; ++i) {
        uf_parent_[i] = i;
    }

    check_initial_consistency();
}

std::string ConnectedConstraint::name() const {
    return "connected";
}

std::vector<VariablePtr> ConnectedConstraint::variables() const {
    return vars_;
}

std::optional<bool> ConnectedConstraint::is_satisfied() const {
    // 全変数が確定していなければ nullopt
    for (const auto& var : vars_) {
        if (!var->is_assigned()) {
            return std::nullopt;
        }
    }

    // subgraph 制約チェック: es[e]=1 → ns[from[e]]=1 ∧ ns[to[e]]=1
    for (size_t e = 0; e < n_edges_; ++e) {
        if (vars_[n_nodes_ + e]->assigned_value().value() == 1) {
            size_t u = static_cast<size_t>(from_[e]);
            size_t v = static_cast<size_t>(to_[e]);
            if (vars_[u]->assigned_value().value() != 1 ||
                vars_[v]->assigned_value().value() != 1) {
                return false;
            }
        }
    }

    // 連結性チェック: BFS
    // 選択ノードを収集
    std::vector<bool> selected(n_nodes_, false);
    size_t start_node = SIZE_MAX;
    for (size_t n = 0; n < n_nodes_; ++n) {
        if (vars_[n]->assigned_value().value() == 1) {
            selected[n] = true;
            if (start_node == SIZE_MAX) start_node = n;
        }
    }

    if (start_node == SIZE_MAX) {
        // ノードが選択されていない → 空グラフは連結とみなす
        return true;
    }

    // BFS: 選択辺のみを使って到達可能なノードを確認
    std::vector<bool> visited(n_nodes_, false);
    std::queue<size_t> bfs_queue;
    bfs_queue.push(start_node);
    visited[start_node] = true;
    size_t visited_count = 1;

    while (!bfs_queue.empty()) {
        size_t cur = bfs_queue.front();
        bfs_queue.pop();
        for (size_t e_idx : adj_[cur]) {
            if (vars_[n_nodes_ + e_idx]->assigned_value().value() != 1) continue;
            size_t neighbor = (static_cast<size_t>(from_[e_idx]) == cur)
                ? static_cast<size_t>(to_[e_idx])
                : static_cast<size_t>(from_[e_idx]);
            if (!visited[neighbor]) {
                visited[neighbor] = true;
                ++visited_count;
                bfs_queue.push(neighbor);
            }
        }
    }

    // 全ての選択ノードが到達可能か
    size_t total_selected = 0;
    for (size_t n = 0; n < n_nodes_; ++n) {
        if (selected[n]) ++total_selected;
    }

    return visited_count == total_selected;
}

bool ConnectedConstraint::prepare_propagation(Model& model) {
    // presolve 後の状態で内部構造を再構築
    for (size_t i = 0; i < n_nodes_; ++i) {
        uf_parent_[i] = i;
        uf_rank_[i] = 0;
    }
    n_selected_nodes_ = 0;
    n_selected_components_ = 0;
    trail_.clear();

    // 確定済みノードを処理
    for (size_t n = 0; n < n_nodes_; ++n) {
        if (vars_[n]->is_assigned() && vars_[n]->assigned_value().value() == 1) {
            ++n_selected_nodes_;
            ++n_selected_components_;
        }
    }

    // 確定済み辺を処理: 選択辺で UF union（両端点が選択済みの場合のみ）
    for (size_t e = 0; e < n_edges_; ++e) {
        if (vars_[n_nodes_ + e]->is_assigned() && vars_[n_nodes_ + e]->assigned_value().value() == 1) {
            size_t u = static_cast<size_t>(from_[e]);
            size_t v = static_cast<size_t>(to_[e]);
            bool u_selected = vars_[u]->is_assigned() && vars_[u]->assigned_value().value() == 1;
            bool v_selected = vars_[v]->is_assigned() && vars_[v]->assigned_value().value() == 1;
            if (u_selected && v_selected) {
                if (uf_union(u, v)) {
                    --n_selected_components_;
                }
            }
        }
    }

    // 全変数が確定済みなら連結性チェック（2WL が起動しないため）
    bool all_assigned = true;
    for (const auto& var : vars_) {
        if (!var->is_assigned()) {
            all_assigned = false;
            break;
        }
    }
    if (all_assigned) {
        auto result = is_satisfied();
        if (result.has_value() && !result.value()) {
            return false;
        }
    }

    return true;
}

bool ConnectedConstraint::presolve(Model& /*model*/) {
    // subgraph 伝播: 辺が選択 → 両端点を選択
    for (size_t e = 0; e < n_edges_; ++e) {
        if (vars_[n_nodes_ + e]->is_assigned() && vars_[n_nodes_ + e]->assigned_value().value() == 1) {
            size_t u = static_cast<size_t>(from_[e]);
            size_t v = static_cast<size_t>(to_[e]);
            if (!vars_[u]->is_assigned()) {
                if (!vars_[u]->assign(1)) return false;
            } else if (vars_[u]->assigned_value().value() != 1) {
                return false;
            }
            if (!vars_[v]->is_assigned()) {
                if (!vars_[v]->assign(1)) return false;
            } else if (vars_[v]->assigned_value().value() != 1) {
                return false;
            }
        }
    }
    // subgraph 伝播: ノードが非選択 → 隣接辺を非選択
    for (size_t n = 0; n < n_nodes_; ++n) {
        if (vars_[n]->is_assigned() && vars_[n]->assigned_value().value() == 0) {
            for (size_t e_idx : adj_[n]) {
                if (!vars_[n_nodes_ + e_idx]->is_assigned()) {
                    if (!vars_[n_nodes_ + e_idx]->assign(0)) return false;
                } else if (vars_[n_nodes_ + e_idx]->assigned_value().value() != 0) {
                    return false;  // 端点が非選択なのに辺が選択 → 矛盾
                }
            }
        }
    }
    return true;
}

size_t ConnectedConstraint::uf_find(size_t x) const {
    while (uf_parent_[x] != x) {
        x = uf_parent_[x];
    }
    return x;
}

bool ConnectedConstraint::uf_union(size_t a, size_t b) {
    size_t ra = uf_find(a);
    size_t rb = uf_find(b);
    if (ra == rb) return false;

    // Union by rank
    if (uf_rank_[ra] < uf_rank_[rb]) std::swap(ra, rb);
    uf_parent_[rb] = ra;
    if (uf_rank_[ra] == uf_rank_[rb]) ++uf_rank_[ra];
    return true;
}

void ConnectedConstraint::save_trail_if_needed(Model& model, int save_point) {
    if (trail_.empty() || trail_.back().first != save_point) {
        trail_.push_back({save_point, TrailEntry{{}, n_selected_nodes_, n_selected_components_}});
        model.mark_constraint_dirty(model_index(), save_point);
    }
}

bool ConnectedConstraint::on_instantiate(Model& model, int save_point,
                                          size_t /*var_idx*/, size_t internal_var_idx,
                                          Domain::value_type value,
                                          Domain::value_type /*prev_min*/,
                                          Domain::value_type /*prev_max*/) {
    save_trail_if_needed(model, save_point);
    auto& trail_entry = trail_.back().second;

    if (is_node_var(internal_var_idx)) {
        size_t n = node_index(internal_var_idx);

        if (value == 1) {
            // ノード n が選択された
            ++n_selected_nodes_;
            ++n_selected_components_;  // 新しい孤立成分

            // 既に確定辺で隣接ノードと接続済みならUF union
            for (size_t e_idx : adj_[n]) {
                if (!vars_[n_nodes_ + e_idx]->is_assigned()) continue;
                if (vars_[n_nodes_ + e_idx]->assigned_value().value() != 1) continue;
                size_t other = (static_cast<size_t>(from_[e_idx]) == n)
                    ? static_cast<size_t>(to_[e_idx])
                    : static_cast<size_t>(from_[e_idx]);
                // other も選択済みであること
                if (!vars_[other]->is_assigned() || vars_[other]->assigned_value().value() != 1) continue;
                size_t ra = uf_find(n);
                size_t rb = uf_find(other);
                if (ra != rb) {
                    // UF undo 記録
                    if (uf_rank_[ra] < uf_rank_[rb]) std::swap(ra, rb);
                    trail_entry.uf_undos.push_back({rb, uf_parent_[rb], uf_rank_[ra]});
                    uf_parent_[rb] = ra;
                    if (uf_rank_[ra] == uf_rank_[rb]) ++uf_rank_[ra];
                    --n_selected_components_;
                }
            }
        } else {
            // ノード n が非選択 → 隣接辺を全て非選択に
            for (size_t e_idx : adj_[n]) {
                if (!vars_[n_nodes_ + e_idx]->is_assigned()) {
                    model.enqueue_instantiate(vars_[n_nodes_ + e_idx]->id(), 0);
                }
            }
        }
    } else {
        size_t e = edge_index(internal_var_idx);
        size_t u = static_cast<size_t>(from_[e]);
        size_t v = static_cast<size_t>(to_[e]);

        if (value == 1) {
            // 辺 e が選択 → 両端点を選択に
            if (!vars_[u]->is_assigned()) {
                model.enqueue_instantiate(vars_[u]->id(), 1);
            } else if (vars_[u]->assigned_value().value() != 1) {
                return false;  // 端点が非選択なのに辺が選択 → 矛盾
            }
            if (!vars_[v]->is_assigned()) {
                model.enqueue_instantiate(vars_[v]->id(), 1);
            } else if (vars_[v]->assigned_value().value() != 1) {
                return false;
            }

            // 両端点が選択済みなら UF union
            bool u_selected = vars_[u]->is_assigned() && vars_[u]->assigned_value().value() == 1;
            bool v_selected = vars_[v]->is_assigned() && vars_[v]->assigned_value().value() == 1;
            if (u_selected && v_selected) {
                size_t ra = uf_find(u);
                size_t rb = uf_find(v);
                if (ra != rb) {
                    if (uf_rank_[ra] < uf_rank_[rb]) std::swap(ra, rb);
                    trail_entry.uf_undos.push_back({rb, uf_parent_[rb], uf_rank_[ra]});
                    uf_parent_[rb] = ra;
                    if (uf_rank_[ra] == uf_rank_[rb]) ++uf_rank_[ra];
                    --n_selected_components_;
                }
            }
        }
        // value == 0: 辺が非選択 → 即座の伝播は不要
    }

    return true;
}

bool ConnectedConstraint::on_final_instantiate() {
    // 全変数確定時: BFS で連結性を検証
    auto result = is_satisfied();
    return result.has_value() && result.value();
}

void ConnectedConstraint::rewind_to(int save_point) {
    while (!trail_.empty() && trail_.back().first > save_point) {
        const auto& entry = trail_.back().second;

        // UF 操作を逆順に undo
        for (auto it = entry.uf_undos.rbegin(); it != entry.uf_undos.rend(); ++it) {
            // it->node = rb (merged node), current parent[rb] = ra
            size_t ra = uf_parent_[it->node];
            uf_rank_[ra] = it->old_rank;
            uf_parent_[it->node] = it->old_parent;
        }

        // カウンタを復元
        n_selected_nodes_ = entry.old_n_selected_nodes;
        n_selected_components_ = entry.old_n_selected_components;

        trail_.pop_back();
    }
}

void ConnectedConstraint::check_initial_consistency() {
    if (is_initially_inconsistent()) return;
    // 初期チェック: from/to のサイズが一致
    if (from_.size() != to_.size() || from_.size() != n_edges_) {
        set_initially_inconsistent(true);
    }
}

// ============================================================================

}  // namespace sabori_csp
