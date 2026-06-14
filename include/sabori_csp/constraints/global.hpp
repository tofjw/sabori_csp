/**
 * @file global.hpp
 * @brief グローバル制約クラス (all_different, int_lin_eq)
 */
#ifndef SABORI_CSP_CONSTRAINTS_GLOBAL_HPP
#define SABORI_CSP_CONSTRAINTS_GLOBAL_HPP

#include "sabori_csp/constraint.hpp"
#include <unordered_map>
#include <numeric>

// 後方互換のための集約 include。実体はカテゴリ別ヘッダに分割。
#include "sabori_csp/constraints/global/alldifferent.hpp"
#include "sabori_csp/constraints/global/linear.hpp"
#include "sabori_csp/constraints/global/element.hpp"
#include "sabori_csp/constraints/global/extensional.hpp"
#include "sabori_csp/constraints/global/counting.hpp"
#include "sabori_csp/constraints/global/graph.hpp"
#include "sabori_csp/constraints/global/scheduling.hpp"
#include "sabori_csp/constraints/global/misc.hpp"

#endif // SABORI_CSP_CONSTRAINTS_GLOBAL_HPP
