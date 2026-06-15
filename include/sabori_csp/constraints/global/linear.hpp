#ifndef SABORI_CSP_CONSTRAINTS_GLOBAL_LINEAR_HPP
#define SABORI_CSP_CONSTRAINTS_GLOBAL_LINEAR_HPP

#include "sabori_csp/constraint.hpp"
#include "sabori_csp/constraint_trail.hpp"
#include <unordered_map>
#include <numeric>
#include <vector>
#include <memory>

namespace sabori_csp {


/**
 * @brief 線形制約 int_lin_* の共通基底
 *
 * 全ての int_lin_* 系制約（eq/le/ne とその reif/imp）が共有する係数列 coeffs_ と、
 * コンストラクタでの線形項集約ロジックを集約する。伝播セマンティクス（==/<=/!=）は
 * 派生クラスに残す。
 */
class LinearConstraintBase : public Constraint {
public:
    /// 係数リストを取得
    const std::vector<int64_t>& coeffs() const { return coeffs_; }

protected:
    std::vector<int64_t> coeffs_;  // 集約後の係数列（派生共通）

    /**
     * @brief 線形項を集約する（同一変数の係数を合算し、係数0の項を除外）
     *
     * 集約後の係数を coeffs_ に格納し、対応する変数リスト（同順）を返す。
     * var_ids_ の構築は呼び出し側が行う（reif の b 追加などがあるため）。
     *
     * @param coeffs 元の係数リスト
     * @param vars   元の変数リスト（coeffs と同長）
     * @return 集約後の一意な変数リスト（coeffs_ と同順・同長）
     */
    std::vector<VariablePtr> aggregate_terms(const std::vector<int64_t>& coeffs,
                                             const std::vector<VariablePtr>& vars);

    /**
     * @brief Σ c_i x_i <= bound を満たすよう線形変数の境界を絞る（enqueue 版）
     *
     * 確定済みの寄与分 fixed_sum と未確定変数の min ポテンシャル min_rem を前提に、
     * 各未確定変数の上限/下限の緩和を enqueue する。先頭 coeffs_.size() 個を対象
     * （reif/imp では末尾の b を除外。素の int_lin_le では b が無く全変数が対象）。
     * int_lin_le / le_reif / le_imp で共通の刈り込みカーネル。
     *
     * @param bound    右辺上限
     * @param fixed_sum 確定変数の c*v の和
     * @param min_rem  未確定変数の最小ポテンシャル
     * @param skip_idx スキップする内部インデックス（SIZE_MAX で全対象）
     */
    void prune_sum_le(Model& model, int64_t bound, int64_t fixed_sum,
                      int64_t min_rem, size_t skip_idx) const;
};


/**
 * @brief int_lin_eq制約: Σ(coeffs[i] * vars[i]) == target_sum
 *
 * O(1) の差分更新で bounds consistency を維持。
 */
class IntLinEqConstraint : public LinearConstraintBase {
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


    bool prepare_propagation(Model& model) override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;

    /**
     * @brief 残り1変数になった時の伝播
     *
     * 残りの変数の値を一意に決定できる場合は確定させる。
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
     * @brief 値削除時のインクリメンタル伝播
     */
    bool on_remove_value(Model& model, int save_point,
                         size_t internal_var_idx,
                         Domain::value_type removed_value) override;

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

    void init_activity(const Model& model, double* activity) const override;

#if 1
    void bump_activity(const Model& model, size_t trigger_var_idx,
                       double* activity, double activity_inc,
                       bool& need_rescale, std::mt19937& rng) const override;
#endif

protected:
    /**
     * @brief 初期整合性チェック
     */


private:
    int64_t target_sum_;

    // 現在の状態
    int64_t current_fixed_sum_;   // 確定した変数の c*v の和
    int64_t min_rem_potential_;   // 未確定変数の最小ポテンシャル
    int64_t max_rem_potential_;   // 未確定変数の最大ポテンシャル

    // 未確定変数カウント（差分更新用）
    size_t unfixed_count_;

    // Trail: (save_point, (fixed_sum, min_pot, max_pot))
    struct TrailEntry {
        int64_t fixed_sum;
        int64_t min_pot;
        int64_t max_pot;
    };
    ConstraintTrail<TrailEntry> trail_;

    /**
     * @brief trail 保存ヘルパー
     */
    void save_trail_if_needed(Model& model, int save_point);

    /**
     * @brief total_max 減少時の伝播（c>0: set_min, c<0: set_max）
     * @param skip_idx トリガー変数の内部インデックス（スキップ）
     */
    bool propagate_lower_bounds(Model& model, size_t skip_idx);

    /**
     * @brief total_min 増加時の伝播（c>0: set_max, c<0: set_min）
     * @param skip_idx トリガー変数の内部インデックス（スキップ）
     */
    bool propagate_upper_bounds(Model& model, size_t skip_idx);
};


/**
 * @brief int_lin_le制約: Σ(coeffs[i] * vars[i]) <= bound
 */
class IntLinLeConstraint : public LinearConstraintBase {
public:
    IntLinLeConstraint(std::vector<int64_t> coeffs,
                       std::vector<VariablePtr> vars,
                       int64_t bound);

    std::string name() const override;

    const std::vector<int64_t>& coeffs() const { return coeffs_; }
    int64_t bound() const { return bound_; }

    bool prepare_propagation(Model& model) override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;

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
     * @brief 値削除時のインクリメンタル伝播
     */
    bool on_remove_value(Model& model, int save_point,
                         size_t internal_var_idx,
                         Domain::value_type removed_value) override;

    void rewind_to(int save_point);

    void init_activity(const Model& model, double* activity) const override;

#if 1
    void bump_activity(const Model& model, size_t trigger_var_idx,
                       double* activity, double activity_inc,
                       bool& need_rescale, std::mt19937& rng) const override;
#endif

protected:
    /**
     * @brief 各変数の境界を絞り込む
     * @param model モデル
     * @param skip_idx この変数はスキップ（SIZE_MAX なら全変数対象）
     * @return false なら矛盾
     */
    bool propagate_bounds(Model& model, size_t skip_idx);

private:
    int64_t bound_;
    int64_t current_fixed_sum_;
    int64_t min_rem_potential_;

    struct TrailEntry {
        int64_t fixed_sum;
        int64_t min_pot;
    };
    ConstraintTrail<TrailEntry> trail_;

    bool is_easy_;

    /**
     * @brief trail 保存ヘルパー
     */
    void save_trail_if_needed(Model& model, int save_point);
};


/**
 * @brief int_lin_ne制約: Σ(coeffs[i] * vars[i]) != target
 *
 * 線形不等式制約。全変数が確定したときに和が target と等しくないことを確認。
 * 残り1変数の場合、禁止値を除外する。
 */
class IntLinNeConstraint : public LinearConstraintBase {
public:
    /**
     * @brief コンストラクタ
     * @param coeffs 係数リスト
     * @param vars 変数リスト
     * @param target 禁止値
     */
    IntLinNeConstraint(std::vector<int64_t> coeffs,
                       std::vector<VariablePtr> vars,
                       int64_t target);

    std::string name() const override;


    bool prepare_propagation(Model& model) override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;

    /**
     * @brief 残り1変数になった時の伝播
     *
     * 残りの変数が禁止値を取る場合、その値を除外する。
     */
    bool on_last_uninstantiated(Model& model, int save_point,
                                 size_t last_var_internal_idx) override;

    /**
     * @brief 指定セーブポイントまで状態を巻き戻す
     */
    void rewind_to(int save_point);

    /**
     * @brief 目標値を取得
     */
    int64_t target() const { return target_; }

    /**
     * @brief 係数リストを取得
     */
    const std::vector<int64_t>& coeffs() const { return coeffs_; }

    void init_activity(const Model& model, double* activity) const override;

protected:


private:
    int64_t target_;

    // 現在の確定変数の和
    int64_t current_fixed_sum_;

    // 未確定変数カウント
    size_t unfixed_count_;

    // Trail: (save_point, (fixed_sum, unfixed_count))
    struct TrailEntry {
        int64_t fixed_sum;
        size_t unfixed_count;
    };
    ConstraintTrail<TrailEntry> trail_;

};


/**
 * @brief int_lin_eq_reif / int_lin_ne_reif の共通基底
 *
 * (Σ(coeffs[i] * vars[i]) == target) <-> b （eq）と、その否定（ne, sum != target）は、
 * 内部状態（fixed_sum / min,max ポテンシャル / unfixed_count / trail）と差分更新が
 * 完全に一致し、述語 P=(sum==target) の極性のみが異なるため、negated_ で出し分ける。
 *
 * eq/ne とも線形変数の bounds は刈り込まず、bounds から b を推論し、b 確定時は矛盾検出
 * のみ行う（既存仕様を踏襲。線形側の刈り込みは le_reif/le_imp のみ）。
 */
class IntLinEqNeReifBase : public LinearConstraintBase {
public:
    const std::vector<int64_t>& coeffs() const { return coeffs_; }
    int64_t target() const { return target_; }
    size_t b_id() const { return b_id_; }

    bool prepare_propagation(Model& model) override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;

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

    void rewind_to(int save_point) override;

    void init_activity(const Model& model, double* activity) const override;

protected:
    /**
     * @brief コンストラクタ
     * @param negated false: (sum==target)<->b / true: (sum!=target)<->b
     */
    IntLinEqNeReifBase(std::vector<int64_t> coeffs,
                       std::vector<VariablePtr> vars,
                       int64_t target,
                       VariablePtr b,
                       bool negated);

private:
    bool negated_;
    int64_t target_;
    int64_t current_fixed_sum_;
    int64_t min_rem_potential_;
    int64_t max_rem_potential_;
    size_t unfixed_count_;

    struct TrailEntry {
        int64_t fixed_sum;
        int64_t min_pot;
        int64_t max_pot;
        size_t unfixed_count;
    };
    ConstraintTrail<TrailEntry> trail_;
    size_t b_id_;

    /**
     * @brief trail 保存ヘルパー
     */
    void save_trail_if_needed(Model& model, int save_point);

    /**
     * @brief ポテンシャル更新後に b<->述語 を整合させる
     *
     * b 未確定なら bounds から b を推論（enqueue）、b 確定なら矛盾を検出する。
     * @return false なら矛盾
     */
    bool reconcile_b(Model& model, int64_t min_sum, int64_t max_sum);
};


/**
 * @brief int_lin_eq_reif制約: (Σ(coeffs[i] * vars[i]) == target) <-> b
 */
class IntLinEqReifConstraint : public IntLinEqNeReifBase {
public:
    IntLinEqReifConstraint(std::vector<int64_t> coeffs,
                           std::vector<VariablePtr> vars,
                           int64_t target,
                           VariablePtr b)
        : IntLinEqNeReifBase(std::move(coeffs), std::move(vars), target, b,
                             /*negated=*/false) {}

    std::string name() const override { return "int_lin_eq_reif"; }
};


/**
 * @brief int_lin_ne_reif制約: (Σ(coeffs[i] * vars[i]) != target) <-> b
 */
class IntLinNeReifConstraint : public IntLinEqNeReifBase {
public:
    IntLinNeReifConstraint(std::vector<int64_t> coeffs,
                           std::vector<VariablePtr> vars,
                           int64_t target,
                           VariablePtr b)
        : IntLinEqNeReifBase(std::move(coeffs), std::move(vars), target, b,
                             /*negated=*/true) {}

    std::string name() const override { return "int_lin_ne_reif"; }
};


/**
 * @brief int_lin_le_reif制約: (Σ(coeffs[i] * vars[i]) <= bound) <-> b
 *
 * 線形不等式の reified 版（双方向）。
 * b が 1 の場合、線形不等式を強制。
 * b が 0 の場合、線形不等式の否定（sum > bound）を強制。
 * 線形変数の bounds から b を推論可能。
 */
class IntLinLeReifConstraint : public LinearConstraintBase {
public:
    /**
     * @brief コンストラクタ
     * @param coeffs 係数リスト
     * @param vars 変数リスト
     * @param bound 上限
     * @param b reified 変数
     */
    IntLinLeReifConstraint(std::vector<int64_t> coeffs,
                           std::vector<VariablePtr> vars,
                           int64_t bound,
                           VariablePtr b);

    std::string name() const override;

    const std::vector<int64_t>& coeffs() const { return coeffs_; }
    int64_t bound() const { return bound_; }
    size_t b_id() const { return b_id_; }

    bool prepare_propagation(Model& model) override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;

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
     * @brief 値削除時のインクリメンタル伝播
     */
    bool on_remove_value(Model& model, int save_point,
                         size_t internal_var_idx,
                         Domain::value_type removed_value) override;

    void rewind_to(int save_point);

    void init_activity(const Model& model, double* activity) const override;

protected:
    /**
     * @brief b=1 時の bounds propagation（sum <= bound）
     */
    bool propagate_bounds_le(Model& model, size_t skip_idx);

    /**
     * @brief b=0 時の bounds propagation（sum > bound）
     */
    bool propagate_bounds_gt(Model& model, size_t skip_idx);

private:
    int64_t bound_;
    int64_t current_fixed_sum_;
    int64_t min_rem_potential_;
    int64_t max_rem_potential_;
    size_t unfixed_count_;

    struct TrailEntry {
        int64_t fixed_sum;
        int64_t min_pot;
        int64_t max_pot;
        size_t unfixed_count;
    };
    ConstraintTrail<TrailEntry> trail_;
    size_t b_id_;

    /**
     * @brief trail 保存ヘルパー
     */
    void save_trail_if_needed(Model& model, int save_point);
};


/**
 * @brief int_lin_le_imp制約: b = 1 -> Σ(coeffs[i] * vars[i]) <= bound
 *
 * 半具象化（含意）版の線形不等式制約。
 * b が 1 の場合のみ、線形不等式を強制する。
 * b が 0 の場合は制約は無条件で充足される。
 *
 * _reif との違い:
 * - _reif: (P) <-> b （双方向）
 * - _imp: b -> P （単方向、b から P への含意のみ）
 */
class IntLinLeImpConstraint : public LinearConstraintBase {
public:
    /**
     * @brief コンストラクタ
     * @param coeffs 係数リスト
     * @param vars 変数リスト
     * @param bound 上限
     * @param b 含意変数（b = 1 のとき制約を強制）
     */
    IntLinLeImpConstraint(std::vector<int64_t> coeffs,
                          std::vector<VariablePtr> vars,
                          int64_t bound,
                          VariablePtr b);

    std::string name() const override;

    const std::vector<int64_t>& coeffs() const { return coeffs_; }
    int64_t bound() const { return bound_; }
    size_t b_id() const { return b_id_; }


    bool prepare_propagation(Model& model) override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;

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
     * @brief 値削除時のインクリメンタル伝播
     */
    bool on_remove_value(Model& model, int save_point,
                         size_t internal_var_idx,
                         Domain::value_type removed_value) override;

    /**
     * @brief 指定セーブポイントまで状態を巻き戻す
     */
    void rewind_to(int save_point) override;

    void init_activity(const Model& model, double* activity) const override;

protected:


private:
    int64_t bound_;
    int64_t current_fixed_sum_;
    int64_t min_rem_potential_;

    struct TrailEntry {
        int64_t fixed_sum;
        int64_t min_pot;
    };
    ConstraintTrail<TrailEntry> trail_;
    size_t b_id_;

    /**
     * @brief trail 保存ヘルパー
     */
    void save_trail_if_needed(Model& model, int save_point);

    /**
     * @brief b=1 時の bounds propagation（int_lin_le と同じロジック）
     */
    bool propagate_bounds(Model& model, size_t skip_idx);

    /**
     * @brief 対偶推論: min_sum > bound → b=0
     */
    bool check_contrapositive(Model& model);
};

} // namespace sabori_csp

#endif // SABORI_CSP_CONSTRAINTS_GLOBAL_LINEAR_HPP
