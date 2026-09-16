/**
 * @file arithmetic.hpp
 * @brief 算術制約クラス (int_times, int_div, int_mod, int_plus, int_abs)
 */
#ifndef SABORI_CSP_CONSTRAINTS_ARITHMETIC_HPP
#define SABORI_CSP_CONSTRAINTS_ARITHMETIC_HPP

#include "sabori_csp/constraint.hpp"

namespace sabori_csp {

/**
 * @brief int_times制約: x * y = z
 */
class IntTimesConstraint : public Constraint {
public:
    IntTimesConstraint(VariablePtr x, VariablePtr y, VariablePtr z);

    std::string name() const override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_set_min(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;
    bool on_set_max(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;
    bool on_final_instantiate(const Model& model) override;

    bool on_last_uninstantiated(Model& model, int save_point,
                                size_t last_var_internal_idx) override;

private:
    size_t x_id_, y_id_, z_id_;

    bool propagate_bounds(Model& model);

    /**
     * @brief スパースオペランド対応の domain 伝播
     *
     * x または y のドメインが小さい（≤ kSparseLimit）とき、bounds 一貫では
     * 取りこぼす枝刈りを行う。mcm のように「2の冪だけのスパースな乗数
     * （区間は広いが実値は少数）× 広いドメイン」で bounds が無力になる問題に効く。
     * - z 確定時: スパースオペランドを約数集合に、相手を対応値集合に絞る
     * - z 未確定時: スパースオペランドの各値の積区間が z 区間に届かなければ除去
     */
    bool propagate_sparse(Model& model);

    /// z=vz 確定時、スパースオペランド s を約数に絞り相手 o を対応値集合に絞る
    bool divisor_filter(Model& model, size_t s_id, size_t o_id, Domain::value_type vz);

    /// スパースオペランド s の各値について、s*[o区間] が z 区間に届かない値を除去
    bool feasibility_filter(Model& model, size_t s_id, size_t o_id);
};

/**
 * @brief int_abs制約: |x| = y
 */
class IntAbsConstraint : public Constraint {
public:
    IntAbsConstraint(VariablePtr x, VariablePtr y);

    std::string name() const override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_set_min(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;
    bool on_set_max(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;
    bool on_final_instantiate(const Model& model) override;

private:
    size_t x_id_, y_id_;

    bool propagate_bounds(Model& model);
};

/**
 * @brief int_div制約: x div y = z (truncated division)
 */
class IntDivConstraint : public Constraint {
public:
    IntDivConstraint(VariablePtr x, VariablePtr y, VariablePtr z);

    std::string name() const override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_set_min(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;
    bool on_set_max(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;
    std::optional<bool> is_satisfied(const Model& model) const override;
    bool on_final_instantiate(const Model& model) override;

    bool on_last_uninstantiated(Model& model, int save_point,
                                size_t last_var_internal_idx) override;

    /// 被除数 x の変数 ID
    size_t x_id() const { return x_id_; }
    /// 除数 y の変数 ID
    size_t y_id() const { return y_id_; }
    /// 商 z の変数 ID
    size_t z_id() const { return z_id_; }

private:
    size_t x_id_, y_id_, z_id_;

    bool propagate_bounds(Model& model);

    /// z_val と y_val から x の有効範囲 [x_lo, x_hi] を計算
    static std::pair<Domain::value_type, Domain::value_type>
    compute_x_range(Domain::value_type z_val, Domain::value_type y_val);
};

/**
 * @brief int_mod制約: x mod y = z (truncated division)
 */
class IntModConstraint : public Constraint {
public:
    IntModConstraint(VariablePtr x, VariablePtr y, VariablePtr z);

    std::string name() const override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_set_min(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;
    bool on_set_max(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;
    std::optional<bool> is_satisfied(const Model& model) const override;
    bool on_final_instantiate(const Model& model) override;

    bool on_last_uninstantiated(Model& model, int save_point,
                                size_t last_var_internal_idx) override;

    /// 被除数 x の変数 ID
    size_t x_id() const { return x_id_; }
    /// 除数 y の変数 ID
    size_t y_id() const { return y_id_; }
    /// 余り z の変数 ID
    size_t z_id() const { return z_id_; }

private:
    size_t x_id_, y_id_, z_id_;

    bool propagate_bounds(Model& model);
};

/**
 * @brief ユークリッド除算チャネル: x = c*q + r かつ 0 <= r < c（c は正の定数）
 *
 * int_div(x, c, q) と int_mod(x, c, r) のペアを 1 本にまとめた制約。
 * x >= 0 / c > 0 の下では (q, r) と x は全単射なので、両方向にドメイン一貫な
 * フィルタが安価に書ける。分解形（IntDiv + IntMod + 線形）に対する利点は
 *   - 伝播器が 3 本 → 1 本（呼び出し回数と activity 配分が変わる）
 *   - IntMod が持たない「余りの範囲から被除数のドメインを削る」向きが入る
 *
 * @note x の下限が負のモデルには使えない（truncated division では r < 0 が
 *       ありうるため）。生成側（DivModChannelAggregator）でガードすること。
 */
class IntDivModChannelConstraint : public Constraint {
public:
    IntDivModChannelConstraint(VariablePtr x, Domain::value_type divisor,
                               VariablePtr q, VariablePtr r);

    std::string name() const override;
    PresolveResult presolve(Model& model) override;
    bool prepare_propagation(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_set_min(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;
    bool on_set_max(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;
    bool on_remove_value(Model& model, int save_point,
                         size_t internal_var_idx,
                         Domain::value_type removed_value) override;
    std::optional<bool> is_satisfied(const Model& model) const override;
    bool on_final_instantiate(const Model& model) override;

    /// 被除数 x の変数 ID
    size_t x_id() const { return x_id_; }
    /// 商 q の変数 ID
    size_t q_id() const { return q_id_; }
    /// 余り r の変数 ID
    size_t r_id() const { return r_id_; }
    /// 定数除数
    Domain::value_type divisor() const { return c_; }

    /// 境界変更イベントでも値走査フィルタを回すか（既定 false = 穴が生じる
    /// イベントのみ）。true にすると枝刈りは強くなるが 1 ノードのコストが上がる。
    void set_sweep_on_bounds(bool v) { sweep_on_bounds_ = v; }

private:
    size_t x_id_, q_id_, r_id_;
    Domain::value_type c_;
    bool sweep_on_bounds_ = false;

    /// 値走査によるドメイン一貫フィルタを行う上限（超えたら bounds のみ）
    static constexpr size_t kScanLimit = 4096;

    /// bounds 伝播（両方向）。矛盾を検出したら false。
    bool propagate_bounds(Model& model);
    /// D(x) を走査して q/r の支持を取り、支持のない値を落とす。
    bool propagate_domain(Model& model);
    /// bounds + domain をまとめて呼ぶ
    bool propagate(Model& model);
};

} // namespace sabori_csp

#endif // SABORI_CSP_CONSTRAINTS_ARITHMETIC_HPP
