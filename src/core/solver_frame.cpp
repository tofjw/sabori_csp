#include "sabori_csp/solver.hpp"
#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/one_hot_channel_aggregator.hpp"
#include <algorithm>
#include <limits>
#include <numeric>
#include <iomanip>
#include <iostream>

namespace sabori_csp {

// 明示スタック探索のフレーム管理（run_search / 値列挙 / 分岐 / frame 生成）。solver.cpp から分離。


SearchResult Solver::run_search(Model& model, int conflict_limit, size_t depth,
                                 SolutionCallback callback, bool find_all) {
    std::vector<SearchFrame> stack;
    SearchResult result = SearchResult::UNSAT;
    bool ascending = false;

    while (true) {
        if (!ascending) {
            // === 降下: 新レベルに入る ===
            size_t current_depth = depth + stack.size();

            if (stopped_) {
                result = SearchResult::UNKNOWN;
                ascending = true;
                continue;
            }

            stats_.depth_sum += current_depth;
            stats_.depth_count++;
            if (current_depth > stats_.max_depth) {
                stats_.max_depth = current_depth;
            }
            mode_policy_.observe_depth(current_depth);

            // 決定ごとに mix_p で activity_first を抽選
            // 1024 段階で離散化（rng() コスト最小、グリッド解像度より細かい）
            bool activity_first = (static_cast<double>(rng_() & 1023) < mode_policy_.mix_p() * 1024.0);
            size_t var_idx = var_selector_.select(model, activity_, temporal_activity_,
                                                  ng_usage_bloom_, activity_first, rng_,
                                                  &community_analysis_);

            if (var_idx == SIZE_MAX) {
                handle_solution(model, callback, find_all, result, ascending);
                continue;
            }

            create_search_frame(model, var_idx, stack, conflict_limit);
        } else {
            // === 上昇: 子の結果を処理 ===
            auto action = handle_ascent(model, stack, result);
            if (action == AscentAction::Return) return result;
            if (action == AscentAction::Continue) continue;
        }

        auto& frame = stack.back();
        if (frame.mode == SearchFrame::Mode::Enumerate)
            try_enumerate_values(model, frame, stack, result, ascending);
        else
            try_bisect_branches(model, frame, stack, result, ascending);
    }
}

void Solver::handle_failure(Model& model, SearchFrame& frame,
                            std::vector<SearchFrame>& stack,
                            SearchResult& result, bool& ascending) {
    activity_[frame.var_idx] += activity_inc_;
    temporal_activity_[frame.var_idx]++;

    stats_.fail_count++;
    save_partial_assignment(model);

    nogood_mgr_.truncate_nogoods(frame.nogoods_before);

    if (nogood_learning_) {
        nogood_mgr_.learn_from_conflict(decision_trail_, activity_, activity_inc_,
                                        stats_.restart_count);
    }

    value_buffer_ = std::move(frame.values);
    stack.pop_back();
    result = SearchResult::UNSAT;
    ascending = true;
}

void Solver::order_values(const Model& model, size_t var_idx) {
    auto& values = value_buffer_;

    // 疑似勾配ヒント（対象変数のみ）
    if (gradient_enabled_ && gradient_strategy_.hint_active_for(var_idx)) {
        const int grad_dir = gradient_strategy_.direction();
        const auto grad_ref = gradient_strategy_.ref_val();
        // 先に全体をシャッフル
        for (size_t i = values.size() - 1; i > 0; --i) {
            size_t j = rng_() % (i + 1);
            if (i != j) std::swap(values[i], values[j]);
        }

        // ランダム（シャッフル済みなので最初に見つけた勾配方向の値）
        for (size_t i = 0; i < values.size(); ++i) {
            if ((grad_dir > 0 && values[i] > grad_ref) ||
                (grad_dir < 0 && values[i] < grad_ref)) {
                if (i != 0) std::swap(values[i], values[0]);
                break;
            }
        }
        // 1番目: grad_ref（ベスト解の値）
        for (size_t i = 1; i < values.size(); ++i) {
            if (values[i] == grad_ref) {
                if (i != 1) std::swap(values[i], values[1]);
                break;
            }
        }
        gradient_strategy_.consume_hint();
    } else if (current_best_assignment_[var_idx] != kNoValue) {
        auto best_val = current_best_assignment_[var_idx];
        auto it = std::find(values.begin(), values.end(), best_val);
        if (it != values.end() && it != values.begin()) {
            std::swap(*it, values[0]);
        }
    } else if (model.var_data(var_idx).randomize_value_order && values.size() > 1) {
        // 値の試行順をランダム化
        for (size_t i = values.size() - 1; i > 0; --i) {
            size_t j = rng_() % (i + 1);
            if (i != j) std::swap(values[i], values[j]);
        }
    }
}

void Solver::try_enumerate_values(Model& model, SearchFrame& frame,
                                  std::vector<SearchFrame>& stack,
                                  SearchResult& result, bool& ascending) {
    bool found_value = false;

    while (frame.value_idx < frame.values.size()) {
        auto val = frame.values[frame.value_idx];

        current_decision_++;

        if (!model.instantiate(current_decision_, frame.var_idx, val)) {
            current_decision_--;
            frame.value_idx++;
            continue;
        }

        unassigned_trail_.push_back({current_decision_,
                                     var_selector_.decision_unassigned_end(),
                                     var_selector_.defined_unassigned_end(),
                                     var_selector_.unconstrained_unassigned_end(),
                                     ng_usage_bloom_});
        if (bloom_tiebreak_) ng_usage_bloom_ |= model.var_ng_bloom(frame.var_idx);

        bool propagate_ok = propagate_instantiate(model, frame.var_idx,
                                                   frame.prev_min, frame.prev_max);
        PropagationResult queue_res = PropagationResult::Conflict;
        if (propagate_ok) {
            queue_res = process_queue(model);
            if (queue_res == PropagationResult::Ok) {
                decision_trail_.push_back({frame.var_idx, val, Literal::Type::Eq});
                if (community_analysis_.is_enabled()) {
                    community_analysis_.on_decision(frame.var_idx);
                    propagation_source_ = frame.var_idx;
                }
                ascending = false;
                found_value = true;
                if (temporal_activity_[frame.var_idx] > 0)
                    temporal_activity_[frame.var_idx]--;
                break;
            }
        }

        if (!propagate_ok || queue_res != PropagationResult::Ok) {
            model.clear_pending_updates();
        }

        current_decision_--;
        backtrack(model, frame.save_point);

        // タイムアウト等で中断された場合は矛盾扱いにせず UNKNOWN として上昇
        if (queue_res == PropagationResult::Stopped) {
            result = SearchResult::UNKNOWN;
            ascending = true;
            return;
        }

        frame.value_idx++;
    }

    if (!found_value) {
        handle_failure(model, frame, stack, result, ascending);
    }
}

void Solver::try_bisect_branches(Model& model, SearchFrame& frame,
                                 std::vector<SearchFrame>& stack,
                                 SearchResult& result, bool& ascending) {
    bool found_branch = false;

    while (frame.branch < 2) {
        frame.branch++;
        current_decision_++;
        unassigned_trail_.push_back({current_decision_,
                                     var_selector_.decision_unassigned_end(),
                                     var_selector_.defined_unassigned_end(),
                                     var_selector_.unconstrained_unassigned_end(),
                                     ng_usage_bloom_});
        if (bloom_tiebreak_) ng_usage_bloom_ |= model.var_ng_bloom(frame.var_idx);

        // right_first なら branch==1 で右、branch==2 で左
        bool try_left = frame.right_first ? (frame.branch == 2) : (frame.branch == 1);

        Literal decision_lit;
        if (try_left) {
            // 左: x <= mid
            model.enqueue_set_max(frame.var_idx, frame.split_point);
            decision_lit = {frame.var_idx, frame.split_point, Literal::Type::Leq};
        } else {
            // 右: x > mid (x >= mid+1)
            model.enqueue_set_min(frame.var_idx, frame.split_point + 1);
            decision_lit = {frame.var_idx, frame.split_point + 1, Literal::Type::Geq};
        }

        PropagationResult queue_res = process_queue(model);
        if (queue_res == PropagationResult::Ok) {
            decision_trail_.push_back(decision_lit);
            if (community_analysis_.is_enabled()) {
                community_analysis_.on_decision(frame.var_idx);
                propagation_source_ = frame.var_idx;
            }
            ascending = false;
            found_branch = true;
            if (temporal_activity_[frame.var_idx] > 0)
                temporal_activity_[frame.var_idx]--;
            break;
        }

        model.clear_pending_updates();
        current_decision_--;
        backtrack(model, frame.save_point);

        // タイムアウト等で中断された場合は矛盾扱いにせず UNKNOWN として上昇
        if (queue_res == PropagationResult::Stopped) {
            result = SearchResult::UNKNOWN;
            ascending = true;
            return;
        }
    }

    if (!found_branch) {
        handle_failure(model, frame, stack, result, ascending);
    }
}

void Solver::create_search_frame(Model& model, size_t var_idx,
                                 std::vector<SearchFrame>& stack,
                                 int conflict_limit) {
    int save_point = current_decision_;
    auto prev_min = model.var_min(var_idx);
    auto prev_max = model.var_max(var_idx);
    size_t nogoods_before = nogood_mgr_.nogoods_count();
    int cl = stack.empty() ? conflict_limit : stack.back().remaining_cl;

    // モード判定
    const auto& variables = model.variables();
    auto& domain = variables[var_idx]->domain();
    auto domain_range = static_cast<size_t>(prev_max - prev_min + 1);
    bool use_bisect = (domain.is_bounds_only() ||
                      (bisection_threshold_ > 0 && domain_range > bisection_threshold_))
                      && !model.is_no_bisect(var_idx);

    if (use_bisect) {
        stats_.bisect_count++;
        auto mid = prev_min + (prev_max - prev_min) / 2;

        // 勾配ヒント > ベスト解 > ランダム の優先順位で分岐方向を決定
        bool right_first;
        if (gradient_enabled_ && gradient_strategy_.hint_active_for(var_idx)) {
            // direction は ref_val を基準とした方向。
            // 右半分 [mid+1, prev_max] / 左半分 [prev_min, mid] のうち、
            // ref_val を超える/下回る値を含む側を先に試す。
            const auto grad_ref = gradient_strategy_.ref_val();
            if (gradient_strategy_.direction() > 0) {
                if (prev_min < grad_ref && grad_ref <= prev_max) {
                    mid = grad_ref - 1;
                    right_first = true;
                    gradient_strategy_.consume_hint();
                }
                else if (prev_min <= grad_ref && prev_min < prev_max) {
                    mid = grad_ref;
                    right_first = false;
                    gradient_strategy_.consume_hint();
                }
                else {
                    right_first = (rng_() & 1) != 0;
                    gradient_strategy_.consume_hint();
                }
            } else {
                if (prev_min <= grad_ref && grad_ref < prev_max) {
                    mid = grad_ref;
                    right_first = false;
                    gradient_strategy_.consume_hint();
                }
                else if (prev_min < prev_max && grad_ref <= prev_max) {
                    mid = grad_ref - 1;
                    right_first = true;
                    gradient_strategy_.consume_hint();
                }
                else {
                    right_first = (rng_() & 1) != 0;
                    gradient_strategy_.consume_hint();
                }
            }
        } else if (current_best_assignment_[var_idx] != kNoValue) {
            auto hint_val = current_best_assignment_[var_idx];
            right_first = (hint_val > mid);
        } else {
            right_first = (rng_() & 1) != 0;
        }

        SearchFrame frame;
        frame.var_idx = var_idx;
        frame.save_point = save_point;
        frame.prev_min = prev_min;
        frame.prev_max = prev_max;
        frame.nogoods_before = nogoods_before;
        frame.remaining_cl = cl;
        frame.mode = SearchFrame::Mode::Bisect;
        frame.split_point = mid;
        frame.branch = 0;
        frame.right_first = right_first;
        frame.value_idx = 0;
        stack.push_back(std::move(frame));
    } else {
        stats_.enumerate_count++;
        domain.copy_values_to(value_buffer_);
        order_values(model, var_idx);

        SearchFrame frame;
        frame.var_idx = var_idx;
        frame.save_point = save_point;
        frame.prev_min = prev_min;
        frame.prev_max = prev_max;
        frame.nogoods_before = nogoods_before;
        frame.remaining_cl = cl;
        frame.mode = SearchFrame::Mode::Enumerate;
        frame.values = std::move(value_buffer_);
        frame.value_idx = 0;
        frame.split_point = 0;
        frame.branch = 0;
        frame.right_first = false;
        stack.push_back(std::move(frame));
    }
}

void Solver::handle_solution(Model& model, SolutionCallback& callback, bool find_all,
                             SearchResult& result, bool& ascending) {
    if (verify_solution(model)) {
        if (!callback(build_solution(model))) {
            result = SearchResult::SAT;
        } else {
            result = find_all ? SearchResult::UNSAT : SearchResult::SAT;
        }
    } else {
        result = SearchResult::UNSAT;
    }
    ascending = true;
}

} // namespace sabori_csp
