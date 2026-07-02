#include "sabori_csp/parallel_solver.hpp"
#include <limits>

namespace sabori_csp {

ParallelSolver::ParallelSolver(size_t num_threads, std::vector<WorkerConfig> configs)
    : num_threads_(num_threads == 0 ? 1 : num_threads)
    , configs_(std::move(configs)) {
    // 構成が不足していればデフォルト（シードだけずらす）で補う。
    while (configs_.size() < num_threads_) {
        WorkerConfig c;
        c.seed = static_cast<uint32_t>(12345678u + configs_.size() * 2654435761u);
        configs_.push_back(c);
    }
}

void ParallelSolver::build_workers(const Model& master) {
    // 再入（同一インスタンスでの複数回 solve）に備え、前回の state を全消去する。
    // workers_ready_ を false に落としてから配列を作り直すことで、構築中に stop() が
    // 走っても古い solvers_ を走査しないようにする。
    workers_ready_.store(false);
    models_.clear();
    solvers_.clear();
    stop_flag_.store(false);
    have_incumbent_.store(false);
    unsat_proven_.store(false);
    optimal_proven_.store(false);
    {
        std::lock_guard<std::mutex> lk(result_mtx_);
        best_solution_.reset();
        best_objective_.reset();
        winner_ = SIZE_MAX;
    }

    // reserve でキャパシティを固定（push_back 後の再確保を防ぎ、stop() の走査を安全にする）。
    models_.reserve(num_threads_);
    solvers_.reserve(num_threads_);
    // verbose 対象ワーカー（範囲外なら最後のワーカーにクランプ）。
    size_t vw = (verbose_worker_ < num_threads_) ? verbose_worker_ : num_threads_ - 1;
    for (size_t i = 0; i < num_threads_; ++i) {
        models_.push_back(master.clone());
        auto s = std::make_unique<Solver>();
        s->apply_worker_config(configs_[i]);
        s->set_verbose(verbose_ && i == vw);  // verbose は 1 ワーカーに限定
        solvers_.push_back(std::move(s));
    }
    workers_ready_.store(true);
    // 構築中に stop が来ていた場合の取りこぼしを防ぐ。
    if (stop_flag_.load()) {
        for (auto& s : solvers_) {
            if (s) s->stop();
        }
    }
}

void ParallelSolver::stop() {
    stop_flag_.store(true);
    if (workers_ready_.load()) {
        for (auto& s : solvers_) {
            if (s) s->stop();
        }
    }
}

void ParallelSolver::worker_sat(size_t i, const SolutionFoundCallback& on_solution) {
    auto sol = solvers_[i]->solve_prepared(*models_[i]);
    if (sol) {
        // 最初に解いたワーカーが勝つ。
        if (!have_incumbent_.exchange(true)) {
            {
                std::lock_guard<std::mutex> lk(result_mtx_);
                best_solution_ = sol;
                winner_ = i;
            }
            if (on_solution) on_solution(*sol);
            stop();  // 他ワーカーを止める
        }
    } else if (!solvers_[i]->is_stopped()) {
        // 解なしで自然終了（探索空間を尽くした）= 大域 UNSAT。
        unsat_proven_.store(true);
        stop();
    }
}

void ParallelSolver::publish_incumbent(size_t i, const Solution& sol,
                                       Domain::value_type obj, bool minimize,
                                       const ImproveCallback& on_improve) {
    std::lock_guard<std::mutex> lk(result_mtx_);
    bool better = !have_incumbent_.load() ||
                  (minimize ? obj < *best_objective_ : obj > *best_objective_);
    if (!better) return;
    best_objective_ = obj;
    best_solution_ = sol;
    winner_ = i;
    best_obj_.store(obj);          // hook が読む大域 incumbent（atomic）
    have_incumbent_.store(true);   // hook が読む（atomic）
    if (on_improve) on_improve(sol, obj);
}

void ParallelSolver::worker_optimize(size_t i, size_t obj_idx, bool minimize,
                                     const std::string& obj_name,
                                     const ImproveCallback& on_improve) {
    // hook: 大域 incumbent を返す（無ければ nullopt）。読み取りは atomic のみ。
    solvers_[i]->set_external_bound_hook(
        [this]() -> std::optional<Domain::value_type> {
            if (!have_incumbent_.load()) return std::nullopt;
            return best_obj_.load();
        });

    // 改善 incumbent を publish するコールバック。
    auto wrapped = [this, i, obj_name, minimize, &on_improve](const Solution& sol) -> bool {
        auto it = sol.find(obj_name);
        if (it != sol.end()) {
            publish_incumbent(i, sol, it->second, minimize, on_improve);
        }
        return true;  // 探索継続
    };

    solvers_[i]->solve_optimize_prepared(*models_[i], obj_idx, minimize, wrapped);

    if (!solvers_[i]->is_stopped()) {
        // 自然終了 = この完全探索が大域 incumbent の最適性を証明した。
        optimal_proven_.store(true);
        stop();
    }
}

ParallelSolver::Result ParallelSolver::solve(Model& master, SolutionFoundCallback on_solution) {
    Result r;

    // presolve を 1 度だけ実行（使い捨ての prep ソルバ）。
    Solver prep;
    if (!prep.prepare(master)) {
        r.status = SearchResult::UNSAT;  // presolve で矛盾 = UNSAT
        return r;
    }

    build_workers(master);

    std::vector<std::thread> threads;
    threads.reserve(num_threads_);
    for (size_t i = 0; i < num_threads_; ++i) {
        threads.emplace_back([this, i, &on_solution] { worker_sat(i, on_solution); });
    }
    for (auto& t : threads) t.join();

    {
        std::lock_guard<std::mutex> lk(result_mtx_);
        r.solution = best_solution_;
        r.winning_thread = winner_;
    }
    if (have_incumbent_.load()) {
        r.status = SearchResult::SAT;
    } else if (unsat_proven_.load()) {
        r.status = SearchResult::UNSAT;
    } else {
        r.status = SearchResult::UNKNOWN;  // タイムアウト等
    }
    if (r.winning_thread != SIZE_MAX) {
        r.winner_stats = solvers_[r.winning_thread]->stats();
    }
    return r;
}

ParallelSolver::Result ParallelSolver::solve_optimize(
        Model& master, size_t obj_var_idx, bool minimize, ImproveCallback on_improve) {
    Result r;

    Solver prep;
    if (!prep.prepare(master)) {
        r.status = SearchResult::UNSAT;
        return r;
    }

    // 目的変数名（clone でも同一）。Solution からの目的値取得に使う。
    std::string obj_name;
    if (Variable* v = master.variable(obj_var_idx)) {
        obj_name = v->name();
    }

    // best_obj_ をセンチネルで初期化（hook は have_incumbent_ で gate するので値は未使用だが念のため）。
    best_obj_.store(minimize ? std::numeric_limits<int64_t>::max()
                             : std::numeric_limits<int64_t>::min());

    build_workers(master);

    std::vector<std::thread> threads;
    threads.reserve(num_threads_);
    for (size_t i = 0; i < num_threads_; ++i) {
        threads.emplace_back([this, i, obj_var_idx, minimize, &obj_name, &on_improve] {
            worker_optimize(i, obj_var_idx, minimize, obj_name, on_improve);
        });
    }
    for (auto& t : threads) t.join();

    {
        std::lock_guard<std::mutex> lk(result_mtx_);
        r.solution = best_solution_;
        r.objective = best_objective_;
        r.winning_thread = winner_;
    }
    if (have_incumbent_.load()) {
        r.status = SearchResult::SAT;  // 実行可能解あり
        r.proved_optimal = optimal_proven_.load();
    } else {
        // incumbent 無し: 自然終了なら UNSAT（実行不能）、停止なら UNKNOWN。
        r.status = optimal_proven_.load() ? SearchResult::UNSAT : SearchResult::UNKNOWN;
    }
    if (r.winning_thread != SIZE_MAX) {
        r.winner_stats = solvers_[r.winning_thread]->stats();
    }
    return r;
}

std::vector<WorkerConfig> make_portfolio_configs(
    size_t n, bool is_optimize, const WorkerConfig& base) {
    std::vector<WorkerConfig> cfgs;
    cfgs.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        WorkerConfig c = base;  // bisection/probe/nogood/conflict などは base から継承
        if (i == 0) {
            cfgs.push_back(c);  // worker0 = base（既定）
            continue;
        }
        // ワーカー1以降のシードは base.seed 起点で導出する（base.seed 既定 12345678
        // なら従来と完全一致。set_seed でポートフォリオ全体が再現的にずれる）。
        c.seed = static_cast<uint32_t>(base.seed + i * 2654435761u);
        // 多様化軸を VBS 限界インパクトの大きい順に採用する（worker1=k0 が最大）。
        // スレッドを 2,3,4... と増やすほど影響の小さい軸が足される。
        // 影響順は問題タイプで異なる（bench_portfolio_diversity / bench_restart_sat, 2026-06-30）:
        //   最適化: conflict > mrv > no_nogood > gradient/probe/temporal > restart
        //   SAT:    restart緩和sc8 > conf_sc8(併用) > mrv > no_nogood > gradient/probe/temporal
        size_t k = (i - 1) % 7;  // 0-based 多様化インデックス（n>8 はシード変えて循環）
        if (is_optimize) {
            switch (k) {
                case 0: c.conflict_learning = !base.conflict_learning; break;
                case 1: c.fixed_mixp = 0; break;
                case 2: c.nogood_learning = false; break;
                case 3: c.gradient_enabled = false; break;
                case 4: c.probe_enabled = false; break;
                case 5: c.temporal_enabled = false; break;
                case 6: c.restart_enabled = false; break;  // no-op/シード変種（最小）
            }
        } else {  // SAT
            switch (k) {
                case 0: c.restart_scale = 8.0; break;
                case 1: c.conflict_learning = !base.conflict_learning;
                        c.restart_scale = 8.0; break;       // conf_sc8（併用）
                case 2: c.fixed_mixp = 0; break;
                case 3: c.nogood_learning = false; break;
                case 4: c.gradient_enabled = false; break;
                case 5: c.probe_enabled = false; break;
                case 6: c.temporal_enabled = false; break;
            }
        }
        cfgs.push_back(c);
    }
    return cfgs;
}

} // namespace sabori_csp
