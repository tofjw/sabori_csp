#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <cstring>

namespace sabori_csp {

namespace {
// dense と sparse のメモリサイズを比較し、sparse のほうが小さい場合に true を返す。
// 同程度のサイズでは dense を選ぶ (キャッシュ局所性のため)。
//   dense: total_supports * num_words * 8 byte
//   sparse: arity * num_tuples * 4 byte (tuple index uint32) + total_flat * 4 byte (lengths)
//                                       (offsets は supports_offsets_flat_ を流用)
inline bool prefer_sparse(size_t total_supports,
                          size_t num_words,
                          size_t arity,
                          size_t num_tuples,
                          size_t total_flat) {
    const size_t dense_bytes  = total_supports * num_words * sizeof(uint64_t);
    const size_t sparse_bytes = arity * num_tuples * sizeof(uint32_t)
                              + total_flat * sizeof(uint32_t);
    // sparse が dense の半分未満ならメリットあり。
    return sparse_bytes * 2 < dense_bytes;
}
}  // namespace

// ============================================================================
// TableConstraint implementation (Compact Table algorithm)
// ============================================================================

TableConstraint::TableConstraint(std::vector<VariablePtr> vars,
                                 std::vector<Domain::value_type> flat_tuples)
    : Constraint(extract_var_ids(vars))
    , arity_(var_ids_.size())
    , flat_tuples_(std::move(flat_tuples))
    , num_tuples_(0)
    , num_words_(0) {

    if (arity_ == 0) {
        // 矛盾条件 (空配列 / 空テーブル) は presolve()/prepare_propagation() で検出される
        return;
    }

    num_tuples_ = flat_tuples_.size() / arity_;
    if (num_tuples_ == 0) {
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

    // 1パス目: 各変数×値の組み合わせを列挙して distinct 値数 (= total_supports) を求める
    size_t total_supports = 0;
    for (size_t t = 0; t < num_tuples_; ++t) {
        for (size_t v = 0; v < arity_; ++v) {
            auto val = flat_tuples_[t * arity_ + v];
            auto& info = var_support_info_[v];
            auto idx = static_cast<size_t>(val - info.min_val);
            if (supports_offsets_flat_[info.flat_offset + idx] == NO_SUPPORT) {
                // 一旦 1 を入れて「存在する」と印を付ける (後でモード別にオフセットに上書き)
                supports_offsets_flat_[info.flat_offset + idx] = 1;
                ++total_supports;
            }
        }
    }

    // モード判定
    use_sparse_ = prefer_sparse(total_supports, num_words_, arity_, num_tuples_, total_flat);

    if (use_sparse_) {
        // sparse モード: supports_offsets_flat_ にリスト開始 index、sparse_lengths_ に長さを格納
        sparse_lengths_.assign(total_flat, 0);
        // 1.5パス目: 各 (var,val) ごとに出現回数をカウント (length に蓄積)
        for (size_t t = 0; t < num_tuples_; ++t) {
            for (size_t v = 0; v < arity_; ++v) {
                auto val = flat_tuples_[t * arity_ + v];
                const auto& info = var_support_info_[v];
                auto idx = static_cast<size_t>(val - info.min_val);
                ++sparse_lengths_[info.flat_offset + idx];
            }
        }
        // 累積でオフセット決定 (NO_SUPPORT 印の位置だけ書き換え)
        size_t cumulative = 0;
        for (size_t v = 0; v < arity_; ++v) {
            const auto& info = var_support_info_[v];
            for (size_t i = 0; i < info.range_size; ++i) {
                size_t flat_idx = info.flat_offset + i;
                if (supports_offsets_flat_[flat_idx] != NO_SUPPORT) {
                    supports_offsets_flat_[flat_idx] = cumulative;
                    cumulative += sparse_lengths_[flat_idx];
                }
            }
        }
        // 2パス目: タプル index を書き込む。書き込み位置 = start + (write_pos - start)
        // sparse_lengths_ は最終長を保持しているので、別途 write カーソルを使う
        std::vector<uint32_t> write_cursor(total_flat, 0);
        sparse_supports_.assign(arity_ * num_tuples_, 0);
        for (size_t t = 0; t < num_tuples_; ++t) {
            for (size_t v = 0; v < arity_; ++v) {
                auto val = flat_tuples_[t * arity_ + v];
                const auto& info = var_support_info_[v];
                auto idx = static_cast<size_t>(val - info.min_val);
                size_t flat_idx = info.flat_offset + idx;
                size_t pos = supports_offsets_flat_[flat_idx] + write_cursor[flat_idx]++;
                sparse_supports_[pos] = static_cast<uint32_t>(t);
            }
        }
        // sparse モードでは scratch_mask_ を確保 (num_words_ 個の word)
        scratch_mask_.assign(num_words_, 0ULL);
    } else {
        // dense モード: supports_offsets_flat_ にビットセットへのオフセット (word 単位)
        size_t total_entries = 0;
        for (size_t v = 0; v < arity_; ++v) {
            const auto& info = var_support_info_[v];
            for (size_t i = 0; i < info.range_size; ++i) {
                size_t flat_idx = info.flat_offset + i;
                if (supports_offsets_flat_[flat_idx] != NO_SUPPORT) {
                    supports_offsets_flat_[flat_idx] = total_entries;
                    total_entries += num_words_;
                }
            }
        }
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
    }

    // current_table_ を全1で初期化（末尾ワードの余剰ビットはクリア）
    current_table_.assign(num_words_, ~0ULL);
    size_t remainder = num_tuples_ % 64;
    if (remainder != 0) {
        current_table_[num_words_ - 1] = (1ULL << remainder) - 1;
    }

    // last_nz_word_ 初期化（全ビットが立っているので末尾ワード）
    last_nz_word_ = num_words_ > 0 ? num_words_ - 1 : 0;

    // residual 初期化 (モードに応じて使い分け)
    if (use_sparse_) {
        sparse_residual_idx_.assign(total_flat, 0);
    } else {
        residual_words_.assign(total_flat, 0);
    }

    // word_saved_at_ 初期化
    word_saved_at_.assign(num_words_, 0);

    // word_modified_at_ 初期化
    word_modified_at_.assign(num_words_, 0);

}

std::string TableConstraint::name() const {
    return "table_int";
}

PresolveResult TableConstraint::presolve(Model& model) {
    bool changed = false;
    // テーブルに存在しない値をドメインから直接除去
    // bounds-only 対応: テーブルの値範囲で反復し、ドメイン外の値をスキップ
    for (size_t v = 0; v < arity_; ++v) {
        auto var = model.variable(var_ids_[v]);
        auto& dom = var->domain();
        const auto& info = var_support_info_[v];
        auto table_max = info.min_val + static_cast<Domain::value_type>(info.range_size - 1);

        // テーブル範囲外の値を一括除去
        if (info.min_val > dom.min().value_or(0)) changed = true;
        if (!dom.remove_below(info.min_val)) return PresolveResult::Contradiction;
        if (table_max < dom.max().value_or(0)) changed = true;
        if (!dom.remove_above(table_max)) return PresolveResult::Contradiction;

        // テーブル範囲内でサポートのない値を除去
        auto dom_min = dom.min().value_or(0);
        auto dom_max = dom.max().value_or(0);
        for (size_t i = 0; i < info.range_size; ++i) {
            auto val = info.min_val + static_cast<Domain::value_type>(i);
            if (val < dom_min || val > dom_max) continue;
            if (!dom.contains(val)) continue;
            if (supports_offsets_flat_[info.flat_offset + i] == NO_SUPPORT) {
                dom.remove(val);
                changed = true;
                if (dom.empty()) return PresolveResult::Contradiction;
            }
        }
    }
    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool TableConstraint::prepare_propagation(Model& model) {
    // 退化ケース: arity 0 またはテーブルが空 → 充足不可能
    if (arity_ == 0 || num_tuples_ == 0) return false;

    // current_table_ を再構築: 各変数の現ドメインに基づいてフィルタ
    current_table_.assign(num_words_, ~0ULL);
    size_t remainder = num_tuples_ % 64;
    if (remainder != 0) {
        current_table_[num_words_ - 1] = (1ULL << remainder) - 1;
    }

    std::vector<uint64_t> var_union(num_words_, 0ULL);
    for (size_t v = 0; v < arity_; ++v) {
        // この変数のドメインに残っている値のsupportsをunion
        // bounds-only 対応: テーブルの値範囲で反復し、model.contains() でチェック
        std::fill(var_union.begin(), var_union.end(), 0ULL);
        auto v_id = var_ids_[v];
        const auto& info = var_support_info_[v];
        auto dom_min = model.var_min(v_id);
        auto dom_max = model.var_max(v_id);
        for (size_t i = 0; i < info.range_size; ++i) {
            size_t flat_idx = info.flat_offset + i;
            size_t offset = supports_offsets_flat_[flat_idx];
            if (offset == NO_SUPPORT) continue;
            auto val = info.min_val + static_cast<Domain::value_type>(i);
            if (val < dom_min || val > dom_max) continue;
            if (!model.contains(v_id, val)) continue;
            if (use_sparse_) {
                uint32_t len = sparse_lengths_[flat_idx];
                const uint32_t* tlist = sparse_supports_.data() + offset;
                for (uint32_t k = 0; k < len; ++k) {
                    uint32_t t = tlist[k];
                    var_union[t >> 6] |= 1ULL << (t & 63);
                }
            } else {
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

    if (use_sparse_) {
        // sparse: scratch_mask_ にこの (var,val) のサポート bitset を構築 → AND
        // current_table_ の last_nz_word_ までを対象にする
        const size_t limit_w = last_nz_word_ + 1;
        std::memset(scratch_mask_.data(), 0, limit_w * sizeof(uint64_t));
        // sparse_supports_ はソート済みなので t/64 > last_nz_word_ になったら break
        const auto& info = var_support_info_[internal_idx];
        size_t flat_idx = info.flat_offset + static_cast<size_t>(value - info.min_val);
        uint32_t len = sparse_lengths_[flat_idx];
        const uint32_t* tlist = sparse_supports_.data() + offset;
        for (uint32_t k = 0; k < len; ++k) {
            uint32_t t = tlist[k];
            size_t w = t >> 6;
            if (w >= limit_w) break;
            scratch_mask_[w] |= 1ULL << (t & 63);
        }
        for (size_t w = 0; w < limit_w; ++w) {
            uint64_t new_val = current_table_[w] & scratch_mask_[w];
            if (new_val != current_table_[w]) {
                save_word(w);
                word_modified_at_[w] = filter_gen_;
                current_table_[w] = new_val;
            }
        }
    } else {
        for (size_t w = 0; w <= last_nz_word_; ++w) {
            uint64_t new_val = current_table_[w] & supports_data_[offset + w];
            if (new_val != current_table_[w]) {
                save_word(w);
                word_modified_at_[w] = filter_gen_;
                current_table_[w] = new_val;
            }
        }
    }
    // Shrink last_nz_word_
    while (last_nz_word_ > 0 && current_table_[last_nz_word_] == 0) {
        --last_nz_word_;
    }

    if (table_is_empty()) return false;

    return filter_domains(model, static_cast<int>(internal_idx));
}

bool TableConstraint::on_final_instantiate(const Model& model) {
    // 全変数が確定: 割当がテーブル内のタプルと一致するか直接チェック
    for (size_t t = 0; t < num_tuples_; ++t) {
        bool match = true;
        for (size_t v = 0; v < arity_; ++v) {
            if (model.value(var_ids_[v]) != flat_tuples_[t * arity_ + v]) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
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
    const auto& info = var_support_info_[last_var_internal_idx];
    auto dom_min = model.var_min(last_var_id);
    auto dom_max = model.var_max(last_var_id);
    for (size_t i = 0; i < info.range_size; ++i) {
        auto val = info.min_val + static_cast<Domain::value_type>(i);
        if (val < dom_min || val > dom_max) continue;
        if (!model.contains(last_var_id, val)) continue;
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
    if (get_support_offset(internal_idx, removed_value) == NO_SUPPORT) return true;

    ++filter_gen_;
    save_trail_if_needed(model, save_point);

    clear_supports_for(internal_idx, removed_value);

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
        if (get_support_offset(internal_idx, val) == NO_SUPPORT) continue;
        if (!changed) {
            ++filter_gen_;
            save_trail_if_needed(model, save_point);
            changed = true;
        }
        clear_supports_for(internal_idx, val);
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
        if (get_support_offset(internal_idx, val) == NO_SUPPORT) continue;
        if (!changed) {
            ++filter_gen_;
            save_trail_if_needed(model, save_point);
            changed = true;
        }
        clear_supports_for(internal_idx, val);
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

bool TableConstraint::clear_supports_for(size_t internal_idx, Domain::value_type val) {
    size_t offset = get_support_offset(internal_idx, val);
    if (offset == NO_SUPPORT) return false;

    bool any_changed = false;
    if (use_sparse_) {
        const auto& info = var_support_info_[internal_idx];
        size_t flat_idx = info.flat_offset + static_cast<size_t>(val - info.min_val);
        uint32_t len = sparse_lengths_[flat_idx];
        const uint32_t* tlist = sparse_supports_.data() + offset;
        const size_t limit_w = last_nz_word_ + 1;
        for (uint32_t k = 0; k < len; ++k) {
            uint32_t t = tlist[k];
            size_t w = t >> 6;
            if (w >= limit_w) break;
            uint64_t bit = 1ULL << (t & 63);
            if (current_table_[w] & bit) {
                save_word(w);
                word_modified_at_[w] = filter_gen_;
                current_table_[w] &= ~bit;
                any_changed = true;
            }
        }
    } else {
        for (size_t w = 0; w <= last_nz_word_; ++w) {
            uint64_t new_val = current_table_[w] & ~supports_data_[offset + w];
            if (new_val != current_table_[w]) {
                save_word(w);
                word_modified_at_[w] = filter_gen_;
                current_table_[w] = new_val;
                any_changed = true;
            }
        }
    }
    return any_changed;
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

        const auto& info = var_support_info_[v];

        // bounds-only 対応: テーブルの値範囲で反復し、model.contains() でチェック
        auto dom_min = model.var_min(v_id);
        auto dom_max = model.var_max(v_id);
        for (size_t i = 0; i < info.range_size; ++i) {
            auto val = info.min_val + static_cast<Domain::value_type>(i);
            if (val < dom_min || val > dom_max) continue;
            if (!model.contains(v_id, val)) continue;

            size_t flat_idx = info.flat_offset + i;
            size_t offset = supports_offsets_flat_[flat_idx];
            if (offset == NO_SUPPORT) {
                model.enqueue_remove_value(v_id, val);
                continue;
            }

            if (use_sparse_) {
                // residual: 前回サポートだった T 内 index
                uint32_t len = sparse_lengths_[flat_idx];
                const uint32_t* tlist = sparse_supports_.data() + offset;
                uint32_t res_k = sparse_residual_idx_[flat_idx];
                if (res_k < len) {
                    uint32_t t = tlist[res_k];
                    size_t res_w = t >> 6;
                    // word が今ラウンドで変更されていないなら residual はまだ生きている
                    if (word_modified_at_[res_w] != filter_gen_) {
                        continue;
                    }
                    // 変更されたが、bit がまだ立っていればOK
                    if (res_w <= last_nz_word_ &&
                        (current_table_[res_w] & (1ULL << (t & 63)))) {
                        continue;
                    }
                }
                // Full scan: 生きているタプルを探す (sorted なので word が last_nz_word_ を超えたら break)
                bool found = false;
                const size_t limit_w = last_nz_word_ + 1;
                for (uint32_t k = 0; k < len; ++k) {
                    uint32_t t = tlist[k];
                    size_t w = t >> 6;
                    if (w >= limit_w) break;
                    if (current_table_[w] & (1ULL << (t & 63))) {
                        sparse_residual_idx_[flat_idx] = k;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    model.enqueue_remove_value(v_id, val);
                }
            } else {
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

    if (use_sparse_) {
        uint32_t len = sparse_lengths_[flat_idx];
        const uint32_t* tlist = sparse_supports_.data() + offset;
        const size_t limit_w = last_nz_word_ + 1;

        // Residual check
        uint32_t res_k = sparse_residual_idx_[flat_idx];
        if (res_k < len) {
            uint32_t t = tlist[res_k];
            size_t res_w = t >> 6;
            if (res_w < limit_w &&
                (current_table_[res_w] & (1ULL << (t & 63)))) {
                return true;
            }
        }

        // Full scan
        for (uint32_t k = 0; k < len; ++k) {
            uint32_t t = tlist[k];
            size_t w = t >> 6;
            if (w >= limit_w) break;
            if (current_table_[w] & (1ULL << (t & 63))) {
                sparse_residual_idx_[flat_idx] = k;
                return true;
            }
        }
        return false;
    }

    // dense
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
