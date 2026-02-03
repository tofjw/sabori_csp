#include "sabori_csp/domain.hpp"
#include <algorithm>
#include <limits>

namespace sabori_csp {

Domain::Domain()
    : n_(0)
    , min_(std::numeric_limits<value_type>::max())
    , max_(std::numeric_limits<value_type>::min()) {}

Domain::Domain(value_type min, value_type max)
    : n_(0)
    , min_(min)
    , max_(max) {
    if (min > max) {
        return;
    }
    for (value_type v = min; v <= max; ++v) {
        sparse_[v] = values_.size();
        values_.push_back(v);
    }
    n_ = values_.size();
}

Domain::Domain(std::vector<value_type> values)
    : n_(0)
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
    for (size_t i = 0; i < n_; ++i) {
        sparse_[values_[i]] = i;
    }
    min_ = values_.front();
    max_ = values_.back();
}

bool Domain::empty() const {
    return n_ == 0;
}

size_t Domain::size() const {
    return n_;
}

std::optional<Domain::value_type> Domain::min() const {
    if (n_ == 0) {
        return std::nullopt;
    }
    return min_;
}

std::optional<Domain::value_type> Domain::max() const {
    if (n_ == 0) {
        return std::nullopt;
    }
    return max_;
}

bool Domain::contains(value_type value) const {
    auto it = sparse_.find(value);
    if (it == sparse_.end()) {
        return false;
    }
    return it->second < n_;
}

bool Domain::remove(value_type value) {
    auto it = sparse_.find(value);
    if (it == sparse_.end() || it->second >= n_) {
        return false;  // 元々存在しない
    }

    size_t idx = it->second;
    swap_at(idx, n_ - 1);
    --n_;

    // min/max の更新
    if (n_ > 0) {
        if (value == min_ || value == max_) {
            update_bounds();
        }
    }
    return true;
}

bool Domain::assign(value_type value) {
    auto it = sparse_.find(value);
    if (it == sparse_.end() || it->second >= n_) {
        return false;
    }

    size_t idx = it->second;
    swap_at(idx, 0);
    n_ = 1;
    min_ = value;
    max_ = value;
    return true;
}

std::vector<Domain::value_type> Domain::values() const {
    return std::vector<value_type>(values_.begin(), values_.begin() + n_);
}

bool Domain::is_singleton() const {
    return n_ == 1;
}

std::vector<Domain::value_type>& Domain::values_ref() {
    return values_;
}

const std::vector<Domain::value_type>& Domain::values_ref() const {
    return values_;
}

std::unordered_map<Domain::value_type, size_t>& Domain::sparse_ref() {
    return sparse_;
}

const std::unordered_map<Domain::value_type, size_t>& Domain::sparse_ref() const {
    return sparse_;
}

size_t Domain::n() const {
    return n_;
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
    sparse_[vi] = j;
    sparse_[vj] = i;
}

void Domain::update_bounds() {
    if (n_ == 0) {
        min_ = std::numeric_limits<value_type>::max();
        max_ = std::numeric_limits<value_type>::min();
        return;
    }

    min_ = values_[0];
    max_ = values_[0];
    for (size_t i = 1; i < n_; ++i) {
        if (values_[i] < min_) min_ = values_[i];
        if (values_[i] > max_) max_ = values_[i];
    }
}

} // namespace sabori_csp
