/**
 * @file sparse_domain.hpp
 * @brief Sparse Set ベースの整数定義域クラス
 */
#ifndef SABORI_CSP_SPARSE_DOMAIN_HPP
#define SABORI_CSP_SPARSE_DOMAIN_HPP

#include "sabori_csp/var_data.hpp"
#include <vector>
#include <optional>
#include <cstdint>
#include <cassert>
#include <limits>

namespace sabori_csp {

/**
 * @brief Sparse Set ベースの整数定義域
 *
 * レンジが小さい（≤ BOUNDS_ONLY_THRESHOLD）変数用。
 * O(1) の値存在確認・削除、O(1) のバックトラック復元を提供する。
 */
class SparseDomain {
public:
    using value_type = int64_t;

    SparseDomain();
    SparseDomain(value_type min, value_type max);
    explicit SparseDomain(std::vector<value_type> values);

    bool empty() const { return n_ == 0; }
    size_t size() const { return n_; }
    std::optional<value_type> min() const { return n_ == 0 ? std::nullopt : std::optional<value_type>(min_); }
    std::optional<value_type> max() const { return n_ == 0 ? std::nullopt : std::optional<value_type>(max_); }
    bool is_singleton() const { return n_ > 0 && min_ == max_; }

    bool contains(value_type value) const;

    bool sparse_contains(value_type value) const {
        auto idx_val = static_cast<size_t>(value - offset_);
        if (value < offset_ || idx_val >= sparse_.size()) return false;
        return sparse_[idx_val] < n_;
    }

    bool remove(value_type value);
    bool remove_below(value_type threshold);
    bool remove_above(value_type threshold);
    bool assign(value_type value);
    std::vector<value_type> values() const;

    size_t initial_range() const { return sparse_.size(); }

    // ===== Trail =====
    size_t trail_data() const { return 0; }
    void restore_trail(size_t /*data*/) {} // no-op

    // ===== 復元用 =====
    size_t n() const { return n_; }
    void set_n(size_t n) { n_ = n; }
    void set_min_cache(value_type min) { min_ = min; }
    void set_max_cache(value_type max) { max_ = max; }

    // ===== Model 操作 =====
    void init_support(VarData& vd) const;
    bool apply_set_min(VarData& vd, value_type new_min, size_t& inst_delta);
    bool apply_set_max(VarData& vd, value_type new_max, size_t& inst_delta);
    bool apply_remove_value(VarData& vd, value_type val, size_t& inst_delta);
    bool apply_instantiate(VarData& vd, value_type val, size_t& inst_delta);

private:
    size_t index_of(value_type val) const;
    void swap_at(size_t i, size_t j);
    void update_bounds();

    std::vector<value_type> values_;  // Dense 配列
    std::vector<size_t> sparse_;      // フラット sparse 配列
    value_type offset_;               // = 初期 min 値
    size_t n_;                        // 有効な値の数
    value_type min_;                  // キャッシュ
    value_type max_;                  // キャッシュ
};

} // namespace sabori_csp

#endif // SABORI_CSP_SPARSE_DOMAIN_HPP
