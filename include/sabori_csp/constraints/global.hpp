/**
 * @file global.hpp
 * @brief グローバル制約クラス (all_different, int_lin_eq)
 */
#ifndef SABORI_CSP_CONSTRAINTS_GLOBAL_HPP
#define SABORI_CSP_CONSTRAINTS_GLOBAL_HPP

#include "sabori_csp/constraint.hpp"
#include <unordered_map>
#include <numeric>

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
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool propagate() override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

    /**
     * @brief 指定セーブポイントまで状態を巻き戻す
     */
    void rewind_to(int save_point);

    /**
     * @brief 現在のプールサイズを取得
     */
    size_t pool_size() const { return pool_n_; }

private:
    // 値プール（Sparse Set）
    std::vector<Domain::value_type> pool_values_;  // Dense 配列
    std::unordered_map<Domain::value_type, size_t> pool_sparse_;  // 値→インデックス
    size_t pool_n_;  // 有効な値の数

    // Trail: (save_point, old_pool_n)
    std::vector<std::pair<int, size_t>> pool_trail_;

    // 変数 ID から内部インデックスへのマップ
    std::unordered_map<size_t, size_t> var_id_to_idx_;

    /**
     * @brief プールから値を削除
     */
    bool remove_from_pool(int save_point, Domain::value_type value);
};

/**
 * @brief int_lin_eq制約: Σ(coeffs[i] * vars[i]) == target_sum
 *
 * O(1) の差分更新で bounds consistency を維持。
 */
class IntLinEqConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param coeffs 係数リスト
     * @param vars 変数リスト
     * @param target_sum 目標値
     */
    IntLinEqConstraint(std::vector<int64_t> coeffs,
                       std::vector<VariablePtr> vars,
                       int64_t target_sum);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool propagate() override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

    /**
     * @brief 指定セーブポイントまで状態を巻き戻す
     */
    void rewind_to(int save_point);

    /**
     * @brief 目標値を取得
     */
    int64_t target_sum() const { return target_sum_; }

    /**
     * @brief 係数リストを取得
     */
    const std::vector<int64_t>& coeffs() const { return coeffs_; }

private:
    std::vector<int64_t> coeffs_;
    int64_t target_sum_;

    // 現在の状態
    int64_t current_fixed_sum_;   // 確定した変数の c*v の和
    int64_t min_rem_potential_;   // 未確定変数の最小ポテンシャル
    int64_t max_rem_potential_;   // 未確定変数の最大ポテンシャル

    // Trail: (save_point, (fixed_sum, min_pot, max_pot))
    struct TrailEntry {
        int64_t fixed_sum;
        int64_t min_pot;
        int64_t max_pot;
    };
    std::vector<std::pair<int, TrailEntry>> trail_;

    // 変数 ID から内部インデックスへのマップ
    std::unordered_map<size_t, size_t> var_id_to_idx_;

    /**
     * @brief 値を割り当て可能か O(1) で判定 (Look-ahead)
     */
    bool can_assign(size_t internal_idx, Domain::value_type value,
                    Domain::value_type prev_min, Domain::value_type prev_max) const;
};

/**
 * @brief int_lin_le制約: Σ(coeffs[i] * vars[i]) <= bound
 */
class IntLinLeConstraint : public Constraint {
public:
    IntLinLeConstraint(std::vector<int64_t> coeffs,
                       std::vector<VariablePtr> vars,
                       int64_t bound);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool propagate() override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

    void rewind_to(int save_point);

private:
    std::vector<int64_t> coeffs_;
    int64_t bound_;
    int64_t current_fixed_sum_;
    int64_t min_rem_potential_;

    struct TrailEntry {
        int64_t fixed_sum;
        int64_t min_pot;
    };
    std::vector<std::pair<int, TrailEntry>> trail_;
    std::unordered_map<size_t, size_t> var_id_to_idx_;
};

} // namespace sabori_csp

#endif // SABORI_CSP_CONSTRAINTS_GLOBAL_HPP
