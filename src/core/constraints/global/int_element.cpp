#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <set>
#include <limits>

namespace sabori_csp {

// ============================================================================
// IntElementConstraint implementation
// ============================================================================

IntElementConstraint::IntElementConstraint(VariablePtr index_var,
                                           std::vector<Domain::value_type> array,
                                           VariablePtr result_var,
                                           bool zero_based)
    : Constraint({index_var, result_var})
    , index_var_(std::move(index_var))
    , result_var_(std::move(result_var))
    , array_(std::move(array))
    , n_(array_.size())
    , zero_based_(zero_based) {

    // CSR 構築: 値 -> インデックスリスト (逆引き)
    Domain::value_type index_offset = zero_based_ ? 0 : 1;
    for (size_t i = 0; i < n_; ++i) {
        Domain::value_type v = array_[i];
        value_to_indices_[v].push_back(static_cast<Domain::value_type>(i) + index_offset);
    }

    // Monotonic Wrapper 構築: prefix/suffix の min/max
    log_n_ = 0;
    if (n_ > 0) {
        p_min_.resize(n_);
        p_max_.resize(n_);
        s_min_.resize(n_);
        s_max_.resize(n_);

        // prefix
        p_min_[0] = p_max_[0] = array_[0];
        for (size_t i = 1; i < n_; ++i) {
            p_min_[i] = std::min(p_min_[i-1], array_[i]);
            p_max_[i] = std::max(p_max_[i-1], array_[i]);
        }

        // suffix
        s_min_[n_-1] = s_max_[n_-1] = array_[n_-1];
        for (size_t i = n_ - 1; i > 0; --i) {
            s_min_[i-1] = std::min(s_min_[i], array_[i-1]);
            s_max_[i-1] = std::max(s_max_[i], array_[i-1]);
        }

        // Sparse Table 構築: O(n log n) 前処理、O(1) range min/max クエリ
        log_n_ = 0;
        while ((1 << (log_n_ + 1)) <= static_cast<int>(n_)) ++log_n_;
        ++log_n_;  // log_n_ = floor(log2(n)) + 1

        sparse_min_.resize(log_n_, std::vector<Domain::value_type>(n_));
        sparse_max_.resize(log_n_, std::vector<Domain::value_type>(n_));

        // レベル 0: 各要素そのもの
        for (size_t i = 0; i < n_; ++i) {
            sparse_min_[0][i] = array_[i];
            sparse_max_[0][i] = array_[i];
        }

        // レベル k: 区間長 2^k
        for (int k = 1; k < log_n_; ++k) {
            size_t half = static_cast<size_t>(1) << (k - 1);
            for (size_t i = 0; i + (1u << k) <= n_; ++i) {
                sparse_min_[k][i] = std::min(sparse_min_[k-1][i], sparse_min_[k-1][i + half]);
                sparse_max_[k][i] = std::max(sparse_max_[k-1][i], sparse_max_[k-1][i + half]);
            }
        }
    }

    // var_ptr_to_idx 構築
    var_ptr_to_idx_[index_var_.get()] = 0;
    var_ptr_to_idx_[result_var_.get()] = 1;

    // 初期整合性チェック
    check_initial_consistency();
}

std::string IntElementConstraint::name() const {
    return "int_element";
}

std::vector<VariablePtr> IntElementConstraint::variables() const {
    return {index_var_, result_var_};
}

Domain::value_type IntElementConstraint::index_to_0based(Domain::value_type idx) const {
    return zero_based_ ? idx : idx - 1;
}

std::optional<bool> IntElementConstraint::is_satisfied() const {
    if (!index_var_->is_assigned() || !result_var_->is_assigned()) {
        return std::nullopt;
    }

    auto idx = index_var_->assigned_value().value();
    auto idx_0based = index_to_0based(idx);

    if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) {
        return false;  // 範囲外
    }

    return array_[static_cast<size_t>(idx_0based)] == result_var_->assigned_value().value();
}

bool IntElementConstraint::presolve(Model& model) {
    // Bounds propagation: index のドメインから result のドメインを絞る

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
        // result を expected に固定
        return result_var_->assign(expected);
    }

    // result が確定している場合
    if (result_var_->is_assigned()) {
        auto result_value = result_var_->assigned_value().value();
        auto it = value_to_indices_.find(result_value);
        if (it == value_to_indices_.end()) {
            return false;  // この値を持つ index がない
        }
        // index のドメインを valid_indices に絞る
        const auto& valid_indices = it->second;
        auto& idx_domain = index_var_->domain();
        auto idx_values = idx_domain.values();
        for (auto v : idx_values) {
            bool found = false;
            for (auto vi : valid_indices) {
                if (v == vi) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (!idx_domain.remove(v)) {
                    return false;
                }
            }
        }
        return true;
    }

    // 両方未確定の場合: 双方向 bounds propagation

    // 1. index のドメインから result の取りうる値を計算
    std::set<Domain::value_type> valid_results;
    auto idx_values = index_var_->domain().values();
    for (auto idx : idx_values) {
        auto idx_0based = index_to_0based(idx);
        if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
            valid_results.insert(array_[static_cast<size_t>(idx_0based)]);
        }
    }

    // 2. result のドメインを絞る
    auto& result_domain = result_var_->domain();
    auto result_values = result_domain.values();
    for (auto v : result_values) {
        if (valid_results.find(v) == valid_results.end()) {
            if (!result_domain.remove(v)) {
                return false;
            }
        }
    }

    // 3. result のドメインから index の有効な値を計算
    std::set<Domain::value_type> valid_indices;
    result_values = result_domain.values();  // 更新後
    for (auto v : result_values) {
        auto it = value_to_indices_.find(v);
        if (it != value_to_indices_.end()) {
            for (auto vi : it->second) {
                valid_indices.insert(vi);
            }
        }
    }

    // 4. index のドメインを絞る
    auto& idx_domain = index_var_->domain();
    idx_values = idx_domain.values();  // 更新後
    for (auto v : idx_values) {
        if (valid_indices.find(v) == valid_indices.end()) {
            if (!idx_domain.remove(v)) {
                return false;
            }
        }
    }

    return true;
}

void IntElementConstraint::check_initial_consistency() {
    // 両方確定している場合のチェック
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

    // index のみ確定している場合
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

    // result のみ確定している場合
    if (!index_var_->is_assigned() && result_var_->is_assigned()) {
        auto result_value = result_var_->assigned_value().value();
        auto it = value_to_indices_.find(result_value);
        if (it == value_to_indices_.end()) {
            // この値を持つインデックスが存在しない
            set_initially_inconsistent(true);
            return;
        }

        // 有効なインデックスが少なくとも1つ index_var のドメインに含まれているか
        bool found_valid = false;
        for (auto valid_idx : it->second) {
            if (index_var_->domain().contains(valid_idx)) {
                found_valid = true;
                break;
            }
        }
        if (!found_valid) {
            set_initially_inconsistent(true);
            return;
        }
    }
}

bool IntElementConstraint::on_instantiate(Model& model, int save_point,
                                           size_t /*var_idx*/, size_t /*internal_var_idx*/, Domain::value_type value,
                                           Domain::value_type /*prev_min*/,
                                           Domain::value_type /*prev_max*/) {
    // 確定した変数を特定
    Variable* assigned_var = nullptr;
    if (index_var_->is_assigned() && index_var_->assigned_value().value() == value) {
        assigned_var = index_var_.get();
    } else if (result_var_->is_assigned() && result_var_->assigned_value().value() == value) {
        assigned_var = result_var_.get();
    }

    if (assigned_var == nullptr) {
        // この制約に関係ない変数
        return true;
    }

    auto it = var_ptr_to_idx_.find(assigned_var);
    if (it == var_ptr_to_idx_.end()) {
        return true;
    }

    size_t internal_idx = it->second;

    if (internal_idx == 0) {
        // index が確定 -> result を array[index] に固定（または一致チェック）
        auto idx_0based = index_to_0based(value);
        if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) {
            return false;  // 範囲外
        }

        auto expected_result = array_[static_cast<size_t>(idx_0based)];

        if (result_var_->is_assigned()) {
            // result も確定済み -> 一致チェック
            if (result_var_->assigned_value().value() != expected_result) {
                return false;
            }
        }
        // result が未確定の場合は on_last_uninstantiated で処理
    } else {
        // result が確定 -> index を対応するインデックスに絞る（または一致チェック）
        auto it_val = value_to_indices_.find(value);
        if (it_val == value_to_indices_.end()) {
            // この値を持つインデックスが存在しない
            return false;
        }

        const auto& valid_indices = it_val->second;

        if (index_var_->is_assigned()) {
            // index も確定済み -> 一致チェック
            auto idx = index_var_->assigned_value().value();
            bool found = false;
            for (auto vi : valid_indices) {
                if (vi == idx) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return false;
            }
        }
        // index が未確定の場合は on_last_uninstantiated で処理
    }

    // 残り変数が 1 or 0 の時
    if (has_uninstantiated()) {
        size_t last_idx = find_last_uninstantiated();
        if (last_idx != SIZE_MAX) {
            if (!on_last_uninstantiated(model, save_point, last_idx)) {
                return false;
            }
        }
    }
    else {
        return on_final_instantiate();
    }

    return true;
}

bool IntElementConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                                    size_t last_var_internal_idx) {
    auto& last_var = vars_[last_var_internal_idx];

    if (last_var.get() == result_var_.get()) {
        // index は確定済み -> result を確定
        auto idx = index_var_->assigned_value().value();
        auto idx_0based = index_to_0based(idx);

        if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) {
            return false;
        }

        auto expected_result = array_[static_cast<size_t>(idx_0based)];

        if (result_var_->is_assigned()) {
            return result_var_->assigned_value().value() == expected_result;
        }

        if (result_var_->domain().contains(expected_result)) {
            model.enqueue_instantiate(result_var_->id(), expected_result);
            return true;
        } else {
            return false;
        }
    } else if (last_var.get() == index_var_.get()) {
        // result は確定済み -> index の候補を探す
        auto result_value = result_var_->assigned_value().value();
        auto it = value_to_indices_.find(result_value);
        if (it == value_to_indices_.end()) {
            return false;
        }

        const auto& valid_indices = it->second;

        if (index_var_->is_assigned()) {
            auto idx = index_var_->assigned_value().value();
            for (auto vi : valid_indices) {
                if (vi == idx) {
                    return true;
                }
            }
            return false;
        }

        // 有効なインデックスのうち、index_var のドメインに含まれるものを収集
        std::vector<Domain::value_type> candidates;
        for (auto vi : valid_indices) {
            if (index_var_->domain().contains(vi)) {
                candidates.push_back(vi);
            }
        }

        if (candidates.empty()) {
            return false;
        } else if (candidates.size() == 1) {
            // 候補が1つだけなら確定
            model.enqueue_instantiate(index_var_->id(), candidates[0]);
        }
        // 複数候補がある場合は選択を solver に任せる
    }

    return true;
}

bool IntElementConstraint::on_final_instantiate() {
    // 全ての変数が確定したときの最終確認: array[index] = result
    auto idx = index_var_->assigned_value().value();
    auto idx_0based = index_to_0based(idx);

    if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) {
        return false;
    }

    return array_[static_cast<size_t>(idx_0based)] == result_var_->assigned_value().value();
}

bool IntElementConstraint::on_set_min(
    Model& model, int /*save_point*/,
    size_t /*var_idx*/, size_t internal_var_idx,
    Domain::value_type new_min, Domain::value_type /*old_min*/) {

    if (n_ == 0) return false;

    if (internal_var_idx == 0) {
        // index.min が増加 → result の bounds を絞る
        auto lo_0 = index_to_0based(new_min);
        if (lo_0 < 0) lo_0 = 0;
        auto idx_max = model.var_max(index_var_->id());
        auto hi_0 = index_to_0based(idx_max);
        if (hi_0 < 0 || lo_0 >= static_cast<Domain::value_type>(n_)) return false;
        if (hi_0 >= static_cast<Domain::value_type>(n_)) hi_0 = static_cast<Domain::value_type>(n_) - 1;
        if (lo_0 > hi_0) return false;

        auto r_min = range_min(static_cast<size_t>(lo_0), static_cast<size_t>(hi_0));
        auto r_max = range_max(static_cast<size_t>(lo_0), static_cast<size_t>(hi_0));

        if (r_min > model.var_min(result_var_->id())) {
            model.enqueue_set_min(result_var_->id(), r_min);
        }
        if (r_max < model.var_max(result_var_->id())) {
            model.enqueue_set_max(result_var_->id(), r_max);
        }
    } else {
        // result.min が増加 → index の bounds を絞る (二分探索)
        auto r_lo = new_min;
        auto idx_min = model.var_min(index_var_->id());
        auto idx_max = model.var_max(index_var_->id());
        auto lo_0 = index_to_0based(idx_min);
        auto hi_0 = index_to_0based(idx_max);
        if (lo_0 < 0) lo_0 = 0;
        if (hi_0 >= static_cast<Domain::value_type>(n_)) hi_0 = static_cast<Domain::value_type>(n_) - 1;
        if (lo_0 > hi_0) return false;

        // s_max_ (非増加) で index.max を絞る:
        // s_max_[k] < r_lo → array[k..n-1] は全て < r_lo → index.max <= k-1
        // 二分探索: s_max_[k] < r_lo を満たす最小の k を探す
        {
            auto left = static_cast<int64_t>(lo_0);
            auto right = static_cast<int64_t>(hi_0);
            int64_t new_hi = right;
            while (left <= right) {
                int64_t mid = left + (right - left) / 2;
                if (s_max_[static_cast<size_t>(mid)] < r_lo) {
                    if (mid == 0) return false;  // 全要素 < r_lo
                    new_hi = mid - 1;
                    right = mid - 1;
                } else {
                    left = mid + 1;
                }
            }
            Domain::value_type new_idx_max = zero_based_ ? static_cast<Domain::value_type>(new_hi)
                                                          : static_cast<Domain::value_type>(new_hi) + 1;
            if (new_idx_max < idx_max) {
                model.enqueue_set_max(index_var_->id(), new_idx_max);
            }
        }

        // p_max_ (非減少) で index.min を絞る:
        // p_max_[k] < r_lo → array[0..k] は全て < r_lo → index.min >= k+1
        {
            auto left = static_cast<int64_t>(lo_0);
            auto right = static_cast<int64_t>(hi_0);
            int64_t new_lo = left;
            while (left <= right) {
                int64_t mid = left + (right - left) / 2;
                if (p_max_[static_cast<size_t>(mid)] < r_lo) {
                    new_lo = mid + 1;
                    left = mid + 1;
                } else {
                    right = mid - 1;
                }
            }
            if (new_lo >= static_cast<int64_t>(n_)) return false;  // 全要素 < r_lo
            Domain::value_type new_idx_min = zero_based_ ? static_cast<Domain::value_type>(new_lo)
                                                          : static_cast<Domain::value_type>(new_lo) + 1;
            if (new_idx_min > idx_min) {
                model.enqueue_set_min(index_var_->id(), new_idx_min);
            }
        }
    }

    return true;
}

bool IntElementConstraint::on_set_max(
    Model& model, int /*save_point*/,
    size_t /*var_idx*/, size_t internal_var_idx,
    Domain::value_type new_max, Domain::value_type /*old_max*/) {

    if (n_ == 0) return false;

    if (internal_var_idx == 0) {
        // index.max が減少 → result の bounds を絞る
        auto idx_min = model.var_min(index_var_->id());
        auto lo_0 = index_to_0based(idx_min);
        auto hi_0 = index_to_0based(new_max);
        if (lo_0 < 0) lo_0 = 0;
        if (hi_0 >= static_cast<Domain::value_type>(n_)) hi_0 = static_cast<Domain::value_type>(n_) - 1;
        if (lo_0 > hi_0 || hi_0 < 0) return false;

        auto r_min = range_min(static_cast<size_t>(lo_0), static_cast<size_t>(hi_0));
        auto r_max = range_max(static_cast<size_t>(lo_0), static_cast<size_t>(hi_0));

        if (r_min > model.var_min(result_var_->id())) {
            model.enqueue_set_min(result_var_->id(), r_min);
        }
        if (r_max < model.var_max(result_var_->id())) {
            model.enqueue_set_max(result_var_->id(), r_max);
        }
    } else {
        // result.max が減少 → index の bounds を絞る (二分探索)
        auto r_hi = new_max;
        auto idx_min = model.var_min(index_var_->id());
        auto idx_max = model.var_max(index_var_->id());
        auto lo_0 = index_to_0based(idx_min);
        auto hi_0 = index_to_0based(idx_max);
        if (lo_0 < 0) lo_0 = 0;
        if (hi_0 >= static_cast<Domain::value_type>(n_)) hi_0 = static_cast<Domain::value_type>(n_) - 1;
        if (lo_0 > hi_0) return false;

        // s_min_ (非減少) で index.max を絞る:
        // s_min_[k] > r_hi → array[k..n-1] は全て > r_hi → index.max <= k-1
        {
            auto left = static_cast<int64_t>(lo_0);
            auto right = static_cast<int64_t>(hi_0);
            int64_t new_hi = right;
            while (left <= right) {
                int64_t mid = left + (right - left) / 2;
                if (s_min_[static_cast<size_t>(mid)] > r_hi) {
                    if (mid == 0) return false;  // 全要素 > r_hi
                    new_hi = mid - 1;
                    right = mid - 1;
                } else {
                    left = mid + 1;
                }
            }
            Domain::value_type new_idx_max = zero_based_ ? static_cast<Domain::value_type>(new_hi)
                                                          : static_cast<Domain::value_type>(new_hi) + 1;
            if (new_idx_max < idx_max) {
                model.enqueue_set_max(index_var_->id(), new_idx_max);
            }
        }

        // p_min_ (非増加) で index.min を絞る:
        // p_min_[k] > r_hi → array[0..k] は全て > r_hi → index.min >= k+1
        {
            auto left = static_cast<int64_t>(lo_0);
            auto right = static_cast<int64_t>(hi_0);
            int64_t new_lo = left;
            while (left <= right) {
                int64_t mid = left + (right - left) / 2;
                if (p_min_[static_cast<size_t>(mid)] > r_hi) {
                    new_lo = mid + 1;
                    left = mid + 1;
                } else {
                    right = mid - 1;
                }
            }
            if (new_lo >= static_cast<int64_t>(n_)) return false;  // 全要素 > r_hi
            Domain::value_type new_idx_min = zero_based_ ? static_cast<Domain::value_type>(new_lo)
                                                          : static_cast<Domain::value_type>(new_lo) + 1;
            if (new_idx_min > idx_min) {
                model.enqueue_set_min(index_var_->id(), new_idx_min);
            }
        }
    }

    return true;
}

Domain::value_type IntElementConstraint::range_min(size_t lo, size_t hi) const {
    int k = 0;
    while ((1u << (k + 1)) <= hi - lo + 1) ++k;
    return std::min(sparse_min_[k][lo], sparse_min_[k][hi - (1u << k) + 1]);
}

Domain::value_type IntElementConstraint::range_max(size_t lo, size_t hi) const {
    int k = 0;
    while ((1u << (k + 1)) <= hi - lo + 1) ++k;
    return std::max(sparse_max_[k][lo], sparse_max_[k][hi - (1u << k) + 1]);
}

void IntElementConstraint::rewind_to(int /*save_point*/) {
    // 状態を持たないので何もしない
}

// ============================================================================

}  // namespace sabori_csp

