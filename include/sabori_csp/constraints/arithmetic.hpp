/**
 * @file arithmetic.hpp
 * @brief 算術制約クラス (int_times, int_div, int_mod, int_plus, int_abs)
 */
#ifndef SABORI_CSP_CONSTRAINTS_ARITHMETIC_HPP
#define SABORI_CSP_CONSTRAINTS_ARITHMETIC_HPP

#include "sabori_csp/constraint.hpp"
#include <unordered_map>

namespace sabori_csp {

/**
 * @brief int_times制約: x * y = z
 *
 * 乗算制約。bounds propagation と domain filtering を実行。
 *
 * 伝播ルール:
 * - x 確定時: z の値を y * x で絞り込み
 * - y 確定時: z の値を x * y で絞り込み
 * - z 確定時: x * y = z となるペアに絞り込み
 * - x = 0 または y = 0 なら z = 0
 */
class IntTimesConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param x 被乗数
     * @param y 乗数
     * @param z 積
     */
    IntTimesConstraint(VariablePtr x, VariablePtr y, VariablePtr z);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;
    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;
    bool on_final_instantiate(const Model& model) override;

    /**
     * @brief 残り1変数になった時の伝播
     *
     * 2変数確定時に残りの1変数を決定できる場合がある。
     */
    bool on_last_uninstantiated(Model& model, int save_point,
                                size_t last_var_internal_idx) override;

    /**
     * @brief 指定セーブポイントまで状態を巻き戻す（状態を持たないので空実装）
     */
    void rewind_to(int save_point);

protected:
    void check_initial_consistency() override;

private:
    VariablePtr x_;  // 被乗数
    VariablePtr y_;  // 乗数
    VariablePtr z_;  // 積
    size_t x_id_, y_id_, z_id_;

    // 変数ポインタ → 内部インデックス (0: x, 1: y, 2: z)
    std::unordered_map<Variable*, size_t> var_ptr_to_idx_;

    /**
     * @brief bounds propagation を実行
     * @return 矛盾がなければ true
     */
    bool propagate_bounds(Model& model);
};

/**
 * @brief int_abs制約: |x| = y
 *
 * 絶対値制約。
 *
 * 伝播ルール:
 * - y >= 0（絶対値は非負）
 * - x >= 0 の場合: y = x
 * - x < 0 の場合: y = -x
 * - y の bounds は x の範囲から計算
 * - x の bounds は y の範囲から制限
 */
class IntAbsConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param x 入力変数
     * @param y 絶対値変数（|x| = y）
     */
    IntAbsConstraint(VariablePtr x, VariablePtr y);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;
    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;
    bool on_final_instantiate(const Model& model) override;

protected:
    void check_initial_consistency() override;

private:
    VariablePtr x_;  // 入力変数
    VariablePtr y_;  // 絶対値変数
    size_t x_id_, y_id_;

    /**
     * @brief bounds propagation を実行
     * @return 矛盾がなければ true
     */
    bool propagate_bounds(Model& model);
};

/**
 * @brief int_mod制約: x mod y = z (truncated division)
 *
 * 剰余制約。結果の符号は x の符号に従う（C++ の % と同じ）。
 *
 * 伝播ルール:
 * - y != 0（ゼロ除算は未定義）
 * - |z| < |y| は常に成立
 * - x >= 0 なら z >= 0、x <= 0 なら z <= 0
 * - z の bounds は x の bounds でも制限
 * - x, y 確定時: z = x % y を計算
 */
class IntModConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param x 被除数
     * @param y 除数
     * @param z 剰余 (x mod y = z)
     */
    IntModConstraint(VariablePtr x, VariablePtr y, VariablePtr z);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;
    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;
    bool on_final_instantiate(const Model& model) override;

    /**
     * @brief 残り1変数になった時の伝播
     */
    bool on_last_uninstantiated(Model& model, int save_point,
                                size_t last_var_internal_idx) override;

    /**
     * @brief 指定セーブポイントまで状態を巻き戻す（状態を持たないので空実装）
     */
    void rewind_to(int save_point);

protected:
    void check_initial_consistency() override;

private:
    VariablePtr x_;  // 被除数
    VariablePtr y_;  // 除数
    VariablePtr z_;  // 剰余
    size_t x_id_, y_id_, z_id_;

    /**
     * @brief bounds propagation を実行
     * @return 矛盾がなければ true
     */
    bool propagate_bounds(Model& model);
};

} // namespace sabori_csp

#endif // SABORI_CSP_CONSTRAINTS_ARITHMETIC_HPP
