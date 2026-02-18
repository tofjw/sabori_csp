#include "sabori_csp/bounds_domain.hpp"
#include <algorithm>
#include <cassert>

namespace sabori_csp {

BoundsDomain::BoundsDomain()
    : n_(0)
    , min_(std::numeric_limits<value_type>::max())
    , max_(std::numeric_limits<value_type>::min())
    , initial_range_(0) {}

BoundsDomain::BoundsDomain(value_type min, value_type max, size_t range)
    : n_(range)
    , min_(min)
    , max_(max)
    , initial_range_(range) {}

bool BoundsDomain::contains(value_type value) const {
    if (value < min_ || value > max_) return false;
    return removed_set_.find(value) == removed_set_.end();
}

bool BoundsDomain::remove(value_type value) {
    if (value < min_ || value > max_) return true;
    if (removed_set_.find(value) != removed_set_.end()) return true;
    if (n_ == 1) return false;
    removed_values_.push_back(value);
    removed_set_.insert(value);
    --n_;
    if (value == min_) {
        value_type new_min = min_ + 1;
        while (new_min <= max_ && !contains(new_min)) new_min++;
        if (new_min > max_) { n_ = 0; return false; }
        min_ = new_min;
    }
    if (value == max_) {
        value_type new_max = max_ - 1;
        while (new_max >= min_ && !contains(new_max)) new_max--;
        if (new_max < min_) { n_ = 0; return false; }
        max_ = new_max;
    }
    return true;
}

bool BoundsDomain::remove_below(value_type threshold) {
    if (threshold <= min_) return true;
    if (threshold > max_) return false;

    size_t kept = 0;
    for (size_t i = 0; i < removed_values_.size(); ++i) {
        if (removed_values_[i] >= threshold) {
            removed_values_[kept++] = removed_values_[i];
        } else {
            removed_set_.erase(removed_values_[i]);
        }
    }
    size_t new_range = static_cast<size_t>(max_ - threshold + 1);
    removed_values_.resize(kept);
    n_ = new_range - kept;
    min_ = threshold;
    while (min_ <= max_ && !contains(min_)) min_++;
    if (min_ > max_) { n_ = 0; return false; }
    return true;
}

bool BoundsDomain::remove_above(value_type threshold) {
    if (threshold >= max_) return true;
    if (threshold < min_) return false;

    size_t kept = 0;
    for (size_t i = 0; i < removed_values_.size(); ++i) {
        if (removed_values_[i] <= threshold) {
            removed_values_[kept++] = removed_values_[i];
        } else {
            removed_set_.erase(removed_values_[i]);
        }
    }
    size_t new_range = static_cast<size_t>(threshold - min_ + 1);
    removed_values_.resize(kept);
    n_ = new_range - kept;
    max_ = threshold;
    while (max_ >= min_ && !contains(max_)) max_--;
    if (max_ < min_) { n_ = 0; return false; }
    return true;
}

bool BoundsDomain::assign(value_type value) {
    if (!contains(value)) return false;
    min_ = value;
    max_ = value;
    n_ = 1;
    return true;
}

std::vector<BoundsDomain::value_type> BoundsDomain::values() const {
    std::vector<value_type> result;
    result.reserve(n_);
    for (value_type v = min_; v <= max_; ++v) {
        if (removed_set_.find(v) == removed_set_.end()) {
            result.push_back(v);
        }
    }
    return result;
}

void BoundsDomain::truncate_removed(size_t count) {
    for (size_t i = count; i < removed_values_.size(); ++i) {
        removed_set_.erase(removed_values_[i]);
    }
    removed_values_.resize(count);
}

void BoundsDomain::init_support(VarData& vd) const {
    vd.support_value = (vd.min + vd.max) / 2;
}

bool BoundsDomain::apply_set_min(VarData& vd, value_type new_min, size_t& inst_delta) {
    value_type actual_min = new_min;
    while (actual_min <= vd.max && !sparse_contains(actual_min)) {
        actual_min++;
    }
    if (actual_min > vd.max) {
        vd.size = 0;
        return false;
    }
    min_ = actual_min;
    vd.min = actual_min;
    vd.support_value = actual_min;

    if (actual_min == vd.max) {
        n_ = 1;
        vd.size = 1;
        inst_delta++;
    }
    return true;
}

bool BoundsDomain::apply_set_max(VarData& vd, value_type new_max, size_t& inst_delta) {
    value_type actual_max = new_max;
    while (actual_max >= vd.min && !sparse_contains(actual_max)) {
        actual_max--;
    }
    if (actual_max < vd.min) {
        vd.size = 0;
        return false;
    }
    max_ = actual_max;
    vd.max = actual_max;
    vd.support_value = actual_max;

    if (actual_max == vd.min) {
        n_ = 1;
        vd.size = 1;
        inst_delta++;
    }
    return true;
}

bool BoundsDomain::apply_remove_value(VarData& vd, value_type val, size_t& inst_delta) {
    if (!contains(val)) return true;

    if (!remove(val)) {
        vd.size = 0;
        return false;
    }
    vd.min = min().value();
    vd.max = max().value();
    vd.size = n_;
    if (val == vd.support_value) {
        vd.support_value = vd.min;
    }
    if (vd.min == vd.max) {
        inst_delta++;
    }
    return true;
}

bool BoundsDomain::apply_instantiate(VarData& vd, value_type val, size_t& inst_delta) {
    if (!contains(val)) return false;

    min_ = val;
    max_ = val;
    n_ = 1;

    vd.min = val;
    vd.max = val;
    vd.size = 1;
    vd.support_value = val;
    inst_delta++;

    return true;
}

} // namespace sabori_csp
