#include "sabori_csp/community_analysis.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <numeric>
#include <unordered_map>

namespace sabori_csp {

void CommunityAnalysis::build_vig(const Model& model) {
    const auto& variables = model.variables();
    const auto& constraints = model.constraints();
    size_t n = variables.size();
    vig_.num_vars = n;

    if (n == 0) return;

    // ペアごとの重み（共有制約数）をカウント
    // key: (min(i,j) * n + max(i,j)) → weight
    std::unordered_map<size_t, size_t> edge_weights;

    for (const auto& constraint : constraints) {
        const auto& var_ids = constraint->var_ids_ref();

        // アリティ > 200 の制約はスキップ（O(n^2) ペア生成を回避）
        if (var_ids.size() > 200) continue;

        // 確定済みでない変数のIDを収集
        std::vector<size_t> active_vars;
        active_vars.reserve(var_ids.size());
        for (size_t vid : var_ids) {
            if (!model.is_instantiated(vid)) {
                active_vars.push_back(vid);
            }
        }

        // ペアを列挙
        for (size_t i = 0; i < active_vars.size(); ++i) {
            for (size_t j = i + 1; j < active_vars.size(); ++j) {
                size_t vi = active_vars[i];
                size_t vj = active_vars[j];
                size_t key = (vi < vj) ? (vi * n + vj) : (vj * n + vi);
                edge_weights[key]++;
            }
        }
    }

    // CSR形式に変換
    // まず各頂点の隣接リストを構築
    std::vector<std::vector<std::pair<size_t, size_t>>> adj(n);
    for (const auto& [key, weight] : edge_weights) {
        size_t vi = key / n;
        size_t vj = key % n;
        adj[vi].emplace_back(vj, weight);
        adj[vj].emplace_back(vi, weight);
    }

    // CSR に変換
    vig_.row_ptr.resize(n + 1, 0);
    size_t total_edges = 0;
    for (size_t i = 0; i < n; ++i) {
        vig_.row_ptr[i] = total_edges;
        total_edges += adj[i].size();
    }
    vig_.row_ptr[n] = total_edges;

    vig_.col_idx.resize(total_edges);
    vig_.weights.resize(total_edges);
    for (size_t i = 0; i < n; ++i) {
        size_t offset = vig_.row_ptr[i];
        for (size_t j = 0; j < adj[i].size(); ++j) {
            vig_.col_idx[offset + j] = adj[i][j].first;
            vig_.weights[offset + j] = adj[i][j].second;
        }
    }
}

void CommunityAnalysis::detect_communities(std::mt19937& rng, size_t max_iterations) {
    size_t n = vig_.num_vars;
    if (n == 0) return;

    // 初期化: 各変数が独立コミュニティ
    structure_.community.resize(n);
    std::iota(structure_.community.begin(), structure_.community.end(), 0);

    // ランダム順序用のインデックス配列
    std::vector<size_t> order(n);
    std::iota(order.begin(), order.end(), 0);

    for (size_t iter = 0; iter < max_iterations; ++iter) {
        bool changed = false;
        std::shuffle(order.begin(), order.end(), rng);

        for (size_t v : order) {
            if (vig_.degree(v) == 0) continue;

            // 隣接変数のコミュニティを重み付きで集計
            std::unordered_map<size_t, size_t> community_weights;
            for (size_t e = vig_.row_ptr[v]; e < vig_.row_ptr[v + 1]; ++e) {
                size_t neighbor = vig_.col_idx[e];
                size_t w = vig_.weights[e];
                community_weights[structure_.community[neighbor]] += w;
            }

            // 最頻コミュニティを選択
            size_t best_community = structure_.community[v];
            size_t best_weight = 0;
            for (const auto& [comm, w] : community_weights) {
                if (w > best_weight || (w == best_weight && comm < best_community)) {
                    best_weight = w;
                    best_community = comm;
                }
            }

            if (best_community != structure_.community[v]) {
                structure_.community[v] = best_community;
                changed = true;
            }
        }

        if (!changed) break;
    }

    // コミュニティIDを 0..k-1 に正規化
    std::unordered_map<size_t, size_t> id_map;
    size_t next_id = 0;
    for (size_t v = 0; v < n; ++v) {
        auto it = id_map.find(structure_.community[v]);
        if (it == id_map.end()) {
            id_map[structure_.community[v]] = next_id;
            structure_.community[v] = next_id;
            next_id++;
        } else {
            structure_.community[v] = it->second;
        }
    }
    structure_.num_communities = next_id;

    // モジュラリティ Q を計算
    // Q = (1/2m) * Σ_ij [A_ij - k_i*k_j/(2m)] * δ(c_i, c_j)
    size_t total_weight = 0;
    for (size_t e = 0; e < vig_.weights.size(); ++e) {
        total_weight += vig_.weights[e];
    }
    double m2 = static_cast<double>(total_weight);  // 2m (各辺が両方向に格納されている)
    // LocalityStats のコミュニティ別カウンタを初期化
    stats_.community_decision_count.assign(structure_.num_communities, 0);

    if (m2 == 0) {
        structure_.modularity = 0.0;
        structure_.intra_edges = 0;
        structure_.inter_edges = 0;
        return;
    }

    // 各頂点の重み付き次数
    std::vector<double> strength(n, 0.0);
    for (size_t v = 0; v < n; ++v) {
        for (size_t e = vig_.row_ptr[v]; e < vig_.row_ptr[v + 1]; ++e) {
            strength[v] += vig_.weights[e];
        }
    }

    double Q = 0.0;
    structure_.intra_edges = 0;
    structure_.inter_edges = 0;

    for (size_t v = 0; v < n; ++v) {
        for (size_t e = vig_.row_ptr[v]; e < vig_.row_ptr[v + 1]; ++e) {
            size_t u = vig_.col_idx[e];
            if (v >= u) continue;  // 各辺を1回だけカウント
            double w = static_cast<double>(vig_.weights[e]);
            if (structure_.community[v] == structure_.community[u]) {
                Q += 2.0 * (w - strength[v] * strength[u] / m2);
                structure_.intra_edges++;
            } else {
                Q += 2.0 * (0.0 - strength[v] * strength[u] / m2);
                structure_.inter_edges++;
            }
        }
    }
    structure_.modularity = Q / m2;

    // community_vars_ を構築
    community_vars_.assign(structure_.num_communities, {});
    for (size_t v = 0; v < n; ++v) {
        if (structure_.community[v] < structure_.num_communities) {
            community_vars_[structure_.community[v]].push_back(v);
        }
    }

    // top_communities_ を構築（サイズ降順、上位5件）
    std::vector<std::pair<size_t, size_t>> comm_sizes;  // (size, community_id)
    comm_sizes.reserve(structure_.num_communities);
    for (size_t c = 0; c < structure_.num_communities; ++c) {
        if (!community_vars_[c].empty()) {
            comm_sizes.emplace_back(community_vars_[c].size(), c);
        }
    }
    std::sort(comm_sizes.begin(), comm_sizes.end(), std::greater<>());
    top_communities_.clear();
    for (size_t i = 0; i < std::min(comm_sizes.size(), size_t(5)); ++i) {
        top_communities_.push_back(comm_sizes[i].second);
    }
}

void CommunityAnalysis::on_decision(size_t var_idx) {
    if (var_idx >= structure_.community.size()) return;

    stats_.total_decisions++;
    size_t comm = structure_.community[var_idx];

    if (comm < stats_.community_decision_count.size()) {
        stats_.community_decision_count[comm]++;
    }

    if (last_decision_var_ != SIZE_MAX && last_decision_var_ < structure_.community.size()) {
        if (structure_.community[last_decision_var_] == comm) {
            stats_.same_community_decisions++;
        } else {
            stats_.cross_community_decisions++;
        }
    }

    last_decision_var_ = var_idx;
}

void CommunityAnalysis::on_propagation(size_t changed_var, size_t source_var) {
    if (changed_var >= structure_.community.size() ||
        source_var >= structure_.community.size()) return;

    stats_.total_propagation_events++;

    if (structure_.community[changed_var] == structure_.community[source_var]) {
        stats_.same_community_propagations++;
    } else {
        stats_.cross_community_propagations++;
    }
}

void CommunityAnalysis::reset_stats() {
    stats_.total_decisions = 0;
    stats_.same_community_decisions = 0;
    stats_.cross_community_decisions = 0;
    stats_.total_propagation_events = 0;
    stats_.same_community_propagations = 0;
    stats_.cross_community_propagations = 0;
    if (structure_.num_communities > 0) {
        stats_.community_decision_count.assign(structure_.num_communities, 0);
    }
    last_decision_var_ = SIZE_MAX;
}

void CommunityAnalysis::print_static_report(std::ostream& os) const {
    // VIG 情報
    size_t total_edges = 0;
    size_t max_degree = 0;
    for (size_t v = 0; v < vig_.num_vars; ++v) {
        size_t d = vig_.degree(v);
        total_edges += d;
        if (d > max_degree) max_degree = d;
    }
    total_edges /= 2;  // 無向グラフなので半分
    double avg_degree = vig_.num_vars > 0
        ? static_cast<double>(total_edges * 2) / vig_.num_vars
        : 0.0;

    os << "% [community] VIG: " << vig_.num_vars << " vars, "
       << total_edges << " edges, avg_degree="
       << std::fixed << std::setprecision(1) << avg_degree
       << ", max_degree=" << max_degree << "\n";

    // コミュニティ情報
    os << "% [community] Communities: " << structure_.num_communities
       << " (modularity Q=" << std::fixed << std::setprecision(2)
       << structure_.modularity << ")\n";

    // コミュニティサイズ
    std::vector<size_t> sizes(structure_.num_communities, 0);
    for (size_t v = 0; v < structure_.community.size(); ++v) {
        if (structure_.community[v] < sizes.size()) {
            sizes[structure_.community[v]]++;
        }
    }
    std::sort(sizes.begin(), sizes.end(), std::greater<>());

    os << "% [community] Sizes: [";
    for (size_t i = 0; i < sizes.size() && i < 20; ++i) {
        if (i > 0) os << ", ";
        os << sizes[i];
    }
    if (sizes.size() > 20) os << ", ...";
    os << "]\n";

    // Intra/inter edges
    size_t total = structure_.intra_edges + structure_.inter_edges;
    if (total > 0) {
        double intra_pct = 100.0 * structure_.intra_edges / total;
        double inter_pct = 100.0 * structure_.inter_edges / total;
        os << "% [community] Intra-edges: " << structure_.intra_edges
           << " (" << std::fixed << std::setprecision(1) << intra_pct
           << "%), inter-edges: " << structure_.inter_edges
           << " (" << std::fixed << std::setprecision(1) << inter_pct << "%)\n";
    }
}

void CommunityAnalysis::print_dynamic_report(std::ostream& os, size_t restart_num) const {
    if (stats_.total_decisions == 0 && stats_.total_propagation_events == 0) return;

    os << "% [locality] restart#" << restart_num
       << ": decisions=" << stats_.total_decisions;
    if (stats_.total_decisions > 0) {
        size_t comparable = stats_.same_community_decisions + stats_.cross_community_decisions;
        if (comparable > 0) {
            double local_pct = 100.0 * stats_.same_community_decisions / comparable;
            os << " local=" << stats_.same_community_decisions
               << "(" << std::fixed << std::setprecision(1) << local_pct << "%)"
               << " cross=" << stats_.cross_community_decisions
               << "(" << std::fixed << std::setprecision(1) << (100.0 - local_pct) << "%)";
        }
    }
    os << "\n";

    if (stats_.total_propagation_events > 0) {
        double local_pct = 100.0 * stats_.same_community_propagations / stats_.total_propagation_events;
        os << "% [locality]   propagations=" << stats_.total_propagation_events
           << " local=" << stats_.same_community_propagations
           << "(" << std::fixed << std::setprecision(1) << local_pct << "%)"
           << " cross=" << stats_.cross_community_propagations
           << "(" << std::fixed << std::setprecision(1) << (100.0 - local_pct) << "%)\n";
    }

    // コミュニティ別判定回数（非ゼロのみ、上位20件を回数降順で表示）
    std::vector<std::pair<size_t, size_t>> nonzero_communities;  // (count, community_id)
    for (size_t c = 0; c < stats_.community_decision_count.size(); ++c) {
        if (stats_.community_decision_count[c] > 0) {
            nonzero_communities.emplace_back(stats_.community_decision_count[c], c);
        }
    }
    if (!nonzero_communities.empty()) {
        std::sort(nonzero_communities.begin(), nonzero_communities.end(), std::greater<>());
        os << "% [locality]   community_decisions: [";
        for (size_t i = 0; i < nonzero_communities.size() && i < 20; ++i) {
            if (i > 0) os << ", ";
            os << nonzero_communities[i].second << ":" << nonzero_communities[i].first;
        }
        if (nonzero_communities.size() > 20) os << ", ...";
        os << "]\n";
    }
}

const std::vector<size_t>& CommunityAnalysis::top_communities(size_t /*n*/) const {
    return top_communities_;
}

const std::vector<size_t>& CommunityAnalysis::community_vars(size_t community_id) const {
    static const std::vector<size_t> empty;
    if (community_id >= community_vars_.size()) return empty;
    return community_vars_[community_id];
}

} // namespace sabori_csp
