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

bool IntElementConstraint::propagate(Model& model) {
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
                                           size_t /*var_idx*/, Domain::value_type value,
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

    // 残り1変数チェック（2WL）
    size_t uninstantiated_count = count_uninstantiated();
    if (uninstantiated_count == 1) {
        size_t last_idx = find_last_uninstantiated();
        if (last_idx != SIZE_MAX) {
            if (!on_last_uninstantiated(model, save_point, last_idx)) {
                return false;
            }
        }
    } else if (uninstantiated_count == 0) {
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

void IntElementConstraint::rewind_to(int /*save_point*/) {
    // 状態を持たないので何もしない
}

// ============================================================================

}  // namespace sabori_csp

