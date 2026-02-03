#include "sabori_csp/solver.hpp"
#include <algorithm>
#include <limits>

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
    decision_trail_.clear();
    nogoods_.clear();
    ng_watches_.clear();
    best_assignments_.clear();
    current_best_assignment_.clear();
    current_decision_ = 0;
    stats_ = SolverStats{};

    // 初期矛盾チェック: 1つでも矛盾している制約があれば探索を打ち切る
    for (const auto& constraint : model.constraints()) {
        if (constraint->is_initially_inconsistent()) {
            return std::nullopt;  // UNSAT
        }
    }

    // 初期伝播: 残り1変数の制約を処理
    if (!initial_propagate(model)) {
        return std::nullopt;  // UNSAT
    }

    // リスタート有効時は専用ループを使用
    if (restart_enabled_) {
        return search_with_restart(model);
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

size_t Solver::solve_all(Model& model, SolutionCallback callback) {
    // 制約ウォッチリストを構築
    model.build_constraint_watch_list();

    // 全解探索ではリスタートを無効化
    bool old_restart = restart_enabled_;
    restart_enabled_ = false;

    // 初期化
    const auto& variables = model.variables();
    activity_.assign(variables.size(), 0.0);
    decision_trail_.clear();
    nogoods_.clear();
    ng_watches_.clear();
    current_decision_ = 0;
    stats_ = SolverStats{};

    // 初期矛盾チェック: 1つでも矛盾している制約があれば探索を打ち切る
    for (const auto& constraint : model.constraints()) {
        if (constraint->is_initially_inconsistent()) {
            restart_enabled_ = old_restart;
            return 0;  // UNSAT
        }
    }

    // 初期伝播: 残り1変数の制約を処理
    if (!initial_propagate(model)) {
        restart_enabled_ = old_restart;
        return 0;  // UNSAT
    }

    size_t count = 0;
    int conflict_limit = std::numeric_limits<int>::max();

    run_search(model, conflict_limit, 0,
               [&count, &callback](const Solution& sol) {
                   count++;
                   return callback(sol);  // trueなら継続
               }, true);

    restart_enabled_ = old_restart;
    return count;
}

std::optional<Solution> Solver::search_with_restart(Model& model) {
    double inner_limit = initial_conflict_limit_;
    double outer_limit = 10.0;

    int root_point = current_decision_;

    while (true) {
        for (int outer = 0; outer < static_cast<int>(outer_limit); ++outer) {
            int conflict_limit = static_cast<int>(inner_limit);
            std::optional<Solution> result;

            auto res = run_search(model, conflict_limit, 0,
                                  [&result](const Solution& sol) {
                                      result = sol;
                                      return false;
                                  }, false);

            if (res == SearchResult::SAT) {
                stats_.nogoods_size = nogoods_.size();
                return result;
            }
            if (res == SearchResult::UNSAT) {
                stats_.nogoods_size = nogoods_.size();
                return std::nullopt;
            }

            // UNKNOWN: リスタート
            backtrack(model, root_point);
            stats_.restart_count++;
            current_best_assignment_ = select_best_assignment();

            // Activity 減衰
            decay_activities();

            // コンフリクト制限を増加
            inner_limit *= conflict_limit_multiplier_;

            if (inner_limit > outer_limit) {
                outer_limit *= 1.001;
                inner_limit = initial_conflict_limit_;
            }
        }
    }

    return std::nullopt;
}

SearchResult Solver::run_search(Model& model, int conflict_limit, size_t depth,
                                 SolutionCallback callback, bool find_all) {
    const auto& variables = model.variables();

    // 統計更新
    stats_.depth_sum += depth;
    stats_.depth_count++;
    if (depth > stats_.max_depth) {
        stats_.max_depth = depth;
    }

    // 全変数が確定しているかチェック
    bool all_assigned = true;
    for (const auto& var : variables) {
        if (!var->is_assigned()) {
            all_assigned = false;
            break;
        }
    }

    if (all_assigned) {
        if (verify_solution(model)) {
            if (!callback(build_solution(model))) {
                return SearchResult::SAT;
            }
            return find_all ? SearchResult::UNSAT : SearchResult::SAT;
        }
        return SearchResult::UNSAT;
    }

    // 変数選択
    size_t var_idx = select_variable(model);
    auto& var = variables[var_idx];

    int save_point = current_decision_;
    auto prev_min = var->domain().min().value();
    auto prev_max = var->domain().max().value();

    // 値を試行順序で取得
    auto values = var->domain().values();

    // 保存された値がある場合は優先
    if (current_best_assignment_.count(var_idx)) {
        auto best_val = current_best_assignment_[var_idx];
        auto it = std::find(values.begin(), values.end(), best_val);
        if (it != values.end()) {
            values.erase(it);
            values.insert(values.begin(), best_val);
        }
    }

    size_t nogoods_before = nogoods_.size();

    for (auto val : values) {
        current_decision_++;

        // 値を割り当て（Trail に保存される）
        if (!model.instantiate(current_decision_, var_idx, val)) {
            current_decision_--;
            continue;
        }

        // 伝播
        if (propagate(model, var_idx, prev_min, prev_max)) {
            // キュー処理
            if (process_queue(model)) {
                decision_trail_.push_back({var_idx, val});

                auto res = run_search(model, conflict_limit, depth + 1, callback, find_all);

                decision_trail_.pop_back();

                if (res == SearchResult::SAT) {
                    return res;
                }
                if (res == SearchResult::UNKNOWN || conflict_limit <= 1) {
                    current_decision_--;
                    backtrack(model, save_point);
                    return SearchResult::UNKNOWN;
                }

                conflict_limit--;
            }
        }

        current_decision_--;
        backtrack(model, save_point);
    }

    // 失敗: Activity 更新
    activity_[var_idx] += 1.0;
    stats_.fail_count++;

    // 部分解を保存
    save_partial_assignment(model);

    // 子ノードで追加された NoGood を削除
    while (nogoods_.size() > nogoods_before) {
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

    return SearchResult::UNSAT;
}

bool Solver::initial_propagate(Model& model) {
    const auto& constraints = model.constraints();

    // 固定点に達するまで繰り返す
    bool changed = true;
    while (changed) {
        changed = false;

        for (const auto& constraint : constraints) {
            // 残り1変数の制約を検出
            size_t uninstantiated_count = constraint->count_uninstantiated();

            if (uninstantiated_count == 1) {
                size_t last_idx = constraint->find_last_uninstantiated();
                if (last_idx != SIZE_MAX) {
                    if (!constraint->on_last_uninstantiated(model, current_decision_, last_idx)) {
                        return false;  // 矛盾
                    }
                }
            } else if (uninstantiated_count == 0) {
                // 全変数確定済み → 最終チェック
                if (!constraint->on_final_instantiate()) {
                    return false;  // 矛盾
                }
            }
        }

        // キューを処理
        if (!process_queue(model)) {
            return false;  // 矛盾
        }

        // pending があれば変更あり
        if (!model.pending_instantiations().empty()) {
            changed = true;
        }
    }

    return true;
}

bool Solver::propagate_all(Model& model) {
    const auto& constraints = model.constraints();

    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& constraint : constraints) {
            size_t total_size_before = 0;
            for (const auto& var : constraint->variables()) {
                total_size_before += var->domain().size();
            }

            if (!constraint->propagate()) {
                return false;
            }

            size_t total_size_after = 0;
            for (const auto& var : constraint->variables()) {
                total_size_after += var->domain().size();
            }

            if (total_size_after < total_size_before) {
                changed = true;
            }
        }
    }
    return true;
}

bool Solver::propagate(Model& model, size_t var_idx,
                        Domain::value_type prev_min, Domain::value_type prev_max) {
    const auto& constraints = model.constraints();
    auto val = model.value(var_idx);

    // ウォッチリストを使った高速制約伝播
    const auto& constraint_indices = model.constraints_for_var(var_idx);
    for (size_t c_idx : constraint_indices) {
        if (!constraints[c_idx]->on_instantiate(model, current_decision_,
                                                 var_idx, val, prev_min, prev_max)) {
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
    propagation_queue_.clear();
    model.clear_pending_instantiations();
    model.rewind_to(save_point);

    // 制約の状態も復元（AllDifferent, IntLinEq, Circuit など）
    for (const auto& constraint : model.constraints()) {
        // dynamic_cast で型をチェックして rewind_to を呼び出す
        if (auto* alldiff = dynamic_cast<AllDifferentConstraint*>(constraint.get())) {
            alldiff->rewind_to(save_point);
        } else if (auto* lineq = dynamic_cast<IntLinEqConstraint*>(constraint.get())) {
            lineq->rewind_to(save_point);
        } else if (auto* linle = dynamic_cast<IntLinLeConstraint*>(constraint.get())) {
            linle->rewind_to(save_point);
        } else if (auto* circuit = dynamic_cast<CircuitConstraint*>(constraint.get())) {
            circuit->rewind_to(save_point);
        } else if (auto* int_element = dynamic_cast<IntElementConstraint*>(constraint.get())) {
            int_element->rewind_to(save_point);
        }
    }
}

Solution Solver::build_solution(const Model& model) const {
    Solution sol;
    for (const auto& var : model.variables()) {
        if (var->is_assigned()) {
            sol[var->name()] = var->assigned_value().value();
        }
    }
    return sol;
}

bool Solver::verify_solution(const Model& model) const {
    for (const auto& constraint : model.constraints()) {
        auto satisfied = constraint->is_satisfied();
        if (satisfied.has_value() && !satisfied.value()) {
            return false;
        }
    }
    return true;
}

size_t Solver::select_variable(const Model& model) {
    const auto& variables = model.variables();
    size_t best_idx = 0;
    bool found = false;

    // MRV (Minimum Remaining Values) + Activity tie-breaking
    // 最小ドメインサイズを優先、同サイズなら Activity 最大を選択
    size_t min_domain_size = SIZE_MAX;
    double best_activity = -1.0;

    for (size_t i = 0; i < variables.size(); ++i) {
        if (!variables[i]->is_assigned()) {
            size_t domain_size = variables[i]->domain().size();

            // 1. ドメインサイズが小さいものを優先
            // 2. 同じサイズなら Activity が高いものを優先
            bool better = false;
            if (!found) {
                better = true;
            } else if (domain_size < min_domain_size) {
                better = true;
            } else if (domain_size == min_domain_size && activity_selection_) {
                if (activity_[i] > best_activity) {
                    better = true;
                }
            }

            if (better) {
                min_domain_size = domain_size;
                best_activity = activity_selection_ ? activity_[i] : 0.0;
                best_idx = i;
                found = true;
            }
        }
    }

    return best_idx;
}

void Solver::decay_activities() {
    for (auto& a : activity_) {
        a *= activity_decay_;
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

    // 上限管理
    while (nogoods_.size() > max_nogoods_) {
        // 最も長い NoGood を削除
        auto worst = std::max_element(nogoods_.begin(), nogoods_.end(),
                                       [](const auto& a, const auto& b) {
                                           return a->literals.size() < b->literals.size();
                                       });
        if (worst != nogoods_.end()) {
            remove_nogood(worst->get());
            nogoods_.erase(worst);
        }
    }
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
        auto& var = variables[lit.var_idx];

        if (!var->is_assigned() ||
            var->assigned_value().value() != lit.value) {
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
    auto& other_var = variables[other_lit.var_idx];

    if (other_var->is_assigned() &&
        other_var->assigned_value().value() == other_lit.value) {
        // 全リテラル成立 → 矛盾
        return false;
    }

    // other_var のドメインから other_val を除去
    stats_.nogood_domain_count++;
    if (!model.remove_value(current_decision_, other_lit.var_idx, other_lit.value)) {
        return false;
    }

    // 残り1値になったら伝播キューに入れる
    if (model.is_instantiated(other_lit.var_idx)) {
        stats_.nogood_instantiate_count++;
        enqueue_instantiate(other_lit.var_idx, model.value(other_lit.var_idx));
    }

    return true;
}

void Solver::save_partial_assignment(const Model& model) {
    const auto& variables = model.variables();

    std::unordered_map<size_t, Domain::value_type> assignment;
    for (size_t i = 0; i < variables.size(); ++i) {
        if (variables[i]->is_assigned()) {
            assignment[i] = variables[i]->assigned_value().value();
        }
    }

    size_t num_instantiated = assignment.size();

    // 閾値チェック
    if (!best_assignments_.empty()) {
        size_t max_num = 0;
        for (const auto& pa : best_assignments_) {
            max_num = std::max(max_num, pa.num_instantiated);
        }
        if (num_instantiated < max_num * 0.9) {
            return;
        }
    }

    // 同じ割り当てがあるかチェック
    for (auto& pa : best_assignments_) {
        if (pa.assignments == assignment) {
            if (num_instantiated > pa.num_instantiated) {
                pa.num_instantiated = num_instantiated;
            }
            return;
        }
    }

    best_assignments_.push_back({num_instantiated, assignment});

    // 上限管理
    if (best_assignments_.size() > max_best_assignments_) {
        // Activity 合計が最小のものを削除
        auto worst = std::min_element(best_assignments_.begin(), best_assignments_.end(),
                                       [this](const auto& a, const auto& b) {
                                           double sum_a = 0, sum_b = 0;
                                           for (const auto& [idx, _] : a.assignments) {
                                               if (idx < activity_.size()) {
                                                   sum_a += activity_[idx];
                                               }
                                           }
                                           for (const auto& [idx, _] : b.assignments) {
                                               if (idx < activity_.size()) {
                                                   sum_b += activity_[idx];
                                               }
                                           }
                                           return sum_a < sum_b;
                                       });
        best_assignments_.erase(worst);
    }
}

std::unordered_map<size_t, Domain::value_type> Solver::select_best_assignment() {
    if (best_assignments_.empty()) {
        return {};
    }

    if (best_assignments_.size() < 2) {
        return best_assignments_[0].assignments;
    }

    // Activity 合計で上位2つを選択
    std::vector<std::pair<size_t, double>> scored;
    for (size_t i = 0; i < best_assignments_.size(); ++i) {
        double sum = 0;
        for (const auto& [idx, _] : best_assignments_[i].assignments) {
            if (idx < activity_.size()) {
                sum += activity_[idx];
            }
        }
        scored.emplace_back(i, sum);
    }
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    size_t idx1 = scored[0].first;
    size_t idx2 = scored.size() > 1 ? scored[1].first : idx1;

    const auto& parent1 = best_assignments_[idx1].assignments;
    const auto& parent2 = best_assignments_[idx2].assignments;

    // GA風クロスオーバー: parent1 をベースに parent2 から補完
    std::unordered_map<size_t, Domain::value_type> child = parent1;
    for (const auto& [idx, val] : parent2) {
        if (child.find(idx) == child.end()) {
            child[idx] = val;
        }
    }

    return child;
}

void Solver::enqueue_instantiate(size_t var_idx, Domain::value_type value) {
    // 重複チェック
    for (const auto& [idx, _] : propagation_queue_) {
        if (idx == var_idx) {
            return;
        }
    }
    propagation_queue_.push_back({var_idx, value});
}

bool Solver::process_queue(Model& model) {
    const auto& variables = model.variables();

    while (true) {
        // Model の pending を取り込む
        for (const auto& [idx, val] : model.pending_instantiations()) {
            enqueue_instantiate(idx, val);
        }
        model.clear_pending_instantiations();

        if (propagation_queue_.empty()) {
            break;
        }

        auto [var_idx, val] = propagation_queue_.front();
        propagation_queue_.pop_front();

        auto& var = variables[var_idx];
        if (var->is_assigned()) {
            // 既に確定済みで異なる値が要求されている場合は矛盾
            if (var->assigned_value().value() != val) {
                return false;
            }
            continue;  // 同じ値なら OK
        }

        auto prev_min = var->domain().min().value();
        auto prev_max = var->domain().max().value();

        if (!model.instantiate(current_decision_, var_idx, val)) {
            return false;
        }

        if (!propagate(model, var_idx, prev_min, prev_max)) {
            return false;
        }
    }

    return true;
}

} // namespace sabori_csp
