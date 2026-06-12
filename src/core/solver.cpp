#include "sabori_csp/solver.hpp"
#include <cassert>
#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/one_hot_channel_aggregator.hpp"
#include <algorithm>
#include <functional>
#include <limits>
#include <numeric>
#include <iomanip>
#include <iostream>

namespace sabori_csp {

namespace {
/// 学習ポリシーバンディットの有効判定 (SABORI_LEARN_BANDIT、既定 off)
bool learn_bandit_enabled() {
    static const bool v = []{
        const char* e = std::getenv("SABORI_LEARN_BANDIT");
        return e ? (std::atoi(e) != 0) : false;
    }();
    return v;
}

/// -L 時に NG 由来の activity bump を使うか (SABORI_NG_BUMP、既定 1)
/// 0 にすると activity は制約駆動 (bump_activity / init_activity) のみになる。
/// 仮説: 制約は自分の構造を知った上で bump しており、NG 由来の長節 bump は
/// その調整済みバランスを乱すノイズかもしれない
bool ng_bump_enabled() {
    static const bool v = []{
        const char* e = std::getenv("SABORI_NG_BUMP");
        return e ? (std::atoi(e) != 0) : true;
    }();
    return v;
}

/// resolution-seen bump のスケール (SABORI_LEARN_SEEN_BUMP、既定 0.01、0=無効)
/// 1UIP 分析で「見た」変数を bump する (MiniSat 流 VSIDS)。最終節のリテラル
/// だけだと、説明成功時に現レベルの決定変数が resolution で消えて
/// クレジットが届かなくなるための対策
double learn_seen_bump_scale() {
    static const double v = []{
        const char* e = std::getenv("SABORI_LEARN_SEEN_BUMP");
        // 既定 0 (無効)。全年度ゲート: 0.001 で 59/53/TO24 と、無効時の
        // 63/55/TO22 に純勝ち・TO の両方で届かず。0.01 は cg2023 無解化。
        // 理論上は正しい方向 (1UIP で決定変数からクレジットが消える対策) だが
        // 現スケールでは利得なし。env で実験継続可能
        return e ? std::atof(e) : 0.0;
    }();
    return v;
}

/// 学習バンディットの EMA 減衰 (SABORI_LEARN_DECAY、既定 0 = 直近エポック100%)
/// 直近100%は win-stay / lose-shift 型: 成功アームに強く張り付き、
/// 失敗時は全アームが床値に落ちて一様探索に戻る
double learn_bandit_decay() {
    static const double v = []{
        const char* e = std::getenv("SABORI_LEARN_DECAY");
        return e ? std::atof(e) : 0.0;
    }();
    return v;
}
} // namespace


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

PropagationResult Solver::apply_unit_nogoods(Model& model) {
    if (nogood_mgr_.unit_nogoods().empty()) return PropagationResult::Ok;
    {
        // unit nogood は root 事実。stale な発生源/aux を継承しないようタグ付け
        ScopedPropagator sp(model, Model::kSourcePresolve);
        nogood_mgr_.enqueue_unit_nogoods(model);
    }
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
    if (learning_enabled_) {
        inference_trail_.clear();
        level_start_.clear();
        var_trail_index_.assign(variables.size(), {});
        mark_stamp_.clear();
        mark_epoch_ = 0;
        apply_fail_.valid = false;
    }
    stats_ = SolverStats{};
    instance_stats_.assign(model.constraints().size(), ConstraintStats{});
    model.resize_var_ng_bloom(variables.size());
    ng_usage_bloom_ = Bloom512{};
    restart_ctrl_.reset();

    if (verbose_) log_presolve_start(model);
    if (!presolve(model)) {
        if (verbose_) std::cerr << "% [verbose] presolve failed\n";
        return false;
    }

    // 勾配に関わる変数インデックスを収集（勾配候補の高速列挙用）
    {
        const auto& all_constraints = model.constraints();
        gradient_eligible_vars_.clear();
        for (size_t vi = 0; vi < variables.size(); ++vi) {
            if (model.is_eliminated(vi))
                continue;

            if (model.is_defined_var(vi))
                continue;

            if (model.is_instantiated(vi))
                continue;

            // ある程度範囲が広いものに限る
            if (model.presolve_max(vi) - model.presolve_min(vi) < 50)
                continue;

            gradient_eligible_vars_.push_back(vi);
        }
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

        // all_different 制約の変数数・値数・比率を表示
        for (const auto& c : model.constraints()) {
            auto* ad = dynamic_cast<const AllDifferentConstraint*>(c.get());
            if (!ad) continue;
            size_t total = ad->var_ids_ref().size();
            size_t unfixed = ad->unfixed_count();
            size_t vals = ad->pool_size();
            double ratio = vals > 0 ? static_cast<double>(unfixed) / vals : 0.0;
            std::cerr << "% [verbose] all_different: vars=" << total
                      << " (unfixed=" << unfixed << ")"
                      << " vals=" << vals
                      << " ratio=" << std::fixed << std::setprecision(2) << ratio << "\n";
        }
    }
    // 制約固有の初期 activity を設定
    for (const auto& c : model.constraints()) {
        c->init_activity(model, activity_.data());
    }
    // 初期 activity を最大値 100.0 にスケーリング（探索中の bump が埋もれないように）
    {
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
    }

    if (community_analysis_.is_enabled()) {
        community_analysis_.build_vig(model);
        community_analysis_.detect_communities(rng_);
        if (verbose_) community_analysis_.print_static_report(std::cerr);
    }
    var_selector_.init_tracking(model);
    if (verbose_) {
        std::cerr << "% [verbose] search vars: decision=" << var_selector_.decision_var_end()
                  << " defined=" << (var_selector_.defined_var_end() - var_selector_.decision_var_end())
                  << " unconstrained=" << (var_selector_.var_order().size() - var_selector_.defined_var_end())
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
    in_optimize_search_ = false;
    obj_cycle_start_valid_ = false;
    ema_obj_delta_ = 0.0;
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
                    // リテラル収集はバックトラック前、NoGood 登録は root 確定変数を
                    // 除外するためバックトラック後に行う
                    auto sol_lits = nogood_mgr_.collect_solution_literals(model);
                    backtrack(model, root_point);
                    nogood_mgr_.add_solution_nogood(model, sol_lits, stats_.restart_count);

                    // If still all assigned after backtrack, no decisions were undone
                    // (presolve fully solved the problem) — no more solutions exist.
                    if (var_selector_.all_assigned()) {
                        sync_nogood_stats();
                        return std::nullopt;
                    }

                    if (apply_unit_nogoods(model) != PropagationResult::Ok) {
                        // Conflict (UNSAT) も Stopped (timeout) も探索終了。
                        // 呼出側は is_stopped() で区別する。
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
                            auto inner_lits = nogood_mgr_.collect_solution_literals(model);
                            backtrack(model, root_point);
                            nogood_mgr_.add_solution_nogood(model, inner_lits, stats_.restart_count);
                            // If still all assigned after backtrack, no decisions to undo
                            if (var_selector_.all_assigned()) {
                                sync_nogood_stats();
                                return std::nullopt;
                            }
                            if (apply_unit_nogoods(model) != PropagationResult::Ok) {
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
            if (__builtin_expect(learning_enabled_, 0)) {
                nogood_mgr_.remove_transient();  // fallback 節はリスタートを跨がせない
            }

            if (apply_unit_nogoods(model) != PropagationResult::Ok) {
                model.clear_pending_updates();
                sync_nogood_stats();
                return std::nullopt;
            }

            stats_.restart_count++;
            if (community_analysis_.is_enabled()) {
                if (verbose_) community_analysis_.print_dynamic_report(std::cerr, stats_.restart_count);
                community_analysis_.reset_stats();
            }
            current_best_assignment_ = select_best_assignment();
            ng_usage_bloom_ = Bloom512{};

            // リスタート後の起点変数を選択（探索多様化）
            var_selector_.select_restart_pivot(model, activity_, rng_);

            // NoGood GC + ブルームフィルタ再構築
            nogood_mgr_.gc(stats_.restart_count, nogood_inactive_restart_limit_);
            nogood_mgr_.rebuild_var_ng_blooms(model);

            // Activity 減衰
            decay_activities();

            // Temporal activity リセット
            std::fill(temporal_activity_.begin(), temporal_activity_.end(), 0);

            // restart 前: 報酬更新と p 抽選
            //   signal = 改善あり ? 2.0 : 1/(1+restart_max_depth_)
            //   r ← α*r + (1-α)*bucket_signal  (α = kModeRewardDecay)
            //   bucket_signal: active = signal, 隣接 = 0.1*signal, それ以外 = 0
            {
                double signal = improvement_in_restart_
                    ? 2.0
                    : 1.0 / static_cast<double>(1 + restart_max_depth_);
                improvement_in_restart_ = false;
                restart_max_depth_ = 0;
                double total = 0.0;
                for (size_t i = 0; i < kModeGridSize; ++i) {
                    double bucket_signal = 0.0;
                    if (i == current_p_idx_) {
                        bucket_signal = signal;
                    } else if (i + 1 == current_p_idx_ || i == current_p_idx_ + 1) {
                        bucket_signal = 0.1 * signal;
                    }
                    mode_reward_[i] = kModeRewardDecay * mode_reward_[i]
                                    + (1.0 - kModeRewardDecay) * bucket_signal;
                    mode_reward_[i] = std::max(mode_reward_[i], kModeRewardFloor);
                    total += mode_reward_[i];
                }
                std::uniform_real_distribution<double> mode_dist(0.0, total);
                double pick = mode_dist(rng_);
                double acc = 0.0;
                current_p_idx_ = kModeGridSize - 1;
                for (size_t i = 0; i < kModeGridSize; ++i) {
                    acc += mode_reward_[i];
                    if (pick < acc) {
                        current_p_idx_ = i;
                        break;
                    }
                }
                mix_p_ = static_cast<double>(current_p_idx_) / static_cast<double>(kModeGridSize - 1);

                // 学習ポリシーのバンディット。最適化では改善の「大きさ」を
                // EMA 正規化したシグナルで評価する (小刻み改善と大ジャンプを区別)
                // 既定 off (全年度ゲート 54/55 < 静的 full 63/55)。RNG 消費も避ける
                if (__builtin_expect(learning_enabled_, 0) && learn_bandit_enabled()) {
                    double lsignal = signal;
                    if (in_optimize_search_ && best_objective_.has_value()) {
                        const double cur = static_cast<double>(*best_objective_);
                        if (obj_cycle_start_valid_) {
                            const double delta = std::abs(cur - obj_at_cycle_start_);
                            if (delta > 0.0) {
                                ema_obj_delta_ = (ema_obj_delta_ <= 0.0)
                                    ? delta : 0.7 * ema_obj_delta_ + 0.3 * delta;
                                lsignal = std::min(8.0, 2.0 * delta / ema_obj_delta_);
                            } else {
                                lsignal = signal;  // 改善なし: mix と同じ低シグナル
                            }
                        }
                        obj_at_cycle_start_ = cur;
                        obj_cycle_start_valid_ = true;
                    }
                    learn_epoch_signal_ += lsignal;
                    if (++learn_epoch_pos_ >= kLearnEpochLen) {
                        const double epoch_mean =
                            learn_epoch_signal_ / static_cast<double>(kLearnEpochLen);
                        learn_epoch_signal_ = 0.0;
                        learn_epoch_pos_ = 0;
                        double larm_total = 0.0;
                        const double ldecay = learn_bandit_decay();
                        for (size_t i = 0; i < kLearnArmCount; ++i) {
                            double bucket_signal = 0.0;
                            if (i == learn_arm_) {
                                bucket_signal = epoch_mean;
                            } else if (i + 1 == learn_arm_ || i == learn_arm_ + 1) {
                                bucket_signal = 0.1 * epoch_mean;
                            }
                            learn_arm_reward_[i] = ldecay * learn_arm_reward_[i]
                                                 + (1.0 - ldecay) * bucket_signal;
                            learn_arm_reward_[i] = std::max(learn_arm_reward_[i], kModeRewardFloor);
                            larm_total += learn_arm_reward_[i];
                        }
                        std::uniform_real_distribution<double> larm_dist(0.0, larm_total);
                        double lpick = larm_dist(rng_);
                        double lacc = 0.0;
                        learn_arm_ = kLearnArmCount - 1;
                        for (size_t i = 0; i < kLearnArmCount; ++i) {
                            lacc += learn_arm_reward_[i];
                            if (lpick < lacc) {
                                learn_arm_ = i;
                                break;
                            }
                        }
                    }
                    ++learn_arm_cycles_[learn_arm_];
                }
            }

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

    if (community_analysis_.is_enabled() && verbose_) {
        community_analysis_.print_dynamic_report(std::cerr, stats_.restart_count + 1);
    }
    if (verbose_) {
        std::cerr << "% [verbose] search stopped (timeout)\n";
        if (learning_enabled_) {
            std::cerr << "% [verbose] learning: explained=" << learn_explained_count_
                      << " fallback=" << learn_fallback_count_ << "\n";
        }
    }
    sync_nogood_stats();
    return std::nullopt;
}

std::optional<Solution> Solver::search_with_restart_optimize(
        Model& model, SolutionCallback callback) {
    int root_point = current_decision_;

    prev_improving_solution_.clear();  // empty() = true means no previous solution yet
    optimize_no_incumbent_ = true;
    in_optimize_search_ = true;
    obj_cycle_start_valid_ = false;
    ema_obj_delta_ = 0.0;
    learn_epoch_signal_ = 0.0;
    learn_epoch_pos_ = 0;
    gradient_.clear();
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
                    improvement_in_restart_ = true;
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

                    // 疑似勾配の計算と変数選択（probe にもヒントを与える）
                    // 改善差分から方向を更新し、対象変数をランダムに1つ選ぶ。
                    // 古い値が残らないよう、再選択できなかった場合は未設定状態にする。
                    gradient_var_idx_ = SIZE_MAX;
                    gradient_direction_ = 0;

                    // porbe の時は勾配が有効にすると少し改善することがある
                    double min_activity = -1.0;
                    size_t max_var_size = 0;
                    if (!gradient_eligible_vars_.empty() && !prev_improving_solution_.empty()) {
                        const auto& variables = model.variables();
                        if (gradient_.empty()) {
                            gradient_.assign(variables.size(), 0.0);
                        }
                        for (size_t vi : gradient_eligible_vars_) {
                            if (prev_improving_solution_[vi] != kNoValue &&
                                current_best_assignment_[vi] != kNoValue) {
                                double delta = static_cast<double>(current_best_assignment_[vi] - prev_improving_solution_[vi]);
                                if (delta < 0)
                                    gradient_[vi] = -1.0;
                                else if (delta > 0.0)
                                    gradient_[vi] = 1.0;
                                else
                                    gradient_[vi] = 0.0;
                            }
                        }
                        std::uniform_int_distribution<size_t> idist(0, gradient_eligible_vars_.size() - 1);
                        size_t start_idx = idist(rng_);
                        for (size_t i = 0; i < gradient_eligible_vars_.size(); i++) {
                            auto idx = (start_idx + i) % gradient_eligible_vars_.size();
                            size_t vi = gradient_eligible_vars_[idx];
                            double g = gradient_[vi];
                            if (g != 0.0
                                && !(g < 0.0 && current_best_assignment_[vi] == model.presolve_min(vi))
                                && !(g > 0.0 && current_best_assignment_[vi] == model.presolve_max(vi))) {
                                if ((min_activity < 0 || activity_[vi] < min_activity)
                                    || (activity_[vi] == min_activity && max_var_size < model.var_size(vi))) {
                                    gradient_var_idx_ = vi;
                                    gradient_direction_ = (g > 0.0) ? +1 : -1;
                                    gradient_ref_val_ = current_best_assignment_[vi];
                                    min_activity = activity_[vi];
                                    max_var_size = model.var_size(vi);
                                }
                            }
                        }
                    }
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
                    model.current_propagator_ = Model::kSourceDecision;
                    model.enqueue_set_max(obj_var_idx_, obj_val - 1);
                } else {
                    model.current_propagator_ = Model::kSourceDecision;
                    model.enqueue_set_min(obj_var_idx_, obj_val + 1);
                }

                {
                    auto pr = process_queue(model);
                    if (pr == PropagationResult::Stopped) break;  // timeout → fall through
                    if (pr == PropagationResult::Conflict) {
                        // 伝播で UNSAT → 最適解が確定
                        model.clear_pending_updates();
                        sync_nogood_stats();
                        if (verbose_) {
                            std::cerr << "% [verbose] optimal (propagation proved no improvement)\n";
                        }
                        return best_solution_;
                    }
                }

                // --- improvement probe: best objective 側から ~5% 改善を軽量プローブで試みる ---
                // minimize: target = obj_ub - improvement (上端側から探索)
                //   SAT → best が大幅改善、UNSAT → 下位 5% をカット
                // maximize: target = obj_lb + improvement (下端側から探索)
                //   SAT → best が大幅改善、UNSAT → 上位 5% をカット
                if (probe_fail_limit_ > 0) {
                    auto obj_lb = model.var_min(obj_var_idx_);
                    auto obj_ub = model.var_max(obj_var_idx_);
                    auto range = obj_ub - obj_lb;
                    auto improvement = std::max(range / 20, static_cast<Domain::value_type>(1));

                    Domain::value_type target;
                    if (minimize_) {
                        target = obj_ub - improvement;       // obj <= target
                    } else {
                        target = obj_lb + improvement;       // obj >= target
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
                            model.current_propagator_ = Model::kSourceDecision;
                    model.enqueue_set_max(obj_var_idx_, target);
                        } else {
                            decision_trail_.push_back({obj_var_idx_, target, Literal::Type::Geq});
                            model.current_propagator_ = Model::kSourceDecision;
                    model.enqueue_set_min(obj_var_idx_, target);
                        }

                        Domain::value_type probe_obj = 0;
                        std::optional<Solution> probe_solution;
                        SearchResult res2 = SearchResult::UNKNOWN;
                        bool probe_propagation_ok = false;

                        in_probe_ = true;
                        if (process_queue(model) == PropagationResult::Ok) {
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

                        in_probe_ = false;
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
                                improvement_in_restart_ = true;
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

                                {
                                    auto pr = process_queue(model);
                                    if (pr == PropagationResult::Stopped) break;
                                    if (pr == PropagationResult::Conflict) {
                                        model.clear_pending_updates();
                                        sync_nogood_stats();
                                        if (verbose_) {
                                            std::cerr << "% [verbose] optimal (probe proved optimality)\n";
                                        }
                                        return best_solution_;
                                    }
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
                                {
                                    auto pr = process_queue(model);
                                    if (pr == PropagationResult::Stopped) break;
                                    if (pr == PropagationResult::Conflict) {
                                        model.clear_pending_updates();
                                        sync_nogood_stats();
                                        return best_solution_;
                                    }
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
                            {
                                auto pr = process_queue(model);
                                if (pr == PropagationResult::Stopped) break;
                                if (pr == PropagationResult::Conflict) {
                                    model.clear_pending_updates();
                                    sync_nogood_stats();
                                    if (verbose_) {
                                        std::cerr << "% [verbose] optimal (probe UNSAT + bound tightening)\n";
                                    }
                                    return best_solution_;
                                }
                            }
                        } else {
                            // UNKNOWN: 情報なし、root 状態を再構築
                            nogood_mgr_.enqueue_unit_nogoods(model);
                            if (minimize_) {
                                model.enqueue_set_max(obj_var_idx_, *best_objective_ - 1);
                            } else {
                                model.enqueue_set_min(obj_var_idx_, *best_objective_ + 1);
                            }
                            {
                                auto pr = process_queue(model);
                                if (pr == PropagationResult::Stopped) break;
                                if (pr == PropagationResult::Conflict) {
                                    model.clear_pending_updates();
                                    sync_nogood_stats();
                                    return best_solution_;
                                }
                            }
                        }

                        // 再初期化
                        (void)apply_unit_nogoods(model);
                        nogood_mgr_.rebuild_var_ng_blooms(model);
                        ng_usage_bloom_ = Bloom512{};
                    }
                }
                // --- end improvement probe ---

                // 疑似勾配の計算と変数選択: off
                gradient_var_idx_ = SIZE_MAX;
                gradient_direction_ = 0;

                prev_improving_solution_ = current_best_assignment_;
                optimize_no_incumbent_ = false;
                if (__builtin_expect(learning_enabled_, 0)) {
                    // 新 incumbent: 旧 incumbent 近傍で学んだ fallback 節は
                    // 改善経路を塞ぐだけなので捨てる
                    nogood_mgr_.remove_transient();
                }

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
            if (__builtin_expect(learning_enabled_, 0)) {
                nogood_mgr_.remove_transient();  // fallback 節はリスタートを跨がせない
            }
            current_decision_ = root_point;

            {
                auto pr = apply_unit_nogoods(model);
                if (pr == PropagationResult::Stopped) break;
                if (pr == PropagationResult::Conflict) {
                    // unit nogood の適用で UNSAT → 最適解が確定
                    model.clear_pending_updates();
                    sync_nogood_stats();
                    return best_solution_;
                }
            }

            stats_.restart_count++;
            if (community_analysis_.is_enabled()) {
                if (verbose_) community_analysis_.print_dynamic_report(std::cerr, stats_.restart_count);
                community_analysis_.reset_stats();
            }
            current_best_assignment_ = select_best_assignment();
            ng_usage_bloom_ = Bloom512{};

            // リスタート後の起点変数を選択（探索多様化）
            var_selector_.select_restart_pivot(model, activity_, rng_);

            // NoGood GC + ブルームフィルタ再構築
            nogood_mgr_.gc(stats_.restart_count, nogood_inactive_restart_limit_);
            nogood_mgr_.rebuild_var_ng_blooms(model);

            // Activity 減衰
            decay_activities();

            // restart 前: 報酬更新と p 抽選
            //   signal = 改善あり ? 2.0 : 1/(1+restart_max_depth_)
            //   r ← α*r + (1-α)*bucket_signal  (α = kModeRewardDecay)
            //   bucket_signal: active = signal, 隣接 = 0.1*signal, それ以外 = 0
            {
                double signal = improvement_in_restart_
                    ? 2.0
                    : 1.0 / static_cast<double>(1 + restart_max_depth_);
                improvement_in_restart_ = false;
                restart_max_depth_ = 0;
                double total = 0.0;
                for (size_t i = 0; i < kModeGridSize; ++i) {
                    double bucket_signal = 0.0;
                    if (i == current_p_idx_) {
                        bucket_signal = signal;
                    } else if (i + 1 == current_p_idx_ || i == current_p_idx_ + 1) {
                        bucket_signal = 0.1 * signal;
                    }
                    mode_reward_[i] = kModeRewardDecay * mode_reward_[i]
                                    + (1.0 - kModeRewardDecay) * bucket_signal;
                    mode_reward_[i] = std::max(mode_reward_[i], kModeRewardFloor);
                    total += mode_reward_[i];
                }
                std::uniform_real_distribution<double> mode_dist(0.0, total);
                double pick = mode_dist(rng_);
                double acc = 0.0;
                current_p_idx_ = kModeGridSize - 1;
                for (size_t i = 0; i < kModeGridSize; ++i) {
                    acc += mode_reward_[i];
                    if (pick < acc) {
                        current_p_idx_ = i;
                        break;
                    }
                }
                mix_p_ = static_cast<double>(current_p_idx_) / static_cast<double>(kModeGridSize - 1);

                // 学習ポリシーのバンディット。最適化では改善の「大きさ」を
                // EMA 正規化したシグナルで評価する (小刻み改善と大ジャンプを区別)
                // 既定 off (全年度ゲート 54/55 < 静的 full 63/55)。RNG 消費も避ける
                if (__builtin_expect(learning_enabled_, 0) && learn_bandit_enabled()) {
                    double lsignal = signal;
                    if (in_optimize_search_ && best_objective_.has_value()) {
                        const double cur = static_cast<double>(*best_objective_);
                        if (obj_cycle_start_valid_) {
                            const double delta = std::abs(cur - obj_at_cycle_start_);
                            if (delta > 0.0) {
                                ema_obj_delta_ = (ema_obj_delta_ <= 0.0)
                                    ? delta : 0.7 * ema_obj_delta_ + 0.3 * delta;
                                lsignal = std::min(8.0, 2.0 * delta / ema_obj_delta_);
                            } else {
                                lsignal = signal;  // 改善なし: mix と同じ低シグナル
                            }
                        }
                        obj_at_cycle_start_ = cur;
                        obj_cycle_start_valid_ = true;
                    }
                    learn_epoch_signal_ += lsignal;
                    if (++learn_epoch_pos_ >= kLearnEpochLen) {
                        const double epoch_mean =
                            learn_epoch_signal_ / static_cast<double>(kLearnEpochLen);
                        learn_epoch_signal_ = 0.0;
                        learn_epoch_pos_ = 0;
                        double larm_total = 0.0;
                        const double ldecay = learn_bandit_decay();
                        for (size_t i = 0; i < kLearnArmCount; ++i) {
                            double bucket_signal = 0.0;
                            if (i == learn_arm_) {
                                bucket_signal = epoch_mean;
                            } else if (i + 1 == learn_arm_ || i == learn_arm_ + 1) {
                                bucket_signal = 0.1 * epoch_mean;
                            }
                            learn_arm_reward_[i] = ldecay * learn_arm_reward_[i]
                                                 + (1.0 - ldecay) * bucket_signal;
                            learn_arm_reward_[i] = std::max(learn_arm_reward_[i], kModeRewardFloor);
                            larm_total += learn_arm_reward_[i];
                        }
                        std::uniform_real_distribution<double> larm_dist(0.0, larm_total);
                        double lpick = larm_dist(rng_);
                        double lacc = 0.0;
                        learn_arm_ = kLearnArmCount - 1;
                        for (size_t i = 0; i < kLearnArmCount; ++i) {
                            lacc += learn_arm_reward_[i];
                            if (lpick < lacc) {
                                learn_arm_ = i;
                                break;
                            }
                        }
                    }
                    ++learn_arm_cycles_[learn_arm_];
                }
            }

            // スキャン順シャッフル
            var_selector_.shuffle(rng_);
            var_selector_.init_tracking(model);
            unassigned_trail_.clear();

            // 解が見つからなかった場合: 探索を多様化するため勾配を使わない
            gradient_var_idx_ = SIZE_MAX;
            gradient_direction_ = 0;

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

    if (community_analysis_.is_enabled() && verbose_) {
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
            if (current_depth > restart_max_depth_) {
                restart_max_depth_ = current_depth;
            }

            // 決定ごとに mix_p_ で activity_first を抽選
            // 1024 段階で離散化（rng() コスト最小、グリッド解像度より細かい）
            bool activity_first = (static_cast<double>(rng_() & 1023) < mix_p_ * 1024.0);
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

void Solver::record_inference(uint32_t var_idx, int64_t value,
                              Literal::Type type, uint32_t source_cid,
                              uint32_t aux) {
    const size_t cd = static_cast<size_t>(current_decision_);
    if (level_start_.size() > cd + 1) {
        truncate_inference_trail(level_start_[cd + 1]);
        level_start_.resize(cd + 1);
    }
    while (level_start_.size() <= cd) {
        level_start_.push_back(static_cast<uint32_t>(inference_trail_.size()));
    }
    if (var_trail_index_.size() <= var_idx) var_trail_index_.resize(var_idx + 1);
    var_trail_index_[var_idx].push_back(
        static_cast<uint32_t>(inference_trail_.size()));
    inference_trail_.push_back({value, var_idx,
                                source_cid & 0xFFFFFFu,
                                static_cast<uint32_t>(type), aux});
}

void Solver::analyze_conflict(const Model& model, const Literal* trial,
                              std::vector<Literal>& out) {
    // 1UIP 衝突分析 (docs-dev/conflict-learning-design.md §4)。
    // 説明できない局面では decision-trail nogood に全面フォールバックする
    // (= 従来挙動。C2 の退化性)。
    out.clear();
    const int L = current_decision_;
    const size_t trail_end = inference_trail_.size();
    const auto& constraints = model.constraints();
    // resolution-seen bump: 分析で「見た」変数へのクレジット (1変数1回)
    const double seen_bump = activity_inc_ * learn_seen_bump_scale();

    auto fallback = [&](const std::string& why) {
        out.assign(decision_trail_.begin(), decision_trail_.end());
        if (trial) out.push_back(*trial);  // 現試行の決定リテラル
        ++learn_fallback_count_;
        if (learn_diag_) ++learn_fb_reasons_[why];
    };

    if (decision_trail_.empty() || trail_end == 0) { fallback("empty-trail"); return; }

    // 深い探索では説明分析のコスト (locate/bounds_at の O(trail) スキャン
    // × 数百リテラル) が割に合わないためスキップし、従来の decision-trail
    // nogood に切り替える。既定 64 は cg2023/cargo2018/marte/solbat での
    // 実測に基づく (SABORI_LEARN_DEPTH_CAP で上書き可、0 = 無制限)
    static const size_t depth_cap = []{
        const char* e = std::getenv("SABORI_LEARN_DEPTH_CAP");
        return e ? static_cast<size_t>(std::atoi(e)) : size_t{64};
    }();
    if (depth_cap && decision_trail_.size() > depth_cap) { fallback("depth-cap"); return; }

    // ---- 衝突の種 ----
    reason_buf_.clear();
    bool seeded = false;
    std::string seed_fail_detail;
    if (last_conflict_source_ == Model::kSourceNoGood) {
        if (NoGood* ng = nogood_mgr_.last_conflict_nogood()) {
            reason_buf_ = ng->literals;  // 全リテラル成立 = 矛盾
            seeded = true;
        }
    } else if (last_conflict_source_ == kSourceApplyFail && apply_fail_.valid) {
        // 伝播由来の更新が適用時に現ドメインと矛盾 (wipeout)。
        // 失敗リテラルはトレイルに無いので、ここで
        // 「矛盾相手の bound 事実」+「推論そのものの説明」に展開して種にする。
        const auto af = apply_fail_;
        apply_fail_.valid = false;
        const auto cur_min = model.var_min(af.var_idx);
        const auto cur_max = model.var_max(af.var_idx);
        bool dom_ok = true;
        switch (static_cast<Literal::Type>(af.lit_type)) {
        case Literal::Type::Eq:
            if (cur_min > af.value) {
                reason_buf_.push_back({af.var_idx, cur_min, Literal::Type::Geq});
            } else if (cur_max < af.value) {
                reason_buf_.push_back({af.var_idx, cur_max, Literal::Type::Leq});
            } else {
                dom_ok = false;  // 内部 hole: bounds 語彙で表現できない
            }
            break;
        case Literal::Type::Geq:
            if (cur_max < af.value) {
                reason_buf_.push_back({af.var_idx, cur_max, Literal::Type::Leq});
            } else {
                dom_ok = false;
            }
            break;
        default:  // Leq
            if (cur_min > af.value) {
                reason_buf_.push_back({af.var_idx, cur_min, Literal::Type::Geq});
            } else {
                dom_ok = false;
            }
            break;
        }
        if (dom_ok) {
            const uint32_t src = af.source_cid & 0xFFFFFFu;
            if (src == (Model::kSourceNoGood & 0xFFFFFFu)) {
                if (NoGood* ng = nogood_mgr_.find_by_id(af.aux)) {
                    size_t sat = 0;
                    for (const auto& lit : ng->literals) {
                        if (lit.is_satisfied(model)) { reason_buf_.push_back(lit); ++sat; }
                    }
                    seeded = (sat + 1 == ng->literals.size());
                    if (!seeded) seed_fail_detail = "applyfail:nogood";
                }
            } else if (src < constraints.size()) {
                struct SeedCtx : ExplainContext {
                    const Solver* s; const Model* m; size_t pos;
                    size_t trail_pos() const override { return pos; }
                    std::pair<Domain::value_type, Domain::value_type>
                    bounds_at(size_t var_idx) const override {
                        return s->bounds_at(*m, static_cast<uint32_t>(var_idx), pos);
                    }
                } ctx;
                ctx.s = this; ctx.m = &model; ctx.pos = trail_end;
                seeded = constraints[src]->explain(model, ctx, af.var_idx, af.value,
                                                   af.lit_type, af.aux, reason_buf_);
                if (!seeded) seed_fail_detail = "applyfail:" + constraints[src]->name();
            } else {
                seed_fail_detail = "applyfail:src-" + std::to_string(src);
            }
        } else {
            seed_fail_detail = "applyfail:dom";
        }
    } else if (last_conflict_source_ < constraints.size()) {
        seeded = constraints[last_conflict_source_]->explain_failure(model, reason_buf_);
    }
    if (!seeded) {
        std::string why = "seed:";
        if (!seed_fail_detail.empty()) why += seed_fail_detail;
        else if (last_conflict_source_ == Model::kSourceNoGood) why += "nogood-lost";
        else if (last_conflict_source_ < constraints.size())
            why += constraints[last_conflict_source_]->name();
        else why += "src-" + std::to_string(last_conflict_source_);
        fallback(why);
        return;
    }

    // ---- 準備 ----
    if (mark_stamp_.size() < trail_end) mark_stamp_.resize(trail_end, 0);
    ++mark_epoch_;
    below_buf_.clear();
    size_t current_count = 0;
    size_t resolution_limit = trail_end;  // 理由はこの位置より前で成立していること

    auto level_of = [&](size_t idx) -> int {
        auto it = std::upper_bound(level_start_.begin(), level_start_.end(),
                                   static_cast<uint32_t>(idx));
        return static_cast<int>(it - level_start_.begin()) - 1;
    };

    // 事実 l を分類して登録する。false = 続行不能(全面フォールバック)
    std::string addfact_why;
    std::vector<size_t> step_marked;  // 解決1ステップ内で新規マークした位置（巻き戻し用）
    std::function<bool(const Literal&)> add_fact = [&](const Literal& l) -> bool {
        // presolve (root) で既に真なら学習節に不要 (locate より先に判定する:
        // 自明に真なリテラルが後段の無関係なエントリにマッチして
        // acyclicity 誤判定になるのを防ぐ)
        switch (l.type) {
        case Literal::Type::Eq:
            if (model.presolve_min(l.var_idx) == l.value &&
                model.presolve_max(l.var_idx) == l.value) return true;
            break;
        case Literal::Type::Geq:
            if (model.presolve_min(l.var_idx) >= l.value) return true;
            break;
        default:
            if (model.presolve_max(l.var_idx) <= l.value) return true;
            break;
        }
        // 成立時点のトレイルエントリ(最古の満たすエントリ)を探す。
        // 変数別索引で当該変数のエントリのみ走査する（旧: 全トレイル走査）
        ptrdiff_t found = -1;
        const auto& vlist = var_trail_index_[l.var_idx];
        for (size_t k = vlist.size(); k-- > 0;) {
            const size_t i = vlist[k];
            const auto& e = inference_trail_[i];
            const auto ty = static_cast<Literal::Type>(e.lit_type);
            bool relevant, sat;
            switch (l.type) {
            case Literal::Type::Eq:
                relevant = (ty == Literal::Type::Eq);
                sat = relevant && (e.value == l.value);
                break;
            case Literal::Type::Geq:
                relevant = (ty == Literal::Type::Geq || ty == Literal::Type::Eq);
                sat = relevant && (e.value >= l.value);
                break;
            default:  // Leq
                relevant = (ty == Literal::Type::Leq || ty == Literal::Type::Eq);
                sat = relevant && (e.value <= l.value);
                break;
            }
            if (!relevant) continue;
            if (sat) {
                found = static_cast<ptrdiff_t>(i);  // より古い成立点を探し続ける
            } else if (found >= 0) {
                break;  // bounds の単調性: これより古い関連エントリは満たさない
            }
        }

        if (found < 0) {
            if (l.type == Literal::Type::Eq) {
                // SetMin/SetMax の合成で成立した Eq → bounds に分解して再試行
                return add_fact({l.var_idx, l.value, Literal::Type::Geq}) &&
                       add_fact({l.var_idx, l.value, Literal::Type::Leq});
            }
            addfact_why = "locate";
            return false;  // 成立点不明 → 健全側に倒してフォールバック
        }

        if (static_cast<size_t>(found) >= resolution_limit) {
            addfact_why = "acyclic";
            static int trace_left = []{ const char* e = std::getenv("SABORI_LEARN_TRACE");
                                        return e ? std::atoi(e) : 0; }();
            if (trace_left > 0) {
                --trace_left;
                std::cerr << "% [trace] acyclic: lit(var=" << l.var_idx
                          << (l.type == Literal::Type::Eq ? " ==" :
                              l.type == Literal::Type::Geq ? " >=" : " <=")
                          << l.value << ") found=" << found
                          << " limit=" << resolution_limit
                          << " trail_end=" << trail_end
                          << " found_src=";
                const auto& fe = inference_trail_[static_cast<size_t>(found)];
                if ((fe.reason_cid) < constraints.size())
                    std::cerr << constraints[fe.reason_cid]->name();
                else std::cerr << "0x" << std::hex << fe.reason_cid << std::dec;
                std::cerr << "\n";
            }
            return false;  // acyclicity 違反 (説明が未来を参照) → フォールバック
        }
        const int lv = level_of(static_cast<size_t>(found));
        if (lv <= 0) return true;  // レベル0の事実は常に真 → 不要
        if (lv >= L) {
            if (mark_stamp_[static_cast<size_t>(found)] != mark_epoch_) {
                mark_stamp_[static_cast<size_t>(found)] = mark_epoch_;
                ++current_count;
                step_marked.push_back(static_cast<size_t>(found));
                activity_[l.var_idx] += seen_bump;
            }
        } else {
            constexpr size_t kMaxBelow = 256;
            for (const auto& bl : below_buf_) {
                if (bl.first == l) return true;  // 重複排除
            }
            if (below_buf_.size() >= kMaxBelow) { addfact_why = "below-overflow"; return false; }  // 爆発防御
            below_buf_.push_back({l, lv});
            activity_[l.var_idx] += 0.1 * seen_bump;  // 下位レベルは弱め
        }
        return true;
    };

    for (const auto& l : reason_buf_) {
        if (!add_fact(l)) { fallback("seed-fact:" + addfact_why); return; }
    }
    if (current_count == 0) { fallback("no-current-level"); return; }

    // ---- 1UIP 解決ループ (トレイルを逆走) ----
    size_t pos = trail_end;
    std::vector<Literal> local_reason;
    std::vector<Literal> kept_buf;  // 解決不能で節に残す現レベルのリテラル
    while (current_count > 1) {
        if (pos == 0) { fallback("pos0"); return; }  // 防御 (起こらないはず)
        --pos;
        if (mark_stamp_[pos] != mark_epoch_) continue;
        mark_stamp_[pos] = 0;
        --current_count;
        resolution_limit = pos;

        const auto& e = inference_trail_[pos];
        const uint32_t src = e.reason_cid;
        local_reason.clear();
        bool ok = false;
        if (src == (Model::kSourceNoGood & 0xFFFFFFu)) {
            if (NoGood* ng = nogood_mgr_.find_by_id(e.aux)) {
                // nogood N が正当化するのは「N の残りリテラルが全て真のとき、
                // 自変数のリテラル lit_k の否定」だけ。記録された推論
                // (適用後の実 bounds) はドメインの穴や前境界との合成で
                // ¬lit_k より強くなり得るため、厳密一致のみ説明とする
                // (不一致は demote)。値を検証しないと不健全な節になる
                // (2014/amaze の偽 UNSAT で実証)
                const Literal* on_var = nullptr;
                size_t unsat = 0;
                for (const auto& lit : ng->literals) {
                    if (lit.is_satisfied(model)) {
                        local_reason.push_back(lit);
                    } else {
                        ++unsat;
                        on_var = &lit;
                    }
                }
                if (unsat == 1 &&
                    on_var->var_idx == static_cast<size_t>(e.var_idx)) {
                    switch (static_cast<Literal::Type>(e.lit_type)) {
                    case Literal::Type::Geq:
                        // ¬(x <= v) = x >= v+1 と厳密一致するか
                        ok = (on_var->type == Literal::Type::Leq &&
                              on_var->value == e.value - 1);
                        break;
                    case Literal::Type::Leq:
                        ok = (on_var->type == Literal::Type::Geq &&
                              on_var->value == e.value + 1);
                        break;
                    default:  // Eq: バイナリ変数の ¬(x = 1-w) → x = w のみ
                        ok = (on_var->type == Literal::Type::Eq &&
                              (e.value == 0 || e.value == 1) &&
                              on_var->value == 1 - e.value &&
                              model.presolve_min(e.var_idx) == 0 &&
                              model.presolve_max(e.var_idx) == 1);
                        break;
                    }
                }
                if (!ok && learn_diag_) ++learn_fb_reasons_["ngdiag:strict-miss"];
            } else if (learn_diag_) {
                ++learn_fb_reasons_["ngdiag:id-lost"];
            }
        } else if (src < constraints.size()) {
            // 説明コンテキスト: 被説明推論の位置と、その時点の bounds 再構成
            struct Ctx : ExplainContext {
                const Solver* s; const Model* m; size_t pos;
                size_t trail_pos() const override { return pos; }
                std::pair<Domain::value_type, Domain::value_type>
                bounds_at(size_t var_idx) const override {
                    return s->bounds_at(*m, static_cast<uint32_t>(var_idx), pos);
                }
            } ctx;
            ctx.s = this; ctx.m = &model; ctx.pos = pos;
            ok = constraints[src]->explain(model, ctx, e.var_idx, e.value,
                                           static_cast<uint8_t>(e.lit_type),
                                           e.aux, local_reason);
        }
        // 説明できないエントリ（決定・未対応制約・verify 失敗）や、理由の
        // 登録に失敗したエントリは「解決せずリテラルとして学習節に残す」
        // （decision 扱いへの降格）。全面フォールバックより小さく強い節になる。
        bool resolved = false;
        if (ok) {
            step_marked.clear();
            const size_t below_before = below_buf_.size();
            resolved = true;
            for (const auto& l : local_reason) {
                if (!add_fact(l)) {
                    // この理由集合を破棄して降格（部分登録を巻き戻す）
                    for (size_t mp : step_marked) {
                        if (mark_stamp_[mp] == mark_epoch_) {
                            mark_stamp_[mp] = 0;
                            --current_count;
                        }
                    }
                    below_buf_.resize(below_before);
                    if (learn_diag_) ++learn_fb_reasons_["demote-fact:" + addfact_why];
                    resolved = false;
                    break;
                }
            }
        } else if (learn_diag_) {
            std::string why = "demote:";
            if (src == (Model::kSourceNoGood & 0xFFFFFFu)) why += "nogood";
            else if (src == (Model::kSourceDecision & 0xFFFFFFu)) why += "DECISION";
            else if (src == (Model::kSourcePresolve & 0xFFFFFFu)) why += "PRESOLVE";
            else if (src < constraints.size()) why += constraints[src]->name();
            else why += "src-" + std::to_string(src);
            ++learn_fb_reasons_[why];
        }
        if (!resolved) {
            kept_buf.push_back({e.var_idx, e.value,
                                static_cast<Literal::Type>(e.lit_type)});
        }
    }

    // ---- 残った1つ = UIP ----
    ptrdiff_t uip = -1;
    for (size_t j = pos; j-- > 0;) {
        if (mark_stamp_[j] == mark_epoch_) { uip = static_cast<ptrdiff_t>(j); break; }
    }
    if (uip < 0 && pos < trail_end && mark_stamp_[pos] == mark_epoch_) {
        uip = static_cast<ptrdiff_t>(pos);
    }
    if (uip < 0) { fallback("no-uip"); return; }

    // ---- nogood 組み立て: below + kept + UIP ----
    for (const auto& bl : below_buf_) out.push_back(bl.first);
    for (const auto& kl : kept_buf) out.push_back(kl);
    const auto& ue = inference_trail_[static_cast<size_t>(uip)];
    out.push_back({ue.var_idx, ue.value, static_cast<Literal::Type>(ue.lit_type)});
    ++learn_explained_count_;

#ifndef NDEBUG
    for (const auto& l : out) {
        assert(l.is_satisfied(model) && "学習 nogood に偽のリテラル (説明バグ)");
    }
#endif
}

std::pair<Domain::value_type, Domain::value_type> Solver::bounds_at(
    const Model& model, uint32_t var_idx, size_t t) const {
    // 推論時点 T の bounds 再構成。
    // 探索中の bounds はトレイル上で単調（min は増加のみ・max は減少のみ、
    // バックトラック時はトレイルごと切り詰め）なので、
    // 「T より前の最後の Geq/Leq/Eq エントリの値」がそのまま T 時点の bound になる。
    Domain::value_type lo = model.presolve_min(var_idx);
    Domain::value_type hi = model.presolve_max(var_idx);
    bool lo_found = false, hi_found = false;
    // 変数別索引上で t 未満の最後のエントリから逆走（旧: 全トレイル走査）
    const auto& vlist = var_trail_index_[var_idx];
    const size_t tcap = std::min(t, inference_trail_.size());
    size_t start = 0;
    if (tcap > 0) {
        start = std::upper_bound(vlist.begin(), vlist.end(),
                                 static_cast<uint32_t>(tcap - 1)) - vlist.begin();
    }
    for (size_t k = start; k-- > 0; ) {
        const auto& e = inference_trail_[vlist[k]];
        const auto ty = static_cast<Literal::Type>(e.lit_type);
        if (!lo_found && (ty == Literal::Type::Geq || ty == Literal::Type::Eq)) {
            lo = e.value;
            lo_found = true;
        }
        if (!hi_found && (ty == Literal::Type::Leq || ty == Literal::Type::Eq)) {
            hi = e.value;
            hi_found = true;
        }
        if (lo_found && hi_found) break;
    }
    return {lo, hi};
}

void Solver::learn_at_conflict(const Model& model, const Literal& trial,
                               bool from_bisect) {
    // 衝突検出の瞬間（モデルが衝突状態のまま、推論トレイルが有効なうち）に
    // 1UIP 分析して学習する。フォールバックは decision_trail_ + trial
    // 一時診断ノブ SABORI_LEARN_MODE (ビットマスク):
    //   1=bumpなし 2=分析のみ(学習なし) 4=フォールバック時は学習しない
    //   8=最適化で初解(incumbent)が出るまで学習しない
    static const int diag_mode = []{
        const char* e = std::getenv("SABORI_LEARN_MODE");
        return e ? std::atoi(e) : 0;
    }();
    const bool bandit_enabled = learn_bandit_enabled();
    static const size_t max_len = []{
        const char* e = std::getenv("SABORI_LEARN_MAXLEN");
        return e ? static_cast<size_t>(std::atoi(e)) : size_t{0};
    }();
    // 診断用: アーム固定 (SABORI_LEARN_ARM=0/1/2)
    static const int forced_arm = []{
        const char* e = std::getenv("SABORI_LEARN_ARM");
        return e ? std::atoi(e) : -1;
    }();
    if (forced_arm >= 0) learn_arm_ = static_cast<size_t>(forced_arm);
    // バンディット (診断モード未指定時のみ): arm 0 = 学習なし
    if (diag_mode == 0 && bandit_enabled && learn_arm_ == 0) return;
    const size_t fb_before = learn_fallback_count_;
    analyze_conflict(model, &trial, analysis_buf_);
    if (diag_mode & 2) return;
    if ((diag_mode & 16) && from_bisect) return;  // bisect 衝突では学習しない
    if ((diag_mode & 64) && in_probe_) return;  // probe 中の衝突から学習しない
    if ((diag_mode & 8) && optimize_no_incumbent_) return;
    const bool was_fallback = learn_fallback_count_ > fb_before;
    if ((diag_mode & 4) && was_fallback) return;
    // arm 1 = 説明節のみ
    if (diag_mode == 0 && bandit_enabled && learn_arm_ == 1 && was_fallback) return;
    if (max_len && !was_fallback && analysis_buf_.size() > max_len) return;
    const bool transient = was_fallback &&
        ((diag_mode & 32) || (diag_mode == 0 && bandit_enabled));
    nogood_mgr_.learn_from_conflict(analysis_buf_, activity_,
                                    ((diag_mode & 1) || !ng_bump_enabled())
                                        ? 0.0 : activity_inc_,
                                    stats_.restart_count, transient);
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
        // 全値枯渇によるプレフィクス nogood（従来挙動）。
        // 1UIP 学習は衝突検出点（try_* 内の learn_at_conflict）で別途行う
        nogood_mgr_.learn_from_conflict(decision_trail_, activity_,
                                        (learning_enabled_ && !ng_bump_enabled())
                                            ? 0.0 : activity_inc_,
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
    if (var_idx == gradient_var_idx_ && gradient_direction_ != 0) {
        // 先に全体をシャッフル
        for (size_t i = values.size() - 1; i > 0; --i) {
            size_t j = rng_() % (i + 1);
            if (i != j) std::swap(values[i], values[j]);
        }

        // ランダム（シャッフル済みなので最初に見つけた勾配方向の値）
        for (size_t i = 0; i < values.size(); ++i) {
            if ((gradient_direction_ > 0 && values[i] > gradient_ref_val_) ||
                (gradient_direction_ < 0 && values[i] < gradient_ref_val_)) {
                if (i != 0) std::swap(values[i], values[0]);
                break;
            }
        }
        // 1番目: gradient_ref_val_（ベスト解の値）
        for (size_t i = 1; i < values.size(); ++i) {
            if (values[i] == gradient_ref_val_) {
                if (i != 1) std::swap(values[i], values[1]);
                break;
            }
        }
        gradient_var_idx_ = SIZE_MAX;
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
        ng_usage_bloom_ |= model.var_ng_bloom(frame.var_idx);

        if (learning_enabled_) {
            // 決定自体を推論トレイルに記録（process_queue を通らないため明示）
            record_inference(static_cast<uint32_t>(frame.var_idx), val,
                             Literal::Type::Eq, Model::kSourceDecision);
        }
        bool propagate_ok = propagate_instantiate(model, frame.var_idx,
                                                   frame.prev_min, frame.prev_max);
        PropagationResult queue_res = PropagationResult::Conflict;
        if (propagate_ok) {
            queue_res = process_queue(model);
        }
        // propagate_ok == false のときの last_conflict_source_ は
        // bump_activity（ハンドラ失敗）/ nogood 伝播が設定済み（上書きしない）
        if (learning_enabled_ && queue_res == PropagationResult::Conflict) {
            learn_at_conflict(model, {frame.var_idx, val, Literal::Type::Eq});
        }
        if (propagate_ok) {
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
        ng_usage_bloom_ |= model.var_ng_bloom(frame.var_idx);

        // right_first なら branch==1 で右、branch==2 で左
        bool try_left = frame.right_first ? (frame.branch == 2) : (frame.branch == 1);

        Literal decision_lit;
        model.current_propagator_ = Model::kSourceDecision;  // 決定の発生源を明示
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
        if (learning_enabled_ && queue_res == PropagationResult::Conflict) {
            learn_at_conflict(model, decision_lit, /*from_bisect=*/true);
        }
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
        if (var_idx == gradient_var_idx_ && gradient_direction_ != 0) {
            // gradient_direction_ は gradient_ref_val_ を基準とした方向。
            // 右半分 [mid+1, prev_max] / 左半分 [prev_min, mid] のうち、
            // ref_val を超える/下回る値を含む側を先に試す。
            if (gradient_direction_ > 0) {
                if (prev_min < gradient_ref_val_ && gradient_ref_val_ <= prev_max) {
                    mid = gradient_ref_val_ - 1;
                    right_first = true;
                    gradient_var_idx_ = SIZE_MAX;
                }
                else if (prev_min <= gradient_ref_val_ && prev_min < prev_max) {
                    mid = gradient_ref_val_;
                    right_first = false;
                    gradient_var_idx_ = SIZE_MAX;
                }
                else {
                    right_first = (rng_() & 1) != 0;
                    gradient_var_idx_ = SIZE_MAX;
                }
            } else {
                if (prev_min <= gradient_ref_val_ && gradient_ref_val_ < prev_max) {
                    mid = gradient_ref_val_;
                    right_first = false;
                    gradient_var_idx_ = SIZE_MAX;
                }
                else if (prev_min < prev_max && gradient_ref_val_ <= prev_max) {
                    mid = gradient_ref_val_ - 1;
                    right_first = true;
                    gradient_var_idx_ = SIZE_MAX;
                }
                else {
                    right_first = (rng_() & 1) != 0;
                    gradient_var_idx_ = SIZE_MAX;
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
        // Phase 1 後: one-hot チャネリングの集約
        // 同一 x に紐付く int_eq_reif(x, const, b_i) 群を IntOneHotChannel へ置換。
        // prepare_propagation の前に行うことで、集約後の制約配列に対して watch
        // 構造が初期化される。
        OneHotChannelAggregator aggregator;
        if (!aggregator.aggregate(model, verbose_)) {
            return false;
        }
        // 制約配列が変化したため、変数 → 制約インデックスを再構築
        model.build_constraint_watch_list();

        // Phase 1 後: 内部構造を再構築（ドメイン変更に対する整合性保証）
        if (!model.prepare_propagation()) {
            return false;
        }
    }

    model.snapshot_presolve_bounds();

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
                    // 発生源の設定: ホットループのため RAII でなく直接代入
                    // (決定 enqueue の前には明示的に kSourceDecision へ戻す)
                    model.current_propagator_ = static_cast<uint32_t>(w.constraint_idx);
        if (verbose_) {
            auto& cs = constraint_stats_[constraints[w.constraint_idx]->name()];
            auto& is = instance_stats_[w.constraint_idx];
            cs.call_count++;
            is.call_count++;
            size_t before = model.pending_updates_size();
            if (!constraints[w.constraint_idx]->on_instantiate(model, current_decision_,
                                        var_idx, w.internal_var_idx, val, prev_min, prev_max)) {
                cs.fail_count++;
                cs.fail_depth_sum += current_decision_;
                is.fail_count++;
                is.fail_depth_sum += current_decision_;
                bump_activity(model, w.constraint_idx, var_idx);
                return false;
            }
            if (model.pending_updates_size() > before) { cs.reduction_count++; is.reduction_count++; }
        } else {
            if (!constraints[w.constraint_idx]->on_instantiate(model, current_decision_,
                                        var_idx, w.internal_var_idx, val, prev_min, prev_max)) {
                bump_activity(model, w.constraint_idx, var_idx);
                return false;
            }
        }
    }

    // NoGood チェック
    if (nogood_learning_) {
        ScopedPropagator sp_guard(model, Model::kSourceNoGood);
        if (!nogood_mgr_.propagate_eq_watches(model, var_idx, val,
                                               stats_.restart_count, activity_, (learning_enabled_ && !ng_bump_enabled()) ? 0.0 : activity_inc_)) {
            last_conflict_source_ = Model::kSourceNoGood;
            return false;
        }
        // instantiate は Leq/Geq 両方を充足しうる
        if (!nogood_mgr_.propagate_bound_nogoods(model, var_idx, true,
                                                  stats_.restart_count, activity_, (learning_enabled_ && !ng_bump_enabled()) ? 0.0 : activity_inc_)) {
            last_conflict_source_ = Model::kSourceNoGood;
            return false;
        }
        if (!nogood_mgr_.propagate_bound_nogoods(model, var_idx, false,
                                                  stats_.restart_count, activity_, (learning_enabled_ && !ng_bump_enabled()) ? 0.0 : activity_inc_)) {
            last_conflict_source_ = Model::kSourceNoGood;
            return false;
        }
    }

    return true;
}

void Solver::backtrack(Model& model, int save_point) {
    model.rewind_to(save_point);
    model.rewind_dirty_constraints(save_point);
    // 推論トレイルの eager 切り詰め。record_inference の lazy 切り詰めは
    // 「より深いレベル」しか落とさないため、同一レベルでの再試行
    // (enumerate の次値・bisect の反対側) で死んだ兄弟試行のエントリが残り、
    // bounds 単調性の前提を壊して analyze_conflict が死んだ枝の事実を
    // 参照してしまう (不健全な学習節の元)。ここで確実に落とす。
    if (__builtin_expect(learning_enabled_, 0)) {
        const size_t lv = static_cast<size_t>(save_point + 1);
        if (level_start_.size() > lv) {
            truncate_inference_trail(level_start_[lv]);
            level_start_.resize(lv);
        }
    }
    // パーティション境界とブルームフィルタを復元
    while (!unassigned_trail_.empty() && unassigned_trail_.back().level > save_point) {
        ng_usage_bloom_ = unassigned_trail_.back().ng_usage_bloom;
        var_selector_.restore_decision_end(unassigned_trail_.back().dec_end);
        var_selector_.restore_defined_end(unassigned_trail_.back().def_end);
        var_selector_.restore_unconstrained_end(unassigned_trail_.back().unc_end);
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
            std::cerr << "constraint verify error (is_satisfied): " << constraint->name()
                      << " [" << constraint->label() << "]" << std::endl;
            abort();
            return false;
        }
        if (!constraint->on_final_instantiate(model)) {
            std::cerr << "constraint verify error (on_final_instantiate): " << constraint->name()
                      << " [" << constraint->label() << "]" << std::endl;
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

PropagationResult Solver::process_queue(Model& model) {
    const auto& constraints = model.constraints();

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
                    note_apply_fail(update, Literal::Type::Eq, update.value);
                    return PropagationResult::Conflict;
                }
                // 同じ値で既に確定済み: ドメイン削減で確定した
                // 二重に同じイベントを呼ばない
                continue;
            }
            if (!model.instantiate(current_decision_, var_idx, update.value)) {
                note_apply_fail(update, Literal::Type::Eq, update.value);
                return PropagationResult::Conflict;
            }
            if (__builtin_expect(learning_enabled_, 0)) {
                record_inference(static_cast<uint32_t>(var_idx), update.value,
                                 Literal::Type::Eq, update.source_cid, update.aux);
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
                note_apply_fail(update, Literal::Type::Geq, update.value);
                return PropagationResult::Conflict;
            }
            if (__builtin_expect(learning_enabled_, 0)) {
                record_inference(static_cast<uint32_t>(var_idx), model.var_min(var_idx),
                                 Literal::Type::Geq, update.source_cid, update.aux);
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
                    // 発生源の設定: ホットループのため RAII でなく直接代入
                    // (決定 enqueue の前には明示的に kSourceDecision へ戻す)
                    model.current_propagator_ = static_cast<uint32_t>(w.constraint_idx);
                    if (verbose_) {
                        auto& cs = constraint_stats_[constraints[w.constraint_idx]->name()];
                        auto& is = instance_stats_[w.constraint_idx];
                        cs.call_count++;
                        is.call_count++;
                        size_t before = model.pending_updates_size();
                        if (!constraints[w.constraint_idx]->on_set_min(model, current_decision_,
                                                             var_idx, w.internal_var_idx, actual_new_min, prev_min)) {
                            cs.fail_count++;
                            cs.fail_depth_sum += current_decision_;
                            is.fail_count++;
                            is.fail_depth_sum += current_decision_;
                            bump_activity(model, w.constraint_idx, var_idx);
                            return PropagationResult::Conflict;
                        }
                        if (model.pending_updates_size() > before) { cs.reduction_count++; is.reduction_count++; }
                    } else {
                        if (!constraints[w.constraint_idx]->on_set_min(model, current_decision_,
                                                             var_idx, w.internal_var_idx, actual_new_min, prev_min)) {
                            bump_activity(model, w.constraint_idx, var_idx);
                            return PropagationResult::Conflict;
                        }
                    }
                }
                // Bound NoGood 伝播
                if (nogood_learning_) {
                    ScopedPropagator sp_ng(model, Model::kSourceNoGood);
                    if (!nogood_mgr_.propagate_bound_nogoods(model, var_idx, true, stats_.restart_count, activity_, (learning_enabled_ && !ng_bump_enabled()) ? 0.0 : activity_inc_)) {
                        last_conflict_source_ = Model::kSourceNoGood;
                        return PropagationResult::Conflict;
                    }
                }
            }
            break;
        }
        case PendingUpdate::Type::SetMax: {
            if (update.value >= prev_max) continue;  // 変化なし
            if (!model.set_max(current_decision_, var_idx, update.value)) {
                note_apply_fail(update, Literal::Type::Leq, update.value);
                return PropagationResult::Conflict;
            }
            if (__builtin_expect(learning_enabled_, 0)) {
                record_inference(static_cast<uint32_t>(var_idx), model.var_max(var_idx),
                                 Literal::Type::Leq, update.source_cid, update.aux);
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
                    // 発生源の設定: ホットループのため RAII でなく直接代入
                    // (決定 enqueue の前には明示的に kSourceDecision へ戻す)
                    model.current_propagator_ = static_cast<uint32_t>(w.constraint_idx);
                    if (verbose_) {
                        auto& cs = constraint_stats_[constraints[w.constraint_idx]->name()];
                        auto& is = instance_stats_[w.constraint_idx];
                        cs.call_count++;
                        is.call_count++;
                        size_t before = model.pending_updates_size();
                        if (!constraints[w.constraint_idx]->on_set_max(model, current_decision_,
                                                             var_idx, w.internal_var_idx, actual_new_max, prev_max)) {
                            cs.fail_count++;
                            cs.fail_depth_sum += current_decision_;
                            is.fail_count++;
                            is.fail_depth_sum += current_decision_;
                            bump_activity(model, w.constraint_idx, var_idx);
                            return PropagationResult::Conflict;
                        }
                        if (model.pending_updates_size() > before) { cs.reduction_count++; is.reduction_count++; }
                    } else {
                        if (!constraints[w.constraint_idx]->on_set_max(model, current_decision_,
                                                             var_idx, w.internal_var_idx, actual_new_max, prev_max)) {
                            bump_activity(model, w.constraint_idx, var_idx);
                            return PropagationResult::Conflict;
                        }
                    }
                }
                // Bound NoGood 伝播
                if (nogood_learning_) {
                    ScopedPropagator sp_ng(model, Model::kSourceNoGood);
                    if (!nogood_mgr_.propagate_bound_nogoods(model, var_idx, false, stats_.restart_count, activity_, (learning_enabled_ && !ng_bump_enabled()) ? 0.0 : activity_inc_)) {
                        last_conflict_source_ = Model::kSourceNoGood;
                        return PropagationResult::Conflict;
                    }
                }
            }
            break;
        }
        case PendingUpdate::Type::RemoveValue: {
            auto removed_value = update.value;
            if (!model.contains(var_idx, removed_value)) continue;  // 既に存在しない
            if (!model.remove_value(current_decision_, var_idx, removed_value)) {
                // バイナリなら Eq [var = 1-v] として説明可能。
                // 非バイナリでも、削除失敗 = 変数がシングルトン {v} なので
                // 発生源制約は現ドメインで失敗状態 → explain_failure を種にできる
                if (prev_min == 0 && prev_max == 1 &&
                    (removed_value == 0 || removed_value == 1)) {
                    note_apply_fail(update, Literal::Type::Eq, 1 - removed_value);
                } else if ((update.source_cid & 0xFFFFFFu) < model.constraints().size()) {
                    last_conflict_source_ = update.source_cid & 0xFFFFFFu;
                } else {
                    last_conflict_source_ = Model::kSourceDecision;
                }
                return PropagationResult::Conflict;
            }
            // バイナリドメインからの除去は Eq [x = 1-v] として記録（≠ リテラルは v1 対象外）
            if (__builtin_expect(learning_enabled_, 0)) {
                if (prev_min == 0 && prev_max == 1 &&
                    (removed_value == 0 || removed_value == 1)) {
                    record_inference(static_cast<uint32_t>(var_idx), 1 - removed_value,
                                     Literal::Type::Eq, update.source_cid, update.aux);
                } else {
                    // 境界値の除去で bounds が動いた場合は Geq/Leq として記録する。
                    // 記録しないと bounds_at の再構成・add_fact の locate が
                    // この変化を見られず、説明可能な衝突までフォールバックする
                    const auto nmin = model.var_min(var_idx);
                    const auto nmax = model.var_max(var_idx);
                    if (nmin > prev_min) {
                        record_inference(static_cast<uint32_t>(var_idx), nmin,
                                         Literal::Type::Geq, update.source_cid, update.aux);
                    }
                    if (nmax < prev_max) {
                        record_inference(static_cast<uint32_t>(var_idx), nmax,
                                         Literal::Type::Leq, update.source_cid, update.aux);
                    }
                }
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
                    // 発生源の設定: ホットループのため RAII でなく直接代入
                    // (決定 enqueue の前には明示的に kSourceDecision へ戻す)
                    model.current_propagator_ = static_cast<uint32_t>(w.constraint_idx);
                        if (verbose_) {
                            auto& cs = constraint_stats_[constraints[w.constraint_idx]->name()];
                            auto& is = instance_stats_[w.constraint_idx];
                            cs.call_count++;
                            is.call_count++;
                            size_t before = model.pending_updates_size();
                            if (!constraints[w.constraint_idx]->on_set_min(model, current_decision_,
                                                                 var_idx, w.internal_var_idx, new_min, prev_min)) {
                                cs.fail_count++;
                                cs.fail_depth_sum += current_decision_;
                                is.fail_count++;
                                is.fail_depth_sum += current_decision_;
                                bump_activity(model, w.constraint_idx, var_idx);
                                return PropagationResult::Conflict;
                            }
                            if (model.pending_updates_size() > before) { cs.reduction_count++; is.reduction_count++; }
                        } else {
                            if (!constraints[w.constraint_idx]->on_set_min(model, current_decision_,
                                                                 var_idx, w.internal_var_idx, new_min, prev_min)) {
                                bump_activity(model, w.constraint_idx, var_idx);
                                return PropagationResult::Conflict;
                            }
                        }
                    }
                    // Bound NoGood 伝播
                    if (nogood_learning_) {
                    ScopedPropagator sp_ng(model, Model::kSourceNoGood);
                    if (!nogood_mgr_.propagate_bound_nogoods(model, var_idx, true, stats_.restart_count, activity_, (learning_enabled_ && !ng_bump_enabled()) ? 0.0 : activity_inc_)) {
                        last_conflict_source_ = Model::kSourceNoGood;
                        return PropagationResult::Conflict;
                    }
                }
                }
                // 上限が変化した場合 → on_set_max
                if (new_max < prev_max) {
                    for (const auto& w : constraint_indices) {
                    // 発生源の設定: ホットループのため RAII でなく直接代入
                    // (決定 enqueue の前には明示的に kSourceDecision へ戻す)
                    model.current_propagator_ = static_cast<uint32_t>(w.constraint_idx);
                        if (verbose_) {
                            auto& cs = constraint_stats_[constraints[w.constraint_idx]->name()];
                            auto& is = instance_stats_[w.constraint_idx];
                            cs.call_count++;
                            is.call_count++;
                            size_t before = model.pending_updates_size();
                            if (!constraints[w.constraint_idx]->on_set_max(model, current_decision_,
                                                                 var_idx, w.internal_var_idx, new_max, prev_max)) {
                                cs.fail_count++;
                                cs.fail_depth_sum += current_decision_;
                                is.fail_count++;
                                is.fail_depth_sum += current_decision_;
                                bump_activity(model, w.constraint_idx, var_idx);
                                return PropagationResult::Conflict;
                            }
                            if (model.pending_updates_size() > before) { cs.reduction_count++; is.reduction_count++; }
                        } else {
                            if (!constraints[w.constraint_idx]->on_set_max(model, current_decision_,
                                                                 var_idx, w.internal_var_idx, new_max, prev_max)) {
                                bump_activity(model, w.constraint_idx, var_idx);
                                return PropagationResult::Conflict;
                            }
                        }
                    }
                    // Bound NoGood 伝播
                    if (nogood_learning_) {
                    ScopedPropagator sp_ng(model, Model::kSourceNoGood);
                    if (!nogood_mgr_.propagate_bound_nogoods(model, var_idx, false, stats_.restart_count, activity_, (learning_enabled_ && !ng_bump_enabled()) ? 0.0 : activity_inc_)) {
                        last_conflict_source_ = Model::kSourceNoGood;
                        return PropagationResult::Conflict;
                    }
                }
                }
                // removed_value が新しい範囲内 → on_remove_value も呼ぶ
                if (removed_value > new_min && removed_value < new_max) {
                    for (const auto& w : constraint_indices) {
                    // 発生源の設定: ホットループのため RAII でなく直接代入
                    // (決定 enqueue の前には明示的に kSourceDecision へ戻す)
                    model.current_propagator_ = static_cast<uint32_t>(w.constraint_idx);
                        if (verbose_) {
                            auto& cs = constraint_stats_[constraints[w.constraint_idx]->name()];
                            auto& is = instance_stats_[w.constraint_idx];
                            cs.call_count++;
                            is.call_count++;
                            size_t before = model.pending_updates_size();
                            if (!constraints[w.constraint_idx]->on_remove_value(model, current_decision_,
                                                                      var_idx, w.internal_var_idx, removed_value)) {
                                cs.fail_count++;
                                cs.fail_depth_sum += current_decision_;
                                is.fail_count++;
                                is.fail_depth_sum += current_decision_;
                                bump_activity(model, w.constraint_idx, var_idx);
                                return PropagationResult::Conflict;
                            }
                            if (model.pending_updates_size() > before) { cs.reduction_count++; is.reduction_count++; }
                        } else {
                            if (!constraints[w.constraint_idx]->on_remove_value(model, current_decision_,
                                                                      var_idx, w.internal_var_idx, removed_value)) {
                                bump_activity(model, w.constraint_idx, var_idx);
                                return PropagationResult::Conflict;
                            }
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

    if (verbose_) {
        auto& cs = constraint_stats_[constraints[batch_idx]->name()];
        auto& is = instance_stats_[batch_idx];
        cs.call_count++;
        is.call_count++;
        size_t before = model.pending_updates_size();
        ScopedPropagator sp_guard(model, static_cast<uint32_t>(batch_idx));
        if (!constraints[batch_idx]->propagate_batch(model, current_decision_)) {
            cs.fail_count++;
            cs.fail_depth_sum += current_decision_;
            is.fail_count++;
            is.fail_depth_sum += current_decision_;
            bump_activity(model, batch_idx, constraints[batch_idx]->var_ids_ref().front());
            return PropagationResult::Conflict;
        }
        if (model.pending_updates_size() > before) { cs.reduction_count++; is.reduction_count++; }
    } else {
        ScopedPropagator sp_guard(model, static_cast<uint32_t>(batch_idx));
        if (!constraints[batch_idx]->propagate_batch(model, current_decision_)) {
            bump_activity(model, batch_idx, constraints[batch_idx]->var_ids_ref().front());
            return PropagationResult::Conflict;
        }
    }
    }  // for (;;)

    return PropagationResult::Ok;
}

} // namespace sabori_csp
