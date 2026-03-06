/**
 * @file solver.hpp
 * @brief CSPソルバークラス（NoGood学習、Activity-based変数選択、リスタート戦略）
 */
#ifndef SABORI_CSP_SOLVER_HPP
#define SABORI_CSP_SOLVER_HPP

#include "sabori_csp/model.hpp"
#include "sabori_csp/nogood_manager.hpp"
#include "sabori_csp/variable_selector.hpp"
#include "sabori_csp/restart_controller.hpp"
#include "sabori_csp/community_analysis.hpp"
#include <functional>
#include <map>
#include <unordered_map>
#include <random>
#include <atomic>
#include <limits>

namespace sabori_csp {

/**
 * @brief 解を表す型
 */
using Solution = std::map<std::string, Domain::value_type>;

/**
 * @brief 解のコールバック関数型
 * @return trueを返すと探索を継続、falseで停止
 */
using SolutionCallback = std::function<bool(const Solution&)>;

/**
 * @brief 探索結果
 */
enum class SearchResult {
    SAT,      // 解が見つかった
    UNSAT,    // 解が存在しない
    UNKNOWN   // 不明（リスタートなど）
};

/**
 * @brief リテラル（変数IDと値のペア + 型）
 */
struct Literal {
    enum class Type : uint8_t {
        Eq,   // var == value
        Leq,  // var <= value
        Geq   // var >= value
    };

    size_t var_idx;
    Domain::value_type value;
    Type type = Type::Eq;

    bool operator==(const Literal& other) const {
        return var_idx == other.var_idx && value == other.value && type == other.type;
    }

    /// このリテラルが現在のモデル状態で成立しているか
    bool is_satisfied(const Model& model) const;

    /// このリテラルの否定を返す (Eq→Eq, Leq↔Geq+1)
    Literal negate() const;
};

/**
 * @brief 名前付きリテラル（NoGood の引き継ぎ用）
 */
struct NamedLiteral {
    std::string var_name;
    Domain::value_type value;
    Literal::Type type = Literal::Type::Eq;
};

/**
 * @brief 名前付き NoGood（NoGood の引き継ぎ用）
 */
struct NamedNoGood {
    std::vector<NamedLiteral> literals;
};

/**
 * @brief NoGood（失敗パターン）
 *
 * これらのリテラルが全て成立すると矛盾。
 * 2-Watched Literal で効率的に伝播。
 */
struct NoGood {
    std::vector<Literal> literals;
    size_t w1 = 0;  // 監視リテラル1
    size_t w2 = 0;  // 監視リテラル2
    size_t last_active = 0;  // 最後に発火（矛盾 or ドメイン削除）したリスタート番号
    size_t id = 0;  // ブルームフィルタ用通し番号
    bool permanent = false;  // trueならメンテナンスで削除しない（解NG用）

    NoGood(std::vector<Literal> lits)
        : literals(std::move(lits))
        , w1(0)
        , w2(literals.size() > 1 ? 1 : 0) {}
};

/**
 * @brief 制約タイプ別統計情報
 */
struct ConstraintStats {
    size_t call_count = 0;
    size_t reduction_count = 0;
    size_t fail_count = 0;
    size_t fail_depth_sum = 0;
};

/**
 * @brief ソルバー統計情報
 */
struct SolverStats {
    size_t max_depth = 0;
    size_t depth_sum = 0;
    size_t depth_count = 0;
    size_t restart_count = 0;
    size_t fail_count = 0;
    size_t nogood_count = 0;
    size_t nogood_check_count = 0;
    size_t nogood_prune_count = 0;
    size_t nogood_domain_count = 0;
    size_t nogood_instantiate_count = 0;
    size_t nogoods_size = 0;
    size_t unit_nogoods_size = 0;
    size_t bisect_count = 0;
    size_t enumerate_count = 0;
};

/**
 * @brief CSPソルバー
 *
 * 以下の最適化技術を使用：
 * - NoGood 学習（失敗パターンの記録と枝刈り）
 * - Activity-based 変数選択（失敗に関与した変数を優先）
 * - リスタート戦略（Luby-like）
 * - 良い部分解の保存と再利用
 */
class Solver {
public:
    Solver();

    /**
     * @brief 最初の解を探索
     * @param model 解くモデル
     * @return 解が見つかればその解、なければstd::nullopt
     */
    std::optional<Solution> solve(Model& model);

    /**
     * @brief 全ての解を探索
     * @param model 解くモデル
     * @param callback 解が見つかるたびに呼ばれるコールバック
     * @return 見つかった解の数
     */
    size_t solve_all(Model& model, SolutionCallback callback);

    /**
     * @brief 最適化問題を探索内 branch-and-bound で解く
     * @param model 解くモデル
     * @param obj_var_idx 目的変数のインデックス
     * @param minimize trueなら最小化、falseなら最大化
     * @param on_improve 改善解が見つかるたびに呼ばれるコールバック（nullptrなら無視）
     * @return 最適解が見つかればその解、なければstd::nullopt
     */
    std::optional<Solution> solve_optimize(
        Model& model, size_t obj_var_idx, bool minimize,
        SolutionCallback on_improve = nullptr);

    /**
     * @brief 統計情報を取得
     */
    const SolverStats& stats() const { return stats_; }

    /**
     * @brief NoGood 学習を有効/無効にする
     */
    void set_nogood_learning(bool enabled) { nogood_learning_ = enabled; }

    /**
     * @brief NoGood の長さ分布を取得（デバッグ用）
     */
    std::map<size_t, size_t> nogood_length_distribution() const {
        return nogood_mgr_.length_distribution();
    }

    /**
     * @brief リスタートを有効/無効にする
     */
    void set_restart_enabled(bool enabled) { restart_enabled_ = enabled; }

    /**
     * @brief Activity-based 変数選択を有効/無効にする
     */
    void set_activity_selection(bool enabled) { activity_selection_ = enabled; }

    /**
     * @brief Activity 優先の変数選択を有効/無効にする
     * true: Activity → ドメインサイズ
     * false: ドメインサイズ → Activity（デフォルト）
     */
    void set_activity_first(bool enabled) { activity_first_ = enabled; }

    /**
     * @brief Activity スコアを取得（最適化の引き継ぎ用）
     */
    const std::vector<double>& activity() const { return activity_; }

    /**
     * @brief Activity スコアを設定（最適化の引き継ぎ用）
     * @param activity 変数名 -> activity スコアのマップ
     * @param model 変数名からインデックスを解決するためのモデル
     */
    void set_activity(const std::map<std::string, double>& activity, const Model& model);

    /**
     * @brief Activity スコアを名前付きマップとして取得
     * @param model 変数名を解決するためのモデル
     */
    std::map<std::string, double> get_activity_map(const Model& model) const;

    /**
     * @brief NoGood を名前付き形式で取得（最適化の引き継ぎ用）
     * @param model 変数名を解決するためのモデル
     * @param max_count 取得する最大数（0 = 全て）
     */
    std::vector<NamedNoGood> get_nogoods(const Model& model, size_t max_count = 0) const;

    /**
     * @brief 名前付き NoGood を追加（最適化の引き継ぎ用）
     * @param nogoods 追加する NoGood のリスト
     * @param model 変数名からインデックスを解決するためのモデル
     * @return 追加された NoGood の数
     */
    size_t add_nogoods(const std::vector<NamedNoGood>& nogoods, const Model& model);

    /**
     * @brief ヒント解を設定（値選択の優先度に使用）
     * @param hint 変数名 -> 値のマップ
     * @param model 変数名からインデックスを解決するためのモデル
     */
    void set_hint_solution(const Solution& hint, const Model& model);

    /**
     * @brief 探索を停止する（シグナルハンドラから呼び出し可能）
     */
    void stop() { stopped_ = true; }

    /**
     * @brief 停止フラグをリセット
     */
    void reset_stop() { stopped_ = false; }

    /**
     * @brief 停止フラグを確認
     */
    bool is_stopped() const { return stopped_; }

    /**
     * @brief 制約タイプ別統計を取得
     */
    const std::unordered_map<std::string, ConstraintStats>& constraint_stats() const { return constraint_stats_; }

    /**
     * @brief verbose モードを有効/無効にする
     */
    void set_verbose(bool enabled) { verbose_ = enabled; }

    /**
     * @brief コミュニティ分析を有効/無効にする
     */
    void set_community_analysis(bool enabled) { community_analysis_.set_enabled(enabled); }

    /**
     * @brief 二分割探索の閾値を設定
     * @param threshold ドメインサイズがこの値を超えたら二分割（0=無効）
     */
    void set_bisection_threshold(size_t threshold) { bisection_threshold_ = threshold; }

    /**
     * @brief improvement probe の fail 上限を設定
     * @param limit fail上限（0=probe無効）
     */
    void set_probe_fail_limit(int limit) { probe_fail_limit_ = limit; }

private:
    void log_presolve_start(const Model& model) const;

    std::atomic<bool> stopped_{false};
    bool verbose_ = false;
    // ===== 探索 =====

    /**
     * @brief メイン探索ループ（リスタート付き）
     * @param callback 全解探索時のコールバック（find_all=falseなら無視）
     * @param find_all trueなら全解探索モード
     */
    std::optional<Solution> search_with_restart(Model& model,
                                                 SolutionCallback callback = nullptr,
                                                 bool find_all = false);

    /**
     * @brief 最適化用探索ループ（探索内 branch-and-bound）
     */
    std::optional<Solution> search_with_restart_optimize(
        Model& model, SolutionCallback callback);

    /**
     * @brief 単一の探索（コンフリクト制限付き）
     */
    SearchResult run_search(Model& model, int conflict_limit, size_t depth,
                            SolutionCallback callback, bool find_all);

    /**
     * @brief 探索フレーム（明示的スタック用）
     */
    struct SearchFrame {
        size_t var_idx;
        int save_point;
        Domain::value_type prev_min, prev_max;
        size_t nogoods_before;
        int remaining_cl;

        enum class Mode : uint8_t { Enumerate, Bisect } mode;

        // Enumerate 用
        std::vector<Domain::value_type> values;
        size_t value_idx;

        // Bisect 用
        Domain::value_type split_point;
        uint8_t branch;  // 0=未開始, 1=first試行済, 2=second試行済
        bool right_first;  // true: 右(x>mid)を先に試す
    };

    /**
     * @brief handle_ascent の戻り値
     */
    enum class AscentAction { Return, Continue, TryNext };

    // ===== run_search ヘルパー =====

    /// 全値/全ブランチ失敗時の共通処理
    void handle_failure(Model& model, SearchFrame& frame,
                        std::vector<SearchFrame>& stack,
                        SearchResult& result, bool& ascending);

    /// 勾配ヒント・ベスト解ヒントによる値の並べ替え
    void order_values(size_t var_idx);

    /// Enumerate モードの値ループ
    void try_enumerate_values(Model& model, SearchFrame& frame,
                              std::vector<SearchFrame>& stack,
                              SearchResult& result, bool& ascending);

    /// Bisect モードの2ブランチ試行
    void try_bisect_branches(Model& model, SearchFrame& frame,
                             std::vector<SearchFrame>& stack,
                             SearchResult& result, bool& ascending);

    /// 変数に対して SearchFrame を構築して stack に push
    void create_search_frame(Model& model, size_t var_idx,
                             std::vector<SearchFrame>& stack, int conflict_limit);

    /// 全変数確定時の解検証・コールバック呼出
    void handle_solution(Model& model, SolutionCallback& callback, bool find_all,
                         SearchResult& result, bool& ascending);

    /// 子ノード結果の処理
    AscentAction handle_ascent(Model& model, std::vector<SearchFrame>& stack,
                               SearchResult& result);

    /**
     * @brief 探索共通の初期化（build_constraint_watch_list → presolve → community 分析）
     * @return presolve 成功なら true、矛盾検出なら false
     */
    bool init_search(Model& model);

    /**
     * @brief presolve（探索前の初期伝播）
     *
     * Phase 1: 各制約の presolve() を固定点まで繰り返す。
     * Phase 1 後に model.prepare_propagation() で内部構造を再構築。
     * Phase 2: 残り1変数の制約に対して on_last_uninstantiated() を呼び出し、
     * キューを処理して固定点に達するまで繰り返す。
     *
     * @return 伝播成功ならtrue、矛盾が検出されたらfalse
     */
    bool presolve(Model& model);

    /**
     * @brief 変数確定時の伝播
     */
    bool propagate_instantiate(Model& model, size_t var_idx,
                                Domain::value_type prev_min, Domain::value_type prev_max);

    /**
     * @brief バックトラック
     */
    void backtrack(Model& model, int save_point);

    /**
     * @brief 現在の解を構築
     */
    Solution build_solution(const Model& model) const;

    /**
     * @brief 全制約が満たされているか検証
     */
    bool verify_solution(const Model& model) const;

    // ===== 変数選択 =====

    /**
     * @brief 制約伝播失敗時に、制約に含まれる割当済み変数の activity を加算
     */
    inline void bump_activity(const Model& model, size_t constraint_idx, size_t trigger_var_idx) {
        if (!bump_activity_enabled_) return;
        const auto& constraint = model.constraints()[constraint_idx];
        bool need_rescale = false;
        constraint->bump_activity(model, trigger_var_idx, activity_.data(), activity_inc_, need_rescale);
        if (need_rescale) {
            rescale_activities();
        }
    }

    /**
     * @brief Activity を減衰（リスタート時に呼ぶ）
     */
    void decay_activities();

    /**
     * @brief Activity をスケーリング（最大値が100になるように）
     */
    void rescale_activities();

    /**
     * @brief コミュニティ構造に基づいて bump_activity の有効/無効を判定
     */
    void update_bump_activity_flag();

    /**
     * @brief Unit nogood をドメインに適用し、process_queue を実行
     * @return false なら UNSAT
     */
    bool apply_unit_nogoods(Model& model);

    // ===== 部分解管理 =====

    /**
     * @brief 良い部分解を保存
     */
    void save_partial_assignment(const Model& model);

    /**
     * @brief リスタート時に使用する割り当てを選択
     */
    const std::vector<Domain::value_type>& select_best_assignment();

    // ===== 伝播キュー =====

    /**
     * @brief 伝播キューを処理
     */
    bool process_queue(Model& model);

    /**
     * @brief NoGoodManager の統計を SolverStats に同期
     */
    void sync_nogood_stats();

    // ===== メンバ変数 =====

    // 設定
    bool nogood_learning_ = true;
    bool restart_enabled_ = true;
    bool activity_selection_ = true;
    bool activity_first_ = false;  // false: MRV優先, true: Activity優先
    size_t bisection_threshold_ = 8;  // ドメインサイズがこの値を超えたら二分割（0=無効）
    int probe_fail_limit_ = 5;      // improvement probe の fail 上限（0=無効）

    // 最適化状態
    bool optimizing_ = false;
    size_t obj_var_idx_ = SIZE_MAX;
    bool minimize_ = true;
    std::optional<Solution> best_solution_;
    std::optional<Domain::value_type> best_objective_;

    // 状態
    int current_decision_ = 0;
    std::vector<double> activity_;
    double activity_inc_ = 1.0;
    std::vector<int> temporal_activity_;  ///< 全値失敗した変数の直近失敗回数
    std::vector<Literal> decision_trail_;

    // NoGood 管理
    NoGoodManager nogood_mgr_;
    Bloom512 ng_usage_bloom_;        // 現在の探索パス上の NG 利用状況（探索状態なので Solver 側で管理）
    size_t nogood_inactive_restart_limit_ = 10;  // この回数リスタートしても非活性なNGを削除

    // 部分解（最良の1つだけ保持）
    // INT64_MIN をセンチネル値として使用（値なし）
    static constexpr Domain::value_type kNoValue = std::numeric_limits<Domain::value_type>::min();
    size_t best_num_instantiated_ = 0;
    std::vector<Domain::value_type> best_assignment_;
    std::vector<Domain::value_type> current_best_assignment_;

    // 疑似勾配（最適化用）
    std::vector<Domain::value_type> prev_improving_solution_;
    std::vector<double> gradient_ema_;  // 移動平均
    size_t gradient_var_idx_ = SIZE_MAX;
    int gradient_direction_ = 0;
    Domain::value_type gradient_ref_val_ = 0;

    // リスタート（Adaptive Restart）
    RestartController restart_ctrl_;

    // 統計
    SolverStats stats_;
    std::unordered_map<std::string, ConstraintStats> constraint_stats_;

    // 変数選択
    VariableSelector var_selector_;

    struct UnassignedTrailEntry {
        int level;
        size_t dec_end;
        size_t def_end;
        Bloom512 ng_usage_bloom;
    };
    std::vector<UnassignedTrailEntry> unassigned_trail_;

    // 値選択バッファ（ヒープ確保を避けるため再利用）
    std::vector<Domain::value_type> value_buffer_;

    // 乱数
    std::mt19937 rng_;

    // コミュニティ分析
    CommunityAnalysis community_analysis_;
    size_t propagation_source_ = SIZE_MAX;  ///< 伝播の起点変数（判定時にセット）
    bool bump_activity_enabled_ = true;     ///< コミュニティ構造が弱い場合は無効化
};

} // namespace sabori_csp

#endif // SABORI_CSP_SOLVER_HPP
