/**
 * @file bounds_domain.hpp
 * @brief 区間ベースの整数定義域クラス（広いレンジ用）
 */
#ifndef SABORI_CSP_BOUNDS_DOMAIN_HPP
#define SABORI_CSP_BOUNDS_DOMAIN_HPP

#include "sabori_csp/var_data.hpp"
#include <vector>
#include <unordered_set>
#include <optional>
#include <cstdint>
#include <limits>

namespace sabori_csp {

/**
 * @brief 区間ベースの整数定義域（広いレンジ用）
 *
 * レンジが大きい（> BOUNDS_ONLY_THRESHOLD）変数用。
 * sparse/dense 配列を確保せず、min/max + removed_values_ で管理する。
 */
class BoundsDomain {
public:
    using value_type = int64_t;

    BoundsDomain();
    BoundsDomain(value_type min, value_type max, size_t range);

    bool empty() const { return n_ == 0; }
    size_t size() const { return n_; }
    std::optional<value_type> min() const { return n_ == 0 ? std::nullopt : std::optional<value_type>(min_); }
    std::optional<value_type> max() const { return n_ == 0 ? std::nullopt : std::optional<value_type>(max_); }
    bool is_singleton() const { return n_ > 0 && min_ == max_; }

    bool contains(value_type value) const;

    bool sparse_contains(value_type value) const {
        if (value < min_ || value > max_) return false;
        return removed_set_.find(value) == removed_set_.end();
    }

    bool remove(value_type value);
    bool remove_below(value_type threshold);
    bool remove_above(value_type threshold);
    bool assign(value_type value);
    std::vector<value_type> values() const;

    size_t initial_range() const { return initial_range_; }

    // ===== Trail =====
    size_t trail_data() const { return removed_values_.size(); }
    void restore_trail(size_t count) { truncate_removed(count); }

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
    void truncate_removed(size_t count);

    size_t n_;
    value_type min_;
    value_type max_;
    size_t initial_range_;
    std::vector<value_type> removed_values_;
    std::unordered_set<value_type> removed_set_;
};

} // namespace sabori_csp

#endif // SABORI_CSP_BOUNDS_DOMAIN_HPP
