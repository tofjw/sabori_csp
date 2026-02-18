#include "sabori_csp/sparse_domain.hpp"
#include <algorithm>
#include <cassert>

namespace sabori_csp {

SparseDomain::SparseDomain()
    : offset_(0)
    , n_(0)
    , min_(std::numeric_limits<value_type>::max())
    , max_(std::numeric_limits<value_type>::min()) {}

SparseDomain::SparseDomain(value_type min, value_type max)
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

SparseDomain::SparseDomain(std::vector<value_type> values)
    : offset_(0)
    , n_(0)
    , min_(std::numeric_limits<value_type>::max())
    , max_(std::numeric_limits<value_type>::min()) {
    if (values.empty()) {
        return;
    }
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

bool SparseDomain::contains(value_type value) const {
    if (value < min_ || value > max_) return false;
    auto idx_val = static_cast<size_t>(value - offset_);
    if (idx_val >= sparse_.size()) {
        return false;
    }
    return sparse_[idx_val] < n_;
}

bool SparseDomain::remove(value_type value) {
    auto idx_val = static_cast<size_t>(value - offset_);
    if (value < offset_ || idx_val >= sparse_.size() || sparse_[idx_val] >= n_) {
        return true;  // 元々存在しない
    }
    if (n_ == 1) {
        return false;
    }
    size_t idx = sparse_[idx_val];
    swap_at(idx, n_ - 1);
    --n_;
    if (value == min_ || value == max_) {
        update_bounds();
    }
    return true;
}

bool SparseDomain::remove_below(value_type threshold) {
    if (threshold <= min_) return true;
    if (threshold > max_) return false;

    size_t i = 0;
    while (i < n_) {
        if (values_[i] < threshold) {
            swap_at(i, n_ - 1);
            --n_;
        } else {
            ++i;
        }
    }
    if (n_ == 0) return false;
    update_bounds();
    return true;
}

bool SparseDomain::remove_above(value_type threshold) {
    if (threshold >= max_) return true;
    if (threshold < min_) return false;

    size_t i = 0;
    while (i < n_) {
        if (values_[i] > threshold) {
            swap_at(i, n_ - 1);
            --n_;
        } else {
            ++i;
        }
    }
    if (n_ == 0) return false;
    update_bounds();
    return true;
}

bool SparseDomain::assign(value_type value) {
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

std::vector<SparseDomain::value_type> SparseDomain::values() const {
    std::vector<value_type> result;
    for (size_t i = 0; i < n_; ++i) {
        if (values_[i] >= min_ && values_[i] <= max_) {
            result.push_back(values_[i]);
        }
    }
    return result;
}

size_t SparseDomain::index_of(value_type val) const {
    if (val < min_ || val > max_) return SIZE_MAX;
    auto idx_val = static_cast<size_t>(val - offset_);
    if (idx_val >= sparse_.size()) {
        return SIZE_MAX;
    }
    size_t idx = sparse_[idx_val];
    if (idx >= n_) {
        return SIZE_MAX;
    }
    return idx;
}

void SparseDomain::swap_at(size_t i, size_t j) {
    assert(i < values_.size() && "swap_at: index i out of bounds");
    assert(j < values_.size() && "swap_at: index j out of bounds");
    if (i == j) return;
    value_type vi = values_[i];
    value_type vj = values_[j];
    assert(static_cast<size_t>(vi - offset_) < sparse_.size() && "swap_at: vi out of sparse range");
    assert(static_cast<size_t>(vj - offset_) < sparse_.size() && "swap_at: vj out of sparse range");
    values_[i] = vj;
    values_[j] = vi;
    sparse_[static_cast<size_t>(vi - offset_)] = j;
    sparse_[static_cast<size_t>(vj - offset_)] = i;
}

void SparseDomain::update_bounds() {
    if (n_ == 0) {
        min_ = std::numeric_limits<value_type>::max();
        max_ = std::numeric_limits<value_type>::min();
        return;
    }
    value_type lo = values_[0];
    value_type hi = values_[0];
    for (size_t i = 1; i < n_; ++i) {
        value_type v = values_[i];
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    min_ = lo;
    max_ = hi;
}

void SparseDomain::init_support(VarData& vd) const {
    vd.support_value = values_[n_ / 2];
}

bool SparseDomain::apply_set_min(VarData& vd, value_type new_min, size_t& inst_delta) {
    if (new_min <= vd.support_value && vd.support_value <= vd.max) {
        // Lazy: support がまだ有効なのでスキャン不要
        vd.min = new_min;
        if (new_min == vd.max) {
            size_t idx = index_of(new_min);
            assert(idx != SIZE_MAX);
            swap_at(idx, 0);
            n_ = 1;
            min_ = new_min;
            max_ = new_min;
            vd.support_value = new_min;
            vd.size = 1;
            inst_delta++;
        }
    }

    // Sync: O(gap) スキャンで actual min を求める
    value_type actual_min = new_min;
    while (actual_min <= vd.max && !sparse_contains(actual_min)) {
        actual_min++;
    }
    if (actual_min > vd.max) {
        vd.size = 0;
        return false;
    }

    if (actual_min == vd.max) {
        size_t idx = index_of(actual_min);
        assert(idx != SIZE_MAX);
        swap_at(idx, 0);
        n_ = 1;
        min_ = actual_min;
        max_ = actual_min;
        vd.min = actual_min;
        vd.max = actual_min;
        vd.size = 1;
        vd.support_value = actual_min;
        inst_delta++;
        return true;
    }

    if (sparse_contains(vd.max)) {
        min_ = actual_min;
        vd.min = actual_min;
        vd.support_value = actual_min;
        return true;
    }

    // max が stale → 逆方向スキャンで actual_max を探す
    value_type actual_max = vd.max - 1;
    while (actual_max > actual_min && !sparse_contains(actual_max)) {
        actual_max--;
    }

    if (actual_max == actual_min) {
        size_t idx = index_of(actual_min);
        assert(idx != SIZE_MAX);
        swap_at(idx, 0);
        n_ = 1;
        min_ = actual_min;
        max_ = actual_min;
        vd.min = actual_min;
        vd.max = actual_min;
        vd.size = 1;
        vd.support_value = actual_min;
        inst_delta++;
    } else {
        min_ = actual_min;
        max_ = actual_max;
        vd.min = actual_min;
        vd.max = actual_max;
        vd.support_value = actual_min;
    }
    return true;
}

bool SparseDomain::apply_set_max(VarData& vd, value_type new_max, size_t& inst_delta) {
    if (new_max >= vd.support_value && vd.support_value >= vd.min) {
        // Lazy: support がまだ有効なのでスキャン不要
        vd.max = new_max;
        if (new_max == vd.min) {
            size_t idx = index_of(new_max);
            assert(idx != SIZE_MAX);
            swap_at(idx, 0);
            n_ = 1;
            min_ = new_max;
            max_ = new_max;
            vd.support_value = new_max;
            vd.size = 1;
            inst_delta++;
        }
    }

    // Sync: O(gap) スキャンで actual max を求める
    value_type actual_max = new_max;
    while (actual_max >= vd.min && !sparse_contains(actual_max)) {
        actual_max--;
    }
    if (actual_max < vd.min) {
        vd.size = 0;
        return false;
    }

    if (actual_max == vd.min) {
        size_t idx = index_of(actual_max);
        assert(idx != SIZE_MAX);
        swap_at(idx, 0);
        n_ = 1;
        min_ = actual_max;
        max_ = actual_max;
        vd.min = actual_max;
        vd.max = actual_max;
        vd.size = 1;
        vd.support_value = actual_max;
        inst_delta++;
        return true;
    }

    if (sparse_contains(vd.min)) {
        max_ = actual_max;
        vd.max = actual_max;
        vd.support_value = actual_max;
        return true;
    }

    // min が stale → 順方向スキャンで actual_min を探す
    value_type actual_min = vd.min + 1;
    while (actual_min < actual_max && !sparse_contains(actual_min)) {
        actual_min++;
    }

    if (actual_min == actual_max) {
        size_t idx = index_of(actual_max);
        assert(idx != SIZE_MAX);
        swap_at(idx, 0);
        n_ = 1;
        min_ = actual_max;
        max_ = actual_max;
        vd.min = actual_max;
        vd.max = actual_max;
        vd.size = 1;
        vd.support_value = actual_max;
        inst_delta++;
    } else {
        min_ = actual_min;
        max_ = actual_max;
        vd.min = actual_min;
        vd.max = actual_max;
        vd.support_value = actual_max;
    }
    return true;
}

bool SparseDomain::apply_remove_value(VarData& vd, value_type val, size_t& inst_delta) {
    size_t idx = index_of(val);
    if (idx == SIZE_MAX) {
        return true;  // 既に無い
    }

    swap_at(idx, n_ - 1);
    size_t new_n = n_ - 1;
    n_ = new_n;

    if (new_n == 0) {
        vd.size = 0;
        return false;
    }

    // 境界値の場合、sparse 配列で O(gap) スキャン
    if (val == vd.min) {
        value_type new_min = val + 1;
        while (new_min <= vd.max && !sparse_contains(new_min)) new_min++;
        if (new_min > vd.max) { vd.size = 0; return false; }
        vd.min = new_min;
        min_ = new_min;
    }
    if (val == vd.max) {
        value_type new_max = val - 1;
        while (new_max >= vd.min && !sparse_contains(new_max)) new_max--;
        if (new_max < vd.min) { vd.size = 0; return false; }
        vd.max = new_max;
        max_ = new_max;
    }

    // support が削除された場合、有効な値で置換
    if (val == vd.support_value) {
        vd.support_value = values_[0];
        if (values_[0] < vd.min || values_[0] > vd.max) {
            for (size_t i = 1; i < new_n; ++i) {
                if (values_[i] >= vd.min && values_[i] <= vd.max) {
                    vd.support_value = values_[i];
                    break;
                }
            }
        }
    }

    if (vd.min == vd.max) {
        inst_delta++;
    }
    vd.size = new_n;

    return true;
}

bool SparseDomain::apply_instantiate(VarData& vd, value_type val, size_t& inst_delta) {
    size_t idx = index_of(val);
    if (idx == SIZE_MAX) {
        return false;
    }

    swap_at(idx, 0);
    n_ = 1;
    min_ = val;
    max_ = val;

    vd.min = val;
    vd.max = val;
    vd.size = 1;
    vd.support_value = val;
    inst_delta++;

    return true;
}

} // namespace sabori_csp
