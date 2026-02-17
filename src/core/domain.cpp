#include "sabori_csp/domain.hpp"
#include <algorithm>
#include <limits>
#include <cassert>

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
    if (range > BOUNDS_ONLY_THRESHOLD) {
        // bounds-only モード: sparse/dense 配列を確保しない
        bounds_only_ = true;
        initial_range_ = range;
        n_ = range;
        return;
    }
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
    if (value < min_ || value > max_) return false;
    if (bounds_only_) {
        for (auto v : removed_values_) {
            if (v == value) return false;
        }
        return true;
    }
    auto idx_val = static_cast<size_t>(value - offset_);
    if (idx_val >= sparse_.size()) {
        return false;
    }
    return sparse_[idx_val] < n_;
}

bool Domain::remove(value_type value) {
    if (bounds_only_) {
        if (value < min_ || value > max_) return true;  // 範囲外
        // removed_values_ に既にあるか確認
        for (auto v : removed_values_) {
            if (v == value) return true;  // 既に除去済み
        }
        if (n_ == 1) return false;  // 空になる
        removed_values_.push_back(value);
        --n_;
        // 境界値の場合は min/max を調整
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

bool Domain::remove_below(value_type threshold) {
    if (threshold <= min_) return true;   // 除去不要
    if (threshold > max_) return false;   // 全除去→空

    if (bounds_only_) {
        // removed_values_ の中で threshold 以上のもののみ残す
        // threshold 未満のものはもう不要（min_ が threshold に上がるため）
        size_t kept = 0;
        for (size_t i = 0; i < removed_values_.size(); ++i) {
            if (removed_values_[i] >= threshold) {
                removed_values_[kept++] = removed_values_[i];
            }
        }
        // n_ の再計算: (max_ - threshold + 1) - kept
        size_t new_range = static_cast<size_t>(max_ - threshold + 1);
        removed_values_.resize(kept);
        n_ = new_range - kept;
        min_ = threshold;
        // min_ が removed_values_ に含まれる場合は調整
        while (min_ <= max_ && !contains(min_)) min_++;
        if (min_ > max_) { n_ = 0; return false; }
        return true;
    }

    size_t i = 0;
    while (i < n_) {
        if (values_[i] < threshold) {
            swap_at(i, n_ - 1);
            --n_;
            // swap先を再チェックするので i は進めない
        } else {
            ++i;
        }
    }
    if (n_ == 0) return false;
    update_bounds();
    return true;
}

bool Domain::remove_above(value_type threshold) {
    if (threshold >= max_) return true;   // 除去不要
    if (threshold < min_) return false;   // 全除去→空

    if (bounds_only_) {
        // removed_values_ の中で threshold 以下のもののみ残す
        size_t kept = 0;
        for (size_t i = 0; i < removed_values_.size(); ++i) {
            if (removed_values_[i] <= threshold) {
                removed_values_[kept++] = removed_values_[i];
            }
        }
        size_t new_range = static_cast<size_t>(threshold - min_ + 1);
        removed_values_.resize(kept);
        n_ = new_range - kept;
        max_ = threshold;
        // max_ が removed_values_ に含まれる場合は調整
        while (max_ >= min_ && !contains(max_)) max_--;
        if (max_ < min_) { n_ = 0; return false; }
        return true;
    }

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

bool Domain::assign(value_type value) {
    if (bounds_only_) {
        if (!contains(value)) return false;
        min_ = value;
        max_ = value;
        n_ = 1;
        return true;
    }

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
    if (bounds_only_) {
        std::vector<value_type> result;
        result.reserve(n_);
        for (value_type v = min_; v <= max_; ++v) {
            bool removed = false;
            for (auto rv : removed_values_) {
                if (rv == v) { removed = true; break; }
            }
            if (!removed) result.push_back(v);
        }
        return result;
    }
    std::vector<value_type> result;
    for (size_t i = 0; i < n_; ++i) {
        if (values_[i] >= min_ && values_[i] <= max_) {
            result.push_back(values_[i]);
        }
    }
    return result;
}

std::vector<Domain::value_type>& Domain::values_ref() {
    assert(!bounds_only_ && "values_ref() not available in bounds-only mode");
    return values_;
}

const std::vector<Domain::value_type>& Domain::values_ref() const {
    assert(!bounds_only_ && "values_ref() not available in bounds-only mode");
    return values_;
}

size_t Domain::index_of(value_type val) const {
    if (bounds_only_) {
        return contains(val) ? 0 : SIZE_MAX;
    }
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
    if (bounds_only_) return;  // no-op
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

void Domain::update_bounds() {
    if (bounds_only_) return;  // no-op
    if (n_ == 0) {
        min_ = std::numeric_limits<value_type>::max();
        max_ = std::numeric_limits<value_type>::min();
        return;
    }

    // Dense 配列の有効部分 [0, n_) をスキャンして min/max を求める
    // O(n_) で sparse 配列サイズに依存しない
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

} // namespace sabori_csp
