#ifndef SABORI_CSP_CONSTRAINTS_GLOBAL_MISC_HPP
#define SABORI_CSP_CONSTRAINTS_GLOBAL_MISC_HPP

#include "sabori_csp/constraint.hpp"
#include <unordered_map>
#include <numeric>
#include <vector>
#include <memory>

namespace sabori_csp {


/**
 * @brief one-hot チャネリング集約制約: bools[i] <-> (x == values[i])
 *
 * 同一の整数変数 x に紐付く複数の int_eq_reif(x, v_i, b_i) を 1 つの制約に
 * 集約する。values[] は実行時定数（重複なし）。
 *
 * - values[] が x の presolve ドメインを完全被覆する場合は exactly-one
 *   （bools のうち厳密に 1 個だけ true）として伝播
 * - 部分被覆の場合は at-most-one ベース（true は高々 1 個）
 *
 * 集約は presolve 後の core 側で OneHotChannelAggregator が自動的に行う。
 * このクラスを直接 add_constraint する直接利用も可。
 */
class IntOneHotChannelConstraint : public Constraint {
public:
    IntOneHotChannelConstraint(VariablePtr x,
                               std::vector<Domain::value_type> values,
                               std::vector<VariablePtr> bools);

    std::string name() const override;
    std::optional<bool> is_satisfied(const Model& model) const override;

    PresolveResult presolve(Model& model) override;
    bool prepare_propagation(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min,
                        Domain::value_type prev_max) override;
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
    bool on_final_instantiate(const Model& model) override;

    void bump_activity(const Model& model, size_t trigger_var_idx,
                       double* activity, double activity_inc,
                       bool& need_rescale, std::mt19937& rng) const override;

    void init_activity(const Model& model, double* activity) const override;

    void rewind_to(int save_point) override;

    // テスト・診断用アクセサ
    size_t x_id() const { return x_id_; }
    const std::vector<Domain::value_type>& values() const { return values_; }
    const std::vector<size_t>& b_ids() const { return b_ids_; }
    /// x の初期ドメインに含まれるが values_ にない値の個数。
    /// 0 のとき exhaustive（exactly-one として伝播可能）、
    /// >0 のときは partial coverage（at-most-one として伝播）。
    size_t holes() const { return holes_; }

private:
    /// values 内で v に対応する index を返す。なければ -1。
    /// values_ はコンストラクタ内で昇順ソート済み。連続値の場合は
    /// `v - offset_` で O(1)、非連続なら std::lower_bound で O(log N)。
    int find_value_index(Domain::value_type v) const;

    /// b_id_ が var_id である場合に、対応する values index を返す。なければ -1。
    int find_b_index(size_t var_id) const;

    size_t x_id_;
    std::vector<Domain::value_type> values_;  ///< 昇順ソート、重複なし
    std::vector<size_t> b_ids_;               ///< values_ と添え字対応
    Domain::value_type offset_;  ///< values_.front() (空なら 0)
    bool contiguous_;            ///< values_ が連続整数（v[i+1] == v[i]+1）か
    /// x の初期ドメインのうち values_ にない値の個数（"穴"）。
    /// holes_ == 0 ⇔ exhaustive（exactly-one として動く）。
    /// holes_ > 0 のときは partial coverage で、x が values_ 外を取りうる
    /// 分だけ伝播力が弱まる（at-most-one ベース）。
    size_t holes_;

    /// 現在未確定の b_ids_ 数（bump_activity の O(1) 参照用）。
    /// on_instantiate で減算、rewind_to で復元。
    size_t uninstantiated_b_count_ = 0;
    /// (save_point, 旧カウンタ値) の trail。
    std::vector<std::pair<int, size_t>> trail_;
};


/**
 * @brief increasing / strictly_increasing 制約
 *
 * `strict=false`: x[0] <= x[1] <= ... <= x[n-1]
 * `strict=true` : x[0] <  x[1] <  ... <  x[n-1]
 *
 * 状態を持たない bounds propagator。プレソルブとイベントハンドラで
 * 隣接する2変数間の制約のみを伝播し、`process_queue` 経由で連鎖伝播させる。
 *
 * init_activity で初期 activity を与え、緩やかにガイドする
 * （dynamic activity を上書きしないごく小さな数）。
 */
class IncreasingConstraint : public Constraint {
public:
    IncreasingConstraint(std::vector<VariablePtr> vars, bool strict);

    std::string name() const override;

    bool prepare_propagation(Model& model) override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;

    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;

    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;

    void init_activity(const Model& model, double* activity) const override;

private:
    /// 全変数 bounds の前方/後方スイープ。prepare_propagation で1回呼ぶ。
    bool sweep(Model& model);

    size_t n_;
    bool strict_;
};

} // namespace sabori_csp

#endif // SABORI_CSP_CONSTRAINTS_GLOBAL_MISC_HPP
