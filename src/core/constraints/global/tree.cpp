#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <limits>

namespace sabori_csp {

// ============================================================================
// TreeConstraint implementation
//
// 選択部分グラフ (ns, es) が根 r の無向木を成す制約。閉路は選択辺の union-find で
// 検出/予防し、連結性・辺数は全確定時に検証する（＋モデル側の辺数=ノード数-1 制約と協調）。
// union-find は毎回モデル状態から再構築するステートレス方式（backtrack 安全）。
// ============================================================================

namespace {
std::vector<VariablePtr> concat_vars(const std::vector<VariablePtr>& ns,
                                     const std::vector<VariablePtr>& es,
                                     const VariablePtr& r) {
    std::vector<VariablePtr> all;
    all.reserve(ns.size() + es.size() + 1);
    for (auto& v : ns) all.push_back(v);
    for (auto& v : es) all.push_back(v);
    all.push_back(r);
    return all;
}

size_t uf_find(const std::vector<size_t>& par, size_t x) {
    while (par[x] != x) x = par[x];
    return x;
}
}  // namespace

TreeConstraint::TreeConstraint(std::vector<VariablePtr> ns, std::vector<VariablePtr> es,
                               VariablePtr r, std::vector<int> from, std::vector<int> to)
    : Constraint(extract_var_ids(concat_vars(ns, es, r)))
    , n_(ns.size())
    , e_(es.size())
    // ノードID は 1..N（ns 配列は 1-based、from/to/r も 1-based ノードID）。
    // 辺端点の min ではなく常に 1（孤立ノードがあると辺 min が 1 でなくなり誤マップする）。
    , node_base_(1) {
    efrom_.resize(e_);
    eto_.resize(e_);
    incident_.assign(n_, {});
    for (size_t e = 0; e < e_; ++e) {
        efrom_[e] = static_cast<int>(from[e] - node_base_);
        eto_[e] = static_cast<int>(to[e] - node_base_);
        if (efrom_[e] >= 0 && static_cast<size_t>(efrom_[e]) < n_) incident_[efrom_[e]].push_back(e);
        if (eto_[e] >= 0 && static_cast<size_t>(eto_[e]) < n_) incident_[eto_[e]].push_back(e);
    }
}

std::string TreeConstraint::name() const { return "tree"; }

bool TreeConstraint::build_uf(const Model& model, std::vector<size_t>& par,
                              std::vector<size_t>& sz) const {
    par.resize(n_);
    sz.assign(n_, 1);
    for (size_t i = 0; i < n_; ++i) par[i] = i;
    for (size_t e = 0; e < e_; ++e) {
        size_t evid = var_ids_[es_idx(e)];
        if (!model.is_instantiated(evid) || model.value(evid) != 1) continue;
        size_t ra = uf_find(par, efrom_[e]), rb = uf_find(par, eto_[e]);
        if (ra == rb) return false;  // 閉路
        if (sz[ra] < sz[rb]) std::swap(ra, rb);
        par[rb] = ra; sz[ra] += sz[rb];
    }
    return true;
}

PresolveResult TreeConstraint::presolve(Model& model) {
    bool changed = false;
    size_t r_id = var_ids_[r_internal()];

    // 根: r 確定なら ns[r]=1
    if (model.variable(r_id)->is_assigned()) {
        auto rv = model.variable(r_id)->min() - node_base_;
        if (rv < 0 || static_cast<size_t>(rv) >= n_) return PresolveResult::Contradiction;
        auto* nv = model.variable(var_ids_[ns_idx(rv)]);
        if (!nv->is_assigned()) {
            if (!nv->assign(1)) return PresolveResult::Contradiction;
            changed = true;
        } else if (nv->min() != 1) {
            return PresolveResult::Contradiction;
        }
    }

    // ns[n]=0 → r≠n
    for (size_t node = 0; node < n_; ++node) {
        auto* nv = model.variable(var_ids_[ns_idx(node)]);
        if (nv->is_assigned() && nv->min() == 0) {
            auto rvcand = static_cast<Domain::value_type>(node) + node_base_;
            auto* rv = model.variable(r_id);
            if (rv->domain().contains(rvcand)) {
                if (!rv->remove(rvcand)) return PresolveResult::Contradiction;
                changed = true;
            }
        }
    }

    // 辺端点: es[e]=1 → ns[両端]=1 / ns[端点]=0 → es[e]=0
    for (size_t e = 0; e < e_; ++e) {
        auto* ev = model.variable(var_ids_[es_idx(e)]);
        auto* av = model.variable(var_ids_[ns_idx(efrom_[e])]);
        auto* bv = model.variable(var_ids_[ns_idx(eto_[e])]);
        if (ev->is_assigned() && ev->min() == 1) {
            for (auto* nv : {av, bv}) {
                if (!nv->is_assigned()) { if (!nv->assign(1)) return PresolveResult::Contradiction; changed = true; }
                else if (nv->min() != 1) return PresolveResult::Contradiction;
            }
        }
        if (!ev->is_assigned()) {
            bool a0 = av->is_assigned() && av->min() == 0;
            bool b0 = bv->is_assigned() && bv->min() == 0;
            if (a0 || b0) {
                if (!ev->assign(0)) return PresolveResult::Contradiction;
                changed = true;
            }
        }
    }

    // 閉路検出/予防
    {
        std::vector<size_t> par, sz;
        if (!build_uf(model, par, sz)) return PresolveResult::Contradiction;
        for (size_t e = 0; e < e_; ++e) {
            auto* ev = model.variable(var_ids_[es_idx(e)]);
            if (ev->is_assigned()) continue;
            if (uf_find(par, efrom_[e]) == uf_find(par, eto_[e])) {
                if (!ev->assign(0)) return PresolveResult::Contradiction;
                changed = true;
            }
        }
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool TreeConstraint::on_instantiate(Model& model, int save_point,
                                    size_t internal_var_idx,
                                    Domain::value_type value,
                                    Domain::value_type prev_min,
                                    Domain::value_type prev_max) {
    // 基底の 2WL watch 更新（has_uninstantiated が正しく動くために必須）
    if (!Constraint::on_instantiate(model, save_point, internal_var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    if (internal_var_idx < n_) {
        // ns[node]
        size_t node = internal_var_idx;
        if (value == 0) {
            for (size_t e : incident_[node]) {
                size_t evid = var_ids_[es_idx(e)];
                if (!model.is_instantiated(evid)) model.enqueue_remove_value(evid, 1);
                else if (model.value(evid) == 1) return false;
            }
            size_t r_id = var_ids_[r_internal()];
            auto rvcand = static_cast<Domain::value_type>(node) + node_base_;
            if (!model.is_instantiated(r_id)) {
                if (model.contains(r_id, rvcand)) model.enqueue_remove_value(r_id, rvcand);
            } else if (model.value(r_id) == rvcand) {
                return false;
            }
        }
    } else if (internal_var_idx < n_ + e_) {
        // es[edge]
        size_t e = internal_var_idx - n_;
        if (value == 1) {
            for (int nd : {efrom_[e], eto_[e]}) {
                size_t nvid = var_ids_[ns_idx(nd)];
                if (!model.is_instantiated(nvid)) model.enqueue_instantiate(nvid, 1);
                else if (model.value(nvid) != 1) return false;
            }
            // 閉路検出 + 予防（現在の選択辺から fresh に UF 構築）
            std::vector<size_t> par, sz;
            if (!build_uf(model, par, sz)) return false;  // 閉路
            for (size_t e2 = 0; e2 < e_; ++e2) {
                size_t evid = var_ids_[es_idx(e2)];
                if (model.is_instantiated(evid)) continue;
                if (uf_find(par, efrom_[e2]) == uf_find(par, eto_[e2])) {
                    model.enqueue_remove_value(evid, 1);
                }
            }
        }
    } else {
        // r（根）
        auto rv = value - node_base_;
        if (rv < 0 || rv >= static_cast<Domain::value_type>(n_)) return false;
        size_t nvid = var_ids_[ns_idx(static_cast<size_t>(rv))];
        if (!model.is_instantiated(nvid)) model.enqueue_instantiate(nvid, 1);
        else if (model.value(nvid) != 1) return false;
    }

    if (!has_uninstantiated(model)) {
        return on_final_instantiate(model);
    }
    return true;
}

bool TreeConstraint::on_final_instantiate(const Model& model) {
    size_t r_id = var_ids_[r_internal()];
    if (!model.is_instantiated(r_id)) return false;
    auto rv = model.value(r_id) - node_base_;
    if (rv < 0 || static_cast<size_t>(rv) >= n_) return false;
    if (model.value(var_ids_[ns_idx(static_cast<size_t>(rv))]) != 1) return false;

    std::vector<size_t> par(n_), sz(n_, 1);
    for (size_t i = 0; i < n_; ++i) par[i] = i;
    size_t sel_nodes = 0, sel_edges = 0;
    for (size_t node = 0; node < n_; ++node) {
        if (model.value(var_ids_[ns_idx(node)]) == 1) sel_nodes++;
    }
    for (size_t e = 0; e < e_; ++e) {
        if (model.value(var_ids_[es_idx(e)]) != 1) continue;
        sel_edges++;
        size_t a = efrom_[e], b = eto_[e];
        if (model.value(var_ids_[ns_idx(a)]) != 1 || model.value(var_ids_[ns_idx(b)]) != 1) return false;
        size_t ra = uf_find(par, a), rb = uf_find(par, b);
        if (ra == rb) return false;  // 閉路
        if (sz[ra] < sz[rb]) std::swap(ra, rb);
        par[rb] = ra; sz[ra] += sz[rb];
    }
    if (sel_edges + 1 != sel_nodes) return false;  // 木: 辺数 = ノード数 - 1
    size_t root_comp = uf_find(par, static_cast<size_t>(rv));
    for (size_t node = 0; node < n_; ++node) {
        if (model.value(var_ids_[ns_idx(node)]) == 1 && uf_find(par, node) != root_comp) return false;
    }
    return true;
}

}  // namespace sabori_csp
