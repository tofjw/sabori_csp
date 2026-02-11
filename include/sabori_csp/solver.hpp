/**
 * @file solver.hpp
 * @brief CSPソルバークラス（NoGood学習、Activity-based変数選択、リスタート戦略）
 */
#ifndef SABORI_CSP_SOLVER_HPP
#define SABORI_CSP_SOLVER_HPP

#include "sabori_csp/model.hpp"
#include <functional>
#include <map>
#include <unordered_map>
#include <random>
#include <atomic>

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
 * @brief リテラル（変数IDと値のペア）
 */
struct Literal {
    size_t var_idx;
    Domain::value_type value;

    bool operator==(const Literal& other) const {
        return var_idx == other.var_idx && value == other.value;
    }
};

/**
 * @brief 名前付きリテラル（NoGood の引き継ぎ用）
 */
struct NamedLiteral {
    std::string var_name;
    Domain::value_type value;
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
    size_t last_active = 0;  // 最後に prune を起こした時点のカウンタ値
    bool permanent = false;  // trueならメンテナンスで削除しない（解NG用）

    NoGood(std::vector<Literal> lits)
        : literals(std::move(lits))
        , w1(0)
        , w2(literals.size() > 1 ? 1 : 0) {}
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
     * @brief 統計情報を取得
     */
    const SolverStats& stats() const { return stats_; }

    /**
     * @brief NoGood 学習を有効/無効にする
     */
    void set_nogood_learning(bool enabled) { nogood_learning_ = enabled; }

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
     * @brief verbose モードを有効/無効にする
     */
    void set_verbose(bool enabled) { verbose_ = enabled; }

private:
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
     * @brief 現在の完全割当を永続NoGoodとして追加
     */
    void add_solution_nogood(const Model& model);

    /**
     * @brief 単一の探索（コンフリクト制限付き）
     */
    SearchResult run_search(Model& model, int conflict_limit, size_t depth,
                            SolutionCallback callback, bool find_all);

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
     * @brief 次に割り当てる変数を選択
     */
    size_t select_variable(const Model& model);

    /**
     * @brief Activity を減衰
     */
    void decay_activities();

    // ===== NoGood 学習 =====

    /**
     * @brief NoGood を追加
     */
    void add_nogood(const std::vector<Literal>& literals);

    /**
     * @brief NoGood を削除
     */
    void remove_nogood(NoGood* ng);

    /**
     * @brief NoGood 伝播
     */
    bool propagate_nogood(Model& model, NoGood* ng, const Literal& triggered);

    // ===== 部分解管理 =====

    /**
     * @brief 良い部分解を保存
     */
    void save_partial_assignment(const Model& model);

    /**
     * @brief リスタート時に使用する割り当てを選択
     */
    std::unordered_map<size_t, Domain::value_type> select_best_assignment();

    // ===== 伝播キュー =====

    /**
     * @brief 伝播キューを処理
     */
    bool process_queue(Model& model);

    // ===== メンバ変数 =====

    // 設定
    bool nogood_learning_ = true;
    bool restart_enabled_ = true;
    bool activity_selection_ = true;
    bool activity_first_ = false;  // false: MRV優先, true: Activity優先

    // 状態
    int current_decision_ = 0;
    std::vector<double> activity_;
    std::vector<Literal> decision_trail_;

    // NoGood
    std::vector<std::unique_ptr<NoGood>> nogoods_;
    std::unordered_map<size_t, std::unordered_map<Domain::value_type, std::vector<NoGood*>>> ng_watches_;
    static constexpr size_t max_nogoods_ = 100000;
    size_t ng_use_counter_ = 0;  // prune 発生ごとにインクリメント

    // 部分解（最良の1つだけ保持）
    size_t best_num_instantiated_ = 0;
    std::unordered_map<size_t, Domain::value_type> best_assignment_;
    std::unordered_map<size_t, Domain::value_type> current_best_assignment_;

    // リスタート
    double initial_conflict_limit_ = 5.0;
    double conflict_limit_multiplier_ = 1.1;
    double activity_decay_ = 0.99;

    // 統計
    SolverStats stats_;

    // 変数スキャン順序（リスタートごとにシャッフル）
    std::vector<size_t> var_order_;

    // 値選択バッファ（ヒープ確保を避けるため再利用）
    std::vector<Domain::value_type> value_buffer_;

    // 乱数
    std::mt19937 rng_;
};

} // namespace sabori_csp

#endif // SABORI_CSP_SOLVER_HPP
