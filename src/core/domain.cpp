#include "sabori_csp/domain.hpp"
#include <algorithm>
#include <limits>

namespace sabori_csp {

Domain::Domain()
    : offset_(0)
    , n_(0)
    , min_(std::numeric_limits<value_type>::max())
    , max_(std::numeric_limits<value_type>::min()) {}

Domain::Domain(value_type min, value_type max)
    : offset_(min)
    , n_(0)
    , min_(min)
    , max_(max) {
    if (min > max) {
        offset_ = 0;
        return;
    }
    size_t range = static_cast<size_t>(max - min + 1);
    sparse_.assign(range, SIZE_MAX);
    values_.reserve(range);
    for (value_type v = min; v <= max; ++v) {
        sparse_[static_cast<size_t>(v - offset_)] = values_.size();
        values_.push_back(v);
    }
    n_ = values_.size();
}

Domain::Domain(std::vector<value_type> values)
    : offset_(0)
    , n_(0)
    , min_(std::numeric_limits<value_type>::max())
    , max_(std::numeric_limits<value_type>::min()) {
    if (values.empty()) {
        return;
    }
    // 重複を除去してソート
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());

    values_ = std::move(values);
    n_ = values_.size();
    min_ = values_.front();
    max_ = values_.back();
    offset_ = min_;

    size_t range = static_cast<size_t>(max_ - offset_ + 1);
    sparse_.assign(range, SIZE_MAX);
    for (size_t i = 0; i < n_; ++i) {
        sparse_[static_cast<size_t>(values_[i] - offset_)] = i;
    }
}

bool Domain::contains(value_type value) const {
    auto idx_val = static_cast<size_t>(value - offset_);
    if (value < offset_ || idx_val >= sparse_.size()) {
        return false;
    }
    return sparse_[idx_val] < n_;
}

bool Domain::remove(value_type value) {
    auto idx_val = static_cast<size_t>(value - offset_);
    if (value < offset_ || idx_val >= sparse_.size() || sparse_[idx_val] >= n_) {
        return true;  // 元々存在しない → 成功（変更なし）
    }

    // 削除すると空になる場合は失敗
    if (n_ == 1) {
        return false;
    }

    size_t idx = sparse_[idx_val];
    swap_at(idx, n_ - 1);
    --n_;

    // min/max の更新
    if (value == min_ || value == max_) {
        update_bounds();
    }
    return true;
}

bool Domain::assign(value_type value) {
    auto idx_val = static_cast<size_t>(value - offset_);
    if (value < offset_ || idx_val >= sparse_.size() || sparse_[idx_val] >= n_) {
        return false;
    }

    size_t idx = sparse_[idx_val];
    swap_at(idx, 0);
    n_ = 1;
    min_ = value;
    max_ = value;
    return true;
}

std::vector<Domain::value_type> Domain::values() const {
    return std::vector<value_type>(values_.begin(), values_.begin() + n_);
}

std::vector<Domain::value_type>& Domain::values_ref() {
    return values_;
}

const std::vector<Domain::value_type>& Domain::values_ref() const {
    return values_;
}

size_t Domain::index_of(value_type val) const {
    auto idx_val = static_cast<size_t>(val - offset_);
    if (val < offset_ || idx_val >= sparse_.size()) {
        return SIZE_MAX;
    }
    size_t idx = sparse_[idx_val];
    if (idx >= n_) {
        return SIZE_MAX;
    }
    return idx;
}

void Domain::set_n(size_t n) {
    n_ = n;
}

void Domain::set_min_cache(value_type min) {
    min_ = min;
}

void Domain::set_max_cache(value_type max) {
    max_ = max;
}

void Domain::swap_at(size_t i, size_t j) {
    if (i == j) return;
    value_type vi = values_[i];
    value_type vj = values_[j];
    values_[i] = vj;
    values_[j] = vi;
    sparse_[static_cast<size_t>(vi - offset_)] = j;
    sparse_[static_cast<size_t>(vj - offset_)] = i;
}

void Domain::update_bounds() {
    if (n_ == 0) {
        min_ = std::numeric_limits<value_type>::max();
        max_ = std::numeric_limits<value_type>::min();
        return;
    }

    // Forward scan from offset to find new min
    for (size_t i = 0; i < sparse_.size(); ++i) {
        if (sparse_[i] < n_) {
            min_ = static_cast<value_type>(i) + offset_;
            break;
        }
    }

    // Backward scan from end to find new max
    for (size_t i = sparse_.size(); i > 0; --i) {
        if (sparse_[i - 1] < n_) {
            max_ = static_cast<value_type>(i - 1) + offset_;
            break;
        }
    }
}

} // namespace sabori_csp
