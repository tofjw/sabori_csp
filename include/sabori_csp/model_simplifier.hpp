/**
 * @file model_simplifier.hpp
 * @brief モデル簡略化クラス（変数消去・制約書き換え）
 */
#ifndef SABORI_CSP_MODEL_SIMPLIFIER_HPP
#define SABORI_CSP_MODEL_SIMPLIFIER_HPP

#include "sabori_csp/model.hpp"
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace sabori_csp {

/**
 * @brief 変数代入情報
 *
 * cx * X + cy * Y = rhs を表す。
 * |cx| == 1 の側が消去対象 X。
 * X = (rhs - cy * Y) / cx で復元可能。
 */
struct SubstitutionInfo {
    size_t x_id;      ///< 消去対象変数のID
    size_t y_id;      ///< 残存変数のID
    int64_t cx;        ///< X の係数（|cx| == 1）
    int64_t cy;        ///< Y の係数
    int64_t rhs;       ///< 定数項
    size_t defining_constraint_idx;  ///< 定義制約のインデックス
};

/**
 * @brief モデル簡略化クラス
 *
 * 2変数 int_lin_eq で |coeff|=1 の変数を消去し、
 * 他の線形制約を書き換える。
 */
class ModelSimplifier {
public:
    /**
     * @brief Model を in-place で簡略化
     * @param model 簡略化対象のモデル
     * @param protected_var_ids 消去禁止の変数ID集合
     * @param verbose 詳細ログ出力
     * @return 簡略化が行われた場合 true
     */
    bool simplify(Model& model,
                  const std::unordered_set<size_t>& protected_var_ids,
                  bool verbose = false);

    /**
     * @brief 代入情報リストを取得
     */
    const std::vector<SubstitutionInfo>& substitutions() const { return substitutions_; }

private:
    /**
     * @brief 変数ごとの制約インデックスリストを構築
     */
    std::unordered_map<size_t, std::vector<size_t>> build_var_constraints(const Model& model) const;

    /**
     * @brief 消去候補を検出し substitutions_ に記録
     */
    void find_elimination_candidates(
        Model& model,
        const std::unordered_set<size_t>& protected_var_ids,
        const std::unordered_map<size_t, std::vector<size_t>>& var_constraints);

    /**
     * @brief 代入を全制約に適用
     * @throws std::runtime_error UNSAT 検出時
     */
    void apply_substitutions(Model& model);

    /**
     * @brief 線形制約の係数・変数ID・RHSに代入を適用
     * @return 変更があった場合 true
     */
    bool substitute_in_linear(std::vector<int64_t>& coeffs,
                               std::vector<size_t>& var_ids,
                               int64_t& rhs) const;

    std::vector<SubstitutionInfo> substitutions_;
    std::unordered_map<size_t, size_t> subst_map_;  ///< x_id -> substitutions_ のインデックス
    std::unordered_set<size_t> y_vars_;              ///< 連鎖防止用
    std::unordered_set<size_t> defining_constraints_;  ///< 定義制約のインデックス集合
};

} // namespace sabori_csp

#endif // SABORI_CSP_MODEL_SIMPLIFIER_HPP
