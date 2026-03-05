#include "sabori_csp/solver.hpp"
#include <algorithm>
#include <limits>
#include <numeric>
#include <iomanip>
#include <iostream>

namespace sabori_csp {

Solver::Solver()
    : rng_(12345678) {}

bool Literal::is_satisfied(const Model& model) const {
    switch (type) {
    case Type::Eq:
        return model.is_instantiated(var_idx) && model.value(var_idx) == value;
    case Type::Leq:
        return model.var_max(var_idx) <= value;
    case Type::Geq:
        return model.var_min(var_idx) >= value;
    }
    return false;
}

Literal Literal::negate() const {
    switch (type) {
    case Type::Eq:
        // Eq の否定は Eq のまま（remove_value で処理）
        return {var_idx, value, Type::Eq};
    case Type::Leq:
        // x <= v の否定は x >= v+1
        return {var_idx, value + 1, Type::Geq};
    case Type::Geq:
        // x >= v の否定は x <= v-1
        return {var_idx, value - 1, Type::Leq};
    }
    return *this;
}

bool Solver::apply_unit_nogoods(Model& model) {
    if (nogood_mgr_.unit_nogoods().empty()) return true;
    nogood_mgr_.enqueue_unit_nogoods(model);
    return process_queue(model);
}

bool Solver::init_search(Model& model) {
    model.build_constraint_watch_list();

    const auto& variables = model.variables();
    activity_.assign(variables.size(), 0.0);
    temporal_activity_.assign(variables.size(), 0);
    var_selector_.build_order(model, rng_);
    decision_trail_.clear();
    nogood_mgr_.clear(variables.size());
    best_num_instantiated_ = 0;
    best_assignment_.assign(variables.size(), kNoValue);
    current_best_assignment_.assign(variables.size(), kNoValue);
    current_decision_ = 0;
    stats_ = SolverStats{};
    model.resize_var_ng_bloom(variables.size());
    ng_usage_bloom_ = Bloom512{};
    restart_ctrl_.reset();

    if (verbose_) log_presolve_start(model);
    if (!presolve(model)) {
        if (verbose_) std::cerr << "% [verbose] presolve failed\n";
        return false;
    }
    if (verbose_) {
        std::cerr << "% [verbose] presolve done\n";
        size_t n_vars = model.variables().size();
        size_t n_defined = 0;
        for (size_t i = 0; i < n_vars; ++i) {
            if (model.is_defined_var(i)) ++n_defined;
        }
        std::cerr << "% [verbose] vars: total=" << n_vars
                  << " decision=" << (n_vars - n_defined)
                  << " defined=" << n_defined << "\n";
    }
    if (community_analysis_.is_enabled()) {
        community_analysis_.build_vig(model);
        community_analysis_.detect_communities(rng_);
        community_analysis_.print_static_report(std::cerr);
        update_bump_activity_flag();
    }
    var_selector_.init_tracking(model);
    if (verbose_) {
        std::cerr << "% [verbose] search vars: decision=" << var_selector_.decision_var_end()
                  << " defined=" << (var_selector_.var_order().size() - var_selector_.decision_var_end())
                  << "\n";
    }
    unassigned_trail_.clear();
    return true;
}

std::optional<Solution> Solver::solve(Model& model) {
    if (!init_search(model)) return std::nullopt;

    if (restart_enabled_) {
        return search_with_restart(model, nullptr, false);
    }

    std::optional<Solution> result;
    int conflict_limit = std::numeric_limits<int>::max();
    run_search(model, conflict_limit, 0,
               [&result](const Solution& sol) {
                   result = sol;
                   return false;
               }, false);
    return result;
}

std::optional<Solution> Solver::solve_optimize(
        Model& model, size_t obj_var_idx, bool minimize,
        SolutionCallback on_improve) {
    optimizing_ = true;
    obj_var_idx_ = obj_var_idx;
    minimize_ = minimize;
    best_solution_ = std::nullopt;
    best_objective_ = std::nullopt;

    if (!init_search(model)) {
        optimizing_ = false;
        return std::nullopt;
    }

    auto result = search_with_restart_optimize(model, on_improve);
    optimizing_ = false;
    return result;
}

size_t Solver::solve_all(Model& model, SolutionCallback callback) {
    if (!init_search(model)) return 0;

    size_t count = 0;

    if (restart_enabled_) {
        search_with_restart(model, [&](const Solution& sol) {
            count++;
            return callback(sol);
        }, true);
    } else {
        int conflict_limit = std::numeric_limits<int>::max();
        run_search(model, conflict_limit, 0,
                   [&count, &callback](const Solution& sol) {
                       count++;
                       return callback(sol);
                   }, true);
    }

    return count;
}

std::optional<Solution> Solver::search_with_restart(Model& model,
                                                      SolutionCallback callback,
                                                      bool find_all) {
    int root_point = current_decision_;

    if (verbose_) {
        std::cerr << "% [verbose] search_with_restart start"
                  << (find_all ? " (find_all)" : "") << "\n";
    }

    while (!stopped_) {
        // ===== cycle 開始 =====
        size_t prune_at_cycle_start = stats_.nogood_prune_count;
        size_t max_depth_at_cycle_start = stats_.max_depth;
        restart_ctrl_.begin_cycle();

        while (restart_ctrl_.inner_within_outer() && !stopped_) {
            int conflict_limit = restart_ctrl_.conflict_limit();
            std::optional<Solution> result;

            auto res = run_search(model, conflict_limit, 0,
                                  [&result](const Solution& sol) {
                                      result = sol;
                                      return false;
                                  }, false);

            if (res == SearchResult::SAT) {
                if (find_all) {
                    // 全解探索: コールバックに報告し、解をNGとして追加して続行
                    if (!callback(*result)) {
                        sync_nogood_stats();
                        return std::nullopt;  // コールバックが停止を要求
                    }
                    model.clear_pending_updates();
                    nogood_mgr_.add_solution_nogood(model, stats_.restart_count);
                    backtrack(model, root_point);

                    if (!apply_unit_nogoods(model)) {
                        model.clear_pending_updates();
                        sync_nogood_stats();
                        return std::nullopt;
                    }

                    // unit nogood + permanent NG の伝播で全変数が確定する場合がある
                    // その場合、有効な解なら報告して続行する
                    while (var_selector_.all_assigned()) {
                        if (verify_solution(model)) {
                            auto sol = build_solution(model);
                            if (!callback(sol)) {
                                sync_nogood_stats();
                                return std::nullopt;
                            }
                            model.clear_pending_updates();
                            nogood_mgr_.add_solution_nogood(model, stats_.restart_count);
                            backtrack(model, root_point);
                            if (!apply_unit_nogoods(model)) {
                                model.clear_pending_updates();
                                sync_nogood_stats();
                                return std::nullopt;
                            }
                        } else {
                            sync_nogood_stats();
                            return std::nullopt;
                        }
                    }

                    continue;
                }
                sync_nogood_stats();
                return result;
            }
            if (res == SearchResult::UNSAT) {
                sync_nogood_stats();
                return std::nullopt;
            }

            // UNKNOWN: リスタート
            model.clear_pending_updates();
            backtrack(model, root_point);

            if (!apply_unit_nogoods(model)) {
                model.clear_pending_updates();
                sync_nogood_stats();
                return std::nullopt;
            }

            stats_.restart_count++;
            if (community_analysis_.is_enabled()) {
                community_analysis_.print_dynamic_report(std::cerr, stats_.restart_count);
                community_analysis_.reset_stats();
            }
            current_best_assignment_ = select_best_assignment();
            ng_usage_bloom_ = Bloom512{};

            // リスタート後の起点変数を選択（探索多様化）
            var_selector_.select_restart_pivot(model, activity_, community_analysis_, stats_.restart_count);

            // NoGood GC + ブルームフィルタ再構築
            nogood_mgr_.gc(stats_.restart_count, nogood_inactive_restart_limit_);
            nogood_mgr_.rebuild_var_ng_blooms(model);

            // Activity 減衰
            decay_activities();

            // Temporal activity リセット
            std::fill(temporal_activity_.begin(), temporal_activity_.end(), 0);

            // domain_size 優先と activity 優先を交互に切り替え
            activity_first_ = !activity_first_;

            // スキャン順シャッフル（タイブレークのランダム化、各区間を独立に）
            var_selector_.shuffle(rng_);
            var_selector_.init_tracking(model);
            unassigned_trail_.clear();

            if (verbose_) {
                std::cerr << "% [verbose] restart #" << stats_.restart_count
                          << " cl=" << conflict_limit
                          << " outer=" << restart_ctrl_.outer()
                          << " fails=" << stats_.fail_count
                          << " max_depth=" << stats_.max_depth
                          << " nogoods=" << nogood_mgr_.nogoods_count()
                          << " prune=" << stats_.nogood_prune_count
                          << "\n";
            }

            restart_ctrl_.advance_inner();
        }

        // ===== cycle 終了: outer を調整 =====
        size_t prune_delta = stats_.nogood_prune_count - prune_at_cycle_start;
        bool depth_grew = stats_.max_depth > max_depth_at_cycle_start;
        restart_ctrl_.end_cycle(prune_delta, depth_grew);

        if (verbose_) {
            std::cerr << "% [verbose] cycle end: prune_delta=" << prune_delta
                      << " depth_grew=" << depth_grew
                      << " new_outer=" << restart_ctrl_.outer() << "\n";
        }
    }

    if (community_analysis_.is_enabled()) {
        community_analysis_.print_dynamic_report(std::cerr, stats_.restart_count + 1);
    }
    if (verbose_) {
        std::cerr << "% [verbose] search stopped (timeout)\n";
    }
    sync_nogood_stats();
    return std::nullopt;
}

std::optional<Solution> Solver::search_with_restart_optimize(
        Model& model, SolutionCallback callback) {
    int root_point = current_decision_;

    prev_improving_solution_.clear();  // empty() = true means no previous solution yet
    gradient_ema_.clear();
    gradient_var_idx_ = SIZE_MAX;
    gradient_direction_ = 0;

    if (verbose_) {
        std::cerr << "% [verbose] search_with_restart_optimize start\n";
    }

    while (!stopped_) {
        // ===== cycle 開始 =====
        size_t prune_at_cycle_start = stats_.nogood_prune_count;
        size_t domain_at_cycle_start = stats_.nogood_domain_count;
        size_t max_depth_at_cycle_start = stats_.max_depth;
        bool cycle_interrupted = false;
        restart_ctrl_.begin_cycle();

        while (restart_ctrl_.inner_within_outer() && !stopped_) {
            int conflict_limit = restart_ctrl_.conflict_limit();
            std::optional<Solution> found_solution;

            auto res = run_search(model, conflict_limit, 0,
                                  [&found_solution](const Solution& sol) {
                                      found_solution = sol;
                                      return false;  // 最初の解で停止
                                  }, false);

            if (res == SearchResult::SAT) {
                auto obj_val = model.value(obj_var_idx_);
                bool improved = !best_objective_ ||
                    (minimize_ && obj_val < *best_objective_) ||
                    (!minimize_ && obj_val > *best_objective_);

                if (improved) {
                    best_objective_ = obj_val;
                    best_solution_ = found_solution;

                    if (verbose_) {
                        std::cerr << "% [verbose] new best objective: " << obj_val << "\n";
                    }

                    // 途中解を報告
                    if (callback) {
                        callback(*found_solution);
                    }

                    // 解で current_best_assignment_ を更新（値選択の優先度に使用）
                    std::fill(current_best_assignment_.begin(), current_best_assignment_.end(), kNoValue);
                    const auto& variables = model.variables();
                    for (size_t i = 0; i < variables.size(); ++i) {
                        if (model.is_instantiated(i)) {
                            current_best_assignment_[i] = model.value(i);
                        }
                    }

                    // 疑似勾配の計算（移動平均で蓄積）
                    gradient_var_idx_ = SIZE_MAX;
                    gradient_direction_ = 0;
                    if (!prev_improving_solution_.empty()) {
                        constexpr double alpha = 0.3;
                        if (gradient_ema_.empty()) {
                            gradient_ema_.assign(variables.size(), 0.0);
                        }
                        for (size_t i = 0; i < var_selector_.decision_var_end(); ++i) {
                            size_t vi = var_selector_.var_order()[i];
                            if (prev_improving_solution_[vi] != kNoValue &&
                                current_best_assignment_[vi] != kNoValue) {
                                double delta = static_cast<double>(current_best_assignment_[vi] - prev_improving_solution_[vi]);
                                gradient_ema_[vi] = alpha * delta + (1.0 - alpha) * gradient_ema_[vi];
                            }
                        }
                        // EMA が有意かつ線形制約がかかっている変数から1つランダムに選択
                        const auto& all_constraints = model.constraints();
                        std::vector<size_t> candidates;
                        for (size_t vi = 0; vi < gradient_ema_.size(); ++vi) {
                            double ema = gradient_ema_[vi];
                            if (ema >= 1.0 || ema <= -1.0) {
                                bool has_lin = false;
                                for (const auto& w : model.constraints_for_var(vi)) {
                                    const auto& cname = all_constraints[w.constraint_idx]->name();
                                    if (cname.compare(0, 8, "int_lin_") == 0 ||
                                        cname.compare(0, 8, "bool_lin") == 0) {
                                        has_lin = true;
                                        break;
                                    }
                                }
                                if (has_lin) candidates.push_back(vi);
                            }
                        }
                        if (!candidates.empty()) {
                            size_t vi = candidates[rng_() % candidates.size()];
                            gradient_var_idx_ = vi;
                            gradient_direction_ = (gradient_ema_[vi] > 0) ? +1 : -1;
                            gradient_ref_val_ = current_best_assignment_[vi];
                        }
                    }
                    prev_improving_solution_ = current_best_assignment_;
                }

                // root へバックトラック
                model.clear_pending_updates();
                backtrack(model, root_point);
                current_decision_ = root_point;
                ng_usage_bloom_ = Bloom512{};
                nogood_mgr_.rebuild_var_ng_blooms(model);

                // unit nogood をドメインに適用
                nogood_mgr_.enqueue_unit_nogoods(model);

                // 目的変数のドメインを縮小（永続的に root_point レベルで保存）
                if (minimize_) {
                    model.enqueue_set_max(obj_var_idx_, obj_val - 1);
                } else {
                    model.enqueue_set_min(obj_var_idx_, obj_val + 1);
                }

                if (!process_queue(model)) {
                    if (stopped_) break;  // timeout → fall through to timeout path
                    // 伝播で UNSAT → 最適解が確定
                    model.clear_pending_updates();
                    sync_nogood_stats();
                    if (verbose_) {
                        std::cerr << "% [verbose] optimal (propagation proved no improvement)\n";
                    }
                    return best_solution_;
                }

                // --- improvement probe: ~10% 改善を軽量プローブで試みる ---
                if (probe_fail_limit_ > 0) {
                    auto obj_lb = model.var_min(obj_var_idx_);
                    auto obj_ub = model.var_max(obj_var_idx_);
                    auto range = obj_ub - obj_lb;
                    auto improvement = std::max(range / 20, static_cast<Domain::value_type>(1));

                    Domain::value_type target;
                    if (minimize_) {
                        target = obj_lb + improvement - 1;  // obj <= target
                    } else {
                        target = obj_ub - improvement + 1;  // obj >= target
                    }

                    bool target_useful = minimize_ ? (target < obj_ub) : (target > obj_lb);

                    if (target_useful) {
                        if (verbose_) {
                            std::cerr << "% [verbose] improvement probe: obj=["
                                      << obj_lb << ".." << obj_ub << "] target=" << target
                                      << " fail_limit=" << probe_fail_limit_ << "\n";
                        }

                        current_decision_++;
                        if (minimize_) {
                            decision_trail_.push_back({obj_var_idx_, target, Literal::Type::Leq});
                            model.enqueue_set_max(obj_var_idx_, target);
                        } else {
                            decision_trail_.push_back({obj_var_idx_, target, Literal::Type::Geq});
                            model.enqueue_set_min(obj_var_idx_, target);
                        }

                        Domain::value_type probe_obj = 0;
                        std::optional<Solution> probe_solution;
                        SearchResult res2 = SearchResult::UNKNOWN;
                        bool probe_propagation_ok = false;

                        if (process_queue(model)) {
                            probe_propagation_ok = true;
                            res2 = run_search(model, probe_fail_limit_, 0,
                                              [&probe_solution](const Solution& sol) {
                                                  probe_solution = sol;
                                                  return false;
                                              }, false);
                            if (res2 == SearchResult::SAT) {
                                probe_obj = model.value(obj_var_idx_);
                                // current_best_assignment_ を backtrack 前に取得
                                const auto& variables = model.variables();
                                std::fill(current_best_assignment_.begin(),
                                          current_best_assignment_.end(), kNoValue);
                                for (size_t i = 0; i < variables.size(); ++i) {
                                    if (model.is_instantiated(i)) {
                                        current_best_assignment_[i] = model.value(i);
                                    }
                                }
                            }
                        }
                        // process_queue 失敗 = 伝播のみで UNSAT（探索より強い証明）
                        bool probe_unsat = !probe_propagation_ok || res2 == SearchResult::UNSAT;

                        // プローブ仮定を除去してバックトラック
                        decision_trail_.pop_back();
                        model.clear_pending_updates();
                        backtrack(model, root_point);
                        current_decision_ = root_point;

                        if (res2 == SearchResult::SAT) {
                            bool probe_improved =
                                (minimize_ && probe_obj < *best_objective_) ||
                                (!minimize_ && probe_obj > *best_objective_);

                            if (probe_improved) {
                                best_objective_ = probe_obj;
                                best_solution_ = probe_solution;

                                if (verbose_) {
                                    std::cerr << "% [verbose] probe improved: " << probe_obj << "\n";
                                }

                                if (callback) {
                                    callback(*probe_solution);
                                }

                                // 永続的に目的変数を縮小
                                if (minimize_) {
                                    model.enqueue_set_max(obj_var_idx_, probe_obj - 1);
                                } else {
                                    model.enqueue_set_min(obj_var_idx_, probe_obj + 1);
                                }

                                if (!process_queue(model)) {
                                    if (stopped_) break;  // timeout → fall through
                                    model.clear_pending_updates();
                                    sync_nogood_stats();
                                    if (verbose_) {
                                        std::cerr << "% [verbose] optimal (probe proved optimality)\n";
                                    }
                                    return best_solution_;
                                }
                            } else {
                                // プローブ解は改善なし（既に best_objective_ で縮小済み）
                                // root 状態を再構築
                                nogood_mgr_.enqueue_unit_nogoods(model);
                                if (minimize_) {
                                    model.enqueue_set_max(obj_var_idx_, *best_objective_ - 1);
                                } else {
                                    model.enqueue_set_min(obj_var_idx_, *best_objective_ + 1);
                                }
                                if (!process_queue(model)) {
                                    if (stopped_) break;  // timeout → fall through
                                    model.clear_pending_updates();
                                    sync_nogood_stats();
                                    return best_solution_;
                                }
                            }
                        } else if (probe_unsat) {
                            // UNSAT: obj <= target が不可能と証明
                            // → obj >= target + 1 (minimize) / obj <= target - 1 (maximize) で永続縮小
                            if (verbose_) {
                                std::cerr << "% [verbose] probe UNSAT: tightening bound past target=" << target << "\n";
                            }
                            nogood_mgr_.enqueue_unit_nogoods(model);
                            if (minimize_) {
                                model.enqueue_set_min(obj_var_idx_, target + 1);
                                model.enqueue_set_max(obj_var_idx_, *best_objective_ - 1);
                            } else {
                                model.enqueue_set_max(obj_var_idx_, target - 1);
                                model.enqueue_set_min(obj_var_idx_, *best_objective_ + 1);
                            }
                            if (!process_queue(model)) {
                                if (stopped_) break;  // timeout → fall through
                                model.clear_pending_updates();
                                sync_nogood_stats();
                                if (verbose_) {
                                    std::cerr << "% [verbose] optimal (probe UNSAT + bound tightening)\n";
                                }
                                return best_solution_;
                            }
                        } else {
                            // UNKNOWN: 情報なし、root 状態を再構築
                            nogood_mgr_.enqueue_unit_nogoods(model);
                            if (minimize_) {
                                model.enqueue_set_max(obj_var_idx_, *best_objective_ - 1);
                            } else {
                                model.enqueue_set_min(obj_var_idx_, *best_objective_ + 1);
                            }
                            if (!process_queue(model)) {
                                if (stopped_) break;  // timeout → fall through
                                model.clear_pending_updates();
                                sync_nogood_stats();
                                return best_solution_;
                            }
                        }

                        // 再初期化
                        apply_unit_nogoods(model);
                        nogood_mgr_.rebuild_var_ng_blooms(model);
                        ng_usage_bloom_ = Bloom512{};
                    }
                }
                // --- end improvement probe ---

                var_selector_.init_tracking(model);
                unassigned_trail_.clear();

                // 改善時: outer をリセットして cycle を中断
                restart_ctrl_.reset_outer();
                cycle_interrupted = true;
                break;
            }

            if (res == SearchResult::UNSAT) {
                // 探索空間が尽きた → 最適 (or nullopt if no solution found)
                sync_nogood_stats();
                if (verbose_) {
                    std::cerr << "% [verbose] optimal (search exhausted)\n";
                }
                return best_solution_;
            }

            // UNKNOWN: リスタート
            model.clear_pending_updates();
            backtrack(model, root_point);
            current_decision_ = root_point;

            if (!apply_unit_nogoods(model)) {
                // unit nogood の適用で UNSAT → 最適解が確定
                model.clear_pending_updates();
                sync_nogood_stats();
                return best_solution_;
            }

            stats_.restart_count++;
            if (community_analysis_.is_enabled()) {
                community_analysis_.print_dynamic_report(std::cerr, stats_.restart_count);
                community_analysis_.reset_stats();
            }
            current_best_assignment_ = select_best_assignment();
            ng_usage_bloom_ = Bloom512{};

            // リスタート後の起点変数を選択（探索多様化）
            var_selector_.select_restart_pivot(model, activity_, community_analysis_, stats_.restart_count);

            // NoGood GC + ブルームフィルタ再構築
            nogood_mgr_.gc(stats_.restart_count, nogood_inactive_restart_limit_);
            nogood_mgr_.rebuild_var_ng_blooms(model);

            // Activity 減衰
            decay_activities();

            // domain_size 優先と activity 優先を交互に切り替え
            activity_first_ = !activity_first_;

            // スキャン順シャッフル
            var_selector_.shuffle(rng_);
            var_selector_.init_tracking(model);
            unassigned_trail_.clear();

            // 解が見つからなかった場合: EMA有意な変数からランダムに選んで勾配を適用
            if (!gradient_ema_.empty()) {
                std::vector<size_t> candidates;
                for (size_t vi = 0; vi < gradient_ema_.size(); ++vi) {
                    double ema = gradient_ema_[vi];
                    if (ema >= 1.0 || ema <= -1.0) {
                        candidates.push_back(vi);
                    }
                }
                if (!candidates.empty()) {
                    size_t vi = candidates[rng_() % candidates.size()];
                    if (current_best_assignment_[vi] != kNoValue) {
                        gradient_var_idx_ = vi;
                        gradient_direction_ = (gradient_ema_[vi] > 0) ? +1 : -1;
                        gradient_ref_val_ = current_best_assignment_[vi];
                    }
                }
            }

            if (verbose_) {
                std::cerr << "% [verbose] restart #" << stats_.restart_count
                          << " cl=" << conflict_limit
                          << " outer=" << restart_ctrl_.outer()
                          << " fails=" << stats_.fail_count
                          << " max_depth=" << stats_.max_depth
                          << " nogoods=" << nogood_mgr_.nogoods_count()
                          << " prune=" << stats_.nogood_prune_count
                          << " best=" << (best_objective_ ? std::to_string(*best_objective_) : "none")
                          << "\n";
            }

            restart_ctrl_.advance_inner();
        }

        // ===== cycle 終了: outer を調整（改善時中断でなければ） =====
        if (!cycle_interrupted) {
            size_t prune_delta = stats_.nogood_prune_count - prune_at_cycle_start;
            size_t domain_delta = stats_.nogood_domain_count - domain_at_cycle_start;
            bool depth_grew = stats_.max_depth > max_depth_at_cycle_start;
            restart_ctrl_.end_cycle(prune_delta + domain_delta, depth_grew);

            if (verbose_) {
                std::cerr << "% [verbose] cycle end: prune_delta=" << prune_delta
                          << " depth_grew=" << depth_grew
                          << " new_outer=" << restart_ctrl_.outer() << "\n";
            }
        }
    }

    if (community_analysis_.is_enabled()) {
        community_analysis_.print_dynamic_report(std::cerr, stats_.restart_count + 1);
    }
    if (verbose_) {
        std::cerr << "% [verbose] search stopped (timeout)\n";
    }
    sync_nogood_stats();
    return best_solution_;
}

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

            size_t var_idx = var_selector_.select(model, activity_, temporal_activity_,
                                                  ng_usage_bloom_, activity_first_, rng_);

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

void Solver::order_values(size_t var_idx) {
    auto& values = value_buffer_;

    // 疑似勾配ヒント（対象変数のみ）
    if (var_idx == gradient_var_idx_ && gradient_direction_ != 0) {
        std::vector<size_t> candidate_indices;
        for (size_t i = 0; i < values.size(); ++i) {
            if (gradient_direction_ > 0 && values[i] > gradient_ref_val_) {
                candidate_indices.push_back(i);
            } else if (gradient_direction_ < 0 && values[i] < gradient_ref_val_) {
                candidate_indices.push_back(i);
            }
        }
        if (!candidate_indices.empty()) {
            size_t pick = candidate_indices[rng_() % candidate_indices.size()];
            if (pick != 0) std::swap(values[pick], values[0]);
            // 2番目の候補としてベスト解の値を配置
            if (values.size() > 1 && current_best_assignment_[var_idx] != kNoValue) {
                auto best_val = current_best_assignment_[var_idx];
                for (size_t i = 1; i < values.size(); ++i) {
                    if (values[i] == best_val) {
                        if (i != 1) std::swap(values[i], values[1]);
                        break;
                    }
                }
            }
        }
        gradient_var_idx_ = SIZE_MAX;
    } else if (current_best_assignment_[var_idx] != kNoValue) {
        auto best_val = current_best_assignment_[var_idx];
        auto it = std::find(values.begin(), values.end(), best_val);
        if (it != values.end() && it != values.begin()) {
            std::swap(*it, values[0]);
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
                                     ng_usage_bloom_});
        ng_usage_bloom_ |= model.var_ng_bloom(frame.var_idx);

        bool propagate_ok = propagate_instantiate(model, frame.var_idx,
                                                   frame.prev_min, frame.prev_max);
        bool queue_ok = false;
        if (propagate_ok) {
            queue_ok = process_queue(model);
            if (queue_ok) {
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

        if (!propagate_ok || !queue_ok) {
            model.clear_pending_updates();
        }

        current_decision_--;
        backtrack(model, frame.save_point);
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
                                     ng_usage_bloom_});
        ng_usage_bloom_ |= model.var_ng_bloom(frame.var_idx);

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

        bool ok = process_queue(model);
        if (ok) {
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

        // ヒント解がある場合はそちら側を優先、なければランダム
        bool right_first;
        if (current_best_assignment_[var_idx] != kNoValue) {
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
        order_values(var_idx);

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

Solver::AscentAction Solver::handle_ascent(Model& model,
                                           std::vector<SearchFrame>& stack,
                                           SearchResult& result) {
    if (stack.empty()) {
        return AscentAction::Return;
    }

    auto& frame = stack.back();
    decision_trail_.pop_back();

    if (result == SearchResult::SAT) {
        value_buffer_ = std::move(frame.values);
        stack.pop_back();
        return AscentAction::Continue;  // SAT を上へ伝播
    }

    if (result == SearchResult::UNKNOWN || frame.remaining_cl <= 1) {
        current_decision_--;
        backtrack(model, frame.save_point);
        value_buffer_ = std::move(frame.values);
        stack.pop_back();
        result = SearchResult::UNKNOWN;
        return AscentAction::Continue;
    }

    // UNSAT: 次の値/branch を試す
    frame.remaining_cl--;
    current_decision_--;
    backtrack(model, frame.save_point);

    if (frame.mode == SearchFrame::Mode::Enumerate) {
        frame.value_idx++;
    }
    // Bisect モードでは branch は try_bisect_branches 側で自動インクリメント

    return AscentAction::TryNext;
}

void Solver::log_presolve_start(const Model& model) const {
    const auto& variables = model.variables();
    size_t max_cpc = 0;
    size_t total_cpc = 0;
    for (size_t i = 0; i < variables.size(); ++i) {
        size_t c = model.constraints_for_var(i).size();
        total_cpc += c;
        if (c > max_cpc) max_cpc = c;
    }
    double avg_cpc = variables.empty() ? 0.0 : static_cast<double>(total_cpc) / variables.size();
    std::cerr << "% [verbose] presolve start: " << model.constraints().size()
              << " constraints, " << variables.size() << " variables"
              << " (avg " << std::fixed << std::setprecision(1) << avg_cpc
              << " constraints/var, max " << max_cpc << ")\n";
}

bool Solver::presolve(Model& model) {
    // presolve_phase_ はデフォルト true なので、ここでは明示的に set する必要はない
    // ただし、リスタートループから再び呼ばれることはないので安全
    const auto& constraints = model.constraints();

    // Phase 1: 全制約の presolve() を固定点まで繰り返す
    // 制約は変数のドメインを直接変更するため、変化の検出は
    // SoA (model.var_size) ではなく変数のドメインを直接参照する。
    {
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& constraint : constraints) {
                auto result = constraint->presolve(model);
                if (result == PresolveResult::Contradiction) {
                    return false;
                }
                if (result == PresolveResult::Changed) {
                    changed = true;
                }
            }
        }
        // Phase 1 後: 内部構造を再構築（ドメイン変更に対する整合性保証）
        if (!model.prepare_propagation()) {
            return false;
        }
    }

    // presolve 終了 — 以降の直接ドメイン操作は（debug ビルドで）assertion failure になる
    model.set_presolve_phase(false);
    return true;
}


bool Solver::propagate_instantiate(Model& model, size_t var_idx,
                                    Domain::value_type prev_min, Domain::value_type prev_max) {
    var_selector_.mark_assigned(var_idx);
    const auto& constraints = model.constraints();
    auto val = model.value(var_idx);

    const auto& constraint_indices = model.constraints_for_var(var_idx);
    for (const auto& w : constraint_indices) {
        if (!constraints[w.constraint_idx]->on_instantiate(model, current_decision_,
						    var_idx, w.internal_var_idx, val, prev_min, prev_max)) {
            bump_activity(model, w.constraint_idx);
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

void Solver::backtrack(Model& model, int save_point) {
    model.rewind_to(save_point);
    model.rewind_dirty_constraints(save_point);
    // パーティション境界とブルームフィルタを復元
    while (!unassigned_trail_.empty() && unassigned_trail_.back().level > save_point) {
        ng_usage_bloom_ = unassigned_trail_.back().ng_usage_bloom;
        var_selector_.restore_decision_end(unassigned_trail_.back().dec_end);
        var_selector_.restore_defined_end(unassigned_trail_.back().def_end);
        unassigned_trail_.pop_back();
    }
}

Solution Solver::build_solution(const Model& model) const {
    Solution sol;
    const auto& variables = model.variables();
    for (size_t i = 0; i < variables.size(); ++i) {
        if (model.is_instantiated(i)) {
            sol[variables[i]->name()] = model.value(i);
        }
    }
    // エイリアスも解に含める
    for (const auto& [alias_name, var_id] : model.variable_aliases()) {
        if (model.is_instantiated(var_id)) {
            sol[alias_name] = model.value(var_id);
        }
    }
    return sol;
}

bool Solver::verify_solution(const Model& model) const {
    for (const auto& constraint : model.constraints()) {
        auto satisfied = constraint->is_satisfied(model);
        if (satisfied.has_value() && !satisfied.value()) {
            std::cerr << "constraint verify error: " << constraint->name() << "\n";
            abort();
            return false;
        }
    }
    return true;
}

void Solver::decay_activities() {
    activity_inc_ /= restart_ctrl_.activity_decay();
    if (activity_inc_ > 10000.0) {
        rescale_activities();
    }
}

void Solver::rescale_activities() {
    double max_act = 0.0;
    for (const auto& a : activity_) {
        if (a > max_act) max_act = a;
    }
    if (max_act > 0.0) {
        double scale = 100.0 / max_act;
        for (auto& a : activity_) {
            a *= scale;
        }
    }
    activity_inc_ = 1.0;
}

void Solver::update_bump_activity_flag() {
    const auto& s = community_analysis_.structure();
    // modularity が小さい → クラスタ構造が弱い
    if (s.modularity < 0.3) {
        bump_activity_enabled_ = false;
        if (verbose_) {
            std::cerr << "% [verbose] bump_activity disabled (modularity="
                      << s.modularity << " < 0.3)\n";
        }
        return;
    }
    // 最大クラスタが2番目の5倍を超える → 実質1クラスタ
    const auto& tops = community_analysis_.top_communities(2);
    if (tops.size() >= 2) {
        size_t largest = community_analysis_.community_vars(tops[0]).size();
        size_t second = community_analysis_.community_vars(tops[1]).size();
        if (second > 0 && largest > second * 5) {
            bump_activity_enabled_ = false;
            if (verbose_) {
                std::cerr << "% [verbose] bump_activity disabled (largest_cluster="
                          << largest << " > 5 * second=" << second << ")\n";
            }
            return;
        }
    }
    bump_activity_enabled_ = true;
    if (verbose_) {
        std::cerr << "% [verbose] bump_activity enabled (modularity="
                  << s.modularity << ")\n";
    }
}

void Solver::set_activity(const std::map<std::string, double>& activity, const Model& model) {
    // activity_ のサイズを確保
    if (activity_.size() < model.variables().size()) {
        activity_.resize(model.variables().size(), 0.0);
    }

    // 名前からインデックスを解決して設定
    for (const auto& [name, score] : activity) {
        try {
            auto var = model.variable(name);
            if (var) {
                // 変数のインデックスを検索
                for (size_t i = 0; i < model.variables().size(); ++i) {
                    if (model.variable(i) == var) {
                        activity_[i] = score;
                        break;
                    }
                }
            }
        } catch (const std::out_of_range&) {
            // Variable may not exist in the new model (e.g. helper variables with static counters)
            continue;
        }
    }
}

std::map<std::string, double> Solver::get_activity_map(const Model& model) const {
    std::map<std::string, double> result;
    for (size_t i = 0; i < activity_.size() && i < model.variables().size(); ++i) {
        auto var = model.variable(i);
        if (var && activity_[i] > 0.0) {
            result[var->name()] = activity_[i];
        }
    }
    return result;
}

std::vector<NamedNoGood> Solver::get_nogoods(const Model& model, size_t max_count) const {
    return nogood_mgr_.get_nogoods(model, max_count);
}

size_t Solver::add_nogoods(const std::vector<NamedNoGood>& nogoods, const Model& model) {
    return nogood_mgr_.add_nogoods(nogoods, model, stats_.restart_count);
}

void Solver::set_hint_solution(const Solution& hint, const Model& model) {
    current_best_assignment_.assign(model.variables().size(), kNoValue);

    // 変数名 → インデックスのマップを構築
    std::unordered_map<std::string, size_t> name_to_idx;
    for (size_t i = 0; i < model.variables().size(); ++i) {
        auto var = model.variable(i);
        if (var) {
            name_to_idx[var->name()] = i;
        }
    }

    // ヒント解をインデックスベースに変換
    for (const auto& [name, value] : hint) {
        auto it = name_to_idx.find(name);
        if (it != name_to_idx.end()) {
            current_best_assignment_[it->second] = value;
        }
    }
}

void Solver::sync_nogood_stats() {
    stats_.nogood_check_count = nogood_mgr_.check_count();
    stats_.nogood_prune_count = nogood_mgr_.prune_count();
    stats_.nogood_domain_count = nogood_mgr_.domain_count();
    stats_.nogoods_size = nogood_mgr_.nogoods_count();
    stats_.unit_nogoods_size = nogood_mgr_.unit_nogoods().size();
}

void Solver::save_partial_assignment(const Model& model) {
    size_t num_instantiated = model.instantiated_count();

    if (num_instantiated <= best_num_instantiated_) {
        return;
    }

    // 前回より多い → 置き換え（未 instantiate の変数は前回の値を引き継ぐ）
    best_num_instantiated_ = num_instantiated;
    size_t n_vars = model.variables().size();
    for (size_t i = 0; i < n_vars; ++i) {
        if (model.is_instantiated(i)) {
            best_assignment_[i] = model.value(i);
        }
    }
}

const std::vector<Domain::value_type>& Solver::select_best_assignment() {
    return best_assignment_;
}

bool Solver::process_queue(Model& model) {
    const auto& constraints = model.constraints();

    while (model.has_pending_updates()) {
        if (stopped_) return false;

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
                    return false;
                }
                // 同じ値で既に確定済み: ドメイン削減で確定した
                // 二重に同じイベントを呼ばない
                continue;
            }
            if (!model.instantiate(current_decision_, var_idx, update.value)) {
                return false;
            }
            if (community_analysis_.is_enabled() && propagation_source_ != SIZE_MAX) {
                community_analysis_.on_propagation(var_idx, propagation_source_);
            }
            if (!propagate_instantiate(model, var_idx, prev_min, prev_max)) {
                return false;
            }
            break;
        }
        case PendingUpdate::Type::SetMin: {
            if (update.value <= prev_min) continue;  // 変化なし
            if (!model.set_min(current_decision_, var_idx, update.value)) {
                return false;
            }
            if (community_analysis_.is_enabled() && propagation_source_ != SIZE_MAX) {
                community_analysis_.on_propagation(var_idx, propagation_source_);
            }
            // 確定した場合は on_instantiate、そうでなければ on_set_min
            if (!was_instantiated && model.is_instantiated(var_idx)) {
                if (!propagate_instantiate(model, var_idx, prev_min, prev_max)) {
                    return false;
                }
            } else if (!was_instantiated) {
                // ドメインのholeにより実際のminは要求値より大きい場合がある
                auto actual_new_min = model.var_min(var_idx);
                const auto& constraint_indices = model.constraints_for_var(var_idx);
                for (const auto& w : constraint_indices) {
                    if (!constraints[w.constraint_idx]->on_set_min(model, current_decision_,
                                                         var_idx, w.internal_var_idx, actual_new_min, prev_min)) {
                        bump_activity(model, w.constraint_idx);
                        return false;
                    }
                }
                // Bound NoGood 伝播
                if (nogood_learning_ && !nogood_mgr_.propagate_bound_nogoods(model, var_idx, true, stats_.restart_count, activity_, activity_inc_)) {
                    return false;
                }
            }
            break;
        }
        case PendingUpdate::Type::SetMax: {
            if (update.value >= prev_max) continue;  // 変化なし
            if (!model.set_max(current_decision_, var_idx, update.value)) {
                return false;
            }
            if (community_analysis_.is_enabled() && propagation_source_ != SIZE_MAX) {
                community_analysis_.on_propagation(var_idx, propagation_source_);
            }
            // 確定した場合は on_instantiate、そうでなければ on_set_max
            if (!was_instantiated && model.is_instantiated(var_idx)) {
                if (!propagate_instantiate(model, var_idx, prev_min, prev_max)) {
                    return false;
                }
            } else if (!was_instantiated) {
                // ドメインのholeにより実際のmaxは要求値より小さい場合がある
                auto actual_new_max = model.var_max(var_idx);
                const auto& constraint_indices = model.constraints_for_var(var_idx);
                for (const auto& w : constraint_indices) {
                    if (!constraints[w.constraint_idx]->on_set_max(model, current_decision_,
                                                         var_idx, w.internal_var_idx, actual_new_max, prev_max)) {
                        bump_activity(model, w.constraint_idx);
                        return false;
                    }
                }
                // Bound NoGood 伝播
                if (nogood_learning_ && !nogood_mgr_.propagate_bound_nogoods(model, var_idx, false, stats_.restart_count, activity_, activity_inc_)) {
                    return false;
                }
            }
            break;
        }
        case PendingUpdate::Type::RemoveValue: {
            auto removed_value = update.value;
            if (!model.contains(var_idx, removed_value)) continue;  // 既に存在しない
            if (!model.remove_value(current_decision_, var_idx, removed_value)) {
                return false;
            }
            if (community_analysis_.is_enabled() && propagation_source_ != SIZE_MAX) {
                community_analysis_.on_propagation(var_idx, propagation_source_);
            }
            if (!was_instantiated && model.is_instantiated(var_idx)) {
                if (!propagate_instantiate(model, var_idx, prev_min, prev_max)) {
                    return false;
                }
            } else if (!was_instantiated) {
                auto new_min = model.var_min(var_idx);
                auto new_max = model.var_max(var_idx);
                const auto& constraint_indices = model.constraints_for_var(var_idx);

                // 下限が変化した場合 → on_set_min
                if (new_min > prev_min) {
                    for (const auto& w : constraint_indices) {
                        if (!constraints[w.constraint_idx]->on_set_min(model, current_decision_,
                                                             var_idx, w.internal_var_idx, new_min, prev_min)) {
                            bump_activity(model, w.constraint_idx);
                            return false;
                        }
                    }
                    // Bound NoGood 伝播
                    if (nogood_learning_ && !nogood_mgr_.propagate_bound_nogoods(model, var_idx, true, stats_.restart_count, activity_, activity_inc_)) {
                        return false;
                    }
                }
                // 上限が変化した場合 → on_set_max
                if (new_max < prev_max) {
                    for (const auto& w : constraint_indices) {
                        if (!constraints[w.constraint_idx]->on_set_max(model, current_decision_,
                                                             var_idx, w.internal_var_idx, new_max, prev_max)) {
                            bump_activity(model, w.constraint_idx);
                            return false;
                        }
                    }
                    // Bound NoGood 伝播
                    if (nogood_learning_ && !nogood_mgr_.propagate_bound_nogoods(model, var_idx, false, stats_.restart_count, activity_, activity_inc_)) {
                        return false;
                    }
                }
                // removed_value が新しい範囲内 → on_remove_value も呼ぶ
                if (removed_value > new_min && removed_value < new_max) {
                    for (const auto& w : constraint_indices) {
                        if (!constraints[w.constraint_idx]->on_remove_value(model, current_decision_,
                                                                  var_idx, w.internal_var_idx, removed_value)) {
                            bump_activity(model, w.constraint_idx);
                            return false;
                        }
                    }
                }
            }
            break;
        }
        }
    }

    return true;
}

} // namespace sabori_csp
