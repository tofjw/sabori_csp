#ifndef SABORI_CSP_CONSTRAINTS_GLOBAL_COUNTING_HPP
#define SABORI_CSP_CONSTRAINTS_GLOBAL_COUNTING_HPP

#include "sabori_csp/constraint.hpp"
#include <unordered_map>
#include <numeric>
#include <vector>
#include <memory>

namespace sabori_csp {


/**
 * @brief count_eq制約: 配列 x[] の中で target に等しい要素の数 = c
 *
 * O(1) のインクリメンタル伝播で bounds consistency を維持。
 *
 * 不変条件: definite_count_ <= c <= definite_count_ + possible_count_
 *
 * - definite_count_: x[i] == target で確定済みの数
 * - possible_count_: 未確定かつ target が domain に含まれる x[i] の数
 */
class CountEqConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param x_vars 配列変数
     * @param target 定数ターゲット値
     * @param count_var カウント変数
     */
    CountEqConstraint(std::vector<VariablePtr> x_vars,
                      Domain::value_type target,
                      VariablePtr count_var);

    std::string name() const override;


    bool prepare_propagation(Model& model) override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;

    bool on_last_uninstantiated(Model& model, int save_point,
                                 size_t last_var_internal_idx) override;

    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;

    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;

    bool on_remove_value(Model& model, int save_point,
                         size_t var_idx, size_t internal_var_idx,
                         Domain::value_type removed_value) override;

    void rewind_to(int save_point);

protected:


private:
    Domain::value_type target_;
    size_t n_;  // 配列サイズ (x[] の要素数)
    size_t c_id_;  // count 変数の ID キャッシュ

    // 内部カウンタ
    size_t definite_count_;   // x[i] == target で確定済みの数
    size_t possible_count_;   // 未確定かつ target が domain に含まれる x[i] の数
    std::vector<bool> is_possible_;  // 各 x[i] が "possible" かどうか

    // Trail
    struct TrailEntry {
        size_t definite_count;
        size_t possible_count;
        std::vector<std::pair<size_t, bool>> is_possible_changes;  // (index, old_value)
    };
    std::vector<std::pair<int, TrailEntry>> trail_;

    /**
     * @brief trail 保存ヘルパー
     */
    void save_trail_if_needed(Model& model, int save_point);

    /**
     * @brief 不変条件に基づく伝播
     */
    bool propagate(Model& model);
};


/**
 * @brief count_eq制約（variable target）: 配列 x[] の中で y に等しい要素の数 = c
 *
 * y が変数（非定数）の場合に使用する。
 * y 未確定の間は弱い bounds 伝播のみ行い、y 確定時に full propagation を開始する。
 *
 * 変数配置: vars_[0..n-1] = x[], vars_[n] = y, vars_[n+1] = c
 */
class CountEqVarTargetConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param x_vars 配列変数
     * @param y_var ターゲット変数
     * @param count_var カウント変数
     */
    CountEqVarTargetConstraint(std::vector<VariablePtr> x_vars,
                                VariablePtr y_var,
                                VariablePtr count_var);

    std::string name() const override;


    bool prepare_propagation(Model& model) override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;

    bool on_last_uninstantiated(Model& model, int save_point,
                                 size_t last_var_internal_idx) override;

    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;

    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;

    bool on_remove_value(Model& model, int save_point,
                         size_t var_idx, size_t internal_var_idx,
                         Domain::value_type removed_value) override;

    void rewind_to(int save_point);

protected:


private:
    size_t n_;  // 配列サイズ (x[] の要素数)
    size_t y_id_;  // y 変数の ID キャッシュ
    size_t c_id_;  // count 変数の ID キャッシュ

    // target の状態
    bool target_known_;           // y が確定済みか
    Domain::value_type target_;   // y の確定値（target_known_ == true のとき有効）

    // 内部カウンタ（target_known_ == true のときのみ有効）
    size_t definite_count_;
    size_t possible_count_;
    std::vector<bool> is_possible_;

    // Trail
    struct TrailEntry {
        bool target_known;
        Domain::value_type target;
        size_t definite_count;
        size_t possible_count;
        std::vector<std::pair<size_t, bool>> is_possible_changes;
    };
    std::vector<std::pair<int, TrailEntry>> trail_;

    void save_trail_if_needed(Model& model, int save_point);

    /**
     * @brief y 確定後の full propagation
     */
    bool propagate(Model& model);

    /**
     * @brief y 確定時に counts を初期化
     */
    void initialize_counts(Model& model);
};


/**
 * @brief nvalue制約: 配列中の異なる値の数を制約する
 *
 * n = |{x[i] : i ∈ index_set(x)}|
 * 各値の support count をビットマップで追跡し、差分更新で伝播。
 */
class NValueConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param n_var 異なる値の数を表す変数
     * @param x_vars 対象変数列
     */
    NValueConstraint(VariablePtr n_var, std::vector<VariablePtr> x_vars);

    std::string name() const override;

    PresolveResult presolve(Model& model) override;
    bool prepare_propagation(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
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

    void rewind_to(int save_point) override;

    void bump_activity(const Model& model, size_t trigger_var_idx,
                       double* activity, double activity_inc,
                       bool& need_rescale, std::mt19937& rng) const override;

private:
    size_t num_x_;        ///< x 変数数
    size_t num_values_;   ///< 値域のユニオンサイズ
    size_t n_id_;         ///< n 変数の ID

    std::vector<Domain::value_type> sorted_values_;       ///< 全値（ソート済み）
    std::unordered_map<Domain::value_type, size_t> value_index_;  ///< 値→インデックス

    std::vector<int> support_count_;      ///< [value_idx] この値をドメインに含む変数数
    std::vector<bool> var_support_bitmap_; ///< [var_i * num_values_ + val_idx]
    std::vector<bool> definite_;          ///< [value_idx] 確定変数がこの値を取るか

    size_t definite_count_;   ///< 確定した異なる値の数
    size_t possible_count_;   ///< ドメインに残っている異なる値の数

    // Trail
    struct TrailEntry {
        size_t definite_count;
        size_t possible_count;
        std::vector<std::pair<size_t, int>> support_count_changes;  ///< (value_idx, old_count)
        std::vector<std::pair<size_t, bool>> definite_changes;      ///< (value_idx, old_value)
        std::vector<size_t> bitmap_changes;                         ///< flat_idx (true→false)
    };
    std::vector<std::pair<int, TrailEntry>> trail_;

    void save_trail_if_needed(Model& model, int save_point);
    bool propagate(Model& model);
};

} // namespace sabori_csp

#endif // SABORI_CSP_CONSTRAINTS_GLOBAL_COUNTING_HPP
