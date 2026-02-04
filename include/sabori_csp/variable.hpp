/**
 * @file variable.hpp
 * @brief CSP変数クラス
 */
#ifndef SABORI_CSP_VARIABLE_HPP
#define SABORI_CSP_VARIABLE_HPP

#include "sabori_csp/domain.hpp"
#include <string>
#include <memory>

namespace sabori_csp {

/**
 * @brief CSP変数を表すクラス
 */
class Variable {
public:
    /**
     * @brief 変数を作成
     * @param name 変数名
     * @param domain 定義域
     * @note 通常は Model::create_variable() を使用してください
     */
    Variable(std::string name, Domain domain);

    /**
     * @brief 変数のModel内IDを取得
     *
     * Model::create_variable() で設定される。
     * Model内のインデックスとして直接使用可能。
     */
    size_t id() const { return id_; }

    /**
     * @brief IDを設定（Modelから呼び出される）
     */
    void set_id(size_t id) { id_ = id; }

    /**
     * @brief 変数名を取得
     */
    const std::string& name() const;

    /**
     * @brief 定義域への参照を取得
     */
    Domain& domain();
    const Domain& domain() const;

    /**
     * @brief 値が割り当てられているか
     */
    bool is_assigned() const;

    /**
     * @brief 割り当てられた値を取得
     */
    std::optional<Domain::value_type> assigned_value() const;

private:
    size_t id_ = SIZE_MAX;
    std::string name_;
    Domain domain_;
};

using VariablePtr = std::shared_ptr<Variable>;

} // namespace sabori_csp

#endif // SABORI_CSP_VARIABLE_HPP
