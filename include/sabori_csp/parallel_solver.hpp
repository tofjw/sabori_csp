/**
 * @file parallel_solver.hpp
 * @brief マルチスレッド・ポートフォリオソルバ（presolve 1 回 → モデル/ソルバを clone して並列探索）
 */
#ifndef SABORI_CSP_PARALLEL_SOLVER_HPP
#define SABORI_CSP_PARALLEL_SOLVER_HPP

#include "sabori_csp/model.hpp"
#include "sabori_csp/solver.hpp"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace sabori_csp {

/**
 * @brief 使い捨てマルチスタート（ラウンドロビン）のターン制御（バンディット版）
 *
 * n 個のメンバー（同じ NG/activity を持たない独立な Solver インスタンス）が
 * 「自分の番」になるまで待ち、番が来たら計算し、リスタートのたびに次のメンバーへ
 * 番を譲る。ある瞬間にアクティブなのは常にちょうど1メンバーなので、n 個の
 * OS スレッドを使っていても実質的に CPU 使用は1スレッド分に収まる。
 *
 * 次の番は単純な巡回ではなく、`mode_reward_policy.hpp`（mix_p バンディット）と
 * 同じ EMA 報酬に比例した抽選で選ぶ——「進捗の良いメンバーに多くターンを回す」。
 * kFloor で全メンバーに最低確率を残すので、どのメンバーも長期的には進捗し続け、
 * UNSAT/最適性の証明能力は損なわれない。
 *
 * stop 時は待機中のメンバーが永遠に起きられないと困るので、待機は stop_flag も
 * 監視し、notify_all() で全待機者を起こせるようにしてある。
 *
 * 適応的ファンアウト（fanout_fail_budget > 0）: 抽選対象を先頭 active_ 本に限る。
 * 最初は 1 本だけ（= 分割税なし）で走り、incumbent が無いまま有効メンバーの fail 累計が
 * 予算を超えるたびに active_ を倍（上限 n）、予算も倍にする。段階 k では 2^k 本が
 * 計 budget·2^k fail を使うので、1 本あたりの予算はどの段階でもほぼ一定。
 * 未有効のメンバーは番が回らないので探索を始めない（スレッドは待機のみ）。
 */
class RoundRobinRing {
public:
    /**
     * @param n メンバー数
     * @param seed 抽選 RNG のシード
     * @param fanout_fail_budget 適応的ファンアウトの初期 fail 予算（0 = 無効、最初から n 本）
     */
    explicit RoundRobinRing(size_t n, uint32_t seed = 12345, uint64_t fanout_fail_budget = 0)
        : n_(n == 0 ? 1 : n), reward_(n_, 1.0), rng_(seed),
          active_(fanout_fail_budget > 0 ? 1 : n_), fanout_budget_(fanout_fail_budget) {}

    /// k番目のメンバーとして自分の番を待つ。stop_flag が立ったら false で即座に抜ける。
    bool wait_turn(size_t k, const std::atomic<bool>& stop_flag) {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [&] { return turn_ == k || stop_flag.load(); });
        return !stop_flag.load();
    }

    /**
     * @brief 番を次のメンバーへ譲る（呼び出し元は「現在の番」のメンバーであること）。
     *
     * signal = productive ? 2.0 : 1.0/(1+depth_gained)（mix_p と同じ式）を現在の
     * メンバーの報酬に EMA で反映してから、報酬比例で次の番を抽選する。
     * @param productive このターンで NoGood 枝刈りが進み、かつ深さが伸びたか
     * @param depth_gained このターンで伸びた探索深さ
     * @param fails_used このターンで使った fail 数（適応的ファンアウトの予算消費）
     * @param may_grow 拡大してよいか（大域 incumbent が無い間だけ true を渡す）
     * @return このターンで有効メンバー数を増やしたら増やした後の本数、増やさなければ 0
     */
    size_t yield_turn(bool productive, size_t depth_gained,
                      uint64_t fails_used = 0, bool may_grow = false) {
        std::lock_guard<std::mutex> lk(mtx_);
        double signal = productive ? 2.0 : 1.0 / static_cast<double>(1 + depth_gained);
        reward_[turn_] = kDecay * reward_[turn_] + (1.0 - kDecay) * signal;
        reward_[turn_] = std::max(reward_[turn_], kFloor);

        size_t grown = 0;
        if (active_ < n_) {
            fanout_used_ += fails_used;
            if (may_grow && fanout_used_ >= fanout_budget_) {
                active_ = std::min(active_ * 2, n_);
                fanout_budget_ *= 2;
                fanout_used_ = 0;
                grown = active_;
            }
        }

        double total = 0.0;
        for (size_t i = 0; i < active_; ++i) total += reward_[i];
        std::uniform_real_distribution<double> dist(0.0, total);
        double pick = dist(rng_);
        double acc = 0.0;
        size_t next = active_ - 1;
        for (size_t i = 0; i < active_; ++i) {
            acc += reward_[i];
            if (pick < acc) { next = i; break; }
        }
        // 番が変わらないとき（有効 1 本の間など）は誰も起こさない。notify_all は待機中の
        // 全メンバーを起こして述語を再評価させるので、リスタートごとに呼ぶと無駄な
        // 起床・再スリープが積もる（リスタートの多い問題でスループットが数 % 落ちた）。
        if (next != turn_) {
            turn_ = next;
            cv_.notify_all();
        }
        return grown;
    }

    /// 現在の有効メンバー数（テスト・verbose 用）
    size_t active() {
        std::lock_guard<std::mutex> lk(mtx_);
        return active_;
    }

    /// 外部 stop 時に全待機者を起こす（stop_flag は呼び出し側で先に立てておくこと）。
    void notify_all() {
        std::lock_guard<std::mutex> lk(mtx_);
        cv_.notify_all();
    }

private:
    static constexpr double kDecay = 0.5;   ///< EMA 減衰率（mix_p と同じ）
    static constexpr double kFloor = 0.1;   ///< 報酬の下限（全メンバーの進捗を保証）

    size_t n_;
    size_t turn_ = 0;
    std::vector<double> reward_;
    std::mt19937 rng_;
    size_t active_;            ///< 抽選対象のメンバー数（先頭 active_ 本）
    uint64_t fanout_budget_;   ///< 次の拡大までの fail 予算（段階ごとに倍）
    uint64_t fanout_used_ = 0; ///< 現段階で使った fail 数
    std::mutex mtx_;
    std::condition_variable cv_;
};

/**
 * @brief ポートフォリオ並列ソルバを束ねるマネージャ
 *
 * 構成:
 *  - master スレッドで presolve を 1 度だけ実行する。
 *  - presolve 済み master モデルを Model::clone() で (num_threads × instances_per_thread)
 *    個複製し、それぞれに fresh Solver（異なるシード・構成）を割り当てる。
 *  - instances_per_thread==1 なら「1スレッド=1インスタンス」の従来どおりの並列ポートフォリオ。
 *    2以上なら、そのスレッド内の instances_per_thread 本は RoundRobinRing で
 *    「常にちょうど1本だけアクティブ」に制御し、リスタートのたびに次のインスタンスへ
 *    CPU を譲る（使い捨てマルチスタート）。各インスタンスは NG/activity を独立に持ち
 *    続ける（捨てない）ので、UNSAT/最適性の証明能力は損なわれない。
 *  - 各インスタンスは独立したモデル/ソルバで探索する（共有は協調用の atomic/mutex のみ）。
 *
 * 協調レベル: 純ポートフォリオ＋bound 共有。
 *  - SAT: 最初に解いたインスタンスが勝ち、他を停止する。
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
        SolverStats winner_stats;                    ///< 勝者インスタンスの統計
        size_t winning_thread = SIZE_MAX;            ///< 勝者インスタンスの flat index（solvers_ 上）
    };

    /// 最適化で改善 incumbent が出るたびに（同期して）呼ばれるコールバック
    using ImproveCallback = std::function<void(const Solution&, Domain::value_type)>;
    /// SAT で解が確定したときに（同期して）呼ばれるコールバック
    using SolutionFoundCallback = std::function<void(const Solution&)>;

    /**
     * @brief コンストラクタ
     * @param num_threads ワーカースレッド数（>=1）
     * @param configs 各スレッドの構成（不足分はデフォルトで補う）
     * @param instances_per_thread 1スレッドあたりのラウンドロビン・マルチスタート本数
     *        （既定1=従来どおり、1スレッド=1インスタンス）。2以上のとき、そのスレッド用に
     *        NG/activity を共有しない独立な Solver インスタンスを instances_per_thread 個
     *        用意し、実スレッドは各インスタンスにつき1本ずつ立てるが、RoundRobinRing で
     *        「常にちょうど1本だけアクティブ」に制御する（リスタートのたびに次のインスタンス
     *        へ CPU を譲る）。各インスタンスは自分の持ち場を独立に探索し続けるため
     *        （使い捨てにしない）、UNSAT/最適性の証明能力は損なわれない。
     */
    ParallelSolver(size_t num_threads, std::vector<WorkerConfig> configs,
                   size_t instances_per_thread = 1);

    /**
     * @brief SAT 探索（最初の解を見つけたワーカーが勝つ）
     * @param master モデル（この呼び出し内で presolve され、先頭ワーカーがそのまま探索に使う）
     * @param on_solution 解確定時に同期して呼ばれる（任意）
     */
    Result solve(Model& master, SolutionFoundCallback on_solution = nullptr);

    /**
     * @brief 最適化探索（bound 共有つきポートフォリオ）
     * @param master モデル（presolve され、先頭ワーカーがそのまま探索に使う）
     * @param obj_var_idx 目的変数のインデックス
     * @param minimize true で最小化
     * @param on_improve 改善 incumbent ごとに同期して呼ばれる（任意）
     */
    Result solve_optimize(Model& master, size_t obj_var_idx, bool minimize,
                          ImproveCallback on_improve = nullptr);

    /**
     * @brief 全ワーカーを停止する（タイムアウト/シグナルハンドラから呼べる）
     * @note ワーカー構築完了後は固定サイズの solvers_ に atomic stop を配るだけ。
     * @note stop は sticky。solve 開始前や presolve 中に呼ばれても失われず、
     *       以後の solve / solve_optimize は即座に UNKNOWN で戻る。reset_stop() で解除する。
     */
    void stop();

    /**
     * @brief stop() の効果を解除する（同一インスタンスで再度 solve する前に呼ぶ）
     */
    void reset_stop();

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

    /**
     * @brief スロットの適応的ファンアウトを設定する（instances_per_thread > 1 のときのみ有効）
     *
     * 各スレッドのリングを 1 本で始め、incumbent が無いまま fail 予算を使い切るたびに
     * 有効スロット数を倍にする（RoundRobinRing 参照）。solve 前に呼ぶこと。
     * @param initial_fail_budget 初期 fail 予算（0 = 無効、最初から全スロット）
     */
    void set_adaptive_fanout(uint64_t initial_fail_budget) {
        fanout_fail_budget_ = initial_fail_budget;
    }

private:
    // ワーカー（models_/solvers_/rings_）を master から構築する。
    void build_workers(Model& master);

    // ラウンドロビンのリスタート yield hook を張る（ターン受け渡し + ファンアウト予算の報告）
    void install_ring_hook(size_t thread_idx, size_t slot_idx, RoundRobinRing* ring);

    // SAT ワーカーの本体（thread_idx 番目のリング内 slot_idx 番目のインスタンス）
    void worker_sat(size_t thread_idx, size_t slot_idx, const SolutionFoundCallback& on_solution);
    // 最適化ワーカーの本体
    void worker_optimize(size_t thread_idx, size_t slot_idx, size_t obj_idx, bool minimize,
                         const std::string& obj_name, const ImproveCallback& on_improve);

    // 改善 incumbent を共有状態へ publish する（mutex 下）。
    void publish_incumbent(size_t flat_idx, const Solution& sol,
                           Domain::value_type obj, bool minimize,
                           const ImproveCallback& on_improve);

    // thread_idx 番目のスレッドの slot_idx 番目（0-indexed）のインスタンス用構成を作る
    // （シードをインスタンスごとにずらすだけ。restart_budget 等は関与しない）。
    WorkerConfig make_instance_config(size_t thread_idx, size_t slot_idx) const;

    // (thread_idx, slot_idx) を models_/solvers_ のフラットな添字へ変換する。
    size_t flat_index(size_t thread_idx, size_t slot_idx) const {
        return thread_idx * instances_per_thread_ + slot_idx;
    }

    size_t num_threads_;
    std::vector<WorkerConfig> configs_;
    size_t instances_per_thread_ = 1;  ///< スレッドあたりのラウンドロビン・マルチスタート本数
    uint64_t fanout_fail_budget_ = 0;  ///< 適応的ファンアウトの初期 fail 予算（0 = 無効）
    bool is_optimize_ = false;  ///< solve()=false / solve_optimize()=true（多様化軸の切替に使う）
    bool verbose_ = false;            ///< verbose 出力を有効にするか
    size_t verbose_worker_ = 0;       ///< verbose を出すワーカー番号
    size_t verbose_worker_clamped_ = 0;  ///< build_workers で確定した実際の verbose 対象スレッド

    // models_/solvers_ は (num_threads_ * instances_per_thread_) 個、flat_index() で参照する。
    // build_workers で一度だけ構築し、以降差し替えない（ラウンドロビンは使い捨てにしない）ので
    // stop() からの並行アクセスに追加の mutex は不要（原本の設計のまま）。
    std::vector<std::unique_ptr<Model>>  models_;      ///< 所有する clone（先頭は nullptr = master を使う）
    std::vector<Model*>                  model_refs_;  ///< 各インスタンスが解くモデル（先頭は master）
    std::vector<std::unique_ptr<Solver>> solvers_;
    std::vector<std::unique_ptr<RoundRobinRing>> rings_;  ///< スレッドごとに1つ（サイズ instances_per_thread_）
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
