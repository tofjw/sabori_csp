#include "sabori_csp/solver.hpp"
#include <algorithm>
#include <limits>
#include <iostream>

namespace sabori_csp {

namespace {
// MurmurHash3 64-bit finalizer
inline uint64_t fmix64(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}
}  // namespace

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

    // presolve: 初期伝播 + 内部構造の構築
    if (!presolve(model)) {
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
    size_t prev_fail_count = 0;

    if (verbose_) {
        std::cerr << "% [verbose] search_with_restart start\n";
    }

    while (!stopped_) {
        for (int outer = 0; outer < static_cast<int>(outer_limit) && !stopped_; ++outer) {
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
            model.clear_pending_updates();
            backtrack(model, root_point);
            stats_.restart_count++;
            current_best_assignment_ = select_best_assignment();

            // Activity 減衰
            decay_activities();

            // fail が発生しなかった場合は inner, outer を +1
            if (stats_.fail_count == prev_fail_count) {
                inner_limit += 1.0;
                outer_limit += 1.0;
            } else {
                // コンフリクト制限を増加
                inner_limit *= conflict_limit_multiplier_;

                if (inner_limit > outer_limit) {
                    outer_limit *= 1.001;
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

SearchResult Solver::run_search(Model& model, int conflict_limit, size_t depth,
                                 SolutionCallback callback, bool find_all) {
    // タイムアウトチェック
    if (stopped_) {
        return SearchResult::UNKNOWN;
    }

    const auto& variables = model.variables();

    // 統計更新
    stats_.depth_sum += depth;
    stats_.depth_count++;
    if (depth > stats_.max_depth) {
        stats_.max_depth = depth;
    }

    // 全変数が確定しているかチェック
    bool all_assigned = true;
    for (size_t i = 0; i < variables.size(); ++i) {
        if (!model.is_instantiated(i)) {
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

    int save_point = current_decision_;
    auto prev_min = model.var_min(var_idx);
    auto prev_max = model.var_max(var_idx);

    // 値を試行順序で取得
    auto values = variables[var_idx]->domain().values();

    // ハッシュ順でソート（値の試行順序にばらつきを持たせる）
    uint64_t hash_seed = static_cast<uint64_t>(var_idx) ^ static_cast<uint64_t>(conflict_limit);
    std::sort(values.begin(), values.end(), [hash_seed](auto a, auto b) {
        uint64_t ha = fmix64(static_cast<uint64_t>(a) ^ hash_seed);
        uint64_t hb = fmix64(static_cast<uint64_t>(b) ^ hash_seed);
        return ha < hb;
    });

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
        bool propagate_ok = propagate_instantiate(model, var_idx, prev_min, prev_max);
        bool queue_ok = false;
        if (propagate_ok) {
            // キュー処理
            queue_ok = process_queue(model);
            if (queue_ok) {
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

        // 伝播失敗時はキューに残りがある可能性があるのでクリア
        if (!propagate_ok || !queue_ok) {
            model.clear_pending_updates();
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
                for (const auto& var : constraint->variables()) {
                    total_size_before += var->domain().size();
                }

                if (!constraint->presolve(model)) {
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
        // Phase 1 後: 内部構造を再構築（ドメイン変更に対する整合性保証）
        if (!model.prepare_propagation()) {
            return false;
        }
    }

    // Phase 2: 固定点に達するまで繰り返す
    bool changed = true;
    while (changed) {
        changed = false;

        // 各ループの先頭で内部状態を同期
        // propagate() や on_last_uninstantiated() が変数を直接確定させた場合に対応
        for (const auto& constraint : constraints) {
            constraint->sync_after_propagation();
        }

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
        if (model.has_pending_updates()) {
            changed = true;
        }
    }

    // 最終的な内部状態を同期（探索フェーズに向けて）
    for (const auto& constraint : constraints) {
        constraint->sync_after_propagation();
    }

    return true;
}


bool Solver::propagate_instantiate(Model& model, size_t var_idx,
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

    // 未確定変数のインデックスを収集
    std::vector<size_t> candidates;
    candidates.reserve(variables.size());
    for (size_t i = 0; i < variables.size(); ++i) {
        if (!variables[i]->is_assigned()) {
            candidates.push_back(i);
        }
    }

    if (candidates.empty()) {
        return 0;
    }

    // シャッフルしてタイブレークをランダム化
    std::shuffle(candidates.begin(), candidates.end(), rng_);

    size_t best_idx = candidates[0];
    size_t min_domain_size = model.var_size(best_idx);
    double best_activity = activity_[best_idx];

    for (size_t j = 1; j < candidates.size(); ++j) {
        size_t i = candidates[j];
        size_t domain_size = model.var_size(i);
        double act = activity_[i];

        bool better = false;
        if (activity_first_) {
            // Activity 優先: Activity が高いものを優先、同じなら MRV
            if (act > best_activity) {
                better = true;
            } else if (act == best_activity && domain_size < min_domain_size) {
                better = true;
            }
        } else {
            // MRV 優先（デフォルト）: ドメインサイズが小さいものを優先、同じなら Activity
            if (domain_size < min_domain_size) {
                better = true;
            } else if (domain_size == min_domain_size && act > best_activity) {
                better = true;
            }
        }

        if (better) {
            min_domain_size = domain_size;
            best_activity = act;
            best_idx = i;
        }
    }

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
    if (!model.remove_value(current_decision_, other_lit.var_idx, other_lit.value)) {
        return false;
    }

    // 残り1値になったら伝播キューに入れる
    if (model.is_instantiated(other_lit.var_idx)) {
        stats_.nogood_instantiate_count++;
        model.enqueue_instantiate(other_lit.var_idx, model.value(other_lit.var_idx));
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

bool Solver::process_queue(Model& model) {
    const auto& constraints = model.constraints();

    while (model.has_pending_updates()) {
        auto update = model.pop_pending_update();

        size_t var_idx = update.var_idx;

        // 操作前の状態を保存
        auto prev_min = model.var_min(var_idx);
        auto prev_max = model.var_max(var_idx);
        size_t prev_size = model.var_size(var_idx);
        bool was_instantiated = model.is_instantiated(var_idx);

        switch (update.type) {
        case PendingUpdate::Type::Instantiate: {
            if (was_instantiated) {
                // 既に確定済みで異なる値が要求されている場合は矛盾
                if (model.value(var_idx) != update.value) {
                    return false;
                }
                // 同じ値で既に確定済み: ドメイン削減で確定した
                // NOTE: この場合は propagate_instantiate を呼ばない
                // 理由: on_instantiate は同じ変数に対して2回呼ばれると
                // 内部カウンタが二重にデクリメントされてしまうため
                // ドメイン削減で確定した変数は、sync_after_propagation で対応する
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
                for (size_t c_idx : constraint_indices) {
                    if (!constraints[c_idx]->on_set_min(model, current_decision_,
                                                         var_idx, new_min, prev_min)) {
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
                for (size_t c_idx : constraint_indices) {
                    if (!constraints[c_idx]->on_set_max(model, current_decision_,
                                                         var_idx, new_max, prev_max)) {
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
            // 確定した場合は on_instantiate、そうでなければ on_remove_value
            if (!was_instantiated && model.is_instantiated(var_idx)) {
                if (!propagate_instantiate(model, var_idx, prev_min, prev_max)) {
                    return false;
                }
            } else if (!was_instantiated) {
                // on_remove_value を関連する全制約に呼び出す
                const auto& constraint_indices = model.constraints_for_var(var_idx);
                for (size_t c_idx : constraint_indices) {
                    if (!constraints[c_idx]->on_remove_value(model, current_decision_,
                                                              var_idx, removed_value)) {
                        return false;
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
