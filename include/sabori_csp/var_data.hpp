/**
 * @file var_data.hpp
 * @brief 変数データ構造体（VarData, VarTrailEntry）
 *
 * model.hpp から分離し、ドメインクラスとの循環依存を回避する。
 */
#ifndef SABORI_CSP_VAR_DATA_HPP
#define SABORI_CSP_VAR_DATA_HPP

#include <cstdint>
#include <cstddef>

namespace sabori_csp {

/**
 * @brief 変数データ（AoS 構造体）
 *
 * 同一変数の min/max/size を同一キャッシュラインに配置する。
 */
struct VarData {
    int64_t min;
    int64_t max;
    size_t size;
    size_t initial_range;
    int64_t support_value;
    int last_saved_level = -1;
    bool is_defined_var = false;
};

/**
 * @brief 変数ドメイン用 Trail エントリ
 */
struct VarTrailEntry {
    size_t var_idx;
    int64_t old_min;
    int64_t old_max;
    size_t old_n;
    size_t old_trail_data;  // BoundsDomain: removed_count, SparseDomain: 0
};

} // namespace sabori_csp

#endif // SABORI_CSP_VAR_DATA_HPP
