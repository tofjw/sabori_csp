/**
 * @file model.hpp
 * @brief FlatZincモデルの中間表現
 */
#ifndef SABORI_CSP_FZN_MODEL_HPP
#define SABORI_CSP_FZN_MODEL_HPP

#include "sabori_csp/model.hpp"
#include "sabori_csp/constraint.hpp"
#include <string>
#include <vector>
#include <map>
#include <variant>
#include <memory>

namespace sabori_csp {
namespace fzn {

/**
 * @brief FlatZinc変数宣言
 */
struct VarDecl {
    std::string name;
    Domain::value_type lb;  // lower bound
    Domain::value_type ub;  // upper bound
    bool is_output = false;
    bool is_bool = false;   // true if declared as "var bool"
    std::optional<Domain::value_type> fixed_value;  // For par (constant) values
    std::vector<Domain::value_type> domain_values;  // For set domain: var {1,3,5}: x
};

/**
 * @brief FlatZinc配列宣言
 */
struct ArrayDecl {
    std::string name;
    size_t size;
    std::vector<std::string> elements;  // Variable names or literals
    bool is_output = false;
    bool is_bool = false;   // true if declared as "array of var bool"
};

/**
 * @brief FlatZinc範囲型 (lb..ub)
 */
struct IntRange {
    Domain::value_type lb;
    Domain::value_type ub;
};

/**
 * @brief FlatZinc制約引数
 */
using ConstraintArg = std::variant<
    Domain::value_type,                 // Integer literal
    std::string,                        // Variable name
    std::vector<Domain::value_type>,    // Array of integers
    std::vector<std::string>,           // Array of variable names
    IntRange                            // Range: lb..ub
>;

/**
 * @brief FlatZinc制約宣言
 */
struct ConstraintDecl {
    std::string name;
    std::vector<ConstraintArg> args;
};

/**
 * @brief FlatZinc solve宣言の種類
 */
enum class SolveKind {
    Satisfy,
    Minimize,
    Maximize
};

/**
 * @brief FlatZinc solve宣言
 */
struct SolveDecl {
    SolveKind kind = SolveKind::Satisfy;
    std::string objective_var;  // For minimize/maximize
};

/**
 * @brief FlatZincモデル
 */
class Model {
public:
    Model() = default;

    /**
     * @brief 変数宣言を追加
     */
    void add_var_decl(VarDecl decl);

    /**
     * @brief 配列宣言を追加
     */
    void add_array_decl(ArrayDecl decl);

    /**
     * @brief 制約宣言を追加
     */
    void add_constraint_decl(ConstraintDecl decl);

    /**
     * @brief solve宣言を設定
     */
    void set_solve_decl(SolveDecl decl);

    /**
     * @brief FlatZincモデルをコアモデルに変換
     */
    std::unique_ptr<sabori_csp::Model> to_model() const;

    /**
     * @brief 出力変数名を取得
     */
    std::vector<std::string> output_vars() const;

    /**
     * @brief 出力配列名を取得
     */
    std::vector<std::string> output_arrays() const;

    /**
     * @brief 変数宣言を取得
     */
    const std::map<std::string, VarDecl>& var_decls() const { return var_decls_; }

    /**
     * @brief 配列宣言を取得
     */
    const std::map<std::string, ArrayDecl>& array_decls() const { return array_decls_; }

    /**
     * @brief 制約宣言を取得
     */
    const std::vector<ConstraintDecl>& constraint_decls() const { return constraint_decls_; }

    /**
     * @brief solve宣言を取得
     */
    const SolveDecl& solve_decl() const { return solve_decl_; }

    /**
     * @brief 変数の上限を設定（最適化用）
     * @return 成功したらtrue
     */
    bool set_var_upper_bound(const std::string& name, Domain::value_type ub);

    /**
     * @brief 変数の下限を設定（最適化用）
     * @return 成功したらtrue
     */
    bool set_var_lower_bound(const std::string& name, Domain::value_type lb);

private:
    std::map<std::string, VarDecl> var_decls_;
    std::map<std::string, ArrayDecl> array_decls_;
    std::vector<ConstraintDecl> constraint_decls_;
    SolveDecl solve_decl_;
};

/**
 * @brief FlatZincファイルをパース
 * @param filename ファイル名
 * @return パースされたモデル
 * @throws std::runtime_error パースエラー時
 */
std::unique_ptr<Model> parse_file(const std::string& filename);

/**
 * @brief FlatZinc文字列をパース
 * @param input 入力文字列
 * @return パースされたモデル
 * @throws std::runtime_error パースエラー時
 */
std::unique_ptr<Model> parse_string(const std::string& input);

} // namespace fzn
} // namespace sabori_csp

#endif // SABORI_CSP_FZN_MODEL_HPP
