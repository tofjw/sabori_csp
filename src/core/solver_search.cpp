#include "sabori_csp/solver.hpp"
#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/one_hot_channel_aggregator.hpp"
#include <algorithm>
#include <limits>
#include <numeric>
#include <iomanip>
#include <iostream>

namespace sabori_csp {

// restart 探索ループ系（search_with_restart[_optimize] とその helper）。solver.cpp から分離。


void Solver::apply_restart_bookkeeping(Model& model) {
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
}

void Solver::resample_and_reshuffle(Model& model) {
    // restart 前: 報酬更新と p 抽選
    mode_policy_.update_and_resample(rng_);
    // スキャン順シャッフル（タイブレークのランダム化、各区間を独立に）
    var_selector_.shuffle(rng_);
    var_selector_.init_tracking(model);
    unassigned_trail_.clear();
}

void Solver::finish_search_on_timeout() {
    if (community_analysis_.is_enabled() && verbose_) {
        community_analysis_.print_dynamic_report(std::cerr, stats_.restart_count + 1);
    }
    if (verbose_) {
        std::cerr << "% [verbose] search stopped (timeout)\n";
    }
    sync_nogood_stats();
}

Solver::FindAllAction Solver::handle_find_all_solution(
        Model& model, SolutionCallback& callback, const Solution& result, int root_point) {
    // 全解探索: コールバックに報告し、解をNGとして追加して続行
    if (!callback(result)) {
        sync_nogood_stats();
        return FindAllAction::Stop;  // コールバックが停止を要求
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
        return FindAllAction::Stop;
    }

    if (apply_unit_nogoods(model) != PropagationResult::Ok) {
        // Conflict (UNSAT) も Stopped (timeout) も探索終了。
        // 呼出側は is_stopped() で区別する。
        model.clear_pending_updates();
        sync_nogood_stats();
        return FindAllAction::Stop;
    }

    // unit nogood + permanent NG の伝播で全変数が確定する場合がある
    // その場合、有効な解なら報告して続行する
    while (var_selector_.all_assigned()) {
        if (verify_solution(model)) {
            auto sol = build_solution(model);
            if (!callback(sol)) {
                sync_nogood_stats();
                return FindAllAction::Stop;
            }
            model.clear_pending_updates();
            auto inner_lits = nogood_mgr_.collect_solution_literals(model);
            backtrack(model, root_point);
            nogood_mgr_.add_solution_nogood(model, inner_lits, stats_.restart_count);
            // If still all assigned after backtrack, no decisions to undo
            if (var_selector_.all_assigned()) {
                sync_nogood_stats();
                return FindAllAction::Stop;
            }
            if (apply_unit_nogoods(model) != PropagationResult::Ok) {
                model.clear_pending_updates();
                sync_nogood_stats();
                return FindAllAction::Stop;
            }
        } else {
            sync_nogood_stats();
            return FindAllAction::Stop;
        }
    }

    return FindAllAction::ContinueLoop;
}

Solver::ProbeAction Solver::run_improvement_probe(
        Model& model, SolutionCallback& callback, int root_point) {
    // --- improvement probe: best objective 側から ~5% 改善を軽量プローブで試みる ---
    // minimize: target = obj_ub - improvement (上端側から探索)
    //   SAT → best が大幅改善、UNSAT → 下位 5% をカット
    // maximize: target = obj_lb + improvement (下端側から探索)
    //   SAT → best が大幅改善、UNSAT → 上位 5% をカット
    if (probe_fail_limit_ <= 0 || !probe_enabled_) return ProbeAction::Continue;

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
    if (!target_useful) return ProbeAction::Continue;

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
            mode_policy_.note_improvement();
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
                if (pr == PropagationResult::Stopped) return ProbeAction::BreakInnerLoop;
                if (pr == PropagationResult::Conflict) {
                    model.clear_pending_updates();
                    sync_nogood_stats();
                    if (verbose_) {
                        std::cerr << "% [verbose] optimal (probe proved optimality)\n";
                    }
                    return ProbeAction::ReturnOptimal;
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
                if (pr == PropagationResult::Stopped) return ProbeAction::BreakInnerLoop;
                if (pr == PropagationResult::Conflict) {
                    model.clear_pending_updates();
                    sync_nogood_stats();
                    return ProbeAction::ReturnOptimal;
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
            if (pr == PropagationResult::Stopped) return ProbeAction::BreakInnerLoop;
            if (pr == PropagationResult::Conflict) {
                model.clear_pending_updates();
                sync_nogood_stats();
                if (verbose_) {
                    std::cerr << "% [verbose] optimal (probe UNSAT + bound tightening)\n";
                }
                return ProbeAction::ReturnOptimal;
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
            if (pr == PropagationResult::Stopped) return ProbeAction::BreakInnerLoop;
            if (pr == PropagationResult::Conflict) {
                model.clear_pending_updates();
                sync_nogood_stats();
                return ProbeAction::ReturnOptimal;
            }
        }
    }

    // 再初期化
    (void)apply_unit_nogoods(model);
    nogood_mgr_.rebuild_var_ng_blooms(model);
    ng_usage_bloom_ = Bloom512{};
    return ProbeAction::Continue;
}

Solver::ProbeAction Solver::run_bottomup_probe(
        Model& model, SolutionCallback& callback, int root_point) {
    // --- bottom-up optimistic probe: lb 側から obj <= lb+δ を投機的に試す ---
    // ペナルティ和型 (G1) では tight bound 自体が伝播ガイドになる
    // (Σpenalty <= K が大半のペナルティを 0 に強制し reified 網が連鎖する)。
    // 学習される NoGood は仮定 (Leq/Geq) を decision literal として含む
    // 条件付き連言なので大域健全。lb 引き上げは証明付き UNSAT のときのみ。
    if (bottomup_fail_limit_ <= 0) return ProbeAction::Continue;
    if (bottomup_skip_ > 0) {
        --bottomup_skip_;
        return ProbeAction::Continue;
    }

    // UNSAT (証明成功) が続く限り階段を連続で登る（生産的な間は投機を継続）。
    // UNKNOWN で長期撤退。1 呼び出しの総ステップ数は安全のため上限を置く。
    for (int step = 0; step < 64 && !stopped_; ++step) {

    auto obj_lb = model.var_min(obj_var_idx_);
    auto obj_ub = model.var_max(obj_var_idx_);
    if (obj_lb >= obj_ub) return ProbeAction::Continue;  // 既に一点

    Domain::value_type target;
    if (minimize_) {
        target = obj_lb + bottomup_delta_;
        if (target >= obj_ub) target = obj_ub - 1;  // 真の部分問題に留める
    } else {
        target = obj_ub - bottomup_delta_;
        if (target <= obj_lb) target = obj_lb + 1;
    }

    if (verbose_) {
        std::cerr << "% [verbose] bottomup probe: obj=[" << obj_lb << ".." << obj_ub
                  << "] target=" << target << " delta=" << bottomup_delta_
                  << " budget=" << bottomup_fail_limit_ << "\n";
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
    const size_t fails_before_step = stats_.fail_count;

    if (process_queue(model) == PropagationResult::Ok) {
        probe_propagation_ok = true;
        // cl はフレーム毎のリトライ予算で深さ方向に爆発し得るため、
        // グローバル fail 予算 (restart_fail_cutoff_) で打ち切る
        size_t saved_cutoff = restart_fail_cutoff_;
        restart_fail_cutoff_ = stats_.fail_count
            + static_cast<size_t>(bottomup_fail_limit_);
        res2 = run_search(model, std::numeric_limits<int>::max(), 0,
                          [&probe_solution](const Solution& sol) {
                              probe_solution = sol;
                              return false;
                          }, false);
        restart_fail_cutoff_ = saved_cutoff;
        if (res2 == SearchResult::SAT) {
            probe_obj = model.value(obj_var_idx_);
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
    bool probe_unsat = !probe_propagation_ok || res2 == SearchResult::UNSAT;

    // 仮定を除去して root へ
    decision_trail_.pop_back();
    model.clear_pending_updates();
    backtrack(model, root_point);
    current_decision_ = root_point;

    // probe の探索で var_selector_ の追跡状態が汚れるため再初期化
    // （improvement probe 後に呼び出し元が行うのと同じ処置。これを怠ると
    //   以降の select() が破綻し本探索が解を見つけられなくなる）
    var_selector_.init_tracking(model);
    unassigned_trail_.clear();

    if (res2 == SearchResult::SAT) {
        // obj_ub は root で best-1 に縮小済みなので probe 解は常に改善
        bottomup_unknown_streak_ = 0;
        mode_policy_.note_improvement();
        best_objective_ = probe_obj;
        best_solution_ = probe_solution;
        if (verbose_) {
            std::cerr << "% [verbose] bottomup probe improved: " << probe_obj << "\n";
        }
        if (callback) {
            callback(*probe_solution);
        }
        nogood_mgr_.enqueue_unit_nogoods(model);
        if (minimize_) {
            model.enqueue_set_max(obj_var_idx_, probe_obj - 1);
        } else {
            model.enqueue_set_min(obj_var_idx_, probe_obj + 1);
        }
        auto pr = process_queue(model);
        if (pr == PropagationResult::Stopped) return ProbeAction::BreakInnerLoop;
        if (pr == PropagationResult::Conflict) {
            model.clear_pending_updates();
            sync_nogood_stats();
            if (verbose_) {
                std::cerr << "% [verbose] optimal (bottomup probe proved optimality)\n";
            }
            return ProbeAction::ReturnOptimal;
        }
    } else if (probe_unsat) {
        // 証明付き UNSAT: obj > target が確定 → root で lb を引き上げ (健全)
        bottomup_unknown_streak_ = 0;
        bottomup_delta_ = bottomup_delta_ * 2 + 1;  // 階段を加速
        if (verbose_) {
            std::cerr << "% [verbose] bottomup probe UNSAT: lb past target=" << target
                      << " step_fails=" << (stats_.fail_count - fails_before_step) << "\n";
        }
        nogood_mgr_.enqueue_unit_nogoods(model);
        if (minimize_) {
            model.enqueue_set_min(obj_var_idx_, target + 1);
            if (best_objective_) model.enqueue_set_max(obj_var_idx_, *best_objective_ - 1);
        } else {
            model.enqueue_set_max(obj_var_idx_, target - 1);
            if (best_objective_) model.enqueue_set_min(obj_var_idx_, *best_objective_ + 1);
        }
        auto pr = process_queue(model);
        if (pr == PropagationResult::Stopped) return ProbeAction::BreakInnerLoop;
        if (pr == PropagationResult::Conflict) {
            // lb が incumbent (or ドメイン) を横断 → best が最適 (解なしなら UNSAT)
            model.clear_pending_updates();
            sync_nogood_stats();
            if (verbose_) {
                std::cerr << "% [verbose] optimal (bottomup lb crossed bound)\n";
            }
            return ProbeAction::ReturnOptimal;
        }
        // 相転移カットオフ: 証明は成功したがコストが急増している場合、
        // SAT/UNSAT 境界に接近している（次段はブローアウトの公算大）。
        // 実測 (zephyrus, opt=12): step_fails は 0 → 2.5k → 4.7k → 壁(>20k)
        // と超線形に上昇する。予算の 1/8 を超えたら階段を止め、lb 進捗を
        // 保持したまま撤退して残り予算を本探索（+ 蓄積した lb の伝播）に回す。
        if (bottomup_cutoff_denom_ > 0) {
            const size_t step_cost = stats_.fail_count - fails_before_step;
            if (step_cost * static_cast<size_t>(bottomup_cutoff_denom_)
                    > static_cast<size_t>(bottomup_fail_limit_)) {
                if (verbose_) {
                    std::cerr << "% [verbose] bottomup phase-transition cutoff: "
                              << "step_fails=" << step_cost << " lb="
                              << model.var_min(obj_var_idx_) << "\n";
                }
                ++bottomup_unknown_streak_;
                bottomup_skip_ = 64 << std::min(bottomup_unknown_streak_, 6);
                return ProbeAction::Continue;
            }
        }
        continue;  // 証明成功 (低コスト) → 次の階段を直ちに登る
    } else {
        // UNKNOWN: この tightness では予算内で決着せず → δ 半減 + 長期撤退
        // （投機は「生産的な間だけ」。決着しない問題では本探索に道を譲る）
        bottomup_delta_ /= 2;
        ++bottomup_unknown_streak_;
        bottomup_skip_ = 64 << std::min(bottomup_unknown_streak_, 6);
        nogood_mgr_.enqueue_unit_nogoods(model);
        if (best_objective_) {
            if (minimize_) {
                model.enqueue_set_max(obj_var_idx_, *best_objective_ - 1);
            } else {
                model.enqueue_set_min(obj_var_idx_, *best_objective_ + 1);
            }
        }
        auto pr = process_queue(model);
        if (pr == PropagationResult::Stopped) return ProbeAction::BreakInnerLoop;
        if (pr == PropagationResult::Conflict) {
            model.clear_pending_updates();
            sync_nogood_stats();
            return ProbeAction::ReturnOptimal;
        }
        return ProbeAction::Continue;  // UNKNOWN → 撤退
    }

    return ProbeAction::Continue;  // SAT → 本探索へ (obj は縮小済み)

    }  // staircase loop
    return ProbeAction::Continue;
}

bool Solver::run_root_probing(Model& model) {
    // --- root probing / failed literal 検出 (SABORI_PROBE_ROOT, opt-in) ---
    // ドメインサイズ2の未確定変数に両値を仮置き伝播し、片側矛盾なら反対値を
    // root で確定。伝播のみ (探索なし) なので1probe は安価。確定は伝播連鎖で
    // 他候補を落としうるため、進捗がある間は限定ラウンドで繰り返す。
    if (root_probe_limit_ <= 0) return true;
    const int root_point = current_decision_;

    // probe 伝播中の過渡的矛盾は record_constraint_call 経由で bump_activity を
    // 呼び、activity を汚染し rng_ を消費する (entailment フラグの教訓と同機構)。
    // probe は無指向の総当たりで bump に情報価値がないため、activity / rng /
    // activity_inc を snapshot/restore して探索軌道への副作用を消す
    // (fixed=0 なら無効時と bit 同一の状態で本探索に入る)。
    const std::vector<double> saved_activity = activity_;
    const std::vector<int> saved_temporal = temporal_activity_;
    const std::mt19937 saved_rng = rng_;
    const double saved_activity_inc = activity_inc_;
    std::vector<size_t> saved_order = var_selector_.var_order();

    std::vector<size_t> cand;
    const size_t n = model.variables().size();
    for (size_t i = 0; i < n; ++i) {
        if (!model.is_instantiated(i) && model.var_size(i) == 2) {
            cand.push_back(i);
        }
    }
    // 構造 activity の高い順 = 制約関与の濃い変数から (予算切れに備える)
    std::sort(cand.begin(), cand.end(), [&](size_t a, size_t b) {
        return activity_[a] > activity_[b];
    });

    int budget = root_probe_limit_;
    size_t probed = 0, fixed = 0;
    bool interrupted = false;

    // 1 probe: v を仮置きして伝播、矛盾なら true。必ず root へ巻き戻す
    auto probe_fails = [&](size_t vid, Domain::value_type v) {
        ++current_decision_;
        decision_trail_.push_back({vid, v, Literal::Type::Eq});
        model.enqueue_instantiate(vid, v);
        auto pr = process_queue(model);
        decision_trail_.pop_back();
        model.clear_pending_updates();
        backtrack(model, root_point);
        current_decision_ = root_point;
        if (pr == PropagationResult::Stopped) interrupted = true;
        return pr == PropagationResult::Conflict;
    };

    for (int round = 0; round < 3 && !interrupted; ++round) {
        bool progress = false;
        for (size_t vid : cand) {
            if (budget <= 0 || interrupted || stopped_) break;
            if (model.is_instantiated(vid)) continue;
            --budget;
            ++probed;
            const auto lo = model.var_min(vid);
            const auto hi = model.var_max(vid);

            Domain::value_type forced = 0;
            bool has_forced = false;
            if (probe_fails(vid, lo)) {
                forced = hi;
                has_forced = true;
            } else if (!interrupted && probe_fails(vid, hi)) {
                forced = lo;
                has_forced = true;
            }
            if (interrupted) break;
            if (!has_forced) continue;

            // 反対値を root レベルで確定 (全解探索でも健全: 矛盾側の値を
            // 持つ解は存在しない)
            model.enqueue_instantiate(vid, forced);
            auto pr = process_queue(model);
            if (pr == PropagationResult::Conflict) {
                return false;  // 両側矛盾 = root UNSAT
            }
            if (pr == PropagationResult::Stopped) {
                interrupted = true;
                break;
            }
            ++fixed;
            progress = true;
        }
        if (!progress || budget <= 0) break;
    }

    activity_ = saved_activity;
    temporal_activity_ = saved_temporal;
    rng_ = saved_rng;
    activity_inc_ = saved_activity_inc;
    if (probed > 0) {
        // probe 伝播の on_instantiate swap は var_order_ を恒久的に並べ替える
        // (backtrack は end しか戻さない) ため、保存した順序ごと復元する。
        // fixed=0 なら probe 前と bit 同一、fixed>0 なら確定分だけが後方へ移る。
        var_selector_.restore_order(std::move(saved_order), model);
        unassigned_trail_.clear();
    }
    if (verbose_) {
        std::cerr << "% [verbose] root probing: candidates=" << cand.size()
                  << " probed=" << probed << " fixed=" << fixed
                  << (interrupted ? " (interrupted)" : "") << "\n";
    }
    return true;
}

std::optional<Solution> Solver::search_with_restart(Model& model,
                                                      SolutionCallback callback,
                                                      bool find_all) {
    int root_point = current_decision_;

    if (!run_root_probing(model)) {
        return std::nullopt;  // root で矛盾 = UNSAT
    }

    if (verbose_) {
        std::cerr << "% [verbose] search_with_restart start"
                  << (find_all ? " (find_all)" : "") << "\n";
    }

    while (!stopped_) {
        // ===== cycle 開始 =====
        // restart_signal_live_: stats_ は探索終了時にしか同期されないため、
        // ライブ信号モードでは nogood_mgr_ のカウンタを直接読む（solver.hpp 参照）。
        size_t prune_at_cycle_start = restart_signal_live_
            ? nogood_mgr_.prune_count() : stats_.nogood_prune_count;
        size_t max_depth_at_cycle_start = stats_.max_depth;
        restart_ctrl_.begin_cycle();

        while (restart_ctrl_.inner_within_outer() && !stopped_) {
            int conflict_limit = restart_ctrl_.conflict_limit();
            std::optional<Solution> result;

            // fixed 系ポリシー: per-node 予算ではなくグローバル fail 数でカット
            int search_cl = conflict_limit;
            if (restart_ctrl_.is_fixed()) {
                restart_fail_cutoff_ = stats_.fail_count + static_cast<size_t>(conflict_limit);
                search_cl = std::numeric_limits<int>::max();
            }

            auto res = run_search(model, search_cl, 0,
                                  [&result](const Solution& sol) {
                                      result = sol;
                                      return false;
                                  }, false);
            restart_fail_cutoff_ = std::numeric_limits<size_t>::max();

            if (res == SearchResult::SAT) {
                if (find_all) {
                    if (handle_find_all_solution(model, callback, *result, root_point)
                            == FindAllAction::Stop) {
                        return std::nullopt;
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

            if (apply_unit_nogoods(model) != PropagationResult::Ok) {
                model.clear_pending_updates();
                sync_nogood_stats();
                return std::nullopt;
            }

            apply_restart_bookkeeping(model);

            // Temporal activity リセット
            std::fill(temporal_activity_.begin(), temporal_activity_.end(), 0);

            resample_and_reshuffle(model);

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
        size_t prune_now = restart_signal_live_
            ? nogood_mgr_.prune_count() : stats_.nogood_prune_count;
        size_t prune_delta = prune_now - prune_at_cycle_start;
        bool depth_grew = stats_.max_depth > max_depth_at_cycle_start;
        bool tightened = restart_ctrl_.end_cycle(prune_delta, depth_grew, rng_);

        if (verbose_) {
            std::cerr << "% [verbose] cycle end: prune_delta=" << prune_delta
                      << " depth_grew=" << depth_grew
                      << " decision=" << (tightened ? "tighten" : "widen")
                      << " new_outer=" << restart_ctrl_.outer() << "\n";
        }
    }

    finish_search_on_timeout();
    return std::nullopt;
}

std::optional<Solution> Solver::search_with_restart_optimize(
        Model& model, SolutionCallback callback) {
    int root_point = current_decision_;

    if (!run_root_probing(model)) {
        return std::nullopt;  // root で矛盾 = UNSAT
    }

    gradient_strategy_.clear();  // prev solution empty = まだ改善解なし

    if (verbose_) {
        std::cerr << "% [verbose] search_with_restart_optimize start\n";
    }

    while (!stopped_) {
        // ===== cycle 開始 =====
        // restart_signal_live_: stats_ は探索終了時にしか同期されないため、
        // ライブ信号モードでは nogood_mgr_ のカウンタを直接読む（solver.hpp 参照）。
        size_t prune_at_cycle_start = restart_signal_live_
            ? nogood_mgr_.prune_count() : stats_.nogood_prune_count;
        size_t domain_at_cycle_start = restart_signal_live_
            ? nogood_mgr_.domain_count() : stats_.nogood_domain_count;
        size_t max_depth_at_cycle_start = stats_.max_depth;
        bool cycle_interrupted = false;
        restart_ctrl_.begin_cycle();

        while (restart_ctrl_.inner_within_outer() && !stopped_) {
            int conflict_limit = restart_ctrl_.conflict_limit();
            std::optional<Solution> found_solution;

            // fixed 系ポリシー: per-node 予算ではなくグローバル fail 数でカット
            int search_cl = conflict_limit;
            if (restart_ctrl_.is_fixed()) {
                restart_fail_cutoff_ = stats_.fail_count + static_cast<size_t>(conflict_limit);
                search_cl = std::numeric_limits<int>::max();
            }

            auto res = run_search(model, search_cl, 0,
                                  [&found_solution](const Solution& sol) {
                                      found_solution = sol;
                                      return false;  // 最初の解で停止
                                  }, false);
            restart_fail_cutoff_ = std::numeric_limits<size_t>::max();

            if (res == SearchResult::SAT) {
                auto obj_val = model.value(obj_var_idx_);
                bool improved = !best_objective_ ||
                    (minimize_ && obj_val < *best_objective_) ||
                    (!minimize_ && obj_val > *best_objective_);

                if (improved) {
                    mode_policy_.note_improvement();
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
                    gradient_strategy_.disable_hint();
                    gradient_strategy_.compute(model, current_best_assignment_, activity_, rng_);
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

                // --- improvement probe ---
                {
                    ProbeAction pa = run_improvement_probe(model, callback, root_point);
                    if (pa == ProbeAction::ReturnOptimal) return best_solution_;
                    if (pa == ProbeAction::BreakInnerLoop) break;  // timeout → cycle 終端へ
                }
                // --- end improvement probe ---

                // 疑似勾配の計算と変数選択: off
                gradient_strategy_.disable_hint();

                gradient_strategy_.set_prev_solution(current_best_assignment_);

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

            apply_restart_bookkeeping(model);

            resample_and_reshuffle(model);

            // --- bottom-up optimistic probe (opt-in: SABORI_BOTTOMUP) ---
            {
                // SABORI_BOTTOMUP_ISOLATE: probe 中の conflict による
                // activity/temporal の bump を隔離（本探索の誘導を汚染しない）。
                // NoGood・lb 引き上げ・best 更新は残す（健全な成果物）。
                const bool isolate = bottomup_isolate_ && bottomup_fail_limit_ > 0
                                     && bottomup_skip_ == 0;
                if (isolate) {
                    bottomup_saved_activity_ = activity_;
                    bottomup_saved_temporal_ = temporal_activity_;
                }
                ProbeAction pa = run_bottomup_probe(model, callback, root_point);
                if (isolate) {
                    activity_ = bottomup_saved_activity_;
                    temporal_activity_ = bottomup_saved_temporal_;
                }
                if (pa == ProbeAction::ReturnOptimal) return best_solution_;
                if (pa == ProbeAction::BreakInnerLoop) break;  // timeout → cycle 終端へ
            }

            // 解が見つからなかった場合: 探索を多様化するため勾配を使わない
            gradient_strategy_.disable_hint();

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
            size_t prune_now = restart_signal_live_
                ? nogood_mgr_.prune_count() : stats_.nogood_prune_count;
            size_t domain_now = restart_signal_live_
                ? nogood_mgr_.domain_count() : stats_.nogood_domain_count;
            size_t prune_delta = prune_now - prune_at_cycle_start;
            size_t domain_delta = domain_now - domain_at_cycle_start;
            bool depth_grew = stats_.max_depth > max_depth_at_cycle_start;
            bool tightened = restart_ctrl_.end_cycle(prune_delta + domain_delta, depth_grew, rng_);

            if (verbose_) {
                std::cerr << "% [verbose] cycle end: prune_delta=" << prune_delta
                          << " domain_delta=" << domain_delta
                          << " depth_grew=" << depth_grew
                          << " decision=" << (tightened ? "tighten" : "widen")
                          << " new_outer=" << restart_ctrl_.outer() << "\n";
            }
        }
    }

    finish_search_on_timeout();
    return best_solution_;
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

} // namespace sabori_csp
