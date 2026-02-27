/**
 * @file constraint_registry.hpp
 * @brief FlatZinc制約レジストリ
 */
#ifndef SABORI_CSP_FZN_CONSTRAINT_REGISTRY_HPP
#define SABORI_CSP_FZN_CONSTRAINT_REGISTRY_HPP

#include "fzn_build_context.hpp"
#include "sabori_csp/constraint.hpp"
#include "sabori_csp/fzn/model.hpp"
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace sabori_csp {
namespace fzn {

/**
 * @brief 制約ファクトリの戻り値
 *
 * - ConstraintPtr (non-null): 制約をモデルに追加
 * - nullopt: 制約なし（ドメイン変更やスキップ等で直接処理済み）
 *
 * 注意: `continue` 相当の動作（ループをスキップ）を表すために nullopt を使用する。
 * set_in のようにモデルに直接制約を追加する場合も nullopt を返す。
 */
using ConstraintFactory = std::function<
    std::optional<ConstraintPtr>(const ConstraintDecl& decl, FznBuildContext& ctx)
>;

/**
 * @brief 制約名からファクトリ関数へのレジストリ
 */
class ConstraintRegistry {
public:
    /**
     * @brief 制約ファクトリを登録
     */
    void register_constraint(const std::string& name, ConstraintFactory factory);

    /**
     * @brief 制約が登録されているか
     */
    bool has(const std::string& name) const;

    /**
     * @brief 制約を作成
     * @return 制約（nullopt の場合は追加不要）
     * @throws std::runtime_error 未登録の制約名
     */
    std::optional<ConstraintPtr> create(const std::string& name,
                                         const ConstraintDecl& decl,
                                         FznBuildContext& ctx) const;

private:
    std::unordered_map<std::string, ConstraintFactory> factories_;
};

/**
 * @brief 全制約をレジストリに登録
 */
void register_all_constraints(ConstraintRegistry& registry);

} // namespace fzn
} // namespace sabori_csp

#endif // SABORI_CSP_FZN_CONSTRAINT_REGISTRY_HPP
