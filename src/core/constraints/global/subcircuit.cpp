#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <vector>

namespace sabori_csp {

// ============================================================================
// SubcircuitConstraint
//
// subcircuit(x): x[i] == i（自己ループ）は「閉路に入らない」。x[i] != i の
// ノード全体がちょうど1つの閉路を形成する（空も可）。alldifferent は registry が
// 併設するため、ここでは閉路構造の推論のみ行う。
//
// 全再計算 batch（確定した非自己エッジのグラフを毎回歩く。alldiff 併設下では
// 入次数 ≤ 1 なので、確定エッジ群は互いに素なパスと閉路に分解される）:
//   R1: 極大パス s→…→e に対し、パス外に must-in ノード（i ∉ dom(x_i)）が
//       あれば x[e] != s（早期閉路の禁止）
//   R2: 閉路が確定したら、外側の全ノードを自己ループに強制
// ============================================================================

SubcircuitConstraint::SubcircuitConstraint(std::vector<VariablePtr> vars,
                                           int64_t index_offset)
    : Constraint(extract_var_ids(vars))
    , n_(vars.size())
    , index_offset_(index_offset) {
}

std::string SubcircuitConstraint::name() const {
    return "sabori_subcircuit";
}

bool SubcircuitConstraint::propagate_impl(Model& model, bool direct, bool& changed) {
    if (n_ == 0) return true;

    auto vassigned = [&](size_t id) -> bool {
        return direct ? model.variable(id)->is_assigned() : model.is_instantiated(id);
    };
    auto vvalue = [&](size_t id) -> Domain::value_type {
        return direct ? model.variable(id)->assigned_value().value() : model.value(id);
    };
    auto vcontains = [&](size_t id, Domain::value_type v) -> bool {
        return direct ? model.variable(id)->domain().contains(v)
                      : model.contains(id, v);
    };
    auto rem_val = [&](size_t id, Domain::value_type v) -> bool {
        if (direct) return model.variable(id)->remove(v);
        model.enqueue_remove_value(id, v);
        return true;
    };
    auto assign_val = [&](size_t id, Domain::value_type v) -> bool {
        if (direct) return model.variable(id)->assign(v);
        model.enqueue_instantiate(id, v);
        return true;
    };
    auto node_val = [&](size_t i) -> Domain::value_type {
        return static_cast<Domain::value_type>(static_cast<int64_t>(i) + index_offset_);
    };

    // --- Step 0: 確定した非自己エッジと must-in 集合を収集 ---
    // succ[i] = 確定後続の内部 index（非自己エッジのみ）、なければ n_
    // must_in[i] = 自己ループが domain から消えている（閉路に入らざるを得ない）
    std::vector<size_t> succ(n_, n_);
    std::vector<uint8_t> must_in(n_, 0);
    std::vector<uint8_t> has_pred(n_, 0);  // 確定非自己エッジの入次数 > 0
    size_t must_in_count = 0;
    for (size_t i = 0; i < n_; ++i) {
        size_t id = var_ids_[i];
        if (!vcontains(id, node_val(i))) {
            must_in[i] = 1;
            ++must_in_count;
        }
        if (vassigned(id)) {
            auto v = vvalue(id);
            int64_t j = static_cast<int64_t>(v) - index_offset_;
            if (j < 0 || static_cast<size_t>(j) >= n_) return false;  // 値域外
            if (static_cast<size_t>(j) != i) {
                succ[i] = static_cast<size_t>(j);
                has_pred[j] = 1;
            }
        }
    }

    // --- Step 1: 確定エッジのグラフを歩く（alldiff 併設下では入次数 ≤ 1 の
    // 前提だが、伝播途中の重複にも停止保証を持たせるため visited で防御）---
    // path_id[i]: ノード i が属するパス/閉路の識別子（歩いた head の index）
    std::vector<size_t> comp(n_, n_);   // 所属 component（head index で識別）
    std::vector<uint8_t> visited(n_, 0);
    size_t closed_cycle_head = n_;      // 確定閉路が見つかったらその head

    // 1a: head（入次数 0 で非自己エッジを持つ）から歩く
    struct PathInfo {
        size_t head, tail;   // tail = パス終端（succ 未確定のノード）
        size_t must_in_on_path;
    };
    std::vector<PathInfo> paths;
    for (size_t h = 0; h < n_; ++h) {
        if (succ[h] == n_ || has_pred[h] || visited[h]) continue;
        size_t cur = h, must_cnt = 0;
        while (true) {
            if (visited[cur]) break;  // 防御（重複ターゲット時）
            visited[cur] = 1;
            comp[cur] = h;
            if (must_in[cur]) ++must_cnt;
            if (succ[cur] == n_) break;  // tail 到達
            cur = succ[cur];
        }
        paths.push_back({h, cur, must_cnt});
    }
    // 1b: 未訪問で確定エッジを持つノード = 閉路のメンバー候補。
    // 歩いて自分の component に戻ったときだけ真の閉路（既存パスへの合流は
    // 入次数 ≥ 2 の一時状態で、alldiff 併設側が処理するのでここでは無視）
    for (size_t s = 0; s < n_; ++s) {
        if (succ[s] == n_ || visited[s]) continue;
        size_t cur = s;
        bool closed = false;
        while (true) {
            if (visited[cur]) {
                closed = (comp[cur] == s);
                break;
            }
            visited[cur] = 1;
            comp[cur] = s;
            if (succ[cur] == n_) break;  // tail 到達（閉じていない）
            cur = succ[cur];
        }
        if (closed) {
            if (closed_cycle_head != n_) return false;  // 2つ目の閉路
            closed_cycle_head = s;
        }
    }

    // --- Step 2 (R2): 閉路が確定していたら、外側の全ノードを自己ループに ---
    if (closed_cycle_head != n_) {
        for (size_t k = 0; k < n_; ++k) {
            if (comp[k] == closed_cycle_head) continue;
            if (must_in[k]) return false;  // 閉路外なのに自己ループ不可
            size_t id = var_ids_[k];
            if (vassigned(id)) {
                if (vvalue(id) != node_val(k)) return false;  // 閉路外の非自己エッジ
                continue;
            }
            if (!assign_val(id, node_val(k))) return false;
            changed = true;
        }
        return true;
    }

    // --- Step 2.5 (R3): 確定した非自己エッジの先は閉路に入る ---
    // x[i] = j (i != j) なら j は in-circuit なので自己ループを除去する
    for (size_t i = 0; i < n_; ++i) {
        if (succ[i] == n_) continue;
        size_t j = succ[i];
        size_t jid = var_ids_[j];
        if (vcontains(jid, node_val(j))) {
            if (!rem_val(jid, node_val(j))) return false;
            changed = true;
        }
    }

    // --- Step 3 (R1): 各極大パス s→…→e について早期閉路を禁止 ---
    // パス外に must-in ノードがあるとき、e から s へ戻るエッジは張れない。
    for (const auto& p : paths) {
        if (p.head == p.tail) continue;  // 単独ノード（エッジなし）は対象外
        bool outside_must = (must_in_count > p.must_in_on_path);
        if (!outside_must) continue;
        size_t tail_id = var_ids_[p.tail];
        if (!vassigned(tail_id) && vcontains(tail_id, node_val(p.head))) {
            if (!rem_val(tail_id, node_val(p.head))) return false;
            changed = true;
        }
    }

    return true;
}

PresolveResult SubcircuitConstraint::presolve(Model& model) {
    bool changed = false;
    if (!propagate_impl(model, /*direct=*/true, changed)) {
        return PresolveResult::Contradiction;
    }
    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool SubcircuitConstraint::prepare_propagation(Model& model) {
    init_watches();
    return true;
}

bool SubcircuitConstraint::on_instantiate(
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

bool SubcircuitConstraint::on_set_min(
    Model& model, int /*save_point*/, size_t /*internal_var_idx*/,
    Domain::value_type /*new_min*/, Domain::value_type /*old_min*/) {
    model.schedule_constraint_batch(model_index());
    return true;
}

bool SubcircuitConstraint::on_set_max(
    Model& model, int /*save_point*/, size_t /*internal_var_idx*/,
    Domain::value_type /*new_max*/, Domain::value_type /*old_max*/) {
    model.schedule_constraint_batch(model_index());
    return true;
}

bool SubcircuitConstraint::on_remove_value(
    Model& model, int /*save_point*/, size_t /*internal_var_idx*/,
    Domain::value_type /*removed_value*/) {
    // must-in（自己ループ値の消滅）の検出に必要
    model.schedule_constraint_batch(model_index());
    return true;
}

bool SubcircuitConstraint::propagate_batch(Model& model, int /*save_point*/) {
    bool changed = false;
    return propagate_impl(model, /*direct=*/false, changed);
}

bool SubcircuitConstraint::on_final_instantiate(const Model& model) {
    // 非自己ノードを収集
    size_t start = n_, in_count = 0;
    for (size_t i = 0; i < n_; ++i) {
        auto v = model.value(var_ids_[i]);
        int64_t j = static_cast<int64_t>(v) - index_offset_;
        if (j < 0 || static_cast<size_t>(j) >= n_) return false;
        if (static_cast<size_t>(j) != i) {
            ++in_count;
            start = i;
        }
    }
    if (in_count == 0) return true;  // 空 subcircuit
    // start から閉路を辿り、非自己ノードをちょうど in_count 個踏んで戻るか
    size_t cur = start, steps = 0;
    while (steps <= in_count) {
        auto v = model.value(var_ids_[cur]);
        size_t j = static_cast<size_t>(static_cast<int64_t>(v) - index_offset_);
        if (j == cur) return false;  // 閉路の途中で自己ループに落ちた
        ++steps;
        cur = j;
        if (cur == start) break;
    }
    return cur == start && steps == in_count;
}

void SubcircuitConstraint::rewind_to(int /*save_point*/) {
    // ステートレス — 復元不要
}

}  // namespace sabori_csp
