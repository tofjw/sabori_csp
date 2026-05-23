/**
 * @file variable_selector.hpp
 * @brief 変数選択クラス（MRV + Activity + NoGood Bloom + Community）
 */
#ifndef SABORI_CSP_VARIABLE_SELECTOR_HPP
#define SABORI_CSP_VARIABLE_SELECTOR_HPP

#include "sabori_csp/model.hpp"
#include "sabori_csp/community_analysis.hpp"
#include <vector>
#include <random>

namespace sabori_csp {

/**
 * @brief 変数選択を管理するクラス
 *
 * var_order_ のパーティション管理と変数選択ヒューリスティックを担当。
 * decision vars / defined vars を未割当 / 割当済に分割管理し、
 * MRV + Activity + NoGood Bloom overlap でスコアリングする。
 */
class VariableSelector {
public:
    VariableSelector() = default;

    // ===== 初期化 =====

    /**
     * @brief var_order_ を構築（decision vars | defined vars にパーティション）
     *
     * solve() の冒頭で呼ぶ。変数リストを decision / defined に分割し、
     * 各区間を独立にシャッフルする。
     */
    void build_order(const Model& model, std::mt19937& rng);

    /**
     * @brief var_position_ を再構築し、割当済み変数を後方へ移す
     *
     * リスタート後や init 後に呼ぶ。
     */
    void init_tracking(const Model& model);

    // ===== 変数選択 =====

    /**
     * @brief 次に割り当てる変数を選択
     *
     * @param model モデル
     * @param activity Activity スコア配列
     * @param ng_usage_bloom 現在の探索パスの NoGood 利用 Bloom
     * @param activity_first true: Activity → MRV, false: MRV → Activity
     * @param rng 乱数生成器
     * @return 選択された変数インデックス、全割当済みなら SIZE_MAX
     */
    size_t select(const Model& model,
                  const std::vector<double>& activity,
                  const std::vector<int>& temporal_activity,
                  const Bloom512& ng_usage_bloom,
                  bool activity_first,
                  std::mt19937& rng,
                  const CommunityAnalysis* community_analysis = nullptr);

    /**
     * @brief リスタート後に起点変数を選択（探索多様化）
     *
     * 未割当の決定変数のうち、ドメインサイズが最小 (MRV) なものから
     * activity 重み付きでランダムに 1 つ選び `community_first_var_` に格納する。
     */
    void select_restart_pivot(const Model& model,
                               const std::vector<double>& activity,
                               std::mt19937& rng);

    // ===== パーティション管理 =====

    /**
     * @brief 変数を割当済みセクションへ移動
     */
    void mark_assigned(size_t var_idx);

    /**
     * @brief スキャン順をシャッフル（各区間を独立に）
     */
    void shuffle(std::mt19937& rng);

    // ===== 状態アクセサ =====

    size_t decision_unassigned_end() const { return decision_unassigned_end_; }
    size_t defined_unassigned_end() const { return defined_unassigned_end_; }
    size_t unconstrained_unassigned_end() const { return unconstrained_unassigned_end_; }

    /**
     * @brief バックトラック時にパーティション境界を復元
     */
    void restore_decision_end(size_t new_end);
    void restore_defined_end(size_t new_end);
    void restore_unconstrained_end(size_t new_end);

    /**
     * @brief 未割当変数があるかどうか（非破壊的）
     */
    bool all_assigned() const {
        return decision_unassigned_end_ == 0 &&
               defined_unassigned_end_ <= decision_var_end_ &&
               unconstrained_unassigned_end_ <= defined_var_end_;
    }

    size_t decision_var_end() const { return decision_var_end_; }
    size_t defined_var_end() const { return defined_var_end_; }
    const std::vector<size_t>& var_order() const { return var_order_; }

    size_t community_first_var() const { return community_first_var_; }
    void set_community_first_var(size_t v) { community_first_var_ = v; }

private:
    // 変数スキャン順序（decision vars | defined vars | unconstrained vars）
    // unconstrained vars はどの制約にも参照されない自由変数。探索に影響しないため
    // 必ず最後に回す（FlatZinc の output_array で生成されるダミー変数等）。
    std::vector<size_t> var_order_;
    size_t decision_var_end_ = 0;
    size_t defined_var_end_ = 0;             // = decision_var_end_ + #defined_vars

    // 未割当/割当済パーティション
    std::vector<size_t> var_position_;       // var_idx → var_order_ 内の位置
    size_t decision_unassigned_end_ = 0;     // [0, decision_unassigned_end_): 未割当 decision vars
    size_t defined_unassigned_end_ = 0;      // [decision_var_end_, defined_unassigned_end_): 未割当 defined vars
    size_t unconstrained_unassigned_end_ = 0;// [defined_var_end_, unconstrained_unassigned_end_): 未割当 unconstrained vars

    // 線形スキャンによる変数選択
    size_t select_linear(const Model& model,
                         const std::vector<double>& activity,
                         const std::vector<int>& temporal_activity,
                         const Bloom512& ng_usage_bloom,
                         bool activity_first,
                         std::mt19937& rng,
                         size_t begin, size_t end,
                         const CommunityAnalysis* community_analysis,
                         size_t target_community);

    // コミュニティローテーション
    size_t community_first_var_ = SIZE_MAX;
};

} // namespace sabori_csp

#endif // SABORI_CSP_VARIABLE_SELECTOR_HPP
