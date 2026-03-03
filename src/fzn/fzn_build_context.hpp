/**
 * @file fzn_build_context.hpp
 * @brief FlatZinc制約構築コンテキスト
 */
#ifndef SABORI_CSP_FZN_BUILD_CONTEXT_HPP
#define SABORI_CSP_FZN_BUILD_CONTEXT_HPP

#include "sabori_csp/fzn/model.hpp"
#include "sabori_csp/model.hpp"
#include "sabori_csp/variable.hpp"
#include <map>
#include <string>
#include <vector>
#include <cstdint>

namespace sabori_csp {
namespace fzn {

/**
 * @brief FlatZinc制約構築時の共有コンテキスト
 *
 * to_model() 内のラムダ関数群をメンバ関数として提供する。
 */
struct FznBuildContext {
    sabori_csp::Model* model;
    std::map<std::string, VariablePtr>& var_map;
    const std::map<std::string, VarDecl>& var_decls;
    const std::map<std::string, ArrayDecl>& array_decls;
    const std::map<std::string, std::vector<Domain::value_type>>& constant_arrays;
    const std::map<std::string, std::string>& alias_map;
    bool verbose;

    /**
     * @brief 引数から変数を取得（変数名 or 定数リテラル）
     */
    VariablePtr get_var(const ConstraintArg& arg);

    /**
     * @brief 変数名から変数を取得（__inline_ プレフィックスも対応）
     */
    VariablePtr get_var_by_name(const std::string& name);

    /**
     * @brief 配列引数を変数名リストに解決
     */
    std::vector<std::string> resolve_var_array(const ConstraintArg& arg) const;

    /**
     * @brief 配列引数を整数リストに解決
     */
    std::vector<Domain::value_type> resolve_int_array(const ConstraintArg& arg) const;
};

} // namespace fzn
} // namespace sabori_csp

#endif // SABORI_CSP_FZN_BUILD_CONTEXT_HPP
