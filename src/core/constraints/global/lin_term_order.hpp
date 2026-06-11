/**
 * @file lin_term_order.hpp
 * @brief int_lin_* 制約の項の並び順を切り替える実験用ヘルパ（内部用）
 */
#ifndef SABORI_CSP_LIN_TERM_ORDER_HPP
#define SABORI_CSP_LIN_TERM_ORDER_HPP

#include "sabori_csp/variable.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <vector>

namespace sabori_csp::detail {

/**
 * @brief 環境変数 SABORI_LIN_ORDER に従って項を並べ替える（実験用）
 *
 * 0/未設定: 初出順（並べ替えなし） / 1: |coeff| 降順 / 2: |coeff| 昇順
 * 3: |coeff|×ドメイン幅 降順 / 4: |coeff|×ドメイン幅 昇順
 * 同点は初出順を保持（stable_sort）。
 */
inline void apply_lin_term_order(std::vector<VariablePtr>& vars,
                                 std::vector<int64_t>& coeffs) {
    static const int mode = [] {
        const char* e = std::getenv("SABORI_LIN_ORDER");
        return e ? std::atoi(e) : 0;
    }();
    if (mode == 0 || vars.size() < 2) return;

    const size_t n = vars.size();
    std::vector<int64_t> key(n);
    for (size_t i = 0; i < n; ++i) {
        int64_t a = coeffs[i] < 0 ? -coeffs[i] : coeffs[i];
        if (mode >= 3) {
            const int64_t width = vars[i]->max() - vars[i]->min() + 1;
            if (__builtin_mul_overflow(a, width, &a)) {
                a = std::numeric_limits<int64_t>::max();
            }
        }
        key[i] = a;
    }
    std::vector<size_t> idx(n);
    std::iota(idx.begin(), idx.end(), 0);
    const bool desc = (mode == 1 || mode == 3);
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
        return desc ? key[a] > key[b] : key[a] < key[b];
    });
    std::vector<VariablePtr> v2(n);
    std::vector<int64_t> c2(n);
    for (size_t i = 0; i < n; ++i) {
        v2[i] = vars[idx[i]];
        c2[i] = coeffs[idx[i]];
    }
    vars.swap(v2);
    coeffs.swap(c2);
}

} // namespace sabori_csp::detail

#endif // SABORI_CSP_LIN_TERM_ORDER_HPP
