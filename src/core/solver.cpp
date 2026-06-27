#include "sabori_csp/solver.hpp"
#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/one_hot_channel_aggregator.hpp"
#include <algorithm>
#include <limits>
#include <numeric>
#include <iomanip>
#include <iostream>
#include <cstdlib>

namespace sabori_csp {

Solver::Solver()
    : rng_(12345678) {
    // 計測用: SABORI_SEED が設定されていれば RNG シードを差し替える（多シード検証用）。
    // 未設定なら従来どおり固定シード（デフォルト動作は不変）。
    if (const char* env = std::getenv("SABORI_SEED")) {
        rng_.seed(static_cast<std::mt19937::result_type>(std::strtoul(env, nullptr, 10)));
    }
    // 計測用: SABORI_FIX_MIXP が設定されていれば mix_p を固定し適応を無効化する。
    // 未設定なら従来どおりバンディット適応（デフォルト動作は不変）。
    if (const char* env = std::getenv("SABORI_FIX_MIXP")) {
        mode_policy_.pin(static_cast<size_t>(std::atoi(env)));
    }
    // 計測用: SABORI_BUMP_MODE で制約側 activity 配分を切り替える（既定2=現行動作）。
    //   0 = 制約からの加点なし / 1 = 基底の poor man's explanation / 2 = 構造特化
    if (const char* env = std::getenv("SABORI_BUMP_MODE")) {
        bump_mode_ = std::atoi(env);
    }
    // 計測用: 指定 name を含む制約だけ構造特化、他は基底に強制（制約別寄与の切り分け）。
    if (const char* env = std::getenv("SABORI_BUMP_STRUCT_ONLY")) {
        bump_struct_only_ = env;
    }
    // 計測用: SABORI_BLOOM=0 で NoGood-Bloom 重なりタイブレークを無効化（既定有効）。
    if (const char* env = std::getenv("SABORI_BLOOM")) {
        bloom_tiebreak_ = (std::atoi(env) != 0);
    }
    // 計測用: SABORI_GRADIENT=0 で擬似勾配ヒント（値順序バイアス）を無効化（既定有効）。
    if (const char* env = std::getenv("SABORI_GRADIENT")) {
        gradient_enabled_ = (std::atoi(env) != 0);
    }
    // 計測用: SABORI_NOGOOD=0 で decision-trail NoGood 学習＋伝播をまるごと無効化（既定有効）。
    if (const char* env = std::getenv("SABORI_NOGOOD")) {
        nogood_learning_ = (std::atoi(env) != 0);
    }
    // 計測用: SABORI_ONEHOT=0 で one-hot チャネル集約 presolve を無効化（既定有効）。
    if (const char* env = std::getenv("SABORI_ONEHOT")) {
        onehot_enabled_ = (std::atoi(env) != 0);
    }
    // 計測用: SABORI_NG_NOBUMP=1 で NoGood 由来の activity bump だけ止める（学習・枝刈りは維持）。
    if (const char* env = std::getenv("SABORI_NG_NOBUMP")) {
        nogood_mgr_.set_activity_bump(std::atoi(env) == 0);
    }
    // 計測用: NoGood bump をさらに学習時/伝播時に分けて切る。
    //   SABORI_NG_LEARN_BUMP=0 → 学習時 bump（0.01 スケール）のみ無効
    //   SABORI_NG_PROP_BUMP=0  → 伝播時 bump（フルスケール/n）のみ無効
    if (const char* env = std::getenv("SABORI_NG_LEARN_BUMP")) {
        nogood_mgr_.set_learn_bump(std::atoi(env) != 0);
    }
    if (const char* env = std::getenv("SABORI_NG_PROP_BUMP")) {
        nogood_mgr_.set_prop_bump(std::atoi(env) != 0);
    }
    // 計測用: SABORI_DECVAR_BUMP=0 で決定変数の activity bump（handle_failure, フルスケール）を無効化。
    if (const char* env = std::getenv("SABORI_DECVAR_BUMP")) {
        decvar_bump_enabled_ = (std::atoi(env) != 0);
    }
    // 計測用: SABORI_TEMPORAL=0 で temporal_activity（Last Conflict 系・変数選択の第1基準）を無効化。
    if (const char* env = std::getenv("SABORI_TEMPORAL")) {
        temporal_enabled_ = (std::atoi(env) != 0);
    }
}

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
    gradient_strategy_.rebuild_eligible(model);

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
        // 計測用 ablation: SABORI_ONEHOT=0 で集約をスキップ（既定有効）。
        if (onehot_enabled_) {
            OneHotChannelAggregator aggregator;
            if (!aggregator.aggregate(model, verbose_)) {
                return false;
            }
            // 制約配列が変化したため、変数 → 制約インデックスを再構築
            model.build_constraint_watch_list();
        }

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

void Solver::backtrack(Model& model, int save_point) {
    model.rewind_to(save_point);
    model.rewind_dirty_constraints(save_point);
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

} // namespace sabori_csp
