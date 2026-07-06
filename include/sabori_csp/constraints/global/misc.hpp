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
 * エントリごとに half-reified (imp) フラグを持てる。imp エントリの意味論は
 * bools[i] -> (x == values[i]) の片方向のみで、許される推論は
 *   b=1 ⇒ x:=v / v ∉ dom(x) ⇒ b:=0（対偶）/ at-most-one
 * に限られる。x==v ⇒ b:=1 と b=0 ⇒ remove v は使えず、exactly-one 推論
 * （残り1個のbを1に）は imp エントリが 1 つでもあると無効になる。
 *
 * 集約は presolve 後の core 側で OneHotChannelAggregator が自動的に行う
 * （int_eq_reif / int_eq_imp の両方を消費する）。
 * このクラスを直接 add_constraint する直接利用も可。
 */
class IntOneHotChannelConstraint : public Constraint {
public:
    IntOneHotChannelConstraint(VariablePtr x,
                               std::vector<Domain::value_type> values,
                               std::vector<VariablePtr> bools);

    /**
     * @brief imp フラグ付きコンストラクタ
     * @param imp imp[i]!=0 なら bools[i] -> (x==values[i]) の片方向
     *            （空なら全エントリ reif）
     */
    IntOneHotChannelConstraint(VariablePtr x,
                               std::vector<Domain::value_type> values,
                               std::vector<VariablePtr> bools,
                               std::vector<uint8_t> imp);

    std::string name() const override;
    std::optional<bool> is_satisfied(const Model& model) const override;

    PresolveResult presolve(Model& model) override;
    bool prepare_propagation(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min,
                        Domain::value_type prev_max) override;
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
    bool on_final_instantiate(const Model& model) override;

    void bump_activity(const Model& model, size_t trigger_var_idx,
                       double* activity, double activity_inc,
                       bool& need_rescale, std::mt19937& rng) const override;

    void init_activity(const Model& model, double* activity) const override;

    // テスト・診断用アクセサ
    size_t x_id() const { return x_id_; }
    const std::vector<Domain::value_type>& values() const { return values_; }
    const std::vector<size_t>& b_ids() const { return b_ids_; }
    const std::vector<uint8_t>& imp_flags() const { return imp_; }
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
    std::vector<uint8_t> imp_;                ///< values_ と添え字対応。!=0 で half-reified
    bool all_reif_;                           ///< imp エントリなし（exactly-one 推論の前提）
    Domain::value_type offset_;  ///< values_.front() (空なら 0)
    bool contiguous_;            ///< values_ が連続整数（v[i+1] == v[i]+1）か
    /// x の初期ドメインのうち values_ にない値の個数（"穴"）。
    /// holes_ == 0 ⇔ exhaustive（exactly-one として動く）。
    /// holes_ > 0 のときは partial coverage で、x が values_ 外を取りうる
    /// 分だけ伝播力が弱まる（at-most-one ベース）。
    size_t holes_;
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

    void init_activity(const Model& model, double* activity) const override;

private:
    /// 全変数 bounds の前方/後方スイープ。prepare_propagation で1回呼ぶ。
    bool sweep(Model& model);

    size_t n_;
    bool strict_;
};


/**
 * @brief value_precede 制約: x[j]==t なら ∃i<j x[i]==s
 *
 * 値 t の出現より前に値 s が出現することを要求する（対称性破壊の定番）。
 * fzn_value_precede_int / fzn_value_precede_chain_int / fzn_seq_precede_chain_int
 * を mznlib でこの propagator（chain は連続ペアの連言）に経路付けする。
 * std 分解（H[i]=max(X[i],H[i-1]) チェーン + マッピング変数）の
 * int_max/element/reif 網を回避する。
 *
 * 伝播 (Law & Lee 2004 の α/β/γ 規則の全再計算版):
 * - α = s を取り得る最小 index。i ≤ α の全変数から t を除去
 *   （s が i より前に来られないため。α が無ければ全域から t を除去）
 * - γ = t に確定した最小 index。γ が存在するとき:
 *   α ≥ γ なら矛盾。γ より前の s サポートが α のみなら x[α] := s
 * - s == t の縮退: s は一切出現できない（全域から除去）
 *
 * ステートレスな全再計算 batch propagator（bin_packing_load と同方式）。
 */
class ValuePrecedeConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param s  先行しなければならない値（定数）
     * @param t  後続の値（定数）
     * @param xs 対象変数列
     */
    ValuePrecedeConstraint(Domain::value_type s, Domain::value_type t,
                           std::vector<VariablePtr> xs);

    std::string name() const override;

    PresolveResult presolve(Model& model) override;
    bool prepare_propagation(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;

    bool on_set_min(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_min, Domain::value_type old_min) override;
    bool on_set_max(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_max, Domain::value_type old_max) override;
    bool on_remove_value(Model& model, int save_point,
                         size_t internal_var_idx,
                         Domain::value_type removed_value) override;

    bool propagate_batch(Model& model, int save_point) override;

    void rewind_to(int save_point) override;  // ステートレス（no-op）

private:
    Domain::value_type s_;
    Domain::value_type t_;
    size_t n_;

    /**
     * @brief 全再計算伝播の本体
     * @param direct true=presolve（直接ドメイン操作）、false=search（enqueue）
     * @param changed 何か変更したら true
     * @return 矛盾なら false
     */
    bool propagate_impl(Model& model, bool direct, bool& changed);
};

} // namespace sabori_csp

#endif // SABORI_CSP_CONSTRAINTS_GLOBAL_MISC_HPP
