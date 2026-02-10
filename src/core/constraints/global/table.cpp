#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>

namespace sabori_csp {

// ============================================================================
// TableConstraint implementation (Compact Table algorithm)
// ============================================================================

TableConstraint::TableConstraint(std::vector<VariablePtr> vars,
                                 std::vector<Domain::value_type> flat_tuples)
    : Constraint(vars)
    , arity_(vars.size())
    , flat_tuples_(std::move(flat_tuples))
    , num_tuples_(0)
    , num_words_(0) {

    if (arity_ == 0) {
        set_initially_inconsistent(true);
        return;
    }

    num_tuples_ = flat_tuples_.size() / arity_;
    if (num_tuples_ == 0) {
        set_initially_inconsistent(true);
        return;
    }

    num_words_ = (num_tuples_ + 63) / 64;

    // var_ptr_to_idx_ 構築
    for (size_t i = 0; i < vars_.size(); ++i) {
        var_ptr_to_idx_[vars_[i].get()] = i;
    }

    // supports_offsets_ と supports_data_ の構築
    supports_offsets_.resize(arity_);

    // 1パス目: 各変数×値の組み合わせを列挙してオフセットを割り当て
    size_t total_entries = 0;
    for (size_t t = 0; t < num_tuples_; ++t) {
        for (size_t v = 0; v < arity_; ++v) {
            auto val = flat_tuples_[t * arity_ + v];
            if (supports_offsets_[v].find(val) == supports_offsets_[v].end()) {
                supports_offsets_[v][val] = total_entries;
                total_entries += num_words_;
            }
        }
    }

    // supports_data_ を 0 で初期化
    supports_data_.assign(total_entries, 0ULL);

    // 2パス目: ビットをセット
    for (size_t t = 0; t < num_tuples_; ++t) {
        size_t word_idx = t / 64;
        uint64_t bit = 1ULL << (t % 64);
        for (size_t v = 0; v < arity_; ++v) {
            auto val = flat_tuples_[t * arity_ + v];
            size_t offset = supports_offsets_[v][val];
            supports_data_[offset + word_idx] |= bit;
        }
    }

    // current_table_ を全1で初期化（末尾ワードの余剰ビットはクリア）
    current_table_.assign(num_words_, ~0ULL);
    size_t remainder = num_tuples_ % 64;
    if (remainder != 0) {
        current_table_[num_words_ - 1] = (1ULL << remainder) - 1;
    }

    check_initial_consistency();
}

std::string TableConstraint::name() const {
    return "table_int";
}

std::vector<VariablePtr> TableConstraint::variables() const {
    return vars_;
}

std::optional<bool> TableConstraint::is_satisfied() const {
    // 全変数が確定している場合のみ判定可能
    for (const auto& var : vars_) {
        if (!var->is_assigned()) {
            return std::nullopt;
        }
    }
    // 割り当てタプルがテーブル内にあるかチェック
    for (size_t t = 0; t < num_tuples_; ++t) {
        bool match = true;
        for (size_t v = 0; v < arity_; ++v) {
            if (vars_[v]->assigned_value().value() != flat_tuples_[t * arity_ + v]) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

bool TableConstraint::presolve(Model& model) {
    // テーブルに存在しない値をドメインから直接除去
    for (size_t v = 0; v < arity_; ++v) {
        auto& dom = vars_[v]->domain();
        auto vals = dom.values();
        for (auto val : vals) {
            if (supports_offsets_[v].find(val) == supports_offsets_[v].end()) {
                dom.remove(val);
                if (dom.empty()) return false;
            }
        }
    }
    return true;
}

bool TableConstraint::prepare_propagation(Model& model) {
    // current_table_ を再構築: 各変数の現ドメインに基づいてフィルタ
    current_table_.assign(num_words_, ~0ULL);
    size_t remainder = num_tuples_ % 64;
    if (remainder != 0) {
        current_table_[num_words_ - 1] = (1ULL << remainder) - 1;
    }

    for (size_t v = 0; v < arity_; ++v) {
        // この変数のドメインに残っている値のsupportsをunion
        std::vector<uint64_t> var_union(num_words_, 0ULL);
        const auto& dom = vars_[v]->domain();
        for (auto it = dom.begin(); it != dom.end(); ++it) {
            auto sit = supports_offsets_[v].find(*it);
            if (sit != supports_offsets_[v].end()) {
                size_t offset = sit->second;
                for (size_t w = 0; w < num_words_; ++w) {
                    var_union[w] |= supports_data_[offset + w];
                }
            }
        }
        // current_table_ &= var_union
        for (size_t w = 0; w < num_words_; ++w) {
            current_table_[w] &= var_union[w];
        }
    }

    if (table_is_empty()) return false;

    trail_.clear();
    init_watches();
    return true;
}

bool TableConstraint::on_instantiate(Model& model, int save_point,
                                     size_t var_idx, Domain::value_type value,
                                     Domain::value_type prev_min,
                                     Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, value, prev_min, prev_max)) {
        return false;
    }

    Variable* var_ptr = model.variable(var_idx).get();
    auto it = var_ptr_to_idx_.find(var_ptr);
    if (it == var_ptr_to_idx_.end()) return true;

    size_t internal_idx = it->second;

    // supports に値があるか確認
    auto sit = supports_offsets_[internal_idx].find(value);
    if (sit == supports_offsets_[internal_idx].end()) return false;

    save_trail_if_needed(model, save_point);

    // current_table_ &= supports[internal_idx][value]
    size_t offset = sit->second;
    for (size_t w = 0; w < num_words_; ++w) {
        current_table_[w] &= supports_data_[offset + w];
    }

    if (table_is_empty()) return false;

    return filter_domains(model, static_cast<int>(internal_idx));
}

bool TableConstraint::on_final_instantiate() {
    auto result = is_satisfied();
    return result.has_value() && result.value();
}

bool TableConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                              size_t last_var_internal_idx) {
    auto& last_var = vars_[last_var_internal_idx];
    if (last_var->is_assigned()) {
        // 既に確定: サポートがあるか確認
        return has_support(last_var_internal_idx, last_var->assigned_value().value());
    }

    // 最後の未確定変数のドメインをフィルタリング
    const auto& dom = last_var->domain();
    for (auto it = dom.begin(); it != dom.end(); ++it) {
        if (!has_support(last_var_internal_idx, *it)) {
            model.enqueue_remove_value(last_var->id(), *it);
        }
    }
    return true;
}

bool TableConstraint::on_remove_value(Model& model, int save_point,
                                       size_t var_idx, Domain::value_type removed_value) {
    Variable* var_ptr = model.variable(var_idx).get();
    auto it = var_ptr_to_idx_.find(var_ptr);
    if (it == var_ptr_to_idx_.end()) return true;

    size_t internal_idx = it->second;

    // supports にその値がなければ何もしない
    auto sit = supports_offsets_[internal_idx].find(removed_value);
    if (sit == supports_offsets_[internal_idx].end()) return true;

    save_trail_if_needed(model, save_point);

    // current_table_ &= ~supports[internal_idx][removed_value]
    size_t offset = sit->second;
    for (size_t w = 0; w < num_words_; ++w) {
        current_table_[w] &= ~supports_data_[offset + w];
    }

    if (table_is_empty()) return false;

    return filter_domains(model, -1);
}

bool TableConstraint::on_set_min(Model& model, int save_point,
                                  size_t var_idx, Domain::value_type new_min,
                                  Domain::value_type old_min) {
    Variable* var_ptr = model.variable(var_idx).get();
    auto it = var_ptr_to_idx_.find(var_ptr);
    if (it == var_ptr_to_idx_.end()) return true;

    size_t internal_idx = it->second;
    bool changed = false;

    // 範囲外の値（old_min から new_min-1）の supports を NOT して AND
    for (auto& [val, offset] : supports_offsets_[internal_idx]) {
        if (val >= old_min && val < new_min) {
            if (!changed) {
                save_trail_if_needed(model, save_point);
                changed = true;
            }
            for (size_t w = 0; w < num_words_; ++w) {
                current_table_[w] &= ~supports_data_[offset + w];
            }
        }
    }

    if (changed) {
        if (table_is_empty()) return false;
        return filter_domains(model, -1);
    }
    return true;
}

bool TableConstraint::on_set_max(Model& model, int save_point,
                                  size_t var_idx, Domain::value_type new_max,
                                  Domain::value_type old_max) {
    Variable* var_ptr = model.variable(var_idx).get();
    auto it = var_ptr_to_idx_.find(var_ptr);
    if (it == var_ptr_to_idx_.end()) return true;

    size_t internal_idx = it->second;
    bool changed = false;

    // 範囲外の値（new_max+1 から old_max）の supports を NOT して AND
    for (auto& [val, offset] : supports_offsets_[internal_idx]) {
        if (val > new_max && val <= old_max) {
            if (!changed) {
                save_trail_if_needed(model, save_point);
                changed = true;
            }
            for (size_t w = 0; w < num_words_; ++w) {
                current_table_[w] &= ~supports_data_[offset + w];
            }
        }
    }

    if (changed) {
        if (table_is_empty()) return false;
        return filter_domains(model, -1);
    }
    return true;
}

void TableConstraint::rewind_to(int save_point) {
    while (!trail_.empty() && trail_.back().first > save_point) {
        current_table_ = std::move(trail_.back().second.old_table);
        trail_.pop_back();
    }
}

void TableConstraint::check_initial_consistency() {
    if (flat_tuples_.empty() || arity_ == 0) {
        set_initially_inconsistent(true);
        return;
    }
    // テーブルが空かチェック（初期状態では全タプルが有効なので空にはならない）
    // ただし、変数のドメインに存在しない値しかテーブルにない場合は矛盾
    for (size_t v = 0; v < arity_; ++v) {
        bool has_any = false;
        for (const auto& [val, offset] : supports_offsets_[v]) {
            if (vars_[v]->domain().contains(val)) {
                has_any = true;
                break;
            }
        }
        if (!has_any) {
            set_initially_inconsistent(true);
            return;
        }
    }
}

void TableConstraint::save_trail_if_needed(Model& model, int save_point) {
    if (trail_.empty() || trail_.back().first != save_point) {
        trail_.push_back({save_point, {current_table_}});
        model.mark_constraint_dirty(model_index(), save_point);
    }
}

bool TableConstraint::filter_domains(Model& model, int skip_var_idx) {
    for (size_t v = 0; v < arity_; ++v) {
        if (static_cast<int>(v) == skip_var_idx) continue;
        if (vars_[v]->is_assigned()) continue;

        const auto& dom = vars_[v]->domain();
        for (auto it = dom.begin(); it != dom.end(); ++it) {
            if (!has_support(v, *it)) {
                model.enqueue_remove_value(vars_[v]->id(), *it);
            }
        }
    }
    return true;
}

bool TableConstraint::has_support(size_t var_idx, Domain::value_type value) const {
    auto it = supports_offsets_[var_idx].find(value);
    if (it == supports_offsets_[var_idx].end()) return false;

    size_t offset = it->second;
    for (size_t w = 0; w < num_words_; ++w) {
        if (supports_data_[offset + w] & current_table_[w]) {
            return true;
        }
    }
    return false;
}

bool TableConstraint::table_is_empty() const {
    for (size_t w = 0; w < num_words_; ++w) {
        if (current_table_[w] != 0) return false;
    }
    return true;
}

}  // namespace sabori_csp
