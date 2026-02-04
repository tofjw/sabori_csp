#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <limits>

namespace sabori_csp {

// ============================================================================
// ArrayVarIntElementConstraint implementation
// ============================================================================

ArrayVarIntElementConstraint::ArrayVarIntElementConstraint(
    VariablePtr index,
    std::vector<VariablePtr> array,
    VariablePtr result,
    bool zero_based)
    : Constraint()
    , index_(std::move(index))
    , array_(std::move(array))
    , result_(std::move(result))
    , n_(array_.size())
    , zero_based_(zero_based)
    , current_result_min_support_(std::numeric_limits<Domain::value_type>::max())
    , current_result_max_support_(std::numeric_limits<Domain::value_type>::min()) {

    // vars_ を構築: index, result, array[0], ..., array[n-1]
    vars_.push_back(index_);
    vars_.push_back(result_);
    for (const auto& v : array_) {
        vars_.push_back(v);
    }

    // var_ptr_to_idx 構築
    var_ptr_to_idx_[index_.get()] = 0;
    var_ptr_to_idx_[result_.get()] = 1;
    for (size_t i = 0; i < n_; ++i) {
        var_ptr_to_idx_[array_[i].get()] = 2 + i;
    }

    // 初期 bounds support を計算
    recompute_bounds_support();

    check_initial_consistency();
}

std::string ArrayVarIntElementConstraint::name() const {
    return "array_var_int_element";
}

std::vector<VariablePtr> ArrayVarIntElementConstraint::variables() const {
    return vars_;
}

Domain::value_type ArrayVarIntElementConstraint::index_to_0based(Domain::value_type idx) const {
    return zero_based_ ? idx : idx - 1;
}

Domain::value_type ArrayVarIntElementConstraint::index_from_0based(size_t idx_0based) const {
    return zero_based_ ? static_cast<Domain::value_type>(idx_0based)
                       : static_cast<Domain::value_type>(idx_0based) + 1;
}

void ArrayVarIntElementConstraint::recompute_bounds_support() {
    current_result_min_support_ = std::numeric_limits<Domain::value_type>::max();
    current_result_max_support_ = std::numeric_limits<Domain::value_type>::min();

    auto index_values = index_->domain().values();
    for (auto idx : index_values) {
        auto idx_0based = index_to_0based(idx);
        if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
            auto& arr_var = array_[static_cast<size_t>(idx_0based)];
            auto arr_min = arr_var->domain().min();
            auto arr_max = arr_var->domain().max();
            if (arr_min && arr_max) {
                current_result_min_support_ = std::min(current_result_min_support_, *arr_min);
                current_result_max_support_ = std::max(current_result_max_support_, *arr_max);
            }
        }
    }
}

std::optional<bool> ArrayVarIntElementConstraint::is_satisfied() const {
    // 全ての関連変数が確定しているかチェック
    if (!index_->is_assigned() || !result_->is_assigned()) {
        return std::nullopt;
    }

    auto idx = index_->assigned_value().value();
    auto idx_0based = index_to_0based(idx);

    if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) {
        return false;
    }

    auto& arr_var = array_[static_cast<size_t>(idx_0based)];
    if (!arr_var->is_assigned()) {
        return std::nullopt;
    }

    return arr_var->assigned_value().value() == result_->assigned_value().value();
}

bool ArrayVarIntElementConstraint::propagate_bounds(Model& /*model*/) {
    // 1. result の bounds を計算
    Domain::value_type new_result_min = std::numeric_limits<Domain::value_type>::max();
    Domain::value_type new_result_max = std::numeric_limits<Domain::value_type>::min();

    auto index_values = index_->domain().values();
    for (auto idx : index_values) {
        auto idx_0based = index_to_0based(idx);
        if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
            auto& arr_var = array_[static_cast<size_t>(idx_0based)];
            auto arr_min = arr_var->domain().min();
            auto arr_max = arr_var->domain().max();
            if (arr_min && arr_max) {
                new_result_min = std::min(new_result_min, *arr_min);
                new_result_max = std::max(new_result_max, *arr_max);
            }
        }
    }

    if (new_result_min > new_result_max) {
        return false;  // 有効なインデックスがない
    }

    // result のドメインから範囲外の値を削除
    auto& result_domain = result_->domain();
    auto result_values = result_domain.values();
    for (auto v : result_values) {
        if (v < new_result_min || v > new_result_max) {
            result_domain.remove(v);
        }
    }
    if (result_domain.empty()) {
        return false;
    }

    // 2. index のドメインから、result と重ならないインデックスを削除
    auto result_min = result_domain.min();
    auto result_max = result_domain.max();
    if (!result_min || !result_max) {
        return false;
    }

    index_values = index_->domain().values();  // 再取得
    auto& index_domain = index_->domain();

    for (auto idx : index_values) {
        auto idx_0based = index_to_0based(idx);
        if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) {
            // 範囲外のインデックスは削除
            index_domain.remove(idx);
            continue;
        }

        auto& arr_var = array_[static_cast<size_t>(idx_0based)];
        auto arr_min = arr_var->domain().min();
        auto arr_max = arr_var->domain().max();
        if (!arr_min || !arr_max) {
            // 配列要素のドメインが空
            index_domain.remove(idx);
            continue;
        }

        // array[i] の bounds と result の bounds に重なりがあるか
        if (*arr_max < *result_min || *arr_min > *result_max) {
            // 重ならない → このインデックスは無効
            index_domain.remove(idx);
        }
    }

    if (index_domain.empty()) {
        return false;
    }

    // 3. index が確定している場合、array[index] と result の bounds を同期
    if (index_->is_assigned()) {
        auto idx = index_->assigned_value().value();
        auto idx_0based = index_to_0based(idx);
        if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
            auto& arr_var = array_[static_cast<size_t>(idx_0based)];
            auto& arr_domain = arr_var->domain();

            auto arr_min = arr_domain.min();
            auto arr_max = arr_domain.max();
            result_min = result_domain.min();
            result_max = result_domain.max();

            if (!arr_min || !arr_max || !result_min || !result_max) {
                return false;
            }

            // array[index] と result の bounds を同期
            auto common_min = std::max(*arr_min, *result_min);
            auto common_max = std::min(*arr_max, *result_max);

            if (common_min > common_max) {
                return false;
            }

            // array[index] から範囲外の値を削除
            auto arr_values = arr_domain.values();
            for (auto v : arr_values) {
                if (v < common_min || v > common_max) {
                    arr_domain.remove(v);
                }
            }
            if (arr_domain.empty()) return false;

            // result から範囲外の値を削除
            result_values = result_domain.values();
            for (auto v : result_values) {
                if (v < common_min || v > common_max) {
                    result_domain.remove(v);
                }
            }
            if (result_domain.empty()) return false;
        }
    }

    return true;
}

bool ArrayVarIntElementConstraint::propagate(Model& model) {
    return propagate_bounds(model);
}

void ArrayVarIntElementConstraint::check_initial_consistency() {
    // index のドメインが全て範囲外の場合
    bool has_valid_index = false;
    auto index_values = index_->domain().values();
    for (auto idx : index_values) {
        auto idx_0based = index_to_0based(idx);
        if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
            has_valid_index = true;
            break;
        }
    }
    if (!has_valid_index) {
        set_initially_inconsistent(true);
        return;
    }

    // bounds support が空の場合
    if (current_result_min_support_ > current_result_max_support_) {
        set_initially_inconsistent(true);
        return;
    }
}

bool ArrayVarIntElementConstraint::on_instantiate(
    Model& model, int save_point,
    size_t /*var_idx*/, Domain::value_type /*value*/,
    Domain::value_type /*prev_min*/, Domain::value_type /*prev_max*/) {

    // Trail に状態を保存
    trail_.push_back({save_point, {current_result_min_support_, current_result_max_support_}});

    // bounds を再伝播
    if (!propagate_bounds(model)) {
        return false;
    }

    // bounds support を更新
    recompute_bounds_support();

    // 残り1変数チェック
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

bool ArrayVarIntElementConstraint::on_set_min(
    Model& model, int save_point,
    size_t var_idx, Domain::value_type /*new_min*/,
    Domain::value_type /*old_min*/) {

    // Trail に状態を保存
    trail_.push_back({save_point, {current_result_min_support_, current_result_max_support_}});

    // 変更された変数を特定
    auto it = var_ptr_to_idx_.find(vars_[var_idx].get());
    if (it == var_ptr_to_idx_.end()) {
        return true;
    }

    size_t internal_idx = it->second;

    if (internal_idx == 1) {
        // result の下限が上がった → index のドメインを絞る
        if (!propagate_bounds(model)) {
            return false;
        }
    } else if (internal_idx >= 2) {
        // 配列要素の下限が上がった → result の下限 support を確認
        size_t arr_idx = internal_idx - 2;
        auto idx_value = index_from_0based(arr_idx);

        if (index_->domain().contains(idx_value)) {
            // このインデックスがまだ有効 → bounds support を再計算
            recompute_bounds_support();
            if (!propagate_bounds(model)) {
                return false;
            }
        }
    }

    return true;
}

bool ArrayVarIntElementConstraint::on_set_max(
    Model& model, int save_point,
    size_t var_idx, Domain::value_type /*new_max*/,
    Domain::value_type /*old_max*/) {

    // Trail に状態を保存
    trail_.push_back({save_point, {current_result_min_support_, current_result_max_support_}});

    auto it = var_ptr_to_idx_.find(vars_[var_idx].get());
    if (it == var_ptr_to_idx_.end()) {
        return true;
    }

    size_t internal_idx = it->second;

    if (internal_idx == 1) {
        // result の上限が下がった → index のドメインを絞る
        if (!propagate_bounds(model)) {
            return false;
        }
    } else if (internal_idx >= 2) {
        // 配列要素の上限が下がった → result の上限 support を確認
        size_t arr_idx = internal_idx - 2;
        auto idx_value = index_from_0based(arr_idx);

        if (index_->domain().contains(idx_value)) {
            recompute_bounds_support();
            if (!propagate_bounds(model)) {
                return false;
            }
        }
    }

    return true;
}

bool ArrayVarIntElementConstraint::on_last_uninstantiated(
    Model& model, int /*save_point*/,
    size_t last_var_internal_idx) {

    auto& last_var = vars_[last_var_internal_idx];

    if (last_var.get() == index_.get()) {
        // index が最後の未確定変数
        // result と array 要素の共通値を持つインデックスのみ有効
        if (index_->domain().size() == 1) {
            // 既に1つしかないなら確定
            auto idx_opt = index_->domain().min();
            if (idx_opt) {
                for (size_t model_idx = 0; model_idx < model.variables().size(); ++model_idx) {
                    if (model.variable(model_idx) == index_) {
                        model.enqueue_instantiate(model_idx, *idx_opt);
                        break;
                    }
                }
            }
        }
    } else if (last_var.get() == result_.get()) {
        // result が最後の未確定変数で、index は確定済み
        if (index_->is_assigned()) {
            auto idx = index_->assigned_value().value();
            auto idx_0based = index_to_0based(idx);

            if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
                auto& arr_var = array_[static_cast<size_t>(idx_0based)];
                if (arr_var->is_assigned()) {
                    // array[index] が確定 → result も確定
                    auto expected = arr_var->assigned_value().value();
                    if (result_->domain().contains(expected)) {
                        for (size_t model_idx = 0; model_idx < model.variables().size(); ++model_idx) {
                            if (model.variable(model_idx) == result_) {
                                model.enqueue_instantiate(model_idx, expected);
                                break;
                            }
                        }
                    } else {
                        return false;
                    }
                }
            }
        }
    } else {
        // 配列要素が最後の未確定変数で、index と result は確定済み
        if (index_->is_assigned() && result_->is_assigned()) {
            auto idx = index_->assigned_value().value();
            auto idx_0based = index_to_0based(idx);

            if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
                auto& arr_var = array_[static_cast<size_t>(idx_0based)];
                if (last_var.get() == arr_var.get()) {
                    // この配列要素を result の値に確定
                    auto expected = result_->assigned_value().value();
                    if (arr_var->domain().contains(expected)) {
                        for (size_t model_idx = 0; model_idx < model.variables().size(); ++model_idx) {
                            if (model.variable(model_idx) == arr_var) {
                                model.enqueue_instantiate(model_idx, expected);
                                break;
                            }
                        }
                    } else {
                        return false;
                    }
                }
            }
        }
    }

    return true;
}

bool ArrayVarIntElementConstraint::on_final_instantiate() {
    // 全変数確定時の最終確認
    if (!index_->is_assigned() || !result_->is_assigned()) {
        return false;
    }

    auto idx = index_->assigned_value().value();
    auto idx_0based = index_to_0based(idx);

    if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) {
        return false;
    }

    auto& arr_var = array_[static_cast<size_t>(idx_0based)];
    if (!arr_var->is_assigned()) {
        return false;
    }

    return arr_var->assigned_value().value() == result_->assigned_value().value();
}

void ArrayVarIntElementConstraint::rewind_to(int save_point) {
    while (!trail_.empty() && trail_.back().first >= save_point) {
        auto& entry = trail_.back().second;
        current_result_min_support_ = entry.min_support;
        current_result_max_support_ = entry.max_support;
        trail_.pop_back();
    }
}

}  // namespace sabori_csp
