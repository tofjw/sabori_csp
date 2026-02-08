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

class Model;  // forward declaration

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
     * @brief 所属Modelを設定（Modelから呼び出される）
     */
    void set_model(Model* model) { model_ = model; }

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
     * @brief 定義域の最小値を取得
     * @pre ドメインが空でないこと
     */
    Domain::value_type min() const { return domain_.min().value(); }

    /**
     * @brief 定義域の最大値を取得
     * @pre ドメインが空でないこと
     */
    Domain::value_type max() const { return domain_.max().value(); }

    /**
     * @brief 指定値に固定（Domain + SoA を更新）
     * @return 成功したらtrue（値がドメインに存在する場合）
     */
    bool assign(Domain::value_type value);

    /**
     * @brief 値を除去（Domain + SoA を更新）
     * @return 成功したらtrue（ドメインが空にならない場合）
     */
    bool remove(Domain::value_type value);

    /**
     * @brief threshold 未満の値を一括除去（Domain + SoA を更新）
     * @return 成功したらtrue（ドメインが空にならない場合）
     */
    bool remove_below(Domain::value_type threshold);

    /**
     * @brief threshold 超の値を一括除去（Domain + SoA を更新）
     * @return 成功したらtrue（ドメインが空にならない場合）
     */
    bool remove_above(Domain::value_type threshold);

    /**
     * @brief 値が割り当てられているか
     */
    bool is_assigned() const { return domain_.is_singleton(); }

    /**
     * @brief 割り当てられた値を取得
     */
    std::optional<Domain::value_type> assigned_value() const {
        if (domain_.is_singleton()) {
            return domain_.min();
        }
        return std::nullopt;
    }

private:
    void sync_soa();  ///< Domain の状態を SoA に反映

    Model* model_ = nullptr;
    size_t id_ = SIZE_MAX;
    std::string name_;
    Domain domain_;
};

using VariablePtr = std::shared_ptr<Variable>;

} // namespace sabori_csp

#endif // SABORI_CSP_VARIABLE_HPP
