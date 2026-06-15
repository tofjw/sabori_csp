/**
 * @file all_different_gac.hpp
 * @brief all_different GAC 制約 (Régin's algorithm: 二部マッチング + SCC フィルタリング)
 *
 * AllDifferentConstraint を継承し、GAC (Generalized Arc Consistency) を追加。
 *
 * 使用条件 (constraint_registry の gac_favorable):
 *   小規模 (変数数 4..24) かつ値域が密 (span <= 24) な alldifferent のみ。
 *   sudoku 等で「全面有効化では overhead 負け、適応的有効化なら大幅改善」を
 *   2026-06-10 に確認。-G フラグで全 alldifferent に強制可能。
 *
 * アルゴリズム:
 *   1. Hopcroft-Karp 最大二部マッチング
 *   2. 残余グラフの SCC 分解 (Tarjan)
 *   3. フィルタリング: マッチ辺でもなく、同一 SCC でもなく、
 *      自由値からも到達不能なエッジ (var, val) を除去
 *
 * 既知の制限:
 *   - コールバック内で消費済み値（instantiated var が使用中）のスキップが必要
 */
#ifndef SABORI_CSP_CONSTRAINTS_ALL_DIFFERENT_GAC_HPP
#define SABORI_CSP_CONSTRAINTS_ALL_DIFFERENT_GAC_HPP

#include "sabori_csp/constraints/global.hpp"
#include <unordered_map>

namespace sabori_csp {

/**
 * @brief all_different GAC 制約 (Régin's algorithm)
 *
 * AllDifferentConstraint に Hopcroft-Karp マッチング + Tarjan SCC による
 * GAC フィルタリングを追加したバージョン。
 */
class AllDifferentGACConstraint : public AllDifferentConstraint {
public:
    explicit AllDifferentGACConstraint(std::vector<VariablePtr> vars);

    std::string name() const override;
    bool prepare_propagation(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;

    bool on_remove_value(Model& model, int save_point,
                         size_t internal_var_idx,
                         Domain::value_type removed_value) override;

    bool on_set_min(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;

    bool on_set_max(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;

    void rewind_to(int save_point);

    /**
     * @brief バッチ伝播: GAC 有効なら Régin フィルタ、無効なら親の bounds(Z)
     */
    bool propagate_batch(Model& model, int save_point) override;

private:
    bool gac_enabled_ = false;

    // 安定した値インデックス（構築後不変）
    size_t total_values_ = 0;
    std::vector<Domain::value_type> gac_idx_to_val_;
    std::unordered_map<Domain::value_type, int> gac_val_to_idx_;

    // マッチング状態
    std::vector<int> match_var_;
    std::vector<int> match_val_;
    bool matching_valid_ = false;

    // 作業配列（再利用）
    std::vector<int> hk_dist_;
    std::vector<int> hk_iter_;
    std::vector<int> bfs_queue_;
    std::vector<bool> reachable_;
    std::vector<int> scc_id_;
    std::vector<int> scc_stack_;
    std::vector<int> scc_low_;
    std::vector<int> scc_num_;
    std::vector<bool> on_stack_;
    std::vector<Domain::value_type> domain_buf_;
    std::vector<std::vector<Domain::value_type>> hk_buf_pool_;  // hk_dfs 深さ別バッファ
    std::vector<std::vector<int>> val_to_vars_;
    std::vector<std::vector<int>> adj_;
    int tarjan_counter_ = 0;
    int scc_count_ = 0;

    // GAC メソッド
    bool run_gac_filtering(Model& model);
    bool find_maximum_matching(Model& model);
    bool hk_bfs(Model& model);
    bool hk_dfs(Model& model, int u, size_t depth);
    void compute_sccs_and_filter(Model& model);
    void tarjan_dfs(int u, const std::vector<std::vector<int>>& adj);

    /// 値がプール内（未消費）かチェック
    inline bool is_val_in_pool(int val_idx) const {
        auto val = gac_idx_to_val_[val_idx];
        return pool_.contains(val);
    }
};

}  // namespace sabori_csp

#endif  // SABORI_CSP_CONSTRAINTS_ALL_DIFFERENT_GAC_HPP
