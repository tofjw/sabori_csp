#include "sabori_csp/solver.hpp"
#include <algorithm>
#include <limits>
#include <numeric>
#include <iostream>

namespace sabori_csp {

Solver::Solver()
    : rng_(12345678) {}

std::optional<Solution> Solver::solve(Model& model) {
    std::optional<Solution> result;

    // 制約ウォッチリストを構築
    model.build_constraint_watch_list();

    // 初期化
    const auto& variables = model.variables();
    activity_.assign(variables.size(), 0.0);
    // var_order_ を decision vars | defined vars にパーティション分割
    var_order_.clear();
    var_order_.reserve(variables.size());
    std::vector<size_t> defined_vars;
    for (size_t i = 0; i < variables.size(); ++i) {
        if (model.is_defined_var(i)) {
            defined_vars.push_back(i);
        } else {
            var_order_.push_back(i);
        }
    }
    decision_var_end_ = var_order_.size();
    var_order_.insert(var_order_.end(), defined_vars.begin(), defined_vars.end());
    std::shuffle(var_order_.begin(), var_order_.begin() + decision_var_end_, rng_);
    std::shuffle(var_order_.begin() + decision_var_end_, var_order_.end(), rng_);
    decision_trail_.clear();
    nogoods_.clear();
    ng_watches_.clear();
    best_num_instantiated_ = 0;
    best_assignment_.clear();
    current_best_assignment_.clear();
    current_decision_ = 0;
    stats_ = SolverStats{};

    // presolve: 初期伝播 + 内部構造の構築
    if (verbose_) {
        std::cerr << "% [verbose] presolve start: " << model.constraints().size()
                  << " constraints, " << variables.size() << " variables\n";
    }
    if (!presolve(model)) {
        if (verbose_) std::cerr << "% [verbose] presolve failed\n";
        return std::nullopt;  // UNSAT
    }
    if (verbose_) std::cerr << "% [verbose] presolve done\n";

    // リスタート有効時は専用ループを使用
    if (restart_enabled_) {
        return search_with_restart(model, nullptr, false);
    }

    // リスタート無効時は単純探索
    int conflict_limit = std::numeric_limits<int>::max();
    auto res = run_search(model, conflict_limit, 0,
                          [&result](const Solution& sol) {
                              result = sol;
                              return false;  // 最初の解で停止
                          }, false);

    return result;
}

std::optional<Solution> Solver::solve_optimize(
        Model& model, size_t obj_var_idx, bool minimize,
        SolutionCallback on_improve) {
    // 最適化状態を設定
    optimizing_ = true;
    obj_var_idx_ = obj_var_idx;
    minimize_ = minimize;
    best_solution_ = std::nullopt;
    best_objective_ = std::nullopt;

    // solve() と同じ初期化
    model.build_constraint_watch_list();

    const auto& variables = model.variables();
    activity_.assign(variables.size(), 0.0);
    var_order_.clear();
    var_order_.reserve(variables.size());
    std::vector<size_t> defined_vars;
    for (size_t i = 0; i < variables.size(); ++i) {
        if (model.is_defined_var(i)) {
            defined_vars.push_back(i);
        } else {
            var_order_.push_back(i);
        }
    }
    decision_var_end_ = var_order_.size();
    var_order_.insert(var_order_.end(), defined_vars.begin(), defined_vars.end());
    std::shuffle(var_order_.begin(), var_order_.begin() + decision_var_end_, rng_);
    std::shuffle(var_order_.begin() + decision_var_end_, var_order_.end(), rng_);
    decision_trail_.clear();
    nogoods_.clear();
    ng_watches_.clear();
    best_num_instantiated_ = 0;
    best_assignment_.clear();
    current_best_assignment_.clear();
    current_decision_ = 0;
    stats_ = SolverStats{};

    if (verbose_) {
        std::cerr << "% [verbose] presolve start: " << model.constraints().size()
                  << " constraints, " << variables.size() << " variables\n";
    }
    if (!presolve(model)) {
        if (verbose_) std::cerr << "% [verbose] presolve failed\n";
        optimizing_ = false;
        return std::nullopt;
    }
    if (verbose_) std::cerr << "% [verbose] presolve done\n";

    auto result = search_with_restart_optimize(model, on_improve);
    optimizing_ = false;
    return result;
}

size_t Solver::solve_all(Model& model, SolutionCallback callback) {
    // 制約ウォッチリストを構築
    model.build_constraint_watch_list();

    // 初期化
    const auto& variables = model.variables();
    activity_.assign(variables.size(), 0.0);
    // var_order_ を decision vars | defined vars にパーティション分割
    var_order_.clear();
    var_order_.reserve(variables.size());
    std::vector<size_t> defined_vars;
    for (size_t i = 0; i < variables.size(); ++i) {
        if (model.is_defined_var(i)) {
            defined_vars.push_back(i);
        } else {
            var_order_.push_back(i);
        }
    }
    decision_var_end_ = var_order_.size();
    var_order_.insert(var_order_.end(), defined_vars.begin(), defined_vars.end());
    std::shuffle(var_order_.begin(), var_order_.begin() + decision_var_end_, rng_);
    std::shuffle(var_order_.begin() + decision_var_end_, var_order_.end(), rng_);
    decision_trail_.clear();
    nogoods_.clear();
    ng_watches_.clear();
    best_num_instantiated_ = 0;
    best_assignment_.clear();
    current_best_assignment_.clear();
    current_decision_ = 0;
    stats_ = SolverStats{};

    // presolve: 初期伝播 + 内部構造の構築
    if (!presolve(model)) {
        return 0;  // UNSAT
    }

    size_t count = 0;

    if (restart_enabled_) {
        // リスタート有効: search_with_restart で全解探索
        search_with_restart(model, [&](const Solution& sol) {
            count++;
            return callback(sol);
        }, true);
    } else {
        // リスタート無効: 従来の単純DFS全探索
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
    double inner_limit = initial_conflict_limit_;
    double outer_limit = 10.0;

    int root_point = current_decision_;
    size_t prev_fail_count = 0;

    if (verbose_) {
        std::cerr << "% [verbose] search_with_restart start"
                  << (find_all ? " (find_all)" : "") << "\n";
    }

    while (!stopped_) {
        for (int outer = 0; outer < static_cast<int>(outer_limit) && !stopped_; ++outer) {
            int conflict_limit = static_cast<int>(inner_limit);
            std::optional<Solution> result;

            size_t nogoods_before = nogoods_.size();
            auto res = run_search(model, conflict_limit, 0,
                                  [&result](const Solution& sol) {
                                      result = sol;
                                      return false;
                                  }, false);

            if (res == SearchResult::SAT) {
                if (find_all) {
                    // 全解探索: コールバックに報告し、解をNGとして追加して続行
                    if (!callback(*result)) {
                        stats_.nogoods_size = nogoods_.size();
                        return std::nullopt;  // コールバックが停止を要求
                    }
                    model.clear_pending_updates();
                    add_solution_nogood(model);
                    backtrack(model, root_point);
                    continue;
                }
                stats_.nogoods_size = nogoods_.size();
                return result;
            }
            if (res == SearchResult::UNSAT) {
                stats_.nogoods_size = nogoods_.size();
                return std::nullopt;
            }

            // UNKNOWN: リスタート
            model.clear_pending_updates();
            backtrack(model, root_point);
            stats_.restart_count++;
            current_best_assignment_ = select_best_assignment();

            // NG を有用度順にソート: permanent を先頭、次に last_active が大きいものを優先
            std::stable_sort(nogoods_.begin(), nogoods_.end(),
                [](const auto& a, const auto& b) {
                    if (a->permanent != b->permanent) return a->permanent > b->permanent;
                    return a->last_active > b->last_active;
                });


            // NG が増えなかった場合は inner_limit を増加
	    // (fail が発生しなかったケースを含む)
	    bool limit_changed = false;
            if (nogoods_.size() <= nogoods_before) {
	        inner_limit ++;
	        outer_limit ++;
		limit_changed = true;
            }

            // 容量管理: 末尾（冷たい NG）を削除（permanent は保護）
            while (nogoods_.size() > max_nogoods_) {
                if (nogoods_.back()->permanent) break;
                remove_nogood(nogoods_.back().get());
                nogoods_.pop_back();
            }

            // Activity 減衰
            decay_activities();

            // スキャン順シャッフル（タイブレークのランダム化、各区間を独立に）
            std::shuffle(var_order_.begin(), var_order_.begin() + decision_var_end_, rng_);
            std::shuffle(var_order_.begin() + decision_var_end_, var_order_.end(), rng_);

            // domain_size 優先と activity 優先を交互に切り替え
            activity_first_ = !activity_first_;

            if (!limit_changed) {
                // コンフリクト制限を増加
                inner_limit *= conflict_limit_multiplier_;

                if (inner_limit > outer_limit) {
                    outer_limit *= conflict_limit_multiplier_;
                    inner_limit = initial_conflict_limit_;
                }
            }
            prev_fail_count = stats_.fail_count;

            if (verbose_) {
                std::cerr << "% [verbose] restart #" << stats_.restart_count
                          << " conflict_limit=" << conflict_limit
                          << " fails=" << stats_.fail_count
                          << " max_depth=" << stats_.max_depth
                          << " nogoods=" << nogoods_.size() << "\n";
            }
        }
    }

    if (verbose_) {
        std::cerr << "% [verbose] search stopped (timeout)\n";
    }
    stats_.nogoods_size = nogoods_.size();
    return std::nullopt;
}

std::optional<Solution> Solver::search_with_restart_optimize(
        Model& model, SolutionCallback callback) {
    double inner_limit = initial_conflict_limit_;
    double outer_limit = 10.0;

    int root_point = current_decision_;

    if (verbose_) {
        std::cerr << "% [verbose] search_with_restart_optimize start\n";
    }

    while (!stopped_) {
        for (int outer = 0; outer < static_cast<int>(outer_limit) && !stopped_; ++outer) {
            int conflict_limit = static_cast<int>(inner_limit);
            std::optional<Solution> found_solution;

            size_t nogoods_before = nogoods_.size();
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
                    current_best_assignment_.clear();
                    const auto& variables = model.variables();
                    for (size_t i = 0; i < variables.size(); ++i) {
                        if (model.is_instantiated(i)) {
                            current_best_assignment_[i] = model.value(i);
                        }
                    }
                }

                // root へバックトラック
                model.clear_pending_updates();
                backtrack(model, root_point);
                current_decision_ = root_point;

                // 目的変数のドメインを縮小（永続的に root_point レベルで保存）
                if (minimize_) {
                    model.enqueue_set_max(obj_var_idx_, obj_val - 1);
                } else {
                    model.enqueue_set_min(obj_var_idx_, obj_val + 1);
                }

                if (!process_queue(model)) {
                    // 伝播で UNSAT → 最適解が確定
                    model.clear_pending_updates();
                    stats_.nogoods_size = nogoods_.size();
                    if (verbose_) {
                        std::cerr << "% [verbose] optimal (propagation proved no improvement)\n";
                    }
                    return best_solution_;
                }

                // リスタートパラメータを完全リセット（新しい問題空間に入るので）
                inner_limit = initial_conflict_limit_;
                outer_limit = 10.0;
                break;  // outer ループを抜けて while から再突入 → outer=0 から
            }

            if (res == SearchResult::UNSAT) {
                // 探索空間が尽きた → 最適 (or nullopt if no solution found)
                stats_.nogoods_size = nogoods_.size();
                if (verbose_) {
                    std::cerr << "% [verbose] optimal (search exhausted)\n";
                }
                return best_solution_;
            }

            // UNKNOWN: リスタート
            model.clear_pending_updates();
            backtrack(model, root_point);
            current_decision_ = root_point;
            stats_.restart_count++;
            current_best_assignment_ = select_best_assignment();

            // NG を有用度順にソート
            std::stable_sort(nogoods_.begin(), nogoods_.end(),
                [](const auto& a, const auto& b) {
                    if (a->permanent != b->permanent) return a->permanent > b->permanent;
                    return a->last_active > b->last_active;
                });

            // NG が増えなかった場合は inner_limit を増加
            bool limit_changed = false;
            if (nogoods_.size() <= nogoods_before) {
                inner_limit++;
                outer_limit++;
                limit_changed = true;
            }

            // 容量管理
            while (nogoods_.size() > max_nogoods_) {
                if (nogoods_.back()->permanent) break;
                remove_nogood(nogoods_.back().get());
                nogoods_.pop_back();
            }

            // Activity 減衰
            decay_activities();

            // スキャン順シャッフル
            std::shuffle(var_order_.begin(), var_order_.begin() + decision_var_end_, rng_);
            std::shuffle(var_order_.begin() + decision_var_end_, var_order_.end(), rng_);

            // domain_size 優先と activity 優先を交互に切り替え
            activity_first_ = !activity_first_;

            if (!limit_changed) {
                inner_limit *= conflict_limit_multiplier_;
                if (inner_limit > outer_limit) {
                    outer_limit *= conflict_limit_multiplier_;
                    inner_limit = initial_conflict_limit_;
                }
            }

            if (verbose_) {
                std::cerr << "% [verbose] restart #" << stats_.restart_count
                          << " conflict_limit=" << conflict_limit
                          << " fails=" << stats_.fail_count
                          << " max_depth=" << stats_.max_depth
                          << " nogoods=" << nogoods_.size()
                          << " best=" << (best_objective_ ? std::to_string(*best_objective_) : "none")
                          << "\n";
            }
        }
    }

    if (verbose_) {
        std::cerr << "% [verbose] search stopped (timeout)\n";
    }
    stats_.nogoods_size = nogoods_.size();
    return best_solution_;
}

void Solver::add_solution_nogood(const Model& model) {
    std::vector<Literal> lits;
    const auto& variables = model.variables();
    for (size_t i = 0; i < variables.size(); ++i) {
        // 定数変数（initial_range == 1）を除外: 全解で同じ値なので情報がなく、
        // NoGood の watched literal が定数に設定されると機能しなくなる
        if (model.is_instantiated(i) && model.initial_range(i) > 1) {
            lits.push_back({i, model.value(i)});
        }
    }
    if (!lits.empty()) {
        add_nogood(lits);
        nogoods_.back()->permanent = true;
    }
}

SearchResult Solver::run_search(Model& model, int conflict_limit, size_t depth,
                                 SolutionCallback callback, bool find_all) {
    // 明示的スタックによる反復探索（再帰によるスタックオーバーフローを回避）
    struct SearchFrame {
        size_t var_idx;
        std::vector<Domain::value_type> values;
        size_t value_idx;
        int save_point;
        Domain::value_type prev_min, prev_max;
        size_t nogoods_before;
        int remaining_cl;  // このレベルの残りコンフリクト予算
    };

    std::vector<SearchFrame> stack;
    SearchResult result = SearchResult::UNSAT;
    bool ascending = false;

    const auto& variables = model.variables();

    while (true) {
        if (!ascending) {
            // === 降下: 新レベルに入る ===
            size_t current_depth = depth + stack.size();

            // タイムアウトチェック
            if (stopped_) {
                result = SearchResult::UNKNOWN;
                ascending = true;
                continue;
            }

            // 統計更新
            stats_.depth_sum += current_depth;
            stats_.depth_count++;
            if (current_depth > stats_.max_depth) {
                stats_.max_depth = current_depth;
            }

            // 変数選択（全変数確定なら SIZE_MAX）
            size_t var_idx = select_variable(model);

            if (var_idx == SIZE_MAX) {
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
                continue;
            }

            int save_point = current_decision_;
            auto prev_min = model.var_min(var_idx);
            auto prev_max = model.var_max(var_idx);

            // 値を取得（ソートなし、オリジナル domain.values() を使用）
            auto& domain = variables[var_idx]->domain();
            auto values = domain.values();

            // 保存された値がある場合は優先（先頭と swap で O(1)）
            if (current_best_assignment_.count(var_idx)) {
                auto best_val = current_best_assignment_[var_idx];
                auto it = std::find(values.begin(), values.end(), best_val);
                if (it != values.end() && it != values.begin()) {
                    std::swap(*it, values[0]);
                }
            }

            size_t nogoods_before = nogoods_.size();

            // フレームの CL を決定（親のCLを継承）
            int cl = stack.empty() ? conflict_limit : stack.back().remaining_cl;

            stack.push_back({var_idx, std::move(values), 0,
                           save_point, prev_min, prev_max, nogoods_before, cl});
            // TRY VALUES へフォールスルー
        } else {
            // === 上昇: 子の結果を処理 ===
            if (stack.empty()) {
                return result;
            }

            auto& frame = stack.back();

            decision_trail_.pop_back();

            if (result == SearchResult::SAT) {
                stack.pop_back();
                continue;  // SAT を上へ伝播
            }

            if (result == SearchResult::UNKNOWN || frame.remaining_cl <= 1) {
                current_decision_--;
                backtrack(model, frame.save_point);
                stack.pop_back();
                result = SearchResult::UNKNOWN;
                continue;  // UNKNOWN を上へ伝播
            }

            // UNSAT: 次の値を試す
            frame.remaining_cl--;
            current_decision_--;
            backtrack(model, frame.save_point);
            frame.value_idx++;
            // TRY VALUES へフォールスルー
        }

        // === TRY VALUES ===
        auto& frame = stack.back();
        bool found_value = false;

        while (frame.value_idx < frame.values.size()) {
            auto val = frame.values[frame.value_idx];

            current_decision_++;

            // 値を割り当て（Trail に保存される）
            if (!model.instantiate(current_decision_, frame.var_idx, val)) {
                current_decision_--;
                frame.value_idx++;
                continue;
            }

            // 伝播
            bool propagate_ok = propagate_instantiate(model, frame.var_idx,
                                                       frame.prev_min, frame.prev_max);
            bool queue_ok = false;
            if (propagate_ok) {
                // キュー処理
                queue_ok = process_queue(model);
                if (queue_ok) {
                    decision_trail_.push_back({frame.var_idx, val});
                    ascending = false;
                    found_value = true;
                    break;  // 降下へ
                }
            }

            // 伝播失敗時はキューに残りがある可能性があるのでクリア
            if (!propagate_ok || !queue_ok) {
                model.clear_pending_updates();
            }

            current_decision_--;
            backtrack(model, frame.save_point);
            frame.value_idx++;
        }

        if (!found_value) {
            // 全値失敗: Activity 更新
            activity_[frame.var_idx] += 1.0;
            stats_.fail_count++;

            // 部分解を保存
            save_partial_assignment(model);

            // 子ノードで追加された NoGood を削除
            while (nogoods_.size() > frame.nogoods_before) {
                remove_nogood(nogoods_.back().get());
                nogoods_.pop_back();
            }

            // NoGood 記録
            if (nogood_learning_ && decision_trail_.size() >= 2) {
                add_nogood(decision_trail_);
                for (const auto& lit : decision_trail_) {
                    activity_[lit.var_idx] += 1.0 / decision_trail_.size();
                }
            }

            stack.pop_back();
            result = SearchResult::UNSAT;
            ascending = true;
        }
    }
}

bool Solver::presolve(Model& model) {
    const auto& constraints = model.constraints();

    // Phase 1: 全制約の presolve() を固定点まで繰り返す
    // 制約は変数のドメインを直接変更するため、変化の検出は
    // SoA (model.var_size) ではなく変数のドメインを直接参照する。
    {
        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto& constraint : constraints) {
                size_t total_size_before = 0;
                int64_t total_range_before = 0;
                for (const auto& var : constraint->variables()) {
                    total_size_before += var->domain().size();
                    total_range_before += var->max() - var->min();
                }

                if (!constraint->presolve(model)) {
                    return false;
                }

                size_t total_size_after = 0;
                int64_t total_range_after = 0;
                for (const auto& var : constraint->variables()) {
                    total_size_after += var->domain().size();
                    total_range_after += var->max() - var->min();
                }

                if (total_size_after < total_size_before || total_range_after < total_range_before) {
                    changed = true;
                }
            }
        }
        // Phase 1 後: 内部構造を再構築（ドメイン変更に対する整合性保証）
        if (!model.prepare_propagation()) {
            return false;
        }
    }

    return true;
}


bool Solver::propagate_instantiate(Model& model, size_t var_idx,
                                    Domain::value_type prev_min, Domain::value_type prev_max) {
    const auto& constraints = model.constraints();
    auto val = model.value(var_idx);

    const auto& constraint_indices = model.constraints_for_var(var_idx);
    for (const auto& w : constraint_indices) {
        if (!constraints[w.constraint_idx]->on_instantiate(model, current_decision_,
						    var_idx, w.internal_var_idx, val, prev_min, prev_max)) {
            return false;
        }
    }

    // NoGood チェック
    if (nogood_learning_) {
        auto it1 = ng_watches_.find(var_idx);
        if (it1 != ng_watches_.end()) {
            auto it2 = it1->second.find(val);
            if (it2 != it1->second.end()) {
                for (auto* ng : std::vector<NoGood*>(it2->second)) {
                    stats_.nogood_check_count++;
                    if (!propagate_nogood(model, ng, {var_idx, val})) {
                        ng->last_active = ++ng_use_counter_;
                        stats_.nogood_prune_count++;
                        return false;
                    }
                }
            }
        }
    }

    return true;
}

void Solver::backtrack(Model& model, int save_point) {
    model.rewind_to(save_point);
    model.rewind_dirty_constraints(save_point);
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
        auto satisfied = constraint->is_satisfied();
        if (satisfied.has_value() && !satisfied.value()) {
            std::cerr << "constraint verify error: " << constraint->name() << "\n";
            abort();
            return false;
        }
    }
    return true;
}

size_t Solver::select_variable(const Model& model) {
    size_t best_idx = SIZE_MAX;
    size_t min_domain_size = SIZE_MAX;
    double best_activity = -1.0;

    auto scan_range = [&](size_t begin, size_t end) {
        for (size_t k = begin; k < end; ++k) {
            size_t i = var_order_[k];
            if (model.is_instantiated(i)) continue;

            size_t domain_size = static_cast<size_t>(model.var_max(i) - model.var_min(i) + 1);
            bool better = false;
            if (activity_first_) {
                if (activity_[i] > best_activity) {
                    better = true;
                } else if (activity_[i] == best_activity && domain_size < min_domain_size) {
                    better = true;
                }
            } else {
                if (domain_size < min_domain_size) {
                    better = true;
                } else if (domain_size == min_domain_size && activity_[i] > best_activity) {
                    better = true;
                }
            }

            if (better) {
                best_idx = i;
                min_domain_size = domain_size;
                best_activity = activity_[i];
            }
        }
    };

#if 1
    // Decision vars を先にスキャン
    scan_range(0, decision_var_end_);
    if (best_idx != SIZE_MAX) return best_idx;
    // 全 decision vars が instantiated → defined vars にフォールバック
    scan_range(decision_var_end_, var_order_.size());
#else
    scan_range(0, var_order_.size());
#endif
    return best_idx;
}

void Solver::decay_activities() {
    for (auto& a : activity_) {
        a *= activity_decay_;
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
    std::vector<NamedNoGood> result;
    size_t count = 0;

    for (const auto& ng : nogoods_) {
        if (max_count > 0 && count >= max_count) {
            break;
        }

        NamedNoGood named_ng;
        bool valid = true;

        for (const auto& lit : ng->literals) {
            if (lit.var_idx >= model.variables().size()) {
                valid = false;
                break;
            }
            auto var = model.variable(lit.var_idx);
            if (!var) {
                valid = false;
                break;
            }
            named_ng.literals.push_back({var->name(), lit.value});
        }

        if (valid && !named_ng.literals.empty()) {
            result.push_back(std::move(named_ng));
            ++count;
        }
    }

    return result;
}

size_t Solver::add_nogoods(const std::vector<NamedNoGood>& nogoods, const Model& model) {
    // 変数名 → インデックスのマップを構築
    std::unordered_map<std::string, size_t> name_to_idx;
    for (size_t i = 0; i < model.variables().size(); ++i) {
        auto var = model.variable(i);
        if (var) {
            name_to_idx[var->name()] = i;
        }
    }

    size_t added = 0;
    for (const auto& named_ng : nogoods) {
        std::vector<Literal> literals;
        bool valid = true;

        for (const auto& named_lit : named_ng.literals) {
            auto it = name_to_idx.find(named_lit.var_name);
            if (it == name_to_idx.end()) {
                valid = false;
                break;
            }
            literals.push_back({it->second, named_lit.value});
        }

        if (valid && !literals.empty()) {
            add_nogood(literals);
            ++added;
        }
    }

    return added;
}

void Solver::set_hint_solution(const Solution& hint, const Model& model) {
    current_best_assignment_.clear();

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

void Solver::add_nogood(const std::vector<Literal>& literals) {
    auto ng = std::make_unique<NoGood>(literals);
    auto* ng_ptr = ng.get();

    // Watch 登録
    const auto& lit1 = ng_ptr->literals[ng_ptr->w1];
    ng_watches_[lit1.var_idx][lit1.value].push_back(ng_ptr);

    if (ng_ptr->w1 != ng_ptr->w2) {
        const auto& lit2 = ng_ptr->literals[ng_ptr->w2];
        ng_watches_[lit2.var_idx][lit2.value].push_back(ng_ptr);
    }

    nogoods_.push_back(std::move(ng));
    // 容量管理はリスタート時に行う
}

void Solver::remove_nogood(NoGood* ng) {
    // Watch から削除
    const auto& lit1 = ng->literals[ng->w1];
    auto& watches1 = ng_watches_[lit1.var_idx][lit1.value];
    watches1.erase(std::remove(watches1.begin(), watches1.end(), ng), watches1.end());

    if (ng->w1 != ng->w2) {
        const auto& lit2 = ng->literals[ng->w2];
        auto& watches2 = ng_watches_[lit2.var_idx][lit2.value];
        watches2.erase(std::remove(watches2.begin(), watches2.end(), ng), watches2.end());
    }
}

bool Solver::propagate_nogood(Model& model, NoGood* ng, const Literal& triggered) {
    const auto& variables = model.variables();
    auto& lits = ng->literals;

    // triggered が w1 か w2 か判定
    size_t triggered_idx, other_idx;
    if (lits[ng->w1].var_idx == triggered.var_idx &&
        lits[ng->w1].value == triggered.value) {
        triggered_idx = ng->w1;
        other_idx = ng->w2;
    } else {
        triggered_idx = ng->w2;
        other_idx = ng->w1;
    }

    // 未成立リテラルを探す（watched 以外で）
    for (size_t i = 0; i < lits.size(); ++i) {
        if (i == ng->w1 || i == ng->w2) {
            continue;
        }
        const auto& lit = lits[i];

        if (!model.is_instantiated(lit.var_idx) ||
            model.value(lit.var_idx) != lit.value) {
            // 未成立 → watch をここに移す
            // 古い watch を削除
            auto& old_watches = ng_watches_[triggered.var_idx][triggered.value];
            old_watches.erase(std::remove(old_watches.begin(), old_watches.end(), ng),
                              old_watches.end());

            // 新しい watch を追加
            if (triggered_idx == ng->w1) {
                ng->w1 = i;
            } else {
                ng->w2 = i;
            }
            ng_watches_[lit.var_idx][lit.value].push_back(ng);

            return true;
        }
    }

    // 移せない → other の watched リテラルが唯一の未成立か確認
    const auto& other_lit = lits[other_idx];

    if (model.is_instantiated(other_lit.var_idx) &&
        model.value(other_lit.var_idx) == other_lit.value) {
        // 全リテラル成立 → 矛盾
        return false;
    }

    // other_var のドメインから other_val を除去
    stats_.nogood_domain_count++;
    model.enqueue_remove_value(other_lit.var_idx, other_lit.value);

    return true;
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

std::unordered_map<size_t, Domain::value_type> Solver::select_best_assignment() {
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
            if (!propagate_instantiate(model, var_idx, prev_min, prev_max)) {
                return false;
            }
            break;
        }
        case PendingUpdate::Type::SetMin: {
            auto new_min = update.value;
            if (new_min <= prev_min) continue;  // 変化なし
            if (!model.set_min(current_decision_, var_idx, new_min)) {
                return false;
            }
            // 確定した場合は on_instantiate、そうでなければ on_set_min
            if (!was_instantiated && model.is_instantiated(var_idx)) {
                if (!propagate_instantiate(model, var_idx, prev_min, prev_max)) {
                    return false;
                }
            } else if (!was_instantiated) {
                // on_set_min を関連する全制約に呼び出す
                const auto& constraint_indices = model.constraints_for_var(var_idx);
                for (const auto& w : constraint_indices) {
                    if (!constraints[w.constraint_idx]->on_set_min(model, current_decision_,
                                                         var_idx, w.internal_var_idx, new_min, prev_min)) {
                        return false;
                    }
                }
            }
            break;
        }
        case PendingUpdate::Type::SetMax: {
            auto new_max = update.value;
            if (new_max >= prev_max) continue;  // 変化なし
            if (!model.set_max(current_decision_, var_idx, new_max)) {
                return false;
            }
            // 確定した場合は on_instantiate、そうでなければ on_set_max
            if (!was_instantiated && model.is_instantiated(var_idx)) {
                if (!propagate_instantiate(model, var_idx, prev_min, prev_max)) {
                    return false;
                }
            } else if (!was_instantiated) {
                // on_set_max を関連する全制約に呼び出す
                const auto& constraint_indices = model.constraints_for_var(var_idx);
                for (const auto& w : constraint_indices) {
                    if (!constraints[w.constraint_idx]->on_set_max(model, current_decision_,
                                                         var_idx, w.internal_var_idx, new_max, prev_max)) {
                        return false;
                    }
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
                            return false;
                        }
                    }
                }
                // 上限が変化した場合 → on_set_max
                if (new_max < prev_max) {
                    for (const auto& w : constraint_indices) {
                        if (!constraints[w.constraint_idx]->on_set_max(model, current_decision_,
                                                             var_idx, w.internal_var_idx, new_max, prev_max)) {
                            return false;
                        }
                    }
                }
                // removed_value が新しい範囲内 → on_remove_value も呼ぶ
                if (removed_value > new_min && removed_value < new_max) {
                    for (const auto& w : constraint_indices) {
                        if (!constraints[w.constraint_idx]->on_remove_value(model, current_decision_,
                                                                  var_idx, w.internal_var_idx, removed_value)) {
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
