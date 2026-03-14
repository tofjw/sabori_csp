#include "sabori_csp/model.hpp"
#include <stdexcept>
#include <algorithm>
#include <limits>
#include <iostream>
#include <cassert>

namespace sabori_csp {

Variable* Model::create_variable(std::string name, Domain domain) {
    auto var = std::make_unique<Variable>(std::move(name), std::move(domain));
    Variable* ptr = var.get();
    add_variable(std::move(var));
    return ptr;
}

Variable* Model::create_variable(std::string name, Domain::value_type value) {
    return create_variable(std::move(name), Domain(value, value));
}

Variable* Model::create_variable(std::string name, Domain::value_type min, Domain::value_type max) {
    return create_variable(std::move(name), Domain(min, max));
}

Variable* Model::create_variable(std::string name, std::vector<Domain::value_type> values) {
    return create_variable(std::move(name), Domain(std::move(values)));
}

size_t Model::add_variable(std::unique_ptr<Variable> var) {
    size_t id = next_var_id_++;
    var->set_id(id);
    var->set_model(this);
    name_to_id_[var->name()] = id;
    Variable* p = var.get();
    variables_.push_back(std::move(var));

    VarData vd;
    vd.min = p->min();
    vd.max = p->max();
    vd.size = p->domain().size();
    vd.initial_range = p->domain().initial_range();
    vd.is_defined_var = false;
    vd.last_saved_level = -1;

    if (p->domain().is_bounds_only()) {
        // bounds-only: support_value を中央値で初期化
        vd.support_value = (vd.min + vd.max) / 2;
    } else {
        // support_value を初期化（dense 配列の中央値）
        const auto& vals = p->domain().values_ref();
        size_t n = p->domain().n();
        vd.support_value = vals[n / 2];
    }
    var_data_.push_back(vd);

    // 初期状態で instantiated ならカウント
    if (vd.min == vd.max) {
        instantiated_count_++;
    }

    return id;
}

void Model::set_defined_var(size_t var_idx) {
    var_data_[var_idx].is_defined_var = true;
}

void Model::add_variable_alias(const std::string& alias_name, size_t var_id) {
    variable_aliases_[alias_name] = var_id;
}

const std::map<std::string, size_t>& Model::variable_aliases() const {
    return variable_aliases_;
}

void Model::add_constraint(ConstraintPtr constraint) {
    constraint->set_model_index(constraints_.size());
    constraint_ptrs_.push_back(constraint.get());
    constraints_.push_back(std::move(constraint));
}

const std::vector<std::unique_ptr<Variable>>& Model::variables() const {
    return variables_;
}

const std::vector<ConstraintPtr>& Model::constraints() const {
    return constraints_;
}

void Model::remove_constraint(size_t idx) {
    constraints_[idx] = nullptr;
    constraint_ptrs_[idx] = nullptr;
}

void Model::replace_constraint(size_t idx, ConstraintPtr new_cst) {
    new_cst->set_model_index(idx);
    constraint_ptrs_[idx] = new_cst.get();
    constraints_[idx] = std::move(new_cst);
}

void Model::compact_constraints() {
    size_t w = 0;
    for (size_t r = 0; r < constraints_.size(); ++r) {
        if (constraints_[r]) {
            if (w != r) {
                constraints_[w] = std::move(constraints_[r]);
                constraint_ptrs_[w] = constraints_[w].get();
                constraints_[w]->set_model_index(w);
            }
            ++w;
        }
    }
    constraints_.resize(w);
    constraint_ptrs_.resize(w);
}

void Model::mark_variable_eliminated(size_t var_idx) {
    if (eliminated_.size() <= var_idx) {
        eliminated_.resize(var_idx + 1, false);
    }
    eliminated_[var_idx] = true;
}

bool Model::is_eliminated(size_t var_idx) const {
    if (var_idx >= eliminated_.size()) return false;
    return eliminated_[var_idx];
}

Variable* Model::variable(size_t id) const {
    if (id >= variables_.size()) {
        throw std::out_of_range("Variable ID out of range");
    }
    return variables_[id].get();
}

Variable* Model::variable(const std::string& name) const {
    auto it = name_to_id_.find(name);
    if (it == name_to_id_.end()) {
        throw std::out_of_range("Variable not found: " + name);
    }
    return variables_[it->second].get();
}

size_t Model::find_variable_index(const std::string& name) const {
    auto it = name_to_id_.find(name);
    if (it != name_to_id_.end()) return it->second;
    // エイリアスも確認
    auto ait = variable_aliases_.find(name);
    if (ait != variable_aliases_.end()) return ait->second;
    return SIZE_MAX;
}

bool Model::contains(size_t var_idx, Domain::value_type val) const {
    auto& vd = var_data_[var_idx];
    if (val < vd.min || val > vd.max) return false;
    return variables_[var_idx]->domain().sparse_contains(val);
}

bool Model::set_min(int save_point, size_t var_idx, Domain::value_type new_min) {
    auto& vd = var_data_[var_idx];
    if (new_min <= vd.min) {
        return true;  // 変更不要
    }
    if (new_min > vd.max) {
        return false;  // ドメインが空になる
    }

    save_var_state(save_point, var_idx);
    auto& domain = variables_[var_idx]->domain();

    if (domain.is_bounds_only()) {
        // bounds-only fast path
        Domain::value_type actual_min = new_min;
        while (actual_min <= vd.max && !domain.sparse_contains(actual_min)) {
            actual_min++;
        }
        if (actual_min > vd.max) {
            vd.size = 0;
            return false;
        }
        domain.set_min_cache(actual_min);
        vd.min = actual_min;
        vd.support_value = actual_min;

        if (actual_min == vd.max) {
            domain.set_n(1);
            vd.size = 1;
            instantiated_count_++;
        }
        return true;
    }

    if (new_min <= vd.support_value && vd.support_value <= vd.max) {
        // Lazy: support がまだ有効なのでスキャン不要
        vd.min = new_min;
        if (new_min == vd.max) {
            // Domain も singleton にする（assigned_value() の整合性のため）
            size_t idx = domain.index_of(new_min);
            assert(idx != SIZE_MAX);

            domain.swap_at(idx, 0);
            domain.set_n(1);
            domain.set_min_cache(new_min);
            domain.set_max_cache(new_min);
            vd.support_value = new_min;
            vd.size = 1;
            instantiated_count_++;
        }
    }

    // Sync: support を超えたので O(gap) スキャンで actual min を求める
    Domain::value_type actual_min = new_min;
    while (actual_min <= vd.max && !domain.sparse_contains(actual_min)) {
        actual_min++;
    }
    if (actual_min > vd.max) {
        vd.size = 0;
        return false;
    }

    // actual_min == max → 確実に1値
    if (actual_min == vd.max) {
        size_t idx = domain.index_of(actual_min);
        assert(idx != SIZE_MAX);

        domain.swap_at(idx, 0);
        domain.set_n(1);
        domain.set_min_cache(actual_min);
        domain.set_max_cache(actual_min);
        vd.min = actual_min;
        vd.max = actual_min;
        vd.size = 1;
        vd.support_value = actual_min;
        instantiated_count_++;
        return true;
    }

    // max が sparse set に存在するか O(1) チェック
    if (domain.sparse_contains(vd.max)) {
        // 2値以上確定 → 通常パス
        domain.set_min_cache(actual_min);
        vd.min = actual_min;
        vd.support_value = actual_min;
        return true;
    }

    // max が stale → 逆方向スキャンで actual_max を探す
    Domain::value_type actual_max = vd.max - 1;
    while (actual_max > actual_min && !domain.sparse_contains(actual_max)) {
        actual_max--;
    }

    if (actual_max == actual_min) {
        // 1値 → instantiate
        size_t idx = domain.index_of(actual_min);
        assert(idx != SIZE_MAX);

        domain.swap_at(idx, 0);
        domain.set_n(1);
        domain.set_min_cache(actual_min);
        domain.set_max_cache(actual_min);
        vd.min = actual_min;
        vd.max = actual_min;
        vd.size = 1;
        vd.support_value = actual_min;
        instantiated_count_++;
    } else {
        // 2値以上。bounds を両方タイトにする
        domain.set_min_cache(actual_min);
        domain.set_max_cache(actual_max);
        vd.min = actual_min;
        vd.max = actual_max;
        vd.support_value = actual_min;
    }
    return true;
}

bool Model::set_max(int save_point, size_t var_idx, Domain::value_type new_max) {
    auto& vd = var_data_[var_idx];
    if (new_max >= vd.max) {
        return true;  // 変更不要
    }
    if (new_max < vd.min) {
        return false;  // ドメインが空になる
    }

    save_var_state(save_point, var_idx);
    auto& domain = variables_[var_idx]->domain();

    if (domain.is_bounds_only()) {
        // bounds-only fast path
        Domain::value_type actual_max = new_max;
        while (actual_max >= vd.min && !domain.sparse_contains(actual_max)) {
            actual_max--;
        }
        if (actual_max < vd.min) {
            vd.size = 0;
            return false;
        }
        domain.set_max_cache(actual_max);
        vd.max = actual_max;
        vd.support_value = actual_max;

        if (actual_max == vd.min) {
            domain.set_n(1);
            vd.size = 1;
            instantiated_count_++;
        }
        return true;
    }

    if (new_max >= vd.support_value && vd.support_value >= vd.min) {
        // Lazy: support がまだ有効なのでスキャン不要
        vd.max = new_max;
        if (new_max == vd.min) {
            // Domain も singleton にする（assigned_value() の整合性のため）
            size_t idx = domain.index_of(new_max);
            assert(idx != SIZE_MAX);

            domain.swap_at(idx, 0);
            domain.set_n(1);
            domain.set_min_cache(new_max);
            domain.set_max_cache(new_max);
            vd.support_value = new_max;
            vd.size = 1;
            instantiated_count_++;
        }
    }

    // Sync: support を下回ったので O(gap) スキャンで actual max を求める
    Domain::value_type actual_max = new_max;
    while (actual_max >= vd.min && !domain.sparse_contains(actual_max)) {
        actual_max--;
    }
    if (actual_max < vd.min) {
        vd.size = 0;
        return false;
    }

    // actual_max == min → 確実に1値
    if (actual_max == vd.min) {
        size_t idx = domain.index_of(actual_max);
        assert(idx != SIZE_MAX);

        domain.swap_at(idx, 0);
        domain.set_n(1);
        domain.set_min_cache(actual_max);
        domain.set_max_cache(actual_max);
        vd.min = actual_max;
        vd.max = actual_max;
        vd.size = 1;
        vd.support_value = actual_max;
        instantiated_count_++;
        return true;
    }

    // min が sparse set に存在するか O(1) チェック
    if (domain.sparse_contains(vd.min)) {
        // 2値以上確定 → 通常パス
        domain.set_max_cache(actual_max);
        vd.max = actual_max;
        vd.support_value = actual_max;
        return true;
    }

    // min が stale → 順方向スキャンで actual_min を探す
    Domain::value_type actual_min = vd.min + 1;
    while (actual_min < actual_max && !domain.sparse_contains(actual_min)) {
        actual_min++;
    }

    if (actual_min == actual_max) {
        // 1値 → instantiate
        size_t idx = domain.index_of(actual_max);
        assert(idx != SIZE_MAX);

        domain.swap_at(idx, 0);
        domain.set_n(1);
        domain.set_min_cache(actual_max);
        domain.set_max_cache(actual_max);
        vd.min = actual_max;
        vd.max = actual_max;
        vd.size = 1;
        vd.support_value = actual_max;
        instantiated_count_++;
    } else {
        // 2値以上。bounds を両方タイトにする
        domain.set_min_cache(actual_min);
        domain.set_max_cache(actual_max);
        vd.min = actual_min;
        vd.max = actual_max;
        vd.support_value = actual_max;
    }
    return true;
}

bool Model::remove_value(int save_point, size_t var_idx, Domain::value_type val) {
    auto& domain = variables_[var_idx]->domain();

    if (domain.is_bounds_only()) {
        if (!domain.contains(val)) return true;  // 既に無い

        auto& vd = var_data_[var_idx];
        bool was_instantiated = (vd.min == vd.max);
        save_var_state(save_point, var_idx);

        if (!domain.remove(val)) {
            vd.size = 0;
            return false;
        }
        vd.min = domain.min().value();
        vd.max = domain.max().value();
        vd.size = domain.size();
        if (val == vd.support_value) {
            vd.support_value = vd.min;
        }
        if (!was_instantiated && vd.min == vd.max) {
            instantiated_count_++;
        }
        return true;
    }

    size_t idx = domain.index_of(val);

    if (idx == SIZE_MAX) {
        return true;  // 既に無い
    }

    auto& vd = var_data_[var_idx];
    bool was_instantiated = (vd.min == vd.max);
    save_var_state(save_point, var_idx);

    domain.swap_at(idx, domain.n() - 1);
    size_t new_n = domain.n() - 1;
    domain.set_n(new_n);

    if (new_n == 0) {
        vd.size = 0;
        return false;
    }

    // 境界値の場合、sparse 配列で O(gap) スキャン（support 更新より先に行う）
    if (val == vd.min) {
        Domain::value_type new_min = val + 1;
        while (new_min <= vd.max && !domain.sparse_contains(new_min)) new_min++;
        if (new_min > vd.max) { vd.size = 0; return false; }
        vd.min = new_min;
        domain.set_min_cache(new_min);
    }
    if (val == vd.max) {
        Domain::value_type new_max = val - 1;
        while (new_max >= vd.min && !domain.sparse_contains(new_max)) new_max--;
        if (new_max < vd.min) { vd.size = 0; return false; }
        vd.max = new_max;
        domain.set_max_cache(new_max);
    }

    // support が削除された場合、bounds 更新後に有効な値で置換
    if (val == vd.support_value) {
        const auto& vals = domain.values_ref();
        vd.support_value = vals[0];
        if (vals[0] < vd.min || vals[0] > vd.max) {
            for (size_t i = 1; i < new_n; ++i) {
                if (vals[i] >= vd.min && vals[i] <= vd.max) {
                    vd.support_value = vals[i];
                    break;
                }
            }
        }
    }

    if (!was_instantiated && vd.min == vd.max) {
        instantiated_count_++;
    }
    vd.size = new_n;

    return true;
}

// Model::instantiate is now inline in model.hpp

void Model::rewind_to(int save_point) {
    // 変数ドメインの復元
    while (!var_trail_.empty() && var_trail_.back().first > save_point) {
        auto& [level, entry] = var_trail_.back();
        size_t var_idx = entry.var_idx;
        auto& vd = var_data_[var_idx];

        // instantiated カウンタ調整
        bool was_instantiated = (vd.min == vd.max);
        bool will_be_instantiated = (entry.old_min == entry.old_max);
        if (was_instantiated && !will_be_instantiated) {
            instantiated_count_--;
        } else if (!was_instantiated && will_be_instantiated) {
            instantiated_count_++;
        }

        // 変数データを復元
        vd.min = entry.old_min;
        vd.max = entry.old_max;
        vd.size = entry.old_n;

        // Domain オブジェクトも復元
        auto& domain = variables_[var_idx]->domain();
        domain.set_n(entry.old_n);
        domain.set_min_cache(entry.old_min);
        domain.set_max_cache(entry.old_max);

        // bounds-only の場合: removed_values_ を切り詰めて復元
        if (domain.is_bounds_only()) {
            domain.truncate_removed(entry.old_removed_count);
        }

        // 保存レベルをリセット
        vd.last_saved_level = -1;

        var_trail_.pop_back();
    }

    // 制約状態の復元は制約側で処理（constraint_trail_ はここでクリア）
    while (!constraint_trail_.empty() && constraint_trail_.back().first > save_point) {
        // 制約の restore_state は Solver から呼び出される想定
        // ここでは Trail のエントリを削除するだけ
        constraint_trail_.pop_back();
    }
}

void Model::rewind_dirty_constraints(int save_point) {
    while (!dirty_constraint_trail_.empty() &&
           dirty_constraint_trail_.back().first > save_point) {
        size_t c_idx = dirty_constraint_trail_.back().second;
        constraint_ptrs_[c_idx]->rewind_to(save_point);
        dirty_constraint_trail_.pop_back();
    }
}

size_t Model::var_trail_size() const {
    return var_trail_.size();
}

size_t Model::constraint_trail_size() const {
    return constraint_trail_.size();
}

void Model::sync_from_domains() {
    instantiated_count_ = 0;
    for (size_t i = 0; i < variables_.size(); ++i) {
        auto& vd = var_data_[i];
        vd.min = variables_[i]->min();
        vd.max = variables_[i]->max();
        vd.size = variables_[i]->domain().size();
        if (variables_[i]->domain().is_bounds_only()) {
            vd.support_value = (vd.min + vd.max) / 2;
        } else {
            const auto& vals = variables_[i]->domain().values_ref();
            size_t n = variables_[i]->domain().n();
            vd.support_value = vals[n / 2];
        }
        if (vd.min == vd.max) {
            instantiated_count_++;
        }
    }
}

void Model::sync_to_domains() {
    // AoS データから Domain オブジェクトを更新
    // 現在は Domain 側が正の情報源なので、特に処理しない
}

void Model::clear_pending_updates() {
    pending_updates_.clear();
    pending_read_idx_ = 0;
}

void Model::build_constraint_watch_list() {
    // 変数インデックス → 関連する制約インデックスのリスト
    var_to_constraint_indices_.clear();
    var_to_constraint_indices_.resize(variables_.size());

    for (size_t c_idx = 0; c_idx < constraints_.size(); ++c_idx) {
        const auto& constraint = constraints_[c_idx];
        const auto& var_ids = constraint->var_ids_ref();

        for (size_t i = 0; i < var_ids.size(); ++i) {
            size_t v_idx = var_ids[i];
            if (v_idx < var_to_constraint_indices_.size()) {
                var_to_constraint_indices_[v_idx].push_back({c_idx, i});
            } else {
                std::cerr << "% [watchlist] WARNING: var id=" << v_idx
                          << " >= variables_.size()=" << variables_.size()
                          << " in constraint #" << c_idx << " (" << constraint->name() << ")\n";
            }
        }
    }
}

bool Model::prepare_propagation() {
    // presolve でドメインが変更されている可能性があるため、先に SoA を同期
    sync_from_domains();

    // 全制約の prepare_propagation を順番に実行
    // 各制約は変数の現在状態を見て内部状態を初期化し、
    // 必要に応じてドメインを絞り込む
    for (const auto& constraint : constraints_) {
        if (!constraint->prepare_propagation(*this)) {
            return false;  // 矛盾検出
        }
        // 2WL 監視変数を Model の状態に基づいて再設定
        constraint->refine_watches(*this);
        // 探索開始時の未確定変数数を記録（bump_activity の分母用）
        constraint->compute_search_var_count(*this);
    }

    // prepare_propagation 後にデータを同期
    sync_from_domains();

    return true;
}

} // namespace sabori_csp
