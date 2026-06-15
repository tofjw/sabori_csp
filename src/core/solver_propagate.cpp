#include "sabori_csp/solver.hpp"
#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/one_hot_channel_aggregator.hpp"
#include <algorithm>
#include <limits>
#include <numeric>
#include <iomanip>
#include <iostream>

namespace sabori_csp {

// 伝播エンジン（process_queue / propagate_instantiate / unit nogood 適用）。solver.cpp から分離。


PropagationResult Solver::apply_unit_nogoods(Model& model) {
    if (nogood_mgr_.unit_nogoods().empty()) return PropagationResult::Ok;
    nogood_mgr_.enqueue_unit_nogoods(model);
    return process_queue(model);
}


bool Solver::propagate_instantiate(Model& model, size_t var_idx,
                                    Domain::value_type prev_min, Domain::value_type prev_max) {
    var_selector_.mark_assigned(var_idx);
    const auto& constraints = model.constraints();
    auto val = model.value(var_idx);

    const auto& constraint_indices = model.constraints_for_var(var_idx);
    for (const auto& w : constraint_indices) {
        if (!record_constraint_call(model, w.constraint_idx, var_idx, [&]{
            return constraints[w.constraint_idx]->on_instantiate(model, current_decision_,
                        w.internal_var_idx, val, prev_min, prev_max);
        })) {
            return false;
        }
    }

    // NoGood チェック
    if (nogood_learning_) {
        if (!nogood_mgr_.propagate_eq_watches(model, var_idx, val,
                                               stats_.restart_count, activity_, activity_inc_)) {
            return false;
        }
        // instantiate は Leq/Geq 両方を充足しうる
        if (!nogood_mgr_.propagate_bound_nogoods(model, var_idx, true,
                                                  stats_.restart_count, activity_, activity_inc_)) {
            return false;
        }
        if (!nogood_mgr_.propagate_bound_nogoods(model, var_idx, false,
                                                  stats_.restart_count, activity_, activity_inc_)) {
            return false;
        }
    }

    return true;
}

PropagationResult Solver::process_queue(Model& model) {
    const auto& constraints = model.constraints();

    // verbose 統計記録 + コールバック呼び出し + 失敗時 bump_activity は record_constraint_call
    // (ConstraintStats レイヤ) に集約。invoke_cb は (v_idx, w, call) → bool の薄いアダプタ。
    auto invoke_cb = [&](size_t v_idx, const auto& w, auto&& call) -> bool {
        return record_constraint_call(model, w.constraint_idx, v_idx,
                                      std::forward<decltype(call)>(call));
    };

    for (;;) {
    while (model.has_pending_updates()) {
        if (stopped_) return PropagationResult::Stopped;

        // ここの update の内容は、まだ変数に反映されていないことに注意
        auto update = model.pop_pending_update();
        size_t var_idx = update.var_idx;

        // 操作前の状態を保存
        auto prev_min = model.var_min(var_idx);
        auto prev_max = model.var_max(var_idx);

        // instantiated なら以下のいずれか
        // * 最初から定数か
        // * 過去に instantiate で処理された
        bool was_instantiated = model.is_instantiated(var_idx);

        switch (update.type) {
        case PendingUpdate::Type::Instantiate: {
            if (was_instantiated) {
                // 既に確定済みで異なる値が要求されている場合は矛盾
                if (model.value(var_idx) != update.value) {
                    return PropagationResult::Conflict;
                }
                // 同じ値で既に確定済み: ドメイン削減で確定した
                // 二重に同じイベントを呼ばない
                continue;
            }
            if (!model.instantiate(current_decision_, var_idx, update.value)) {
                return PropagationResult::Conflict;
            }
            if (verbose_ && community_analysis_.is_enabled() && propagation_source_ != SIZE_MAX) {
                community_analysis_.on_propagation(var_idx, propagation_source_);
            }
            if (!propagate_instantiate(model, var_idx, prev_min, prev_max)) {
                return PropagationResult::Conflict;
            }
            break;
        }
        case PendingUpdate::Type::SetMin: {
            if (update.value <= prev_min) continue;  // 変化なし
            if (!model.set_min(current_decision_, var_idx, update.value)) {
                return PropagationResult::Conflict;
            }
            if (verbose_ && community_analysis_.is_enabled() && propagation_source_ != SIZE_MAX) {
                community_analysis_.on_propagation(var_idx, propagation_source_);
            }
            // 確定した場合は on_instantiate、そうでなければ on_set_min
            if (!was_instantiated && model.is_instantiated(var_idx)) {
                if (!propagate_instantiate(model, var_idx, prev_min, prev_max)) {
                    return PropagationResult::Conflict;
                }
            } else if (!was_instantiated) {
                // ドメインのholeにより実際のminは要求値より大きい場合がある
                auto actual_new_min = model.var_min(var_idx);
                const auto& constraint_indices = model.constraints_for_var(var_idx);
                for (const auto& w : constraint_indices) {
                    if (!invoke_cb(var_idx, w, [&]{
                        return constraints[w.constraint_idx]->on_set_min(model, current_decision_,
                            w.internal_var_idx, actual_new_min, prev_min);
                    })) {
                        return PropagationResult::Conflict;
                    }
                }
                // Bound NoGood 伝播
                if (nogood_learning_ && !nogood_mgr_.propagate_bound_nogoods(model, var_idx, true, stats_.restart_count, activity_, activity_inc_)) {
                    return PropagationResult::Conflict;
                }
            }
            break;
        }
        case PendingUpdate::Type::SetMax: {
            if (update.value >= prev_max) continue;  // 変化なし
            if (!model.set_max(current_decision_, var_idx, update.value)) {
                return PropagationResult::Conflict;
            }
            if (verbose_ && community_analysis_.is_enabled() && propagation_source_ != SIZE_MAX) {
                community_analysis_.on_propagation(var_idx, propagation_source_);
            }
            // 確定した場合は on_instantiate、そうでなければ on_set_max
            if (!was_instantiated && model.is_instantiated(var_idx)) {
                if (!propagate_instantiate(model, var_idx, prev_min, prev_max)) {
                    return PropagationResult::Conflict;
                }
            } else if (!was_instantiated) {
                // ドメインのholeにより実際のmaxは要求値より小さい場合がある
                auto actual_new_max = model.var_max(var_idx);
                const auto& constraint_indices = model.constraints_for_var(var_idx);
                for (const auto& w : constraint_indices) {
                    if (!invoke_cb(var_idx, w, [&]{
                        return constraints[w.constraint_idx]->on_set_max(model, current_decision_,
                            w.internal_var_idx, actual_new_max, prev_max);
                    })) {
                        return PropagationResult::Conflict;
                    }
                }
                // Bound NoGood 伝播
                if (nogood_learning_ && !nogood_mgr_.propagate_bound_nogoods(model, var_idx, false, stats_.restart_count, activity_, activity_inc_)) {
                    return PropagationResult::Conflict;
                }
            }
            break;
        }
        case PendingUpdate::Type::RemoveValue: {
            auto removed_value = update.value;
            if (!model.contains(var_idx, removed_value)) continue;  // 既に存在しない
            if (!model.remove_value(current_decision_, var_idx, removed_value)) {
                return PropagationResult::Conflict;
            }
            if (verbose_ && community_analysis_.is_enabled() && propagation_source_ != SIZE_MAX) {
                community_analysis_.on_propagation(var_idx, propagation_source_);
            }
            if (!was_instantiated && model.is_instantiated(var_idx)) {
                if (!propagate_instantiate(model, var_idx, prev_min, prev_max)) {
                    return PropagationResult::Conflict;
                }
            } else if (!was_instantiated) {
                auto new_min = model.var_min(var_idx);
                auto new_max = model.var_max(var_idx);
                const auto& constraint_indices = model.constraints_for_var(var_idx);

                // 下限が変化した場合 → on_set_min
                if (new_min > prev_min) {
                    for (const auto& w : constraint_indices) {
                        if (!invoke_cb(var_idx, w, [&]{
                            return constraints[w.constraint_idx]->on_set_min(model, current_decision_,
                                w.internal_var_idx, new_min, prev_min);
                        })) {
                            return PropagationResult::Conflict;
                        }
                    }
                    // Bound NoGood 伝播
                    if (nogood_learning_ && !nogood_mgr_.propagate_bound_nogoods(model, var_idx, true, stats_.restart_count, activity_, activity_inc_)) {
                        return PropagationResult::Conflict;
                    }
                }
                // 上限が変化した場合 → on_set_max
                if (new_max < prev_max) {
                    for (const auto& w : constraint_indices) {
                        if (!invoke_cb(var_idx, w, [&]{
                            return constraints[w.constraint_idx]->on_set_max(model, current_decision_,
                                w.internal_var_idx, new_max, prev_max);
                        })) {
                            return PropagationResult::Conflict;
                        }
                    }
                    // Bound NoGood 伝播
                    if (nogood_learning_ && !nogood_mgr_.propagate_bound_nogoods(model, var_idx, false, stats_.restart_count, activity_, activity_inc_)) {
                        return PropagationResult::Conflict;
                    }
                }
                // removed_value が新しい範囲内 → on_remove_value も呼ぶ
                if (removed_value > new_min && removed_value < new_max) {
                    for (const auto& w : constraint_indices) {
                        if (!invoke_cb(var_idx, w, [&]{
                            return constraints[w.constraint_idx]->on_remove_value(model, current_decision_,
                                w.internal_var_idx, removed_value);
                        })) {
                            return PropagationResult::Conflict;
                        }
                    }
                }
            }
            break;
        }
        }
    }

    // イベントキューが空 → スケジュールされたバッチ propagator を1つ実行。
    // バッチが新たなイベントを enqueue したら先にそれを処理する（イベント優先）
    size_t batch_idx = model.pop_scheduled_constraint();
    if (batch_idx == SIZE_MAX) break;
    if (stopped_) return PropagationResult::Stopped;

    if (!record_constraint_call(model, batch_idx,
                                constraints[batch_idx]->var_ids_ref().front(), [&]{
            return constraints[batch_idx]->propagate_batch(model, current_decision_);
        })) {
        return PropagationResult::Conflict;
    }
    }  // for (;;)

    return PropagationResult::Ok;
}

} // namespace sabori_csp
