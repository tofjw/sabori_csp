/**
 * @file community_analysis.hpp
 * @brief コミュニティ構造・探索局所性の分析機能（診断専用）
 *
 * Variable Interaction Graph (VIG) を構築し、Label Propagation で
 * コミュニティを検出。探索中の判定・伝播の局所性メトリクスを収集する。
 * CLI フラグ `-c` で有効化（デフォルト off）。
 *
 * @note **診断専用機能**。複数のベンチマークで探索性能の改善は確認できなかった
 *       （VSIDS/activity が同等の情報を暗黙的に学習するため）。問題構造の
 *       観察や張り付き状態の診断ツールとしてのみ利用すること。
 *       探索ロジックでデフォルト有効化しないこと。
 */
#ifndef SABORI_CSP_COMMUNITY_ANALYSIS_HPP
#define SABORI_CSP_COMMUNITY_ANALYSIS_HPP

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <random>
#include <vector>

namespace sabori_csp {

// Forward declaration
class Model;

/**
 * @brief Variable Interaction Graph（CSR形式）
 */
struct VIG {
    size_t num_vars = 0;
    std::vector<size_t> row_ptr;   ///< size = num_vars + 1
    std::vector<size_t> col_idx;   ///< 隣接先
    std::vector<size_t> weights;   ///< 共有制約数

    size_t degree(size_t v) const {
        return row_ptr[v + 1] - row_ptr[v];
    }
};

/**
 * @brief コミュニティ検出結果
 */
struct CommunityStructure {
    std::vector<size_t> community;  ///< community[var_idx] = community_id
    size_t num_communities = 0;
    double modularity = 0.0;        ///< Q値
    size_t intra_edges = 0;
    size_t inter_edges = 0;
};

/**
 * @brief 局所性統計
 */
struct LocalityStats {
    size_t total_decisions = 0;
    size_t same_community_decisions = 0;
    size_t cross_community_decisions = 0;
    size_t total_propagation_events = 0;
    size_t same_community_propagations = 0;
    size_t cross_community_propagations = 0;
    std::vector<size_t> community_decision_count;  ///< コミュニティ別判定回数
};

/**
 * @brief コミュニティ構造・探索局所性の分析（診断専用）
 *
 * @warning 探索性能の改善目的では使用しないこと。ベンチマーク上、
 *          VSIDS/activity 主導の判定に対して有意な高速化は得られていない。
 *          問題構造の観察用ツールとしてのみ利用する。
 */
class CommunityAnalysis {
public:
    /**
     * @brief VIG を構築
     * @param model presolve後のモデル
     */
    void build_vig(const Model& model);

    /**
     * @brief Label Propagation でコミュニティを検出
     * @param rng 乱数生成器
     * @param max_iterations 最大反復回数
     */
    void detect_communities(std::mt19937& rng, size_t max_iterations = 100);

    /**
     * @brief 判定イベントを記録
     * @param var_idx 判定された変数のインデックス
     */
    void on_decision(size_t var_idx);

    /**
     * @brief 伝播イベントを記録
     * @param changed_var 変更された変数
     * @param source_var 伝播の起点変数
     */
    void on_propagation(size_t changed_var, size_t source_var);

    /**
     * @brief 統計をリセット
     */
    void reset_stats();

    /**
     * @brief 静的レポートを出力（presolve後に1回）
     */
    void print_static_report(std::ostream& os) const;

    /**
     * @brief 動的レポートを出力（restart毎 + 最終）
     */
    void print_dynamic_report(std::ostream& os, size_t restart_num) const;

    bool is_enabled() const { return enabled_; }
    void set_enabled(bool e) { enabled_ = e; }

    /// 統計収集の有効/無効。on_decision の last_decision_var_ 更新（タイブレーク用）には影響しない
    bool is_collecting_stats() const { return collect_stats_; }
    void set_collect_stats(bool e) { collect_stats_ = e; }

    const CommunityStructure& structure() const { return structure_; }

    /// サイズ上位 N コミュニティの ID リストを返す
    const std::vector<size_t>& top_communities(size_t n) const;

    /// 指定コミュニティの変数 ID リストを返す
    const std::vector<size_t>& community_vars(size_t community_id) const;

    /// 直近で判定された変数のコミュニティ ID。判定がまだ無い場合は SIZE_MAX
    size_t last_decision_community() const {
        if (last_decision_var_ == SIZE_MAX || last_decision_var_ >= structure_.community.size()) {
            return SIZE_MAX;
        }
        return structure_.community[last_decision_var_];
    }

private:
    bool enabled_ = false;
    bool collect_stats_ = false;
    VIG vig_;
    CommunityStructure structure_;
    LocalityStats stats_;
    size_t last_decision_var_ = SIZE_MAX;
    std::vector<std::vector<size_t>> community_vars_;  ///< community_id → [var_idx, ...]
    std::vector<size_t> top_communities_;               ///< サイズ降順の上位コミュニティID
};

} // namespace sabori_csp

#endif // SABORI_CSP_COMMUNITY_ANALYSIS_HPP
