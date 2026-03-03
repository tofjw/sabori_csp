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
    , n_(array.size())
    , zero_based_(zero_based)
    , current_result_min_support_(std::numeric_limits<Domain::value_type>::max())
    , current_result_max_support_(std::numeric_limits<Domain::value_type>::min()) {

    // var_ids_ を構築: [index, result, array[0], ..., array[n-1]]
    std::vector<VariablePtr> all_vars;
    all_vars.push_back(index);
    all_vars.push_back(result);
    for (const auto& v : array) {
        all_vars.push_back(v);
    }
    var_ids_ = extract_var_ids(all_vars);

    index_id_ = index->id();
    result_id_ = result->id();

    // 注意: 内部状態は presolve() で初期化
}

std::string ArrayVarIntElementConstraint::name() const {
    return "array_var_int_element";
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

    const auto& idx_dom = model.variable(index_id_)->domain();
    for (auto it = idx_dom.begin(); it != idx_dom.end(); ++it) {
        auto idx_0based = index_to_0based(*it);
        if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
            auto arr_id = var_ids_[2 + static_cast<size_t>(idx_0based)];
            auto arr_min = model.var_min(arr_id);
            auto arr_max = model.var_max(arr_id);
            current_result_min_support_ = std::min(current_result_min_support_, arr_min);
            current_result_max_support_ = std::max(current_result_max_support_, arr_max);
        }
    }
}

bool ArrayVarIntElementConstraint::propagate_bounds(Model& model, int save_point) {
    auto* index_var = model.variable(index_id_);
    auto* result_var = model.variable(result_id_);

    // 1. result の bounds を計算
    Domain::value_type new_result_min = std::numeric_limits<Domain::value_type>::max();
    Domain::value_type new_result_max = std::numeric_limits<Domain::value_type>::min();

    std::vector<Domain::value_type> index_values;
    index_var->domain().copy_values_to(index_values);
    for (auto idx : index_values) {
        auto idx_0based = index_to_0based(idx);
        if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
            auto arr_id = var_ids_[2 + static_cast<size_t>(idx_0based)];
            auto arr_min = model.var_min(arr_id);
            auto arr_max = model.var_max(arr_id);
            new_result_min = std::min(new_result_min, arr_min);
            new_result_max = std::max(new_result_max, arr_max);
        }
    }

    if (new_result_min > new_result_max) {
        return false;  // 有効なインデックスがない
    }

    // result のドメインから範囲外の値を削除（remove_below/remove_above で効率化）
    auto& result_domain = result_var->domain();
    if (save_point >= 0) {
        if (!model.set_min(save_point, result_id_, new_result_min)) return false;
        if (!model.set_max(save_point, result_id_, new_result_max)) return false;
    } else {
        if (!result_domain.remove_below(new_result_min)) return false;
        if (!result_domain.remove_above(new_result_max)) return false;
    }

    // 2. index のドメインから、result と重ならないインデックスを削除
    auto result_min = model.var_min(result_id_);
    auto result_max = model.var_max(result_id_);

    index_var->domain().copy_values_to(index_values);  // 再取得
    auto& index_domain = index_var->domain();

    for (auto idx : index_values) {
        auto idx_0based = index_to_0based(idx);
        if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) {
            // 範囲外のインデックスは削除
            bool success;
            if (save_point >= 0) {
                success = model.remove_value(save_point, index_id_, idx);
            } else {
                success = index_domain.remove(idx);
            }
            if (!success) {
                return false;
            }
            continue;
        }

        auto arr_id = var_ids_[2 + static_cast<size_t>(idx_0based)];
        auto arr_min = model.var_min(arr_id);
        auto arr_max = model.var_max(arr_id);
        // array[i] の bounds と result の bounds に重なりがあるか
        if (arr_max < result_min || arr_min > result_max) {
            // 重ならない → このインデックスは無効
            bool success;
            if (save_point >= 0) {
                success = model.remove_value(save_point, index_id_, idx);
            } else {
                success = index_domain.remove(idx);
            }
            if (!success) {
                return false;
            }
        }
    }

    // 3. index が確定している場合、array[index] と result の bounds を同期
    if (index_var->is_assigned()) {
        auto idx = index_var->assigned_value().value();
        auto idx_0based = index_to_0based(idx);
        if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
            auto arr_id = var_ids_[2 + static_cast<size_t>(idx_0based)];
            auto* arr_var = model.variable(arr_id);
            auto& arr_domain = arr_var->domain();

            auto arr_min = model.var_min(arr_id);
            auto arr_max = model.var_max(arr_id);
            result_min = model.var_min(result_id_);
            result_max = model.var_max(result_id_);

            // array[index] と result の bounds を同期
            auto common_min = std::max(arr_min, result_min);
            auto common_max = std::min(arr_max, result_max);

            if (common_min > common_max) {
                return false;
            }

            // array[index] から範囲外の値を削除（remove_below/remove_above で効率化）
            if (save_point >= 0) {
                if (!model.set_min(save_point, arr_id, common_min)) return false;
                if (!model.set_max(save_point, arr_id, common_max)) return false;
            } else {
                if (!arr_domain.remove_below(common_min)) return false;
                if (!arr_domain.remove_above(common_max)) return false;
            }

            // result から範囲外の値を削除
            if (save_point >= 0) {
                if (!model.set_min(save_point, result_id_, common_min)) return false;
                if (!model.set_max(save_point, result_id_, common_max)) return false;
            } else {
                if (!result_domain.remove_below(common_min)) return false;
                if (!result_domain.remove_above(common_max)) return false;
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
    model.variable(index_id_)->domain().for_each_value([&](auto idx) {
        if (!has_valid_index) {
            auto idx_0based = index_to_0based(idx);
            if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
                has_valid_index = true;
            }
        }
    });
    if (!has_valid_index) {
        return false;
    }

    // bounds support が空の場合
    if (current_result_min_support_ > current_result_max_support_) {
        return false;
    }

    return true;
}

PresolveResult ArrayVarIntElementConstraint::presolve(Model& model) {
    // Snapshot domain sizes before propagation
    size_t total_size_before = 0;
    for (size_t vid : var_ids_) {
        total_size_before += model.variable(vid)->domain().size();
    }
    if (!propagate_bounds(model)) return PresolveResult::Contradiction;
    size_t total_size_after = 0;
    for (size_t vid : var_ids_) {
        total_size_after += model.variable(vid)->domain().size();
    }
    return (total_size_after < total_size_before) ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool ArrayVarIntElementConstraint::propagate_via_queue(Model& model) {
    // result の bounds 計算と index フィルタリングを単一ループで実行
    Domain::value_type new_result_min = std::numeric_limits<Domain::value_type>::max();
    Domain::value_type new_result_max = std::numeric_limits<Domain::value_type>::min();

    auto result_min = model.var_min(result_id_);
    auto result_max = model.var_max(result_id_);

    const auto& idx_dom = model.variable(index_id_)->domain();
    for (auto it = idx_dom.begin(); it != idx_dom.end(); ++it) {
        auto idx = *it;
        auto idx_0based = index_to_0based(idx);
        if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) {
            model.enqueue_remove_value(index_id_, idx);
            continue;
        }

        auto arr_id = var_ids_[2 + static_cast<size_t>(idx_0based)];
        auto arr_min = model.var_min(arr_id);
        auto arr_max = model.var_max(arr_id);

        // result と重ならないインデックスを削除
        if (arr_max < result_min || arr_min > result_max) {
            model.enqueue_remove_value(index_id_, idx);
            continue;
        }

        // 有効なインデックスの bounds を集約
        new_result_min = std::min(new_result_min, arr_min);
        new_result_max = std::max(new_result_max, arr_max);
    }

    if (new_result_min > new_result_max) {
        return false;  // 有効なインデックスがない
    }

    // bounds support をインラインで更新
    current_result_min_support_ = new_result_min;
    current_result_max_support_ = new_result_max;

    // result の bounds をキューに追加
    model.enqueue_set_min(result_id_, new_result_min);
    model.enqueue_set_max(result_id_, new_result_max);

    // 3. index が確定している場合、array[index] と result の bounds を同期
    if (model.is_instantiated(index_id_)) {
        auto idx = model.value(index_id_);
        auto idx_0based = index_to_0based(idx);
        if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
            auto arr_id = var_ids_[2 + static_cast<size_t>(idx_0based)];
            auto arr_min = model.var_min(arr_id);
            auto arr_max = model.var_max(arr_id);
            result_min = model.var_min(result_id_);
            result_max = model.var_max(result_id_);

            auto common_min = std::max(arr_min, result_min);
            auto common_max = std::min(arr_max, result_max);

            if (common_min > common_max) {
                return false;
            }

            model.enqueue_set_min(result_id_, common_min);
            model.enqueue_set_max(result_id_, common_max);
            model.enqueue_set_min(arr_id, common_min);
            model.enqueue_set_max(arr_id, common_max);

            // 一方が確定していれば他方も確定
            if (model.is_instantiated(arr_id)) {
                model.enqueue_instantiate(result_id_, model.value(arr_id));
            }
            if (model.is_instantiated(result_id_)) {
                model.enqueue_instantiate(arr_id, model.value(result_id_));
            }
        }
    }

    return true;
}

bool ArrayVarIntElementConstraint::on_instantiate(
    Model& model, int save_point,
    size_t /*var_idx*/, size_t /*internal_var_idx*/, Domain::value_type /*value*/,
    Domain::value_type /*prev_min*/, Domain::value_type /*prev_max*/) {

    // Trail に状態を保存
    trail_.push_back({save_point, {current_result_min_support_, current_result_max_support_}});
    model.mark_constraint_dirty(model_index(), save_point);

    // キュー経由で伝播（他の制約にも通知が届く）
    // propagate_via_queue 内で bounds support も更新される
    return propagate_via_queue(model);
}

bool ArrayVarIntElementConstraint::on_set_min(
    Model& model, int save_point,
    size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type /*new_min*/,
    Domain::value_type /*old_min*/) {

    // Trail に状態を保存
    trail_.push_back({save_point, {current_result_min_support_, current_result_max_support_}});
    model.mark_constraint_dirty(model_index(), save_point);

    // キュー経由で伝播（bounds support も更新される）
    return propagate_via_queue(model);
}

bool ArrayVarIntElementConstraint::on_set_max(
    Model& model, int save_point,
    size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type /*new_max*/,
    Domain::value_type /*old_max*/) {

    // Trail に状態を保存
    trail_.push_back({save_point, {current_result_min_support_, current_result_max_support_}});
    model.mark_constraint_dirty(model_index(), save_point);

    // キュー経由で伝播（bounds support も更新される）
    return propagate_via_queue(model);
}

bool ArrayVarIntElementConstraint::on_last_uninstantiated(
    Model& model, int /*save_point*/,
    size_t last_var_internal_idx) {

    // var_ids_ layout: [index, result, array[0], ..., array[n-1]]
    auto last_var_id = var_ids_[last_var_internal_idx];

    if (last_var_id == index_id_) {
        // index が最後の未確定変数
        // result と array 要素の共通値を持つインデックスのみ有効
        if (model.is_instantiated(index_id_)) {
            // 既に1つしかないなら確定
            auto idx_val = model.var_min(index_id_);
            model.enqueue_instantiate(index_id_, idx_val);
        }
    } else if (last_var_id == result_id_) {
        // result が最後の未確定変数で、index は確定済み
        if (model.is_instantiated(index_id_)) {
            auto idx = model.value(index_id_);
            auto idx_0based = index_to_0based(idx);

            if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
                auto arr_id = var_ids_[2 + static_cast<size_t>(idx_0based)];
                if (model.is_instantiated(arr_id)) {
                    // array[index] が確定 → result も確定
                    auto expected = model.value(arr_id);
                    if (model.contains(result_id_, expected)) {
                        model.enqueue_instantiate(result_id_, expected);
                    } else {
                        return false;
                    }
                }
            }
        }
    } else {
        // 配列要素が最後の未確定変数で、index と result は確定済み
        if (model.is_instantiated(index_id_) && model.is_instantiated(result_id_)) {
            auto idx = model.value(index_id_);
            auto idx_0based = index_to_0based(idx);

            if (idx_0based >= 0 && static_cast<size_t>(idx_0based) < n_) {
                auto arr_id = var_ids_[2 + static_cast<size_t>(idx_0based)];
                if (last_var_id == arr_id) {
                    // この配列要素を result の値に確定
                    auto expected = model.value(result_id_);
                    if (model.contains(arr_id, expected)) {
                        model.enqueue_instantiate(arr_id, expected);
                    } else {
                        return false;
                    }
                }
            }
        }
    }

    return true;
}

bool ArrayVarIntElementConstraint::on_final_instantiate(const Model& model) {
    // 全変数確定時の最終確認
    if (!model.is_instantiated(index_id_) || !model.is_instantiated(result_id_)) {
        return false;
    }

    auto idx = model.value(index_id_);
    auto idx_0based = index_to_0based(idx);

    if (idx_0based < 0 || static_cast<size_t>(idx_0based) >= n_) {
        return false;
    }

    // base var_ids_ layout: [index, result, array[0], ..., array[n-1]]
    size_t arr_var_id = var_ids_[2 + static_cast<size_t>(idx_0based)];
    if (!model.is_instantiated(arr_var_id)) {
        return false;
    }

    return model.value(arr_var_id) == model.value(result_id_);
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
