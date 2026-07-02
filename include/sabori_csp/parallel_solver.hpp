/**
 * @file parallel_solver.hpp
 * @brief マルチスレッド・ポートフォリオソルバ（presolve 1 回 → モデル/ソルバを clone して並列探索）
 */
#ifndef SABORI_CSP_PARALLEL_SOLVER_HPP
#define SABORI_CSP_PARALLEL_SOLVER_HPP

#include "sabori_csp/model.hpp"
#include "sabori_csp/solver.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace sabori_csp {

/**
 * @brief ポートフォリオ並列ソルバを束ねるマネージャ
 *
 * 構成:
 *  - master スレッドで presolve を 1 度だけ実行する。
 *  - presolve 済み master モデルを Model::clone() でワーカー数だけ複製し、
 *    各ワーカーに fresh Solver（異なるシード・構成）を割り当てる。
 *  - 各ワーカーは独立したモデル/ソルバで探索する（共有は協調用の atomic/mutex のみ）。
 *
 * 協調レベル: 純ポートフォリオ＋bound 共有。
 *  - SAT: 最初に解いたワーカーが勝ち、他を停止する。
 *  - 最適化: best objective を atomic で共有し、各ワーカーが目的変数を締める。
 *    nogood/clause の共有はしない。
 */
class ParallelSolver {
public:
    /**
     * @brief 探索結果
     */
    struct Result {
        std::optional<Solution> solution;            ///< 見つかった解（最良）
        SearchResult status = SearchResult::UNKNOWN; ///< SAT/UNSAT/UNKNOWN
        std::optional<Domain::value_type> objective; ///< 最適化の目的値
        bool proved_optimal = false;                 ///< 最適性が証明されたか
        SolverStats winner_stats;                    ///< 勝者ワーカーの統計
        size_t winning_thread = SIZE_MAX;            ///< 勝者ワーカーの index
    };

    /// 最適化で改善 incumbent が出るたびに（同期して）呼ばれるコールバック
    using ImproveCallback = std::function<void(const Solution&, Domain::value_type)>;
    /// SAT で解が確定したときに（同期して）呼ばれるコールバック
    using SolutionFoundCallback = std::function<void(const Solution&)>;

    /**
     * @brief コンストラクタ
     * @param num_threads ワーカースレッド数（>=1）
     * @param configs 各ワーカーの構成（不足分はデフォルトで補う）
     */
    ParallelSolver(size_t num_threads, std::vector<WorkerConfig> configs);

    /**
     * @brief SAT 探索（最初の解を見つけたワーカーが勝つ）
     * @param master モデル（この呼び出し内で presolve され、mutate される）
     * @param on_solution 解確定時に同期して呼ばれる（任意）
     */
    Result solve(Model& master, SolutionFoundCallback on_solution = nullptr);

    /**
     * @brief 最適化探索（bound 共有つきポートフォリオ）
     * @param master モデル（presolve され mutate される）
     * @param obj_var_idx 目的変数のインデックス
     * @param minimize true で最小化
     * @param on_improve 改善 incumbent ごとに同期して呼ばれる（任意）
     */
    Result solve_optimize(Model& master, size_t obj_var_idx, bool minimize,
                          ImproveCallback on_improve = nullptr);

    /**
     * @brief 全ワーカーを停止する（タイムアウト/シグナルハンドラから呼べる）
     * @note ワーカー構築完了後は固定サイズの solvers_ に atomic stop を配るだけ。
     */
    void stop();

    /**
     * @brief verbose 出力を指定ワーカーに限定して有効化する
     *
     * 並列では全ワーカーの verbose を出すと交錯して読めないため、1 ワーカーだけに
     * 限定する。worker_idx がワーカー数以上なら最後のワーカーにクランプする。
     * @param enabled verbose を出すか
     * @param worker_idx verbose を出すワーカー番号（既定 0）
     */
    void set_verbose(bool enabled, size_t worker_idx = 0) {
        verbose_ = enabled;
        verbose_worker_ = worker_idx;
    }

private:
    // ワーカー（models_/solvers_）を master から構築する。
    void build_workers(const Model& master);

    // SAT ワーカーの本体
    void worker_sat(size_t i, const SolutionFoundCallback& on_solution);
    // 最適化ワーカーの本体
    void worker_optimize(size_t i, size_t obj_idx, bool minimize,
                         const std::string& obj_name, const ImproveCallback& on_improve);

    // 改善 incumbent を共有状態へ publish する（mutex 下）。
    void publish_incumbent(size_t i, const Solution& sol,
                           Domain::value_type obj, bool minimize,
                           const ImproveCallback& on_improve);

    size_t num_threads_;
    std::vector<WorkerConfig> configs_;
    bool verbose_ = false;            ///< verbose 出力を有効にするか
    size_t verbose_worker_ = 0;       ///< verbose を出すワーカー番号

    std::vector<std::unique_ptr<Model>>  models_;
    std::vector<std::unique_ptr<Solver>> solvers_;
    std::atomic<bool> workers_ready_{false};  ///< solvers_/models_ が安定（stop が配れる）

    // ===== 共有協調状態 =====
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> have_incumbent_{false};
    std::atomic<bool> unsat_proven_{false};
    std::atomic<bool> optimal_proven_{false};
    std::atomic<int64_t> best_obj_{0};  ///< hook が読む大域 incumbent（mutex 下で書く）

    std::mutex result_mtx_;             ///< best_solution_/best_objective_/winner_ を守る
    std::optional<Solution> best_solution_;
    std::optional<Domain::value_type> best_objective_;
    size_t winner_ = SIZE_MAX;
};

/**
 * @brief ポートフォリオの多様化構成テーブルを構築する
 *
 * worker0 = base（既定シード 12345678, adaptive mix_p, 全機能 ON）で
 * 「単一スレッドより悪くならない」軸を確保し、worker1.. はシードをずらしつつ
 * 多様化軸を VBS 限界インパクトの大きい順に適用する。影響順は問題タイプ
 * （is_optimize）で異なる。fzn CLI と Python バインディングの双方が共有する。
 *
 * @param n ワーカー数（>=1）
 * @param is_optimize 最適化なら true（多様化軸の順序が変わる）
 * @param base worker0 に用いる基準構成（bisection_threshold / probe_fail_limit /
 *             nogood_learning / conflict_learning などを事前に詰めておく）
 * @return n 個の WorkerConfig
 */
std::vector<WorkerConfig> make_portfolio_configs(
    size_t n, bool is_optimize, const WorkerConfig& base = WorkerConfig{});

} // namespace sabori_csp

#endif // SABORI_CSP_PARALLEL_SOLVER_HPP
