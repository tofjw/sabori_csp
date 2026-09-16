#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <vector>

namespace sabori_csp {

// ============================================================================
// ValuePrecedeConstraint
//
// value_precede(s, t, x): x[j] == t なら ∃i<j x[i] == s。
// ステートレスな全再計算 batch propagator（bin_packing_load と同方式）。
// 規則（Law & Lee 2004 の α/γ を全再計算で）:
//   Step 0: s == t の縮退 → s は一切出現できない
//   Step 1: α = s を取り得る最小 index。i ≤ α（α 無しなら全域）から t を除去
//   Step 2: γ = t に確定した最小 index。存在すれば α < γ が必要で、
//           γ より前の s サポートが α のみなら x[α] := s
// ============================================================================

ValuePrecedeConstraint::ValuePrecedeConstraint(Domain::value_type s,
                                               Domain::value_type t,
                                               std::vector<VariablePtr> xs)
    : Constraint(extract_var_ids(xs))
    , s_(s)
    , t_(t)
    , n_(xs.size()) {
}

std::string ValuePrecedeConstraint::name() const {
    return "sabori_value_precede";
}

bool ValuePrecedeConstraint::propagate_impl(Model& model, bool direct, bool& changed,
                                            int save_point) {
    if (n_ == 0) return true;

    // direct(presolve) は Variable を直接操作、search は enqueue を使う
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

    // --- Step 0: s == t の縮退。x[j]==s は自分より前の s を要求し、最初の
    // 出現が常に違反するので s はどこにも出現できない ---
    if (s_ == t_) {
        for (size_t i = 0; i < n_; ++i) {
            size_t id = var_ids_[i];
            if (vcontains(id, s_)) {
                if (!rem_val(id, s_)) return false;
                changed = true;
            }
        }
        // s の除去を全域に enqueue 済み → 以後常に充足
        // （矛盾する代入はドメイン層で検出される）
        if (!direct) model.set_constraint_entailed(model_index(), save_point);
        return true;
    }

    // --- Step 1: α = s を取り得る最小 index。i ≤ α から t を除去 ---
    // （i < α に s は来られず、i == α でも「t より厳密に前」の s が必要）
    // α が存在しない場合は全域から t を除去。
    size_t alpha = n_;
    for (size_t i = 0; i < n_; ++i) {
        if (vcontains(var_ids_[i], s_)) {
            alpha = i;
            break;
        }
    }
    const size_t t_limit = (alpha == n_) ? n_ : alpha + 1;  // [0, t_limit) から t 除去
    for (size_t i = 0; i < t_limit; ++i) {
        size_t id = var_ids_[i];
        if (vcontains(id, t_)) {
            if (!rem_val(id, t_)) return false;
            changed = true;
        }
    }

    // --- Entailment (a): x[α] が s に確定していれば、[0..α] から t は除去済み
    // なので「最初に t が来得る位置より前に s が確定」= 以後常に充足
    // （community-detection で scheduling 呼び出しの 99.8% がこの状態への着弾）
    if (!direct && alpha < n_) {
        size_t aid = var_ids_[alpha];
        if (vassigned(aid) && vvalue(aid) == s_) {
            model.set_constraint_entailed(model_index(), save_point);
            return true;
        }
    }

    // --- Step 2: γ = t に確定した最小 index（t_any: t が残存するか）---
    size_t gamma = n_;
    bool t_any = false;
    for (size_t i = 0; i < n_; ++i) {
        size_t id = var_ids_[i];
        if (vcontains(id, t_)) t_any = true;
        if (vassigned(id) && vvalue(id) == t_) {
            gamma = i;
            break;
        }
    }
    // Entailment (b): t がどのドメインにも無い → 以後常に充足
    if (!direct && !t_any && gamma == n_) {
        model.set_constraint_entailed(model_index(), save_point);
        return true;
    }
    if (gamma < n_) {
        // s は γ より厳密に前に必要
        if (alpha >= gamma) return false;
        // γ より前の s サポートが α のみなら x[α] := s
        size_t support_count = 0;
        for (size_t i = 0; i < gamma; ++i) {
            if (vcontains(var_ids_[i], s_)) {
                ++support_count;
                if (support_count > 1) break;
            }
        }
        if (support_count == 1) {
            size_t aid = var_ids_[alpha];
            if (!vassigned(aid) || vvalue(aid) != s_) {
                if (!assign_val(aid, s_)) return false;
                changed = true;
            }
        }
    }

    return true;
}

PresolveResult ValuePrecedeConstraint::presolve(Model& model) {
    bool changed = false;
    if (!propagate_impl(model, /*direct=*/true, changed)) {
        return PresolveResult::Contradiction;
    }
    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool ValuePrecedeConstraint::prepare_propagation(Model& model) {
    init_watches();
    return true;
}

bool ValuePrecedeConstraint::on_instantiate(
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

bool ValuePrecedeConstraint::on_set_min(
    Model& model, int /*save_point*/, size_t /*internal_var_idx*/,
    Domain::value_type /*new_min*/, Domain::value_type /*old_min*/) {
    model.schedule_constraint_batch(model_index());
    return true;
}

bool ValuePrecedeConstraint::on_set_max(
    Model& model, int /*save_point*/, size_t /*internal_var_idx*/,
    Domain::value_type /*new_max*/, Domain::value_type /*old_max*/) {
    model.schedule_constraint_batch(model_index());
    return true;
}

bool ValuePrecedeConstraint::on_remove_value(
    Model& model, int /*save_point*/, size_t /*internal_var_idx*/,
    Domain::value_type /*removed_value*/) {
    model.schedule_constraint_batch(model_index());
    return true;
}

bool ValuePrecedeConstraint::propagate_batch(Model& model, int save_point) {
    bool changed = false;
    return propagate_impl(model, /*direct=*/false, changed, save_point);
}

bool ValuePrecedeConstraint::on_final_instantiate(const Model& model) {
    if (s_ == t_) {
        // 縮退: s はどこにも出現できない
        for (size_t i = 0; i < n_; ++i) {
            if (model.value(var_ids_[i]) == s_) return false;
        }
        return true;
    }
    for (size_t i = 0; i < n_; ++i) {
        auto v = model.value(var_ids_[i]);
        if (v == s_) return true;   // s が先に出現
        if (v == t_) return false;  // s より先に t が出現
    }
    return true;  // t が出現しない
}

void ValuePrecedeConstraint::rewind_to(int /*save_point*/) {
    // ステートレス — 復元不要
}

}  // namespace sabori_csp
