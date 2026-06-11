/**
 * @file literal.hpp
 * @brief リテラル（変数と値の述語）— nogood / conflict learning の原子
 */
#ifndef SABORI_CSP_LITERAL_HPP
#define SABORI_CSP_LITERAL_HPP

#include "sabori_csp/domain.hpp"
#include <cstdint>

namespace sabori_csp {

class Model;

/**
 * @brief リテラル（変数IDと値のペア + 型）
 */
struct Literal {
    enum class Type : uint8_t {
        Eq,   // var == value
        Leq,  // var <= value
        Geq   // var >= value
    };

    size_t var_idx;
    Domain::value_type value;
    Type type = Type::Eq;

    bool operator==(const Literal& other) const {
        return var_idx == other.var_idx && value == other.value && type == other.type;
    }

    /// このリテラルが現在のモデル状態で成立しているか
    bool is_satisfied(const Model& model) const;

    /// このリテラルの否定を返す (Eq→Eq, Leq↔Geq+1)
    Literal negate() const;
};

}  // namespace sabori_csp

#endif  // SABORI_CSP_LITERAL_HPP
