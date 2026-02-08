#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <limits>
#include <iostream>

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

    // 注意: 内部状態は presolve() で初期化
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

void ArrayVarIntElementConstraint::recompute_bounds_support(Model& model) {
    current_result_min_support_ = std::numeric_limits<Domain::value_type>::max();
    current_result_max_support_ = std::numeric_limits<Domain::value_type>::min();

    auto index_values = index_->domain().values();
    for (auto idx : index_values) {
        auto idx_0based = index_to_0based(idx);
        if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
            auto& arr_var = array_[static_cast<size_t>(idx_0based)];
            auto arr_min = model.var_min(arr_var->id());
            auto arr_max = model.var_max(arr_var->id());
            current_result_min_support_ = std::min(current_result_min_support_, arr_min);
            current_result_max_support_ = std::max(current_result_max_support_, arr_max);
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

bool ArrayVarIntElementConstraint::propagate_bounds(Model& model, int save_point) {
#if 0 // DEBUG_ARRAY_VAR_ELEMENT
    std::cerr << "[DEBUG] propagate_bounds: index=" << index_->name()
              << " domain=[" << std::to_string(model.var_min(index_->id()))
              << ".." << std::to_string(model.var_max(index_->id())) << "]"
              << " result=" << result_->name()
              << " domain=[" << std::to_string(model.var_min(result_->id()))
              << ".." << std::to_string(model.var_max(result_->id())) << "]"
              << std::endl;
#endif
    // 1. result の bounds を計算
    Domain::value_type new_result_min = std::numeric_limits<Domain::value_type>::max();
    Domain::value_type new_result_max = std::numeric_limits<Domain::value_type>::min();

    auto index_values = index_->domain().values();
    for (auto idx : index_values) {
        auto idx_0based = index_to_0based(idx);
        if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
            auto& arr_var = array_[static_cast<size_t>(idx_0based)];
            auto arr_min = model.var_min(arr_var->id());
            auto arr_max = model.var_max(arr_var->id());
            new_result_min = std::min(new_result_min, arr_min);
            new_result_max = std::max(new_result_max, arr_max);
        }
    }

    if (new_result_min > new_result_max) {
#if 1
        std::cerr << "[DEBUG] FAIL(1): no valid index for result=" << result_->name()
                  << " index=" << index_->name()
                  << " index_domain=[" << std::to_string(model.var_min(index_->id()))
                  << ".." << std::to_string(model.var_max(index_->id())) << "]";
        // Print array element domains
        for (auto idx : index_values) {
            auto idx_0based = index_to_0based(idx);
            if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
                auto& arr_var = array_[static_cast<size_t>(idx_0based)];
                std::cerr << " arr[" << idx << "]=" << arr_var->name()
                          << "[" << std::to_string(model.var_min(arr_var->id()))
                          << ".." << std::to_string(model.var_max(arr_var->id())) << "]";
            }
        }
        std::cerr << std::endl;
#endif
        return false;  // 有効なインデックスがない
    }

    // result のドメインから範囲外の値を削除
    auto& result_domain = result_->domain();
    auto result_values = result_domain.values();
    for (auto v : result_values) {
        if (v < new_result_min || v > new_result_max) {
            bool success;
            if (save_point >= 0) {
                success = model.remove_value(save_point, result_->id(), v);
            } else {
                success = result_domain.remove(v);
            }
            if (!success) {
#if 0
                std::cerr << "[DEBUG] FAIL(3): result remove failed v=" << v
                          << " result=" << result_->name()
                          << " result_domain=[" << std::to_string(model.var_min(result_->id()))
                          << ".." << std::to_string(model.var_max(result_->id())) << "]"
                          << " new_result=[" << new_result_min << ".." << new_result_max << "]"
                          << " index=" << index_->name()
                          << " index_domain=[" << std::to_string(model.var_min(index_->id()))
                          << ".." << std::to_string(model.var_max(index_->id())) << "]";
                // Print array elements for valid indices
                auto idx_vals = index_->domain().values();
                for (auto idx : idx_vals) {
                    auto idx_0based = index_to_0based(idx);
                    if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
                        auto& arr_var = array_[static_cast<size_t>(idx_0based)];
                        std::cerr << " arr[" << idx << "]=" << arr_var->name()
                                  << "[" << std::to_string(model.var_min(arr_var->id()))
                                  << ".." << std::to_string(model.var_max(arr_var->id())) << "]";
                    }
                }
                std::cerr << std::endl;
#endif
                return false;
            }
        }
    }

    // 2. index のドメインから、result と重ならないインデックスを削除
    auto result_min = model.var_min(result_->id());
    auto result_max = model.var_max(result_->id());

    index_values = index_->domain().values();  // 再取得
    auto& index_domain = index_->domain();

    for (auto idx : index_values) {
        auto idx_0based = index_to_0based(idx);
        if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) {
            // 範囲外のインデックスは削除
            bool success;
            if (save_point >= 0) {
                success = model.remove_value(save_point, index_->id(), idx);
            } else {
                success = index_domain.remove(idx);
            }
            if (!success) {
#if 0
                std::cerr << "[DEBUG] FAIL(4): index out of range remove failed idx=" << idx
                          << " index=" << index_->name() << " result=" << result_->name() << std::endl;
#endif
                return false;
            }
            continue;
        }

        auto& arr_var = array_[static_cast<size_t>(idx_0based)];
        auto arr_min = model.var_min(arr_var->id());
        auto arr_max = model.var_max(arr_var->id());
        // array[i] の bounds と result の bounds に重なりがあるか
        if (arr_max < result_min || arr_min > result_max) {
            // 重ならない → このインデックスは無効
            bool success;
            if (save_point >= 0) {
                success = model.remove_value(save_point, index_->id(), idx);
            } else {
                success = index_domain.remove(idx);
            }
            if (!success) {
#if 0
                std::cerr << "[DEBUG] FAIL(6): bounds mismatch remove failed idx=" << idx
                          << " index=" << index_->name() << " result=" << result_->name()
                          << " arr=[" << arr_min << ".." << arr_max << "]"
                          << " res=[" << result_min << ".." << result_max << "]"
                          << std::endl;
#endif
                return false;
            }
        }
    }

    // 3. index が確定している場合、array[index] と result の bounds を同期
    if (index_->is_assigned()) {
        auto idx = index_->assigned_value().value();
        auto idx_0based = index_to_0based(idx);
        if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
            auto& arr_var = array_[static_cast<size_t>(idx_0based)];
            auto& arr_domain = arr_var->domain();

            auto arr_min = model.var_min(arr_var->id());
            auto arr_max = model.var_max(arr_var->id());
            result_min = model.var_min(result_->id());
            result_max = model.var_max(result_->id());

            // array[index] と result の bounds を同期
            auto common_min = std::max(arr_min, result_min);
            auto common_max = std::min(arr_max, result_max);

            if (common_min > common_max) {
                return false;
            }

            // array[index] から範囲外の値を削除
            auto arr_values = arr_domain.values();
            for (auto v : arr_values) {
                if (v < common_min || v > common_max) {
                    bool success;
                    if (save_point >= 0) {
                        success = model.remove_value(save_point, arr_var->id(), v);
                    } else {
                        success = arr_domain.remove(v);
                    }
                    if (!success) {
                        return false;
                    }
                }
            }

            // result から範囲外の値を削除
            result_values = result_domain.values();
            for (auto v : result_values) {
                if (v < common_min || v > common_max) {
                    bool success;
                    if (save_point >= 0) {
                        success = model.remove_value(save_point, result_->id(), v);
                    } else {
                        success = result_domain.remove(v);
                    }
                    if (!success) {
                        return false;
                    }
                }
            }
        }
    }

    return true;
}

bool ArrayVarIntElementConstraint::prepare_propagation(Model& model) {
    // bounds support を計算
    recompute_bounds_support(model);

    // 2WL を初期化
    init_watches();

    // trail をクリア
    trail_.clear();

    // 初期整合性チェック
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
        return false;
    }

    // bounds support が空の場合
    if (current_result_min_support_ > current_result_max_support_) {
        return false;
    }

    return true;
}

bool ArrayVarIntElementConstraint::presolve(Model& model) {
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
    model.mark_constraint_dirty(model_index(), save_point);

    // bounds を再伝播
    if (!propagate_bounds(model, save_point)) {
#if 0
        std::cerr << "[DEBUG] on_instantiate FAIL: propagate_bounds idx=" << index_->name()
                  << " idx_dom=[" << std::to_string(model.var_min(index_->id()))
                  << ".." << std::to_string(model.var_max(index_->id())) << "]"
                  << " result=" << result_->name()
                  << " res_dom=[" << std::to_string(model.var_min(result_->id()))
                  << ".." << std::to_string(model.var_max(result_->id())) << "]"
                  << std::endl;
#endif
        return false;
    }

    // bounds support を更新
    recompute_bounds_support(model);

    // 残り1変数チェック
    size_t uninstantiated_count = count_uninstantiated();
    if (uninstantiated_count == 1) {
        size_t last_idx = find_last_uninstantiated();
        if (last_idx != SIZE_MAX) {
            if (!on_last_uninstantiated(model, save_point, last_idx)) {
#if 0
                std::cerr << "[DEBUG] on_instantiate FAIL: on_last_uninstantiated" << std::endl;
#endif
                return false;
            }
        }
    } else if (uninstantiated_count == 0) {
        bool ok = on_final_instantiate();
#if 0
        if (!ok) {
            std::cerr << "[DEBUG] on_instantiate FAIL: on_final_instantiate" << std::endl;
        }
#endif
        return ok;
    }

    return true;
}

bool ArrayVarIntElementConstraint::on_set_min(
    Model& model, int save_point,
    size_t var_idx, Domain::value_type /*new_min*/,
    Domain::value_type /*old_min*/) {

    // Trail に状態を保存
    trail_.push_back({save_point, {current_result_min_support_, current_result_max_support_}});
    model.mark_constraint_dirty(model_index(), save_point);

    // 変更された変数を特定
    auto it = var_ptr_to_idx_.find(vars_[var_idx].get());
    if (it == var_ptr_to_idx_.end()) {
        return true;
    }

    size_t internal_idx = it->second;

    if (internal_idx == 1) {
        // result の下限が上がった → index のドメインを絞る
        if (!propagate_bounds(model, save_point)) {
            return false;
        }
    } else if (internal_idx >= 2) {
        // 配列要素の下限が上がった → result の下限 support を確認
        size_t arr_idx = internal_idx - 2;
        auto idx_value = index_from_0based(arr_idx);

        if (index_->domain().contains(idx_value)) {
            // このインデックスがまだ有効 → bounds support を再計算
            recompute_bounds_support(model);
            if (!propagate_bounds(model, save_point)) {
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
    model.mark_constraint_dirty(model_index(), save_point);

    auto it = var_ptr_to_idx_.find(vars_[var_idx].get());
    if (it == var_ptr_to_idx_.end()) {
        return true;
    }

    size_t internal_idx = it->second;

    if (internal_idx == 1) {
        // result の上限が下がった → index のドメインを絞る
        if (!propagate_bounds(model, save_point)) {
            return false;
        }
    } else if (internal_idx >= 2) {
        // 配列要素の上限が下がった → result の上限 support を確認
        size_t arr_idx = internal_idx - 2;
        auto idx_value = index_from_0based(arr_idx);

        if (index_->domain().contains(idx_value)) {
            recompute_bounds_support(model);
            if (!propagate_bounds(model, save_point)) {
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
        if (model.is_instantiated(index_->id())) {
            // 既に1つしかないなら確定
            auto idx_val = model.var_min(index_->id());
            model.enqueue_instantiate(index_->id(), idx_val);
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
                        model.enqueue_instantiate(result_->id(), expected);
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
                        model.enqueue_instantiate(arr_var->id(), expected);
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
#if 0
        std::cerr << "[DEBUG] on_final_instantiate FAIL: index or result not assigned" << std::endl;
#endif
        return false;
    }

    auto idx = index_->assigned_value().value();
    auto idx_0based = index_to_0based(idx);

    if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) {
#if 0
        std::cerr << "[DEBUG] on_final_instantiate FAIL: index out of range" << std::endl;
#endif
        return false;
    }

    auto& arr_var = array_[static_cast<size_t>(idx_0based)];
    if (!arr_var->is_assigned()) {
#if 0
        std::cerr << "[DEBUG] on_final_instantiate FAIL: arr_var not assigned" << std::endl;
#endif
        return false;
    }

    bool ok = arr_var->assigned_value().value() == result_->assigned_value().value();
#if 0
    if (!ok) {
        std::cerr << "[DEBUG] on_final_instantiate FAIL: value mismatch idx=" << idx
                  << " arr_var=" << arr_var->assigned_value().value()
                  << " result=" << result_->assigned_value().value()
                  << " result_name=" << result_->name()
                  << std::endl;
    }
#endif
    return ok;
}

void ArrayVarIntElementConstraint::rewind_to(int save_point) {
    while (!trail_.empty() && trail_.back().first > save_point) {
        auto& entry = trail_.back().second;
        current_result_min_support_ = entry.min_support;
        current_result_max_support_ = entry.max_support;
        trail_.pop_back();
    }
}

}  // namespace sabori_csp
