/**
 * @file nogood_manager.hpp
 * @brief NoGood 管理クラス（学習・伝播・監視リテラル・GC）
 */
#ifndef SABORI_CSP_NOGOOD_MANAGER_HPP
#define SABORI_CSP_NOGOOD_MANAGER_HPP

#include "sabori_csp/model.hpp"
#include <map>
#include <unordered_map>
#include <vector>
#include <memory>

namespace sabori_csp {

// Forward declarations (defined in solver.hpp)
struct Literal;
struct NoGood;
struct NamedNoGood;
struct NamedLiteral;

/**
 * @brief NoGood の管理を一手に担うクラス
 *
 * Solver から以下の責務を分離:
 * - NoGood の追加・削除・GC
 * - 2-Watched Literal による伝播
 * - Eq / Bound watch の管理
 * - Unit NoGood の管理
 * - Bloom filter の管理
 * - NoGood のインポート/エクスポート
 */
class NoGoodManager {
public:
    NoGoodManager();

    /// 全状態をリセット（新しい solve 開始時に呼ぶ）
    void clear();

    /// 変数数を指定して全状態をリセット（watch 配列の事前確保用）
    void clear(size_t n_vars);

    // ===== Core operations =====

    /**
     * @brief NoGood を追加し、watched literal を登録
     * @param literals リテラルのリスト
     * @param restart_count 現在のリスタート番号（last_active 初期値）
     */
    void add_nogood(const std::vector<Literal>& literals, size_t restart_count);

    /**
     * @brief NoGood を削除し、watched literal を解除
     */
    void remove_nogood(NoGood* ng);

    // ===== Propagation =====

    /**
     * @brief 変数確定時の Eq watch 伝播
     *
     * var_idx == val が成立した時、対応する Eq watch を持つ NoGood を伝播する。
     * conflict 時は activity を bump する。
     *
     * @return false なら矛盾
     */
    bool propagate_eq_watches(Model& model, size_t var_idx, Domain::value_type val,
                              size_t restart_count, std::vector<double>& activity,
                              double activity_inc);

    /**
     * @brief 変数の境界変更時の NoGood 伝播
     *
     * 下限上昇 → Geq watch、上限下降 → Leq watch を検査。
     * conflict 時は activity を bump する。
     *
     * @return false なら矛盾
     */
    bool propagate_bound_nogoods(Model& model, size_t var_idx, bool is_lower_bound,
                                 size_t restart_count, std::vector<double>& activity,
                                 double activity_inc);

    // ===== Unit NoGood =====

    /**
     * @brief Unit NoGood（長さ1）を追加
     */
    void add_unit_nogood(const Literal& lit);

    /**
     * @brief Unit NoGood の否定を Model のキューに追加
     *
     * 呼び出し側で process_queue() を呼ぶこと。
     */
    void enqueue_unit_nogoods(Model& model) const;

    /**
     * @brief Unit NoGood リストへの参照
     */
    const std::vector<Literal>& unit_nogoods() const { return unit_nogoods_; }

    // ===== NoGood Learning =====

    /**
     * @brief 判定トレイルからコンフリクト NoGood を学習
     *
     * trail.size() >= 2 なら multi-literal NoGood を追加し activity を bump。
     * trail.size() == 1 なら unit NoGood を追加。
     */
    void learn_from_conflict(const std::vector<Literal>& decision_trail,
                             std::vector<double>& activity, double activity_inc,
                             size_t restart_count);

    // ===== Solution NoGood =====

    /**
     * @brief 現在の完全割当を永続 NoGood として追加（全解探索用）
     */
    void add_solution_nogood(const Model& model, size_t restart_count);

    // ===== Maintenance (GC) =====

    /**
     * @brief 非活性 NoGood の削除・ソート・容量制限
     * @param restart_count 現在のリスタート番号
     * @param inactive_limit この回数リスタートで非活性なら削除
     */
    void gc(size_t restart_count, size_t inactive_limit);

    // ===== Bloom Filter =====

    /**
     * @brief NoGood ID から Bloom512 ビットパターンを生成
     */
    static Bloom512 ng_bloom_bits(size_t ng_id);

    /**
     * @brief 全 NoGood から変数ごとの Bloom filter を再構築
     */
    void rebuild_var_ng_blooms(Model& model) const;

    // ===== Backtrack Support =====

    /**
     * @brief 現在の NoGood 数を返す
     */
    size_t nogoods_count() const { return nogoods_.size(); }

    /**
     * @brief count 以降に追加された NoGood を削除（バックトラック用）
     */
    void truncate_nogoods(size_t count);

    // ===== Import / Export =====

    /**
     * @brief NoGood を名前付き形式でエクスポート
     */
    std::vector<NamedNoGood> get_nogoods(const Model& model, size_t max_count = 0) const;

    /**
     * @brief 名前付き NoGood をインポート
     * @return 追加された NoGood 数
     */
    size_t add_nogoods(const std::vector<NamedNoGood>& nogoods,
                       const Model& model, size_t restart_count);

    // ===== Stats =====

    size_t check_count() const { return check_count_; }
    size_t prune_count() const { return prune_count_; }
    size_t domain_count() const { return domain_count_; }

    // ===== Debug =====

    /**
     * @brief NoGood の長さ分布を返す
     */
    std::map<size_t, size_t> length_distribution() const;

private:
    /**
     * @brief 2-Watched Literal による NoGood 伝播
     * @return false なら矛盾（全リテラル成立）
     */
    bool propagate_nogood(Model& model, NoGood* ng, const Literal& triggered,
                          size_t restart_count);

    void register_watch(const Literal& lit, NoGood* ng);
    void unregister_watch(const Literal& lit, NoGood* ng);

    // NoGood ストレージ
    size_t ng_id_counter_ = 0;
    std::vector<Literal> unit_nogoods_;
    std::vector<std::unique_ptr<NoGood>> nogoods_;

    // Watch 構造（外側は変数インデックスでインデキシング）
    std::vector<std::unordered_map<Domain::value_type, std::vector<NoGood*>>> ng_eq_watches_;
    std::vector<std::vector<std::pair<Domain::value_type, NoGood*>>> ng_leq_watches_;
    std::vector<std::vector<std::pair<Domain::value_type, NoGood*>>> ng_geq_watches_;

    // 容量制限
    static constexpr size_t max_nogoods_ = 100000;

    // 統計カウンタ
    size_t check_count_ = 0;
    size_t prune_count_ = 0;
    size_t domain_count_ = 0;
};

} // namespace sabori_csp

#endif // SABORI_CSP_NOGOOD_MANAGER_HPP
