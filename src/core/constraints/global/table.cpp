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

    // var_support_info_ の構築: 各変数の min/max を求める
    var_support_info_.resize(arity_);
    for (size_t v = 0; v < arity_; ++v) {
        auto val0 = flat_tuples_[v];
        Domain::value_type vmin = val0, vmax = val0;
        for (size_t t = 1; t < num_tuples_; ++t) {
            auto val = flat_tuples_[t * arity_ + v];
            if (val < vmin) vmin = val;
            if (val > vmax) vmax = val;
        }
        var_support_info_[v].min_val = vmin;
        var_support_info_[v].range_size = static_cast<size_t>(vmax - vmin + 1);
    }

    // supports_offsets_flat_ の構築
    size_t total_flat = 0;
    for (size_t v = 0; v < arity_; ++v) {
        var_support_info_[v].flat_offset = total_flat;
        total_flat += var_support_info_[v].range_size;
    }
    supports_offsets_flat_.assign(total_flat, NO_SUPPORT);

    // 1パス目: 各変数×値の組み合わせを列挙してオフセットを割り当て
    size_t total_entries = 0;
    for (size_t t = 0; t < num_tuples_; ++t) {
        for (size_t v = 0; v < arity_; ++v) {
            auto val = flat_tuples_[t * arity_ + v];
            auto& info = var_support_info_[v];
            auto idx = static_cast<size_t>(val - info.min_val);
            if (supports_offsets_flat_[info.flat_offset + idx] == NO_SUPPORT) {
                supports_offsets_flat_[info.flat_offset + idx] = total_entries;
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
            size_t offset = get_support_offset(v, val);
            supports_data_[offset + word_idx] |= bit;
        }
    }

    // current_table_ を全1で初期化（末尾ワードの余剰ビットはクリア）
    current_table_.assign(num_words_, ~0ULL);
    size_t remainder = num_tuples_ % 64;
    if (remainder != 0) {
        current_table_[num_words_ - 1] = (1ULL << remainder) - 1;
    }

    // last_nz_word_ 初期化（全ビットが立っているので末尾ワード）
    last_nz_word_ = num_words_ > 0 ? num_words_ - 1 : 0;

    // residual_words_ 初期化（全エントリを 0 で初期化）
    residual_words_.assign(total_flat, 0);

    // word_saved_at_ 初期化
    word_saved_at_.assign(num_words_, 0);

    // word_modified_at_ 初期化
    word_modified_at_.assign(num_words_, 0);

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
    // bounds-only 対応: テーブルの値範囲で反復し、ドメイン外の値をスキップ
    for (size_t v = 0; v < arity_; ++v) {
        auto& dom = vars_[v]->domain();
        const auto& info = var_support_info_[v];
        auto table_max = info.min_val + static_cast<Domain::value_type>(info.range_size - 1);

        // テーブル範囲外の値を一括除去
        if (!dom.remove_below(info.min_val)) return false;
        if (!dom.remove_above(table_max)) return false;

        // テーブル範囲内でサポートのない値を除去
        auto dom_min = dom.min().value_or(0);
        auto dom_max = dom.max().value_or(0);
        for (size_t i = 0; i < info.range_size; ++i) {
            auto val = info.min_val + static_cast<Domain::value_type>(i);
            if (val < dom_min || val > dom_max) continue;
            if (!dom.contains(val)) continue;
            if (supports_offsets_flat_[info.flat_offset + i] == NO_SUPPORT) {
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
        // bounds-only 対応: テーブルの値範囲で反復し、dom.contains() でチェック
        std::vector<uint64_t> var_union(num_words_, 0ULL);
        const auto& dom = vars_[v]->domain();
        const auto& info = var_support_info_[v];
        auto dom_min = dom.min().value_or(0);
        auto dom_max = dom.max().value_or(0);
        for (size_t i = 0; i < info.range_size; ++i) {
            size_t offset = supports_offsets_flat_[info.flat_offset + i];
            if (offset == NO_SUPPORT) continue;
            auto val = info.min_val + static_cast<Domain::value_type>(i);
            if (val < dom_min || val > dom_max) continue;
            if (!dom.contains(val)) continue;
            for (size_t w = 0; w < num_words_; ++w) {
                var_union[w] |= supports_data_[offset + w];
            }
        }
        // current_table_ &= var_union
        for (size_t w = 0; w < num_words_; ++w) {
            current_table_[w] &= var_union[w];
        }
    }

    // last_nz_word_ を再計算
    last_nz_word_ = 0;
    for (size_t w = num_words_; w > 0; --w) {
        if (current_table_[w - 1] != 0) {
            last_nz_word_ = w - 1;
            break;
        }
    }

    if (table_is_empty()) return false;

    trail_.clear();
    trail_generation_ = 0;
    std::fill(word_saved_at_.begin(), word_saved_at_.end(), 0);
    filter_gen_ = 0;
    std::fill(word_modified_at_.begin(), word_modified_at_.end(), 1);
    init_watches();
    return true;
}

bool TableConstraint::on_instantiate(Model& model, int save_point,
                                     size_t var_idx, size_t internal_var_idx,
                                     Domain::value_type value,
                                     Domain::value_type prev_min,
                                     Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value, prev_min, prev_max)) {
        return false;
    }

    size_t internal_idx = internal_var_idx;

    // supports に値があるか確認
    size_t offset = get_support_offset(internal_idx, value);
    if (offset == NO_SUPPORT) return false;

    ++filter_gen_;
    save_trail_if_needed(model, save_point);
    for (size_t w = 0; w <= last_nz_word_; ++w) {
        uint64_t new_val = current_table_[w] & supports_data_[offset + w];
        if (new_val != current_table_[w]) {
            save_word(w);
            word_modified_at_[w] = filter_gen_;
            current_table_[w] = new_val;
        }
    }
    // Shrink last_nz_word_
    while (last_nz_word_ > 0 && current_table_[last_nz_word_] == 0) {
        --last_nz_word_;
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
    auto last_var_id = var_ids_[last_var_internal_idx];
    if (model.is_instantiated(last_var_id)) {
        // 既に確定: サポートがあるか確認
        return has_support(last_var_internal_idx, model.value(last_var_id));
    }

    // 最後の未確定変数のドメインをフィルタリング
    // bounds-only 対応: テーブルの値範囲で反復
    const auto& dom = vars_[last_var_internal_idx]->domain();
    const auto& info = var_support_info_[last_var_internal_idx];
    auto dom_min = model.var_min(last_var_id);
    auto dom_max = model.var_max(last_var_id);
    for (size_t i = 0; i < info.range_size; ++i) {
        auto val = info.min_val + static_cast<Domain::value_type>(i);
        if (val < dom_min || val > dom_max) continue;
        if (!dom.contains(val)) continue;
        if (!has_support(last_var_internal_idx, val)) {
            model.enqueue_remove_value(last_var_id, val);
        }
    }
    return true;
}

bool TableConstraint::on_remove_value(Model& model, int save_point,
                                       size_t var_idx, size_t internal_var_idx,
                                       Domain::value_type removed_value) {
    size_t internal_idx = internal_var_idx;

    // supports にその値がなければ何もしない
    size_t offset = get_support_offset(internal_idx, removed_value);
    if (offset == NO_SUPPORT) return true;

    ++filter_gen_;
    save_trail_if_needed(model, save_point);
    for (size_t w = 0; w <= last_nz_word_; ++w) {
        uint64_t new_val = current_table_[w] & ~supports_data_[offset + w];
        if (new_val != current_table_[w]) {
            save_word(w);
            word_modified_at_[w] = filter_gen_;
            current_table_[w] = new_val;
        }
    }
    // Shrink last_nz_word_
    while (last_nz_word_ > 0 && current_table_[last_nz_word_] == 0) {
        --last_nz_word_;
    }

    if (table_is_empty()) return false;

    return filter_domains(model, -1);
}

bool TableConstraint::on_set_min(Model& model, int save_point,
                                  size_t var_idx, size_t internal_var_idx,
                                  Domain::value_type new_min,
                                  Domain::value_type old_min) {
    size_t internal_idx = internal_var_idx;
    bool changed = false;

    // 範囲外の値（old_min から new_min-1）の supports を NOT して AND
    for (auto val = old_min; val < new_min; ++val) {
        size_t offset = get_support_offset(internal_idx, val);
        if (offset != NO_SUPPORT) {
            if (!changed) {
                ++filter_gen_;
                save_trail_if_needed(model, save_point);
                changed = true;
            }
            for (size_t w = 0; w <= last_nz_word_; ++w) {
                uint64_t new_val = current_table_[w] & ~supports_data_[offset + w];
                if (new_val != current_table_[w]) {
                    save_word(w);
                    word_modified_at_[w] = filter_gen_;
                    current_table_[w] = new_val;
                }
            }
        }
    }

    if (changed) {
        // Shrink last_nz_word_
        while (last_nz_word_ > 0 && current_table_[last_nz_word_] == 0) {
            --last_nz_word_;
        }
        if (table_is_empty()) return false;
        return filter_domains(model, -1);
    }
    return true;
}

bool TableConstraint::on_set_max(Model& model, int save_point,
                                  size_t var_idx, size_t internal_var_idx,
                                  Domain::value_type new_max,
                                  Domain::value_type old_max) {
    size_t internal_idx = internal_var_idx;
    bool changed = false;

    // 範囲外の値（new_max+1 から old_max）の supports を NOT して AND
    for (auto val = new_max + 1; val <= old_max; ++val) {
        size_t offset = get_support_offset(internal_idx, val);
        if (offset != NO_SUPPORT) {
            if (!changed) {
                ++filter_gen_;
                save_trail_if_needed(model, save_point);
                changed = true;
            }
            for (size_t w = 0; w <= last_nz_word_; ++w) {
                uint64_t new_val = current_table_[w] & ~supports_data_[offset + w];
                if (new_val != current_table_[w]) {
                    save_word(w);
                    word_modified_at_[w] = filter_gen_;
                    current_table_[w] = new_val;
                }
            }
        }
    }

    if (changed) {
        // Shrink last_nz_word_
        while (last_nz_word_ > 0 && current_table_[last_nz_word_] == 0) {
            --last_nz_word_;
        }
        if (table_is_empty()) return false;
        return filter_domains(model, -1);
    }
    return true;
}

void TableConstraint::rewind_to(int save_point) {
    while (!trail_.empty() && trail_.back().first > save_point) {
        for (auto& [w, old_val] : trail_.back().second.word_diffs) {
            current_table_[w] = old_val;
        }
        last_nz_word_ = trail_.back().second.old_last_nz_word;
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
        const auto& info = var_support_info_[v];
        for (size_t i = 0; i < info.range_size; ++i) {
            if (supports_offsets_flat_[info.flat_offset + i] != NO_SUPPORT) {
                auto val = info.min_val + static_cast<Domain::value_type>(i);
                if (vars_[v]->domain().contains(val)) {
                    has_any = true;
                    break;
                }
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
        ++trail_generation_;
        trail_.push_back({save_point, {{}, last_nz_word_}});
        model.mark_constraint_dirty(model_index(), save_point);
    }
}

bool TableConstraint::filter_domains(Model& model, int skip_var_idx) {
    for (size_t v = 0; v < arity_; ++v) {
        if (static_cast<int>(v) == skip_var_idx) continue;
        auto v_id = var_ids_[v];
        if (model.is_instantiated(v_id)) continue;

        const auto& dom = vars_[v]->domain();
        const auto& info = var_support_info_[v];

        // bounds-only 対応: テーブルの値範囲で反復し、dom.contains() でチェック
        auto dom_min = model.var_min(v_id);
        auto dom_max = model.var_max(v_id);
        for (size_t i = 0; i < info.range_size; ++i) {
            auto val = info.min_val + static_cast<Domain::value_type>(i);
            if (val < dom_min || val > dom_max) continue;
            if (!dom.contains(val)) continue;

            size_t flat_idx = info.flat_offset + i;
            size_t offset = supports_offsets_flat_[flat_idx];
            if (offset == NO_SUPPORT) {
                model.enqueue_remove_value(v_id, val);
                continue;
            }

            // Generation skip: residual word 未変更ならサポート確定
            size_t res_w = residual_words_[flat_idx];
            if (word_modified_at_[res_w] != filter_gen_) {
                continue;
            }

            // Residual word 変更済みだが overlap が残っているかチェック
            if (res_w <= last_nz_word_ &&
                (supports_data_[offset + res_w] & current_table_[res_w])) {
                continue;
            }

            // Full scan
            bool found = false;
            for (size_t w = 0; w <= last_nz_word_; ++w) {
                if (supports_data_[offset + w] & current_table_[w]) {
                    residual_words_[flat_idx] = w;
                    found = true;
                    break;
                }
            }
            if (!found) {
                model.enqueue_remove_value(v_id, val);
            }
        }
    }
    return true;
}

bool TableConstraint::has_support(size_t var_idx, Domain::value_type value) const {
    const auto& info = var_support_info_[var_idx];
    auto diff = value - info.min_val;
    if (diff < 0 || static_cast<size_t>(diff) >= info.range_size) return false;
    size_t flat_idx = info.flat_offset + static_cast<size_t>(diff);
    size_t offset = supports_offsets_flat_[flat_idx];
    if (offset == NO_SUPPORT) return false;

    // Residual check: 前回サポートが見つかった word を先にチェック
    size_t res_w = residual_words_[flat_idx];
    if (res_w <= last_nz_word_ &&
        (supports_data_[offset + res_w] & current_table_[res_w])) {
        return true;
    }

    // Full scan (last_nz_word_ までに制限)
    size_t limit = last_nz_word_ + 1;
    for (size_t w = 0; w < limit; ++w) {
        if (supports_data_[offset + w] & current_table_[w]) {
            residual_words_[flat_idx] = w;
            return true;
        }
    }
    return false;
}

bool TableConstraint::table_is_empty() const {
    return current_table_[last_nz_word_] == 0;
}

}  // namespace sabori_csp
