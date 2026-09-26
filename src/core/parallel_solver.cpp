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

// スロット間（ラウンドロビン・マルチスタート）用の多様化軸。
// 役割分担（multistart-portfolio-design.md §2(a)）:
//   スレッド間 = CPU を独占する構造的戦略（conflict/nogood/mrv/probe/gradient）
//   スロット間 = リスタート単位で CPU を明け渡すため、切り替えコストが低く
//                初期分散の大きい軸（シード・分岐方向・restart_scale）に限定する。
//                長い連続計算を要する戦略（証明系・学習系）はスロットに置かない。
// 注: 2026-09-26 のグリッドでは restart_scale / bisect_low がスレッド間アームとしても
// 最上位だったため、スレッド間ラダーにも入れた（役割分担は計測で上書き）。
// スロット表の値は未計測の暫定配置のまま。時分割下では restart_scale の意味が変わりうる
// （リスタート単位で CPU を譲る機構と相互作用する）ので、SABORI_MULTISTART_N で実測してから決める。
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
        // 多様化ラダー（k = (i-1) % 7 で循環）。
        // 最適化の根拠 (bench_axis_seed_grid.py, 2026-09-26, 73問×3シード×30s, -j1):
        //   perf 群マージ + build_order post-presolve 化で単スレッド軌道が変わり、
        //   2026-07-03 の「全軸マイナス・純シード第一」は失効した。base@s0 に足す
        //   アームとしての限界値（2本 VBS, 同じ base@別シードとの比較）は
        //   rs8 +0.205 / rs4 +0.205 / probe_root +0.068 / bisect_low +0.062 /
        //   conflict +0.027 / gradient_off -0.014 / mrv -0.171。
        //   3本 VBS では「restart_scale アーム1本」が骨格で、2本目は probe_root /
        //   conflict / bisect / rs4 がノイズ幅内で並ぶ → 構造の異なる順に並べる。
        //   mrv（旧 case9）と gradient_off（旧 case0/1）は外した。
        //   軸の組み合わせ（旧 case0 の gradient_off+rs4+conflict 等）は未計測なので単軸のみ。
        // SAT: 上記コーパスは SAT 4問のみで判断材料にならないため、旧ラダーの実効部分
        //   （全ワーカー probe_off + restart_scale 各種）を維持。到達しない case7〜13 を削除し、
        //   旧 case6 の restart_scale=0（set_initial_scale で 1.0 に丸められる）は純シードと明記。
        // defined-bool 昇格(promote)アームは実並列 -j4/-j8 A/B で既定配線を正当化できず
        // 不採用。promote は opt-in(WorkerConfig.promote_def_bool)のみ温存
        // [[reif-promote-central-band]]。
        size_t k = (i - 1) % 7;
        if (is_optimize) {
            switch (k) {
                case 0: c.restart_scale = 8.0; break;
                case 1: c.root_probe_limit = 2000; break;   // probe_root（=1 糖衣と同じ予算）
                case 2: c.conflict_learning = !base.conflict_learning; break;
                case 3: c.restart_scale = 4.0; break;
                case 4: c.bisect_low_prob = 1.0; break;     // 常に下側
                case 5: break;                              // 純シード
                case 6: break;                              // 純シード
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
                case 6: break;                              // 純シード
            }
        }
        cfgs.push_back(c);
    }
    return cfgs;
}

} // namespace sabori_csp
