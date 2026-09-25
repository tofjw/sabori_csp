#include "sabori_csp/parallel_solver.hpp"
#include <cstdlib>
#include <string>
#include <limits>

namespace sabori_csp {

void apply_probe_env_overrides(WorkerConfig& cfg) {
    // 糖衣は Solver コンストラクタの env 解釈と同一に保つこと（=1 → 既定予算）。
    if (const char* e = std::getenv("SABORI_PROBE_ROOT")) {
        cfg.root_probe_limit = std::atoi(e);
        if (cfg.root_probe_limit == 1) cfg.root_probe_limit = 2000;
    }
    if (const char* e = std::getenv("SABORI_PROMOTE_IMPACT")) {
        cfg.promote_impact_k = std::atoi(e);
        if (cfg.promote_impact_k == 1) cfg.promote_impact_k = 32;
    }
    if (const char* e = std::getenv("SABORI_PROMOTE_IMPACT_PERIOD")) {
        cfg.promote_impact_period = std::atoi(e);
    }
    if (const char* e = std::getenv("SABORI_BOTTOMUP")) {
        cfg.bottomup_fail_limit = std::atoi(e);
        if (cfg.bottomup_fail_limit == 1) cfg.bottomup_fail_limit = 2000;
    }
    if (const char* e = std::getenv("SABORI_BOTTOMUP_ISOLATE")) {
        cfg.bottomup_isolate = std::atoi(e) != 0;
    }
    if (const char* e = std::getenv("SABORI_BOTTOMUP_CUTOFF")) {
        cfg.bottomup_cutoff_denom = std::atoi(e);
    }
    // 分岐方向（low/high/p のみ。vote/cycle は per-worker 化しない — env 専用）。
    if (const char* e = std::getenv("SABORI_BISECT_DIR")) {
        std::string v(e);
        if (v == "low") {
            cfg.bisect_low_prob = 1.0;
        } else if (v.rfind("low:", 0) == 0) {
            double q = std::atof(v.c_str() + 4);
            if (q < 0.0) q = 0.0;
            if (q > 1.0) q = 1.0;
            cfg.bisect_low_prob = q;
        } else if (v == "high") {
            cfg.bisect_low_prob = 2.0;
        }
    }
}

namespace {

// 多様化ラダー: 「シード優先・軸は疎に」の交互配置。make_portfolio_configs（スレッド間）と
// ParallelSolver::make_instance_config（ラウンドロビン・スロット間）の両方から使う共通軸。
// 根拠 (bench_axis_seed_grid.py, 2026-07-03, 決定性修正後・軸×シード分離計測):
//   - 全 ablation 軸は同シード差分で平均マイナス。旧ラダー(2026-06-30)の
//     正の Δ はシード運の混入だった（軸ごとに別シードで計測していた）。
//   - 最適化: 純シード変種が全軸構成に勝る第一選択(+0.026)。
//     軸で限界ゲインは no_nogood(+0.009) のみ。conflict はシード特異的勝ち。
//   - SAT: no_probe(+0.030) > sc8(+0.006) > 純シード。no_gradient が唯一平均正。
//     mrv(-0.118)/no_temporal(-0.149)/off(-0.096) は SAT ラダーから排除。
//   - 偶数スロットの純シードは「軸なし・導出シードのみ」（solbat14 の -j8
//     解禁がシード単独で再現した知見を反映）。
// defined-bool 昇格(promote)アームは実並列 -j4/-j8 A/B(bench_ladder_parallel.py, best-of
// 反復)で既定ラダー配線を正当化できず不採用。単スレ greedy VBS では worker1 に大寄与
// (Δ=+0.125)だったが、それは bound 共有カップリングを無視した過大評価で fillomino14 依存。
// 実並列は -j4 で 12-8、-j8 で 11-13 と wash〜微負、大勝も thread/rep で反転、
// arithmetic-target 等で promote が base の軌道を悪化させる実 regression あり。
// → 既定ラダーは従来のまま。promote は opt-in(SABORI_PROMOTE_DEF_BOOL /
//    WorkerConfig.promote_def_bool)のみ温存。[[reif-promote-central-band]] と同じ判断。
// 注: 現在はどこからも呼ばれない（スレッド間は make_portfolio_configs 内の実験中
// switch、スロット間は apply_slot_diversification_axis に分離）。2026-07-03 計測の
// 7ケース表の原本として手順7（ラダー再チューニング）まで参照用に残す。
[[maybe_unused]] void apply_diversification_axis(WorkerConfig& c, size_t k, bool is_optimize,
                                const WorkerConfig& base) {
    if (is_optimize) {
        switch (k) {
            case 0: break;                              // 純シード（第一選択）
            case 1: c.conflict_learning = !base.conflict_learning; break;
            case 2: break;                              // 純シード
            case 3: break;                              // 純シード
            case 4: c.nogood_learning = false; break;
            case 5: c.fixed_mixp = 0; break;            // mrv（旧2位ヘッジ）
            case 6: break;                              // 純シード
        }
    } else {  // SAT（promote は入れない: 中立〜有害）
        switch (k) {
            case 0: c.probe_enabled = false; break;
            case 1: c.conflict_learning = !base.conflict_learning; break;
            case 2: break;                              // 純シード
            case 3: break;                              // 純シード
            case 4: break;                              // 純シード
            case 5: c.gradient_enabled = false; break;
            case 6: break;                              // 純シード
        }
    }
}

// スロット間（ラウンドロビン・マルチスタート）用の多様化軸。
// 役割分担（multistart-portfolio-design.md §2(a)）:
//   スレッド間 = CPU を独占する構造的戦略（conflict/nogood/mrv/probe/gradient）
//   スロット間 = リスタート単位で CPU を明け渡すため、切り替えコストが低く
//                初期分散の大きい軸（シード・分岐方向・restart_scale）に限定する。
//                長い連続計算を要する戦略（証明系・学習系）はスロットに置かない。
// 値は未計測の暫定配置（bisect_low_prob は §3 で「ラダー軸の第一候補」と名指し）。
// 手順7の軸×シードグリッドで再チューニングすること。
void apply_slot_diversification_axis(WorkerConfig& c, size_t k) {
    switch (k) {
        case 0: c.bisect_low_prob = 1.0; break;   // 常に下側（"low"）
        case 1: c.restart_scale = 2.0; break;
        case 2: break;                             // 純シード
        case 3: c.bisect_low_prob = 2.0; break;   // 常に上側（"high"）
        case 4: c.restart_scale = 8.0; break;
        case 5: break;                             // 純シード
        case 6: break;                             // 純シード
    }
}

} // namespace

ParallelSolver::ParallelSolver(size_t num_threads, std::vector<WorkerConfig> configs,
                               size_t instances_per_thread)
    : num_threads_(num_threads == 0 ? 1 : num_threads)
    , configs_(std::move(configs))
    , instances_per_thread_(instances_per_thread == 0 ? 1 : instances_per_thread) {
    // 構成が不足していればデフォルト（シードだけずらす）で補う。
    while (configs_.size() < num_threads_) {
        WorkerConfig c;
        c.seed = static_cast<uint32_t>(12345678u + configs_.size() * 2654435761u);
        configs_.push_back(c);
    }
}

WorkerConfig ParallelSolver::make_instance_config(size_t thread_idx, size_t slot_idx) const {
    const WorkerConfig& thread_base = configs_[thread_idx];
    WorkerConfig c = thread_base;
    if (slot_idx > 0) {
        // 0x86545D77u: スレッド間シード導出（2654435761u）と衝突しないよう別定数を使う。
        c.seed = static_cast<uint32_t>(c.seed + slot_idx * 0x86545D77u);
        // シードだけでなく構成軸もスロット間でずらす（シングルスレッドでも構成の異なる
        // Solver が並ぶようにする）。軸表はスレッド間ラダーと分離（§2(a) の役割分担）:
        // スロットは時分割なので初期分散系（分岐方向・restart_scale）のみ。
        apply_slot_diversification_axis(c, (slot_idx - 1) % 7);
    }
    return c;
}

void ParallelSolver::build_workers(const Model& master) {
    // 再入（同一インスタンスでの複数回 solve）に備え、前回の state を全消去する。
    // workers_ready_ を false に落としてから配列を作り直すことで、構築中に stop() が
    // 走っても古い solvers_ を走査しないようにする。
    workers_ready_.store(false);
    models_.clear();
    solvers_.clear();
    rings_.clear();
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

    size_t total = num_threads_ * instances_per_thread_;
    // reserve でキャパシティを固定（push_back 後の再確保を防ぎ、stop() の走査を安全にする）。
    models_.reserve(total);
    solvers_.reserve(total);
    rings_.reserve(num_threads_);
    // verbose 対象スレッド（範囲外なら最後のスレッドにクランプ）。
    verbose_worker_clamped_ = (verbose_worker_ < num_threads_) ? verbose_worker_ : num_threads_ - 1;

    for (size_t t = 0; t < num_threads_; ++t) {
        // リング内バンディットの抽選 RNG はスレッドの base seed から導出（決定論を保つ）。
        rings_.push_back(std::make_unique<RoundRobinRing>(instances_per_thread_, configs_[t].seed));
        for (size_t s = 0; s < instances_per_thread_; ++s) {
            models_.push_back(master.clone());
            auto solver = std::make_unique<Solver>();
            solver->apply_worker_config(make_instance_config(t, s));
            // verbose は 1 インスタンス（対象スレッドの slot0）だけに限定。
            solver->set_verbose(verbose_ && t == verbose_worker_clamped_ && s == 0);
            solvers_.push_back(std::move(solver));
        }
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
        // ラウンドロビンで自分の番待ちのままブロックしているインスタンスを起こす。
        for (auto& r : rings_) {
            if (r) r->notify_all();
        }
    }
}

void ParallelSolver::worker_sat(size_t thread_idx, size_t slot_idx,
                                const SolutionFoundCallback& on_solution) {
    size_t idx = flat_index(thread_idx, slot_idx);
    RoundRobinRing* ring = (instances_per_thread_ > 1) ? rings_[thread_idx].get() : nullptr;

    if (ring) {
        if (!ring->wait_turn(slot_idx, stop_flag_)) return;  // 開始前に stop 済み
        solvers_[idx]->set_restart_yield_hook([ring, slot_idx, this](bool productive, size_t depth_gained) {
            ring->yield_turn(productive, depth_gained);
            ring->wait_turn(slot_idx, stop_flag_);
        });
    }

    auto sol = solvers_[idx]->solve_prepared(*models_[idx]);
    if (sol) {
        // 最初に解いたインスタンスが勝つ。
        if (!have_incumbent_.exchange(true)) {
            {
                std::lock_guard<std::mutex> lk(result_mtx_);
                best_solution_ = sol;
                winner_ = idx;
            }
            if (on_solution) on_solution(*sol);
            stop();  // 他インスタンスを止める
        }
    } else if (!solvers_[idx]->is_stopped()) {
        // 解なしで自然終了（探索空間を尽くした）= 大域 UNSAT。
        unsat_proven_.store(true);
        stop();
    }
}

void ParallelSolver::publish_incumbent(size_t flat_idx, const Solution& sol,
                                       Domain::value_type obj, bool minimize,
                                       const ImproveCallback& on_improve) {
    std::lock_guard<std::mutex> lk(result_mtx_);
    bool better = !have_incumbent_.load() ||
                  (minimize ? obj < *best_objective_ : obj > *best_objective_);
    if (!better) return;
    best_objective_ = obj;
    best_solution_ = sol;
    winner_ = flat_idx;
    best_obj_.store(obj);          // hook が読む大域 incumbent（atomic）
    have_incumbent_.store(true);   // hook が読む（atomic）
    if (on_improve) on_improve(sol, obj);
}

void ParallelSolver::worker_optimize(size_t thread_idx, size_t slot_idx, size_t obj_idx,
                                     bool minimize, const std::string& obj_name,
                                     const ImproveCallback& on_improve) {
    size_t idx = flat_index(thread_idx, slot_idx);
    RoundRobinRing* ring = (instances_per_thread_ > 1) ? rings_[thread_idx].get() : nullptr;

    if (ring) {
        if (!ring->wait_turn(slot_idx, stop_flag_)) return;  // 開始前に stop 済み
        solvers_[idx]->set_restart_yield_hook([ring, slot_idx, this](bool productive, size_t depth_gained) {
            ring->yield_turn(productive, depth_gained);
            ring->wait_turn(slot_idx, stop_flag_);
        });
    }

    // hook: 大域 incumbent を返す（無ければ nullopt）。読み取りは atomic のみ。
    solvers_[idx]->set_external_bound_hook(
        [this]() -> std::optional<Domain::value_type> {
            if (!have_incumbent_.load()) return std::nullopt;
            return best_obj_.load();
        });

    // 改善 incumbent を publish するコールバック。
    auto wrapped = [this, idx, obj_name, minimize, &on_improve](const Solution& sol) -> bool {
        auto it = sol.find(obj_name);
        if (it != sol.end()) {
            publish_incumbent(idx, sol, it->second, minimize, on_improve);
        }
        return true;  // 探索継続
    };

    // 【診断】SABORI_WORKER_FULLSOLVE=1: prepared をやめ各インスタンスが非 presolve clone を
    // full solve_optimize（自前 init_search で presolve+build_order を実行→
    // SABORI_BUILDORDER_PREPRESOLVE を尊重）。並列 × pre-presolve 軌道の健全性ストレステスト用。
    static const bool full = std::getenv("SABORI_WORKER_FULLSOLVE") != nullptr;
    if (full) {
        solvers_[idx]->solve_optimize(*models_[idx], obj_idx, minimize, wrapped);
    } else {
        solvers_[idx]->solve_optimize_prepared(*models_[idx], obj_idx, minimize, wrapped);
    }

    if (!solvers_[idx]->is_stopped()) {
        // 自然終了 = この完全探索が大域 incumbent の最適性を証明した。
        optimal_proven_.store(true);
        stop();
    }
}

ParallelSolver::Result ParallelSolver::solve(Model& master, SolutionFoundCallback on_solution) {
    Result r;
    is_optimize_ = false;

    // presolve を 1 度だけ実行（使い捨ての prep ソルバ）。
    Solver prep;
    if (!prep.prepare(master)) {
        r.status = SearchResult::UNSAT;  // presolve で矛盾 = UNSAT
        return r;
    }

    build_workers(master);

    std::vector<std::thread> threads;
    threads.reserve(num_threads_ * instances_per_thread_);
    for (size_t t = 0; t < num_threads_; ++t) {
        for (size_t s = 0; s < instances_per_thread_; ++s) {
            threads.emplace_back([this, t, s, &on_solution] { worker_sat(t, s, on_solution); });
        }
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
    is_optimize_ = true;

    // SABORI_WORKER_FULLSOLVE=1 のときは各インスタンスが自前 presolve するので prep をスキップ
    // （非 presolve clone を配る）。診断専用。既定は従来どおり presolve-once + prepared。
    static const bool full = std::getenv("SABORI_WORKER_FULLSOLVE") != nullptr;
    if (!full) {
        Solver prep;
        if (!prep.prepare(master)) {
            r.status = SearchResult::UNSAT;
            return r;
        }
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
    threads.reserve(num_threads_ * instances_per_thread_);
    for (size_t t = 0; t < num_threads_; ++t) {
        for (size_t s = 0; s < instances_per_thread_; ++s) {
            threads.emplace_back([this, t, s, obj_var_idx, minimize, &obj_name, &on_improve] {
                worker_optimize(t, s, obj_var_idx, minimize, obj_name, on_improve);
            });
        }
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
        // 多様化ラダー: 「シード優先・軸は疎に」の交互配置。
        // 根拠 (bench_axis_seed_grid.py, 2026-07-03, 決定性修正後・軸×シード分離計測):
        //   - 全 ablation 軸は同シード差分で平均マイナス。旧ラダー(2026-06-30)の
        //     正の Δ はシード運の混入だった（軸ごとに別シードで計測していた）。
        //   - 最適化: 純シード変種が全軸構成に勝る第一選択(+0.026)。
        //     軸で限界ゲインは no_nogood(+0.009) のみ。conflict はシード特異的勝ち。
        //   - SAT: no_probe(+0.030) > sc8(+0.006) > 純シード。no_gradient が唯一平均正。
        //     mrv(-0.118)/no_temporal(-0.149)/off(-0.096) は SAT ラダーから排除。
        //   - 偶数スロットの純シードは「軸なし・導出シードのみ」（solbat14 の -j8
        //     解禁がシード単独で再現した知見を反映）。
        // defined-bool 昇格(promote)アームは実並列 -j4/-j8 A/B(bench_ladder_parallel.py, best-of
        // 反復)で既定ラダー配線を正当化できず不採用。単スレ greedy VBS では worker1 に大寄与
        // (Δ=+0.125)だったが、それは bound 共有カップリングを無視した過大評価で fillomino14 依存。
        // 実並列は -j4 で 12-8、-j8 で 11-13 と wash〜微負、大勝も thread/rep で反転、
        // arithmetic-target 等で promote が base の軌道を悪化させる実 regression あり。
        // → 既定ラダーは従来のまま。promote は opt-in(SABORI_PROMOTE_DEF_BOOL /
        //    WorkerConfig.promote_def_bool)のみ温存。[[reif-promote-central-band]] と同じ判断。
        size_t k = (i - 1) % 7;
#if 0
        if (is_optimize) {
            switch (k) {
                case 0: break;                              // 純シード（第一選択）
                case 1: c.nogood_learning = false; break;
                case 2: break;                              // 純シード
                case 3: c.conflict_learning = !base.conflict_learning; break;
                case 4: break;                              // 純シード
                case 5: c.fixed_mixp = 0; break;            // mrv（旧2位ヘッジ）
                case 6: break;                              // 純シード
            }
        } else {  // SAT（promote は入れない: 中立〜有害）
            switch (k) {
                case 0: c.probe_enabled = false; break;
                case 1: break;                              // 純シード
                case 2: c.restart_scale = 8.0; break;
                case 3: break;                              // 純シード
                case 4: c.gradient_enabled = false; break;
                case 5: break;                              // 純シード
                case 6: c.conflict_learning = !base.conflict_learning;
                        c.restart_scale = 8.0; break;       // conf_sc8（ヘッジ）
            }
        }
#endif
#if 1
        if (is_optimize) {
            switch (k) {
                case 0: c.gradient_enabled = false; c.restart_scale = 4.0; c.conflict_learning = !base.conflict_learning; break;
                case 1: c.gradient_enabled = false; break;
                case 3: break;                              // 純シード
                case 4: break;                              // 純シード
                case 5: break;                              // 純シード
                case 6: break;                              // 純シード
                case 7: break;                              // 純シード
                case 8: c.nogood_learning = false; break;
                case 9: c.fixed_mixp = 0; break;            // mrv（旧2位ヘッジ）
                case 10: break;                              // 純シード
                case 11: c.conflict_learning = !base.conflict_learning; break;
            }
        } else {  // SAT（promote は入れない: 中立〜有害）
            c.probe_enabled = false; 
            switch (k) {
                case 0: c.restart_scale = 4.0; c.conflict_learning = !base.conflict_learning; break;
                case 1: c.restart_scale = 2.0; break;
                case 2: c.restart_scale = 8.0; break;
                case 3: c.restart_scale = 3.0; break;
                case 4: c.restart_scale = 5.0; break;
                case 5: c.restart_scale = 7.0; break;
                case 6: c.restart_scale = .0; break;
                case 7: c.conflict_learning = !base.conflict_learning; break;
                case 8: break;                              // 純シード
                case 9: break;                              // 純シード
                case 10: break;                              // 純シード
                case 11: break;                              // 純シード
                case 12: c.gradient_enabled = false; break;
                case 13: break;                              // 純シード
            }
        }
#endif
        cfgs.push_back(c);
    }
    return cfgs;
}

} // namespace sabori_csp
