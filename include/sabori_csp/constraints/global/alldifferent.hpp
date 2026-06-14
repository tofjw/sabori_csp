#ifndef SABORI_CSP_CONSTRAINTS_GLOBAL_ALLDIFFERENT_HPP
#define SABORI_CSP_CONSTRAINTS_GLOBAL_ALLDIFFERENT_HPP

#include "sabori_csp/constraint.hpp"
#include "sabori_csp/sparse_set_pool.hpp"
#include "sabori_csp/constraint_trail.hpp"
#include <numeric>
#include <vector>
#include <memory>

namespace sabori_csp {


/**
 * @brief all_different制約: 全ての変数が異なる値を取る
 *
 * Sparse Set を使用した値プールで効率的に重複をチェック。
 * 鳩の巣原理による枝刈りを実行。
 */
class AllDifferentConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param vars 制約に関与する変数リスト
     */
    explicit AllDifferentConstraint(std::vector<VariablePtr> vars);

    std::string name() const override;


    bool prepare_propagation(Model& model) override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;

    /**
     * @brief 残り1変数になった時の伝播
     *
     * 利用可能な値が1つだけなら、その値で確定させる。
     */
    bool on_last_uninstantiated(Model& model, int save_point,
                                 size_t last_var_internal_idx) override;

    bool on_remove_value(Model& model, int save_point,
                         size_t var_idx, size_t internal_var_idx,
                         Domain::value_type removed_value) override;

    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;

    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;

    /**
     * @brief 指定セーブポイントまで状態を巻き戻す
     */
    void rewind_to(int save_point);

    /**
     * @brief バッチ伝播: bounds(Z) フィルタを1回実行
     */
    bool propagate_batch(Model& model, int save_point) override;

    /**
     * @brief 現在のプールサイズを取得
     */
    size_t pool_size() const { return pool_.size(); }

    /**
     * @brief 未確定変数の数を取得
     */
    size_t unfixed_count() const { return unfixed_count_; }

    /**
     * @brief 制約固有の activity bump
     *
     * 値重複の場合は同じ値を持つ変数のみ bump。
     * 鳩の巣原理による矛盾の場合はデフォルト動作。
     */
    void bump_activity(const Model& model, size_t trigger_var_idx,
                       double* activity, double activity_inc,
                       bool& need_rescale, std::mt19937& rng) const override;

    void init_activity(const Model& model, double* activity) const override;

protected:
    // 値プール（Sparse Set）— サブクラスからアクセス可能
    SparseSetPool pool_;

    // 未確定変数カウント（差分更新用）
    size_t unfixed_count_;

    // bounds(Z) フィルタの有効フラグ。GAC サブクラスが GAC 有効時に false にする
    // （domain consistency は bounds consistency を包含するため二重実行を回避）
    bool bounds_z_enabled_ = true;

private:
    /**
     * @brief サイズ2のドメインペアによる Hall set 検出
     */
    bool check_hall_pair(Model& model, size_t trigger_var_idx);

    /**
     * @brief bounds(Z) consistency フィルタの本体 (López-Ortiz et al., IJCAI 2003)
     *
     * bz_min_/bz_max_ に格納された区間 [min_i, max_i] に対して
     * Hall interval を検出し、bz_newmin_/bz_newmax_ に絞り込み後の bounds を書き込む。
     * 計算量 O(n log n)。
     *
     * @param n 区間数（= 変数数）
     * @param changed bounds が絞り込まれたら true
     * @return 矛盾（Hall interval の容量超過）を検出したら false
     */
    bool run_bounds_filter(size_t n, bool& changed);

    /**
     * @brief 探索中の bounds(Z) 伝播
     *
     * Model から現在の bounds を読み、フィルタ結果を enqueue_set_min/max で反映する。
     */
    bool propagate_bounds_z(Model& model, int save_point);

    // ===== bounds(Z) スクラッチ（毎回現在の bounds から再構築、trail 不要）=====
    std::vector<size_t> bz_minsorted_, bz_maxsorted_;   // min/max 昇順の変数 index
    std::vector<int> bz_minrank_, bz_maxrank_;          // bounds 配列内のランク
    std::vector<Domain::value_type> bz_min_, bz_max_;   // 入力区間
    std::vector<Domain::value_type> bz_newmin_, bz_newmax_;  // フィルタ結果
    std::vector<Domain::value_type> bz_bounds_, bz_d_;  // 臨界容量
    std::vector<int> bz_t_, bz_h_;                      // tree / hall interval リンク

    // 自分が enqueue した bounds 更新のエコーを検出して再実行を抑制する。
    // epoch は rewind_to() でインクリメントされ、バックトラック前のエントリを無効化する。
    uint64_t bz_epoch_ = 1;
    std::vector<Domain::value_type> bz_expected_min_, bz_expected_max_;
    std::vector<uint64_t> bz_expected_min_epoch_, bz_expected_max_epoch_;

    // Trail: (save_point, (old_pool_n, old_unfixed_count))
    struct TrailEntry {
        size_t old_pool_n;
        size_t old_unfixed_count;
    };
    ConstraintTrail<TrailEntry> pool_trail_;

    /**
     * @brief プールから値を削除
     */
    bool remove_from_pool(int save_point, Domain::value_type value);
};


/**
 * @brief alldifferent_except_0制約: 非0値は全て異なる、0は重複可
 *
 * 非0値の一意性をSparse Setプールで管理。
 */
class AllDifferentExcept0Constraint : public Constraint {
public:
    explicit AllDifferentExcept0Constraint(std::vector<VariablePtr> vars);

    std::string name() const override;


    bool prepare_propagation(Model& model) override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    std::optional<bool> is_satisfied(const Model& model) const override;
    bool on_final_instantiate(const Model& model) override;
    bool on_last_uninstantiated(Model& model, int save_point,
                                 size_t last_var_internal_idx) override;

    bool on_remove_value(Model& model, int save_point,
                         size_t var_idx, size_t internal_var_idx,
                         Domain::value_type removed_value) override;
    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;
    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;

    void rewind_to(int save_point);

    void bump_activity(const Model& model, size_t trigger_var_idx,
                       double* activity, double activity_inc,
                       bool& need_rescale, std::mt19937& rng) const override;

protected:


private:
    bool check_hall_pair(Model& model, size_t trigger_var_idx);

    SparseSetPool pool_;
    size_t unfixed_count_;

    struct TrailEntry {
        size_t old_pool_n;
        size_t old_unfixed_count;
    };
    ConstraintTrail<TrailEntry> pool_trail_;

    bool remove_from_pool(int save_point, Domain::value_type value);
};

} // namespace sabori_csp

#endif // SABORI_CSP_CONSTRAINTS_GLOBAL_ALLDIFFERENT_HPP
