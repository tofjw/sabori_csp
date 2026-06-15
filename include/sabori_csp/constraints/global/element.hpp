#ifndef SABORI_CSP_CONSTRAINTS_GLOBAL_ELEMENT_HPP
#define SABORI_CSP_CONSTRAINTS_GLOBAL_ELEMENT_HPP

#include "sabori_csp/constraint.hpp"
#include <unordered_map>
#include <numeric>
#include <vector>
#include <memory>

namespace sabori_csp {


/**
 * @brief int_element制約: array[index] = result を維持する
 *
 * - index: インデックス変数（1-based、MiniZinc 仕様。zero_based=true で 0-based）
 * - array: 定数整数の配列
 * - result: 結果変数
 *
 * 状態を持たないため、rewind_to は空実装。
 */
class IntElementConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param index_var インデックス変数
     * @param array 定数整数の配列
     * @param result_var 結果変数
     * @param zero_based true なら 0-based インデックス（デフォルトは 1-based）
     */
    IntElementConstraint(VariablePtr index_var,
                         std::vector<Domain::value_type> array,
                         VariablePtr result_var,
                         bool zero_based = false);

    std::string name() const override;


    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;

    /**
     * @brief 残り1変数になった時の伝播
     *
     * index 確定済み → result を確定
     * result 確定済み → index の候補が1つなら確定
     */
    bool on_last_uninstantiated(Model& model, int save_point,
                                 size_t last_var_internal_idx) override;

    /**
     * @brief 下限更新時のインクリメンタル伝播
     */
    bool on_set_min(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;

    /**
     * @brief 上限更新時のインクリメンタル伝播
     */
    bool on_set_max(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;

    /**
     * @brief 指定セーブポイントまで状態を巻き戻す（空実装）
     */
    void rewind_to(int save_point);

protected:
    /**
     * @brief 初期整合性チェック
     */


private:
    std::vector<Domain::value_type> array_;
    size_t n_;
    bool zero_based_;
    size_t index_id_;
    size_t result_id_;

    // CSR: 値 -> インデックスリスト（逆引き）
    std::unordered_map<Domain::value_type, std::vector<Domain::value_type>> value_to_indices_;

    // Monotonic prefix/suffix arrays for reverse bounds propagation
    std::vector<Domain::value_type> p_min_;  // prefix min (non-increasing)
    std::vector<Domain::value_type> p_max_;  // prefix max (non-decreasing)
    std::vector<Domain::value_type> s_min_;  // suffix min (non-decreasing)
    std::vector<Domain::value_type> s_max_;  // suffix max (non-increasing)

    // Sparse Table for O(1) range min/max queries
    std::vector<std::vector<Domain::value_type>> sparse_min_;
    std::vector<std::vector<Domain::value_type>> sparse_max_;
    int log_n_;

    // 変数ポインタ → 内部インデックス (0: index, 1: result)

    /**
     * @brief index を 0-based に変換
     */
    Domain::value_type index_to_0based(Domain::value_type idx) const;

    /**
     * @brief 区間 [lo, hi] (0-based) の最小値を O(1) で取得
     */
    Domain::value_type range_min(size_t lo, size_t hi) const;

    /**
     * @brief 区間 [lo, hi] (0-based) の最大値を O(1) で取得
     */
    Domain::value_type range_max(size_t lo, size_t hi) const;
};


/**
 * @brief 単調配列用 array_int_element 制約の特殊化
 *
 * 配列が非減少または非増加の場合、Sparse Table や value_to_indices マップが不要になり、
 * O(n) メモリ + O(log n) 二分探索で伝播できる。
 *
 * 伝播ルール (非減少の場合):
 * - index bounds 変更 → result bounds = a[index_min], a[index_max] (O(1))
 * - result bounds 変更 → index bounds = lower_bound/upper_bound (O(log n))
 */
class IntElementMonotonicConstraint : public Constraint {
public:
    enum class Monotonicity { NON_DECREASING, NON_INCREASING };

    IntElementMonotonicConstraint(VariablePtr index_var,
                                   std::vector<Domain::value_type> array,
                                   VariablePtr result_var,
                                   Monotonicity mono,
                                   bool zero_based = false);

    std::string name() const override;


    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;

    bool on_last_uninstantiated(Model& model, int save_point,
                                 size_t last_var_internal_idx) override;

    bool on_set_min(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;

    bool on_set_max(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;

    void rewind_to(int save_point);

protected:


private:
    std::vector<Domain::value_type> array_;  // 元配列そのまま (O(n))
    size_t n_;
    bool zero_based_;
    Monotonicity mono_;
    size_t index_id_;
    size_t result_id_;

    Domain::value_type index_to_0based(Domain::value_type idx) const;
};


/**
 * @brief array_int_maximum制約: m = max(x[0], x[1], ..., x[n-1])
 *
 * 変数 m は配列 x の最大値と等しい。
 *
 * 伝播ルール:
 * - m.min = max(x[i].min) - m は全ての x[i] の最小値の最大値以上
 * - m.max = max(x[i].max) - m は全ての x[i] の最大値の最大値以下
 * - x[i].max <= m.max - 各 x[i] は m の最大値以下
 * - m 確定時: 少なくとも1つの x[i] が m に等しくなれる必要がある
 */
class ArrayIntMaximumConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param m 最大値を表す変数
     * @param vars 配列変数
     */
    ArrayIntMaximumConstraint(VariablePtr m, std::vector<VariablePtr> vars);

    std::string name() const override;


    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;

    bool on_last_uninstantiated(Model& model, int save_point,
                                size_t last_var_internal_idx) override;

    /**
     * @brief 指定セーブポイントまで状態を巻き戻す（状態を持たないので空実装）
     */
    void rewind_to(int save_point);

protected:


private:
    size_t n_;                     // 配列サイズ
    size_t m_id_;                  // m_ の変数ID キャッシュ

    // 変数ポインタ → 内部インデックス (0: m, 1..n: x[0]..x[n-1])
};


/**
 * @brief array_int_minimum制約: m = min(x[0], x[1], ..., x[n-1])
 *
 * 変数 m は配列 x の最小値と等しい。
 */
class ArrayIntMinimumConstraint : public Constraint {
public:
    ArrayIntMinimumConstraint(VariablePtr m, std::vector<VariablePtr> vars);

    std::string name() const override;


    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;

    bool on_last_uninstantiated(Model& model, int save_point,
                                size_t last_var_internal_idx) override;

    void rewind_to(int save_point);

protected:


private:
    size_t n_;
    size_t m_id_;                  // m_ の変数ID キャッシュ
};


/**
 * @brief array_var_int_element制約: array[index] = result（配列要素が変数）
 *
 * 配列要素が変数の element 制約。bounds consistency を維持。
 *
 * 伝播ルール:
 * - result.min = min { array[i].min : i ∈ dom(index) }
 * - result.max = max { array[i].max : i ∈ dom(index) }
 * - index から i を削除: array[i] と result の bounds が重ならない場合
 * - index 確定時: array[index] = result（等式として伝播）
 *
 * Bounds support tracking:
 * - result の下限をサポートするインデックス集合を追跡
 * - result の上限をサポートするインデックス集合を追跡
 * - サポートが失われたら bounds を再計算
 */
class ArrayVarIntElementConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param index インデックス変数
     * @param array 変数の配列
     * @param result 結果変数
     * @param zero_based true なら 0-based インデックス（デフォルトは 1-based）
     */
    ArrayVarIntElementConstraint(VariablePtr index,
                                  std::vector<VariablePtr> array,
                                  VariablePtr result,
                                  bool zero_based = false);

    std::string name() const override;


    bool prepare_propagation(Model& model) override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    std::optional<bool> is_satisfied(const Model& model) const override;
    bool on_final_instantiate(const Model& model) override;

    bool on_last_uninstantiated(Model& model, int save_point,
                                size_t last_var_internal_idx) override;

    /**
     * @brief 下限更新時のインクリメンタル伝播
     */
    bool on_set_min(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;

    /**
     * @brief 上限更新時のインクリメンタル伝播
     */
    bool on_set_max(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;

    /**
     * @brief 指定セーブポイントまで状態を巻き戻す
     */
    void rewind_to(int save_point) override;

    bool on_remove_value(Model& model, int save_point,
                         size_t internal_var_idx,
                         Domain::value_type removed_value) override;

    void bump_activity(const Model& model, size_t trigger_var_idx,
                       double* activity, double activity_inc,
                       bool& need_rescale, std::mt19937& rng) const override;

protected:


private:
    size_t n_;  // array size
    bool zero_based_;
    size_t index_id_;
    size_t result_id_;

    // Support tracking mode (elem_dom が小さい問題でのみ有効)
    bool use_support_tracking_ = false;

    // Bounds support tracking
    Domain::value_type current_result_min_support_;  ///< result の下限サポート値
    Domain::value_type current_result_max_support_;  ///< result の上限サポート値
    size_t min_support_arr_idx_ = SIZE_MAX;  ///< result min を提供する配列要素の 0-based index
    size_t max_support_arr_idx_ = SIZE_MAX;  ///< result max を提供する配列要素の 0-based index

    // Trail for bounds support
    struct TrailEntry {
        Domain::value_type min_support;
        Domain::value_type max_support;
        size_t min_support_idx;
        size_t max_support_idx;
    };
    std::vector<std::pair<int, TrailEntry>> trail_;

    Domain::value_type index_to_0based(Domain::value_type idx) const;
    Domain::value_type index_from_0based(size_t idx_0based) const;

    bool propagate_bounds(Model& model, int save_point = -1);
    bool propagate_via_queue(Model& model);
    bool filter_index_against_result(Model& model);
    void recompute_bounds_support(Model& model);

    /**
     * @brief support 状態を trail に保存（save_point ごとに1回だけ）
     */
    void save_support_trail(Model& model, int save_point);
};

} // namespace sabori_csp

#endif // SABORI_CSP_CONSTRAINTS_GLOBAL_ELEMENT_HPP
