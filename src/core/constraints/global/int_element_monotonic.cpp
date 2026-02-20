#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <functional>

namespace sabori_csp {

// ============================================================================
// IntElementMonotonicConstraint implementation
// ============================================================================

IntElementMonotonicConstraint::IntElementMonotonicConstraint(
    VariablePtr index_var,
    std::vector<Domain::value_type> array,
    VariablePtr result_var,
    Monotonicity mono,
    bool zero_based)
    : Constraint({index_var, result_var})
    , index_var_(std::move(index_var))
    , result_var_(std::move(result_var))
    , array_(std::move(array))
    , n_(array_.size())
    , zero_based_(zero_based)
    , mono_(mono) {

    index_id_ = index_var_->id();
    result_id_ = result_var_->id();

    check_initial_consistency();
}

std::string IntElementMonotonicConstraint::name() const {
    return "int_element_monotonic";
}

std::vector<VariablePtr> IntElementMonotonicConstraint::variables() const {
    return {index_var_, result_var_};
}

Domain::value_type IntElementMonotonicConstraint::index_to_0based(Domain::value_type idx) const {
    return zero_based_ ? idx : idx - 1;
}

std::optional<bool> IntElementMonotonicConstraint::is_satisfied() const {
    if (!index_var_->is_assigned() || !result_var_->is_assigned()) {
        return std::nullopt;
    }

    auto idx = index_var_->assigned_value().value();
    auto idx_0based = index_to_0based(idx);

    if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) {
        return false;
    }

    return array_[static_cast<size_t>(idx_0based)] == result_var_->assigned_value().value();
}

void IntElementMonotonicConstraint::check_initial_consistency() {
    if (index_var_->is_assigned() && result_var_->is_assigned()) {
        auto idx = index_var_->assigned_value().value();
        auto idx_0based = index_to_0based(idx);

        if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) {
            set_initially_inconsistent(true);
            return;
        }

        if (array_[static_cast<size_t>(idx_0based)] != result_var_->assigned_value().value()) {
            set_initially_inconsistent(true);
            return;
        }
    }

    if (index_var_->is_assigned() && !result_var_->is_assigned()) {
        auto idx = index_var_->assigned_value().value();
        auto idx_0based = index_to_0based(idx);

        if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) {
            set_initially_inconsistent(true);
            return;
        }

        auto expected_result = array_[static_cast<size_t>(idx_0based)];
        if (!result_var_->domain().contains(expected_result)) {
            set_initially_inconsistent(true);
            return;
        }
    }

    if (!index_var_->is_assigned() && result_var_->is_assigned()) {
        auto result_value = result_var_->assigned_value().value();
        // 単調配列なので二分探索で存在チェック
        bool found = false;
        if (mono_ == Monotonicity::NON_DECREASING) {
            auto it = std::lower_bound(array_.begin(), array_.end(), result_value);
            found = (it != array_.end() && *it == result_value);
        } else {
            auto it = std::lower_bound(array_.begin(), array_.end(), result_value, std::greater<>());
            found = (it != array_.end() && *it == result_value);
        }
        if (!found) {
            set_initially_inconsistent(true);
            return;
        }

        // 有効なインデックスが index_var のドメインに含まれているか
        size_t first, last;
        if (mono_ == Monotonicity::NON_DECREASING) {
            auto lo = std::lower_bound(array_.begin(), array_.end(), result_value);
            auto hi = std::upper_bound(array_.begin(), array_.end(), result_value);
            first = static_cast<size_t>(lo - array_.begin());
            last = static_cast<size_t>(hi - array_.begin()) - 1;
        } else {
            auto lo = std::lower_bound(array_.begin(), array_.end(), result_value, std::greater<>());
            auto hi = std::upper_bound(array_.begin(), array_.end(), result_value, std::greater<>());
            first = static_cast<size_t>(lo - array_.begin());
            last = static_cast<size_t>(hi - array_.begin()) - 1;
        }

        Domain::value_type offset = zero_based_ ? 0 : 1;
        bool found_valid = false;
        for (size_t i = first; i <= last; ++i) {
            auto idx_val = static_cast<Domain::value_type>(i) + offset;
            if (index_var_->domain().contains(idx_val)) {
                found_valid = true;
                break;
            }
        }
        if (!found_valid) {
            set_initially_inconsistent(true);
        }
    }
}

bool IntElementMonotonicConstraint::presolve(Model& model) {
    if (n_ == 0) return false;

    // index が確定している場合
    if (index_var_->is_assigned()) {
        auto idx = index_var_->assigned_value().value();
        auto idx_0based = index_to_0based(idx);
        if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) {
            return false;
        }
        auto expected = array_[static_cast<size_t>(idx_0based)];
        if (result_var_->is_assigned()) {
            return result_var_->assigned_value().value() == expected;
        }
        return result_var_->assign(expected);
    }

    // result が確定している場合
    if (result_var_->is_assigned()) {
        auto result_value = result_var_->assigned_value().value();

        // 二分探索で対応インデックス範囲を見つける
        size_t first, last;
        bool found;
        if (mono_ == Monotonicity::NON_DECREASING) {
            auto lo = std::lower_bound(array_.begin(), array_.end(), result_value);
            auto hi = std::upper_bound(array_.begin(), array_.end(), result_value);
            found = (lo != hi);
            if (found) {
                first = static_cast<size_t>(lo - array_.begin());
                last = static_cast<size_t>(hi - array_.begin()) - 1;
            }
        } else {
            auto lo = std::lower_bound(array_.begin(), array_.end(), result_value, std::greater<>());
            auto hi = std::upper_bound(array_.begin(), array_.end(), result_value, std::greater<>());
            found = (lo != hi);
            if (found) {
                first = static_cast<size_t>(lo - array_.begin());
                last = static_cast<size_t>(hi - array_.begin()) - 1;
            }
        }

        if (!found) return false;

        Domain::value_type offset = zero_based_ ? 0 : 1;
        auto new_idx_min = static_cast<Domain::value_type>(first) + offset;
        auto new_idx_max = static_cast<Domain::value_type>(last) + offset;

        auto& idx_domain = index_var_->domain();
        if (new_idx_min > idx_domain.min().value()) {
            if (!idx_domain.remove_below(new_idx_min)) return false;
        }
        if (new_idx_max < idx_domain.max().value()) {
            if (!idx_domain.remove_above(new_idx_max)) return false;
        }
        return true;
    }

    // 両方未確定: bounds propagation
    auto idx_min = index_var_->domain().min().value();
    auto idx_max = index_var_->domain().max().value();
    auto lo_0 = index_to_0based(idx_min);
    auto hi_0 = index_to_0based(idx_max);
    if (lo_0 < 0) lo_0 = 0;
    if (hi_0 >= static_cast<Domain::value_type>(n_)) hi_0 = static_cast<Domain::value_type>(n_) - 1;
    if (lo_0 > hi_0) return false;

    // index bounds → result bounds
    Domain::value_type r_min, r_max;
    if (mono_ == Monotonicity::NON_DECREASING) {
        r_min = array_[static_cast<size_t>(lo_0)];
        r_max = array_[static_cast<size_t>(hi_0)];
    } else {
        r_min = array_[static_cast<size_t>(hi_0)];
        r_max = array_[static_cast<size_t>(lo_0)];
    }

    auto& result_domain = result_var_->domain();
    if (r_min > result_domain.min().value()) {
        if (!result_domain.remove_below(r_min)) return false;
    }
    if (r_max < result_domain.max().value()) {
        if (!result_domain.remove_above(r_max)) return false;
    }

    // result bounds → index bounds (二分探索)
    auto cur_r_min = result_domain.min().value();
    auto cur_r_max = result_domain.max().value();

    if (mono_ == Monotonicity::NON_DECREASING) {
        // index.min: a[i] >= cur_r_min を満たす最小の i
        auto it_lo = std::lower_bound(array_.begin() + lo_0, array_.begin() + hi_0 + 1, cur_r_min);
        if (it_lo == array_.begin() + hi_0 + 1) return false;
        auto new_lo = static_cast<Domain::value_type>(it_lo - array_.begin());

        // index.max: a[i] <= cur_r_max を満たす最大の i
        auto it_hi = std::upper_bound(array_.begin() + lo_0, array_.begin() + hi_0 + 1, cur_r_max);
        if (it_hi == array_.begin() + lo_0) return false;
        auto new_hi = static_cast<Domain::value_type>((it_hi - array_.begin()) - 1);

        Domain::value_type offset = zero_based_ ? 0 : 1;
        auto new_idx_min = new_lo + offset;
        auto new_idx_max = new_hi + offset;
        if (new_idx_min > idx_min) {
            if (!index_var_->domain().remove_below(new_idx_min)) return false;
        }
        if (new_idx_max < idx_max) {
            if (!index_var_->domain().remove_above(new_idx_max)) return false;
        }
    } else {
        // NON_INCREASING: a[0] >= a[1] >= ... >= a[n-1]
        // index.min: a[i] <= cur_r_max を満たす最小の i
        auto it_lo = std::lower_bound(array_.begin() + lo_0, array_.begin() + hi_0 + 1, cur_r_max, std::greater<>());
        if (it_lo == array_.begin() + hi_0 + 1) return false;
        auto new_lo = static_cast<Domain::value_type>(it_lo - array_.begin());

        // index.max: a[i] >= cur_r_min を満たす最大の i
        auto it_hi = std::upper_bound(array_.begin() + lo_0, array_.begin() + hi_0 + 1, cur_r_min, std::greater<>());
        if (it_hi == array_.begin() + lo_0) return false;
        auto new_hi = static_cast<Domain::value_type>((it_hi - array_.begin()) - 1);

        Domain::value_type offset = zero_based_ ? 0 : 1;
        auto new_idx_min = new_lo + offset;
        auto new_idx_max = new_hi + offset;
        if (new_idx_min > idx_min) {
            if (!index_var_->domain().remove_below(new_idx_min)) return false;
        }
        if (new_idx_max < idx_max) {
            if (!index_var_->domain().remove_above(new_idx_max)) return false;
        }
    }

    return true;
}

bool IntElementMonotonicConstraint::on_instantiate(
    Model& model, int save_point,
    size_t /*var_idx*/, size_t /*internal_var_idx*/, Domain::value_type value,
    Domain::value_type /*prev_min*/, Domain::value_type /*prev_max*/) {

    Variable* assigned_var = nullptr;
    if (model.is_instantiated(index_id_) && model.value(index_id_) == value) {
        assigned_var = index_var_.get();
    } else if (model.is_instantiated(result_id_) && model.value(result_id_) == value) {
        assigned_var = result_var_.get();
    }

    if (assigned_var == nullptr) {
        return true;
    }

    bool is_index = (assigned_var == index_var_.get());

    if (is_index) {
        // index が確定 -> result = array[index]
        auto idx_0based = index_to_0based(value);
        if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) {
            return false;
        }

        auto expected_result = array_[static_cast<size_t>(idx_0based)];

        if (model.is_instantiated(result_id_)) {
            return model.value(result_id_) == expected_result;
        }
    } else {
        // result が確定 -> index の候補範囲を二分探索で特定
        size_t first, last;
        bool found;
        if (mono_ == Monotonicity::NON_DECREASING) {
            auto lo = std::lower_bound(array_.begin(), array_.end(), value);
            auto hi = std::upper_bound(array_.begin(), array_.end(), value);
            found = (lo != hi);
            if (found) {
                first = static_cast<size_t>(lo - array_.begin());
                last = static_cast<size_t>(hi - array_.begin()) - 1;
            }
        } else {
            auto lo = std::lower_bound(array_.begin(), array_.end(), value, std::greater<>());
            auto hi = std::upper_bound(array_.begin(), array_.end(), value, std::greater<>());
            found = (lo != hi);
            if (found) {
                first = static_cast<size_t>(lo - array_.begin());
                last = static_cast<size_t>(hi - array_.begin()) - 1;
            }
        }

        if (!found) return false;

        if (model.is_instantiated(index_id_)) {
            auto idx = model.value(index_id_);
            auto idx_0based = index_to_0based(idx);
            if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) return false;
            return static_cast<size_t>(idx_0based) >= first && static_cast<size_t>(idx_0based) <= last;
        }
    }

    // 残り変数が 1 or 0 の時
    if (has_uninstantiated()) {
        size_t last_idx = find_last_uninstantiated();
        if (last_idx != SIZE_MAX) {
            if (!on_last_uninstantiated(model, save_point, last_idx)) {
                return false;
            }
        }
    } else {
        return on_final_instantiate();
    }

    return true;
}

bool IntElementMonotonicConstraint::on_last_uninstantiated(
    Model& model, int /*save_point*/, size_t last_var_internal_idx) {

    auto& last_var = vars_[last_var_internal_idx];

    if (last_var.get() == result_var_.get()) {
        // index は確定済み -> result を確定
        auto idx = model.value(index_id_);
        auto idx_0based = index_to_0based(idx);

        if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) {
            return false;
        }

        auto expected_result = array_[static_cast<size_t>(idx_0based)];

        if (model.is_instantiated(result_id_)) {
            return model.value(result_id_) == expected_result;
        }

        if (result_var_->domain().contains(expected_result)) {
            model.enqueue_instantiate(result_id_, expected_result);
            return true;
        } else {
            return false;
        }
    } else if (last_var.get() == index_var_.get()) {
        // result は確定済み -> index の候補を二分探索で特定
        auto result_value = model.value(result_id_);

        size_t first, last;
        bool found;
        if (mono_ == Monotonicity::NON_DECREASING) {
            auto lo = std::lower_bound(array_.begin(), array_.end(), result_value);
            auto hi = std::upper_bound(array_.begin(), array_.end(), result_value);
            found = (lo != hi);
            if (found) {
                first = static_cast<size_t>(lo - array_.begin());
                last = static_cast<size_t>(hi - array_.begin()) - 1;
            }
        } else {
            auto lo = std::lower_bound(array_.begin(), array_.end(), result_value, std::greater<>());
            auto hi = std::upper_bound(array_.begin(), array_.end(), result_value, std::greater<>());
            found = (lo != hi);
            if (found) {
                first = static_cast<size_t>(lo - array_.begin());
                last = static_cast<size_t>(hi - array_.begin()) - 1;
            }
        }

        if (!found) return false;

        if (model.is_instantiated(index_id_)) {
            auto idx = model.value(index_id_);
            auto idx_0based = index_to_0based(idx);
            if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) return false;
            return static_cast<size_t>(idx_0based) >= first && static_cast<size_t>(idx_0based) <= last;
        }

        Domain::value_type offset = zero_based_ ? 0 : 1;

        // 候補が1つだけなら確定
        std::vector<Domain::value_type> candidates;
        for (size_t i = first; i <= last; ++i) {
            auto idx_val = static_cast<Domain::value_type>(i) + offset;
            if (index_var_->domain().contains(idx_val)) {
                candidates.push_back(idx_val);
            }
        }

        if (candidates.empty()) {
            return false;
        } else if (candidates.size() == 1) {
            model.enqueue_instantiate(index_id_, candidates[0]);
        }
    }

    return true;
}

bool IntElementMonotonicConstraint::on_final_instantiate() {
    auto idx = index_var_->assigned_value().value();
    auto idx_0based = index_to_0based(idx);

    if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) {
        return false;
    }

    return array_[static_cast<size_t>(idx_0based)] == result_var_->assigned_value().value();
}

bool IntElementMonotonicConstraint::on_set_min(
    Model& model, int /*save_point*/,
    size_t /*var_idx*/, size_t internal_var_idx,
    Domain::value_type new_min, Domain::value_type /*old_min*/) {

    if (n_ == 0) return false;

    if (internal_var_idx == 0) {
        // index.min が増加 → result の bounds を更新
        auto lo_0 = index_to_0based(new_min);
        if (lo_0 < 0) lo_0 = 0;
        auto idx_max = model.var_max(index_id_);
        auto hi_0 = index_to_0based(idx_max);
        if (hi_0 < 0 || lo_0 >= static_cast<Domain::value_type>(n_)) return false;
        if (hi_0 >= static_cast<Domain::value_type>(n_)) hi_0 = static_cast<Domain::value_type>(n_) - 1;
        if (lo_0 > hi_0) return false;

        Domain::value_type r_min, r_max;
        if (mono_ == Monotonicity::NON_DECREASING) {
            r_min = array_[static_cast<size_t>(lo_0)];
            r_max = array_[static_cast<size_t>(hi_0)];
        } else {
            r_min = array_[static_cast<size_t>(hi_0)];
            r_max = array_[static_cast<size_t>(lo_0)];
        }

        if (r_min > model.var_min(result_id_)) {
            model.enqueue_set_min(result_id_, r_min);
        }
        if (r_max < model.var_max(result_id_)) {
            model.enqueue_set_max(result_id_, r_max);
        }
    } else {
        // result.min が増加 → index の bounds を更新 (二分探索)
        auto r_lo = new_min;
        auto idx_min = model.var_min(index_id_);
        auto idx_max = model.var_max(index_id_);
        auto lo_0 = index_to_0based(idx_min);
        auto hi_0 = index_to_0based(idx_max);
        if (lo_0 < 0) lo_0 = 0;
        if (hi_0 >= static_cast<Domain::value_type>(n_)) hi_0 = static_cast<Domain::value_type>(n_) - 1;
        if (lo_0 > hi_0) return false;

        auto begin = array_.begin() + lo_0;
        auto end = array_.begin() + hi_0 + 1;

        if (mono_ == Monotonicity::NON_DECREASING) {
            // a は非減少: a[i] >= r_lo を満たす最小の i を探す
            auto it = std::lower_bound(begin, end, r_lo);
            if (it == end) return false;  // 全要素 < r_lo
            auto new_lo_0 = static_cast<Domain::value_type>(it - array_.begin());
            Domain::value_type new_idx_min = zero_based_ ? new_lo_0 : new_lo_0 + 1;
            if (new_idx_min > idx_min) {
                model.enqueue_set_min(index_id_, new_idx_min);
            }
        } else {
            // a は非増加: a[i] >= r_lo を満たす最大の i を探す
            // 非増加なので、a[i] < r_lo を満たす最小の i が upper_bound
            auto it = std::upper_bound(begin, end, r_lo, std::greater<>());
            if (it == begin) return false;  // 全要素 < r_lo
            auto new_hi_0 = static_cast<Domain::value_type>((it - array_.begin()) - 1);
            Domain::value_type new_idx_max = zero_based_ ? new_hi_0 : new_hi_0 + 1;
            if (new_idx_max < idx_max) {
                model.enqueue_set_max(index_id_, new_idx_max);
            }
        }
    }

    return true;
}

bool IntElementMonotonicConstraint::on_set_max(
    Model& model, int /*save_point*/,
    size_t /*var_idx*/, size_t internal_var_idx,
    Domain::value_type new_max, Domain::value_type /*old_max*/) {

    if (n_ == 0) return false;

    if (internal_var_idx == 0) {
        // index.max が減少 → result の bounds を更新
        auto idx_min = model.var_min(index_id_);
        auto lo_0 = index_to_0based(idx_min);
        auto hi_0 = index_to_0based(new_max);
        if (lo_0 < 0) lo_0 = 0;
        if (hi_0 >= static_cast<Domain::value_type>(n_)) hi_0 = static_cast<Domain::value_type>(n_) - 1;
        if (lo_0 > hi_0 || hi_0 < 0) return false;

        Domain::value_type r_min, r_max;
        if (mono_ == Monotonicity::NON_DECREASING) {
            r_min = array_[static_cast<size_t>(lo_0)];
            r_max = array_[static_cast<size_t>(hi_0)];
        } else {
            r_min = array_[static_cast<size_t>(hi_0)];
            r_max = array_[static_cast<size_t>(lo_0)];
        }

        if (r_min > model.var_min(result_id_)) {
            model.enqueue_set_min(result_id_, r_min);
        }
        if (r_max < model.var_max(result_id_)) {
            model.enqueue_set_max(result_id_, r_max);
        }
    } else {
        // result.max が減少 → index の bounds を更新 (二分探索)
        auto r_hi = new_max;
        auto idx_min = model.var_min(index_id_);
        auto idx_max = model.var_max(index_id_);
        auto lo_0 = index_to_0based(idx_min);
        auto hi_0 = index_to_0based(idx_max);
        if (lo_0 < 0) lo_0 = 0;
        if (hi_0 >= static_cast<Domain::value_type>(n_)) hi_0 = static_cast<Domain::value_type>(n_) - 1;
        if (lo_0 > hi_0) return false;

        auto begin = array_.begin() + lo_0;
        auto end = array_.begin() + hi_0 + 1;

        if (mono_ == Monotonicity::NON_DECREASING) {
            // a は非減少: a[i] <= r_hi を満たす最大の i を探す
            auto it = std::upper_bound(begin, end, r_hi);
            if (it == begin) return false;  // 全要素 > r_hi
            auto new_hi_0 = static_cast<Domain::value_type>((it - array_.begin()) - 1);
            Domain::value_type new_idx_max = zero_based_ ? new_hi_0 : new_hi_0 + 1;
            if (new_idx_max < idx_max) {
                model.enqueue_set_max(index_id_, new_idx_max);
            }
        } else {
            // a は非増加: a[i] <= r_hi を満たす最小の i を探す
            auto it = std::lower_bound(begin, end, r_hi, std::greater<>());
            if (it == end) return false;  // 全要素 > r_hi
            auto new_lo_0 = static_cast<Domain::value_type>(it - array_.begin());
            Domain::value_type new_idx_min = zero_based_ ? new_lo_0 : new_lo_0 + 1;
            if (new_idx_min > idx_min) {
                model.enqueue_set_min(index_id_, new_idx_min);
            }
        }
    }

    return true;
}

void IntElementMonotonicConstraint::rewind_to(int /*save_point*/) {
    // 状態を持たないので何もしない
}

// ============================================================================

}  // namespace sabori_csp
