#include "sabori_csp/model.hpp"
#include <stdexcept>
#include <algorithm>
#include <limits>
#include <iostream>

namespace sabori_csp {

VariablePtr Model::create_variable(std::string name, Domain domain) {
    auto var = std::make_shared<Variable>(std::move(name), std::move(domain));
    add_variable(var);
    return var;
}

VariablePtr Model::create_variable(std::string name, Domain::value_type value) {
    return create_variable(std::move(name), Domain(value, value));
}

VariablePtr Model::create_variable(std::string name, Domain::value_type min, Domain::value_type max) {
    return create_variable(std::move(name), Domain(min, max));
}

VariablePtr Model::create_variable(std::string name, std::vector<Domain::value_type> values) {
    return create_variable(std::move(name), Domain(std::move(values)));
}

size_t Model::add_variable(VariablePtr var) {
    size_t id = next_var_id_++;
    var->set_id(id);
    var->set_model(this);
    name_to_id_[var->name()] = id;
    variables_.push_back(var);

    // SoA データを更新
    mins_.push_back(var->min());
    maxs_.push_back(var->max());
    sizes_.push_back(var->domain().size());
    initial_ranges_.push_back(var->domain().initial_range());

    // support_value を初期化（dense 配列の中央値）
    const auto& vals = var->domain().values_ref();
    size_t n = var->domain().n();
    support_values_.push_back(vals[n / 2]);

    // 重複保存防止用
    last_saved_level_.push_back(-1);

    return id;
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

const std::vector<VariablePtr>& Model::variables() const {
    return variables_;
}

const std::vector<ConstraintPtr>& Model::constraints() const {
    return constraints_;
}

VariablePtr Model::variable(size_t id) const {
    if (id >= variables_.size()) {
        throw std::out_of_range("Variable ID out of range");
    }
    return variables_[id];
}

VariablePtr Model::variable(const std::string& name) const {
    auto it = name_to_id_.find(name);
    if (it == name_to_id_.end()) {
        throw std::out_of_range("Variable not found: " + name);
    }
    return variables_[it->second];
}

const std::vector<Domain::value_type>& Model::mins() const {
    return mins_;
}

std::vector<Domain::value_type>& Model::mins() {
    return mins_;
}

const std::vector<Domain::value_type>& Model::maxs() const {
    return maxs_;
}

std::vector<Domain::value_type>& Model::maxs() {
    return maxs_;
}

const std::vector<size_t>& Model::sizes() const {
    return sizes_;
}

std::vector<size_t>& Model::sizes() {
    return sizes_;
}

bool Model::contains(size_t var_idx, Domain::value_type val) const {
    if (val < mins_[var_idx] || val > maxs_[var_idx]) return false;
    return variables_[var_idx]->domain().sparse_contains(val);
}

void Model::save_var_state(int save_point, size_t var_idx) {
    // TODO: イベントごとに保存する内容を変えて、push_back内容を減らす
    // 同じレベルで既に保存済みならスキップ
    if (last_saved_level_[var_idx] == save_point) {
        return;
    }
    last_saved_level_[var_idx] = save_point;

    VarTrailEntry entry;
    entry.var_idx = var_idx;
    entry.old_min = mins_[var_idx];
    entry.old_max = maxs_[var_idx];
    entry.old_n = sizes_[var_idx];
    var_trail_.push_back({save_point, entry});
}

bool Model::set_min(int save_point, size_t var_idx, Domain::value_type new_min) {
    if (new_min <= mins_[var_idx]) {
        return true;  // 変更不要
    }
    if (new_min > maxs_[var_idx]) {
        return false;  // ドメインが空になる
    }

    save_var_state(save_point, var_idx);

    if (new_min <= support_values_[var_idx]) {
        // Lazy: support がまだ有効なのでスキャン不要
        mins_[var_idx] = new_min;
        return true;
    }

    // Sync: support を超えたので O(gap) スキャンで actual min を求める
    auto& domain = variables_[var_idx]->domain();
    Domain::value_type actual_min = new_min;
    while (actual_min <= maxs_[var_idx] && !domain.sparse_contains(actual_min)) {
        actual_min++;
    }
    if (actual_min > maxs_[var_idx]) {
        sizes_[var_idx] = 0;
        return false;
    }

    // actual_min == maxs_ → 確実に1値
    if (actual_min == maxs_[var_idx]) {
        domain.swap_at(domain.index_of(actual_min), 0);
        domain.set_n(1);
        domain.set_min_cache(actual_min);
        domain.set_max_cache(actual_min);
        mins_[var_idx] = actual_min;
        maxs_[var_idx] = actual_min;
        sizes_[var_idx] = 1;
        support_values_[var_idx] = actual_min;
        return true;
    }

    // maxs_ が sparse set に存在するか O(1) チェック
    if (domain.sparse_contains(maxs_[var_idx])) {
        // 2値以上確定 → 通常パス
        domain.set_min_cache(actual_min);
        mins_[var_idx] = actual_min;
        support_values_[var_idx] = actual_min;
        return true;
    }

    // maxs_ が stale → 逆方向スキャンで actual_max を探す
    Domain::value_type actual_max = maxs_[var_idx] - 1;
    while (actual_max > actual_min && !domain.sparse_contains(actual_max)) {
        actual_max--;
    }

    if (actual_max == actual_min) {
        // 1値 → instantiate
        domain.swap_at(domain.index_of(actual_min), 0);
        domain.set_n(1);
        domain.set_min_cache(actual_min);
        domain.set_max_cache(actual_min);
        mins_[var_idx] = actual_min;
        maxs_[var_idx] = actual_min;
        sizes_[var_idx] = 1;
        support_values_[var_idx] = actual_min;
    } else {
        // 2値以上。bounds を両方タイトにする
        domain.set_min_cache(actual_min);
        domain.set_max_cache(actual_max);
        mins_[var_idx] = actual_min;
        maxs_[var_idx] = actual_max;
        support_values_[var_idx] = actual_min;
    }
    return true;
}

bool Model::set_max(int save_point, size_t var_idx, Domain::value_type new_max) {
    if (new_max >= maxs_[var_idx]) {
        return true;  // 変更不要
    }
    if (new_max < mins_[var_idx]) {
        return false;  // ドメインが空になる
    }

    save_var_state(save_point, var_idx);

    if (new_max >= support_values_[var_idx]) {
        // Lazy: support がまだ有効なのでスキャン不要
        maxs_[var_idx] = new_max;
        return true;
    }

    // Sync: support を下回ったので O(gap) スキャンで actual max を求める
    auto& domain = variables_[var_idx]->domain();
    Domain::value_type actual_max = new_max;
    while (actual_max >= mins_[var_idx] && !domain.sparse_contains(actual_max)) {
        actual_max--;
    }
    if (actual_max < mins_[var_idx]) {
        sizes_[var_idx] = 0;
        return false;
    }

    // actual_max == mins_ → 確実に1値
    if (actual_max == mins_[var_idx]) {
        domain.swap_at(domain.index_of(actual_max), 0);
        domain.set_n(1);
        domain.set_min_cache(actual_max);
        domain.set_max_cache(actual_max);
        mins_[var_idx] = actual_max;
        maxs_[var_idx] = actual_max;
        sizes_[var_idx] = 1;
        support_values_[var_idx] = actual_max;
        return true;
    }

    // mins_ が sparse set に存在するか O(1) チェック
    if (domain.sparse_contains(mins_[var_idx])) {
        // 2値以上確定 → 通常パス
        domain.set_max_cache(actual_max);
        maxs_[var_idx] = actual_max;
        support_values_[var_idx] = actual_max;
        return true;
    }

    // mins_ が stale → 順方向スキャンで actual_min を探す
    Domain::value_type actual_min = mins_[var_idx] + 1;
    while (actual_min < actual_max && !domain.sparse_contains(actual_min)) {
        actual_min++;
    }

    if (actual_min == actual_max) {
        // 1値 → instantiate
        domain.swap_at(domain.index_of(actual_max), 0);
        domain.set_n(1);
        domain.set_min_cache(actual_max);
        domain.set_max_cache(actual_max);
        mins_[var_idx] = actual_max;
        maxs_[var_idx] = actual_max;
        sizes_[var_idx] = 1;
        support_values_[var_idx] = actual_max;
    } else {
        // 2値以上。bounds を両方タイトにする
        domain.set_min_cache(actual_min);
        domain.set_max_cache(actual_max);
        mins_[var_idx] = actual_min;
        maxs_[var_idx] = actual_max;
        support_values_[var_idx] = actual_max;
    }
    return true;
}

bool Model::remove_value(int save_point, size_t var_idx, Domain::value_type val) {
    auto& domain = variables_[var_idx]->domain();
    size_t idx = domain.index_of(val);

    if (idx == SIZE_MAX) {
        return true;  // 既に無い
    }

    save_var_state(save_point, var_idx);

    domain.swap_at(idx, domain.n() - 1);
    size_t new_n = domain.n() - 1;
    domain.set_n(new_n);

    if (new_n == 0) {
        sizes_[var_idx] = 0;
        return false;
    }

    // support が削除された場合、dense[0] を新 support に（O(1)、必ず domain 内）
    if (val == support_values_[var_idx]) {
        support_values_[var_idx] = domain.values_ref()[0];
    }

    // 境界値の場合、sparse 配列で O(gap) スキャン
    if (val == mins_[var_idx]) {
        Domain::value_type new_min = val + 1;
        while (new_min <= maxs_[var_idx] && !domain.sparse_contains(new_min)) new_min++;
        if (new_min > maxs_[var_idx]) { sizes_[var_idx] = 0; return false; }
        mins_[var_idx] = new_min;
        domain.set_min_cache(new_min);
    }
    if (val == maxs_[var_idx]) {
        Domain::value_type new_max = val - 1;
        while (new_max >= mins_[var_idx] && !domain.sparse_contains(new_max)) new_max--;
        if (new_max < mins_[var_idx]) { sizes_[var_idx] = 0; return false; }
        maxs_[var_idx] = new_max;
        domain.set_max_cache(new_max);
    }
    sizes_[var_idx] = new_n;

    return true;
}

bool Model::instantiate(int save_point, size_t var_idx, Domain::value_type val) {
    auto& domain = variables_[var_idx]->domain();
    size_t idx = domain.index_of(val);

    if (idx == SIZE_MAX) {
        return false;  // ドメインに無い
    }

    save_var_state(save_point, var_idx);

    domain.swap_at(idx, 0);
    domain.set_n(1);
    domain.set_min_cache(val);
    domain.set_max_cache(val);

    mins_[var_idx] = val;
    maxs_[var_idx] = val;
    sizes_[var_idx] = 1;
    support_values_[var_idx] = val;
    return true;
}

void Model::rewind_to(int save_point) {
    // 変数ドメインの復元
    while (!var_trail_.empty() && var_trail_.back().first > save_point) {
        auto& [level, entry] = var_trail_.back();
        size_t var_idx = entry.var_idx;

        // SoA データを復元
        mins_[var_idx] = entry.old_min;
        maxs_[var_idx] = entry.old_max;
        sizes_[var_idx] = entry.old_n;

        // Domain オブジェクトも復元
        auto& domain = variables_[var_idx]->domain();
        domain.set_n(entry.old_n);
        domain.set_min_cache(entry.old_min);
        domain.set_max_cache(entry.old_max);

        // 保存レベルをリセット
        last_saved_level_[var_idx] = -1;

        var_trail_.pop_back();
    }

    // 制約状態の復元は制約側で処理（constraint_trail_ はここでクリア）
    while (!constraint_trail_.empty() && constraint_trail_.back().first > save_point) {
        // 制約の restore_state は Solver から呼び出される想定
        // ここでは Trail のエントリを削除するだけ
        constraint_trail_.pop_back();
    }
}

void Model::mark_constraint_dirty(size_t constraint_idx, int save_point) {
    dirty_constraint_trail_.push_back({save_point, constraint_idx});
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
    for (size_t i = 0; i < variables_.size(); ++i) {
        mins_[i] = variables_[i]->min();
        maxs_[i] = variables_[i]->max();
        sizes_[i] = variables_[i]->domain().size();
        const auto& vals = variables_[i]->domain().values_ref();
        size_t n = variables_[i]->domain().n();
        support_values_[i] = vals[n / 2];
    }
}

void Model::sync_to_domains() {
    // SoA データから Domain オブジェクトを更新
    // 現在は Domain 側が正の情報源なので、特に処理しない
}

void Model::enqueue_instantiate(size_t var_idx, Domain::value_type value) {
    pending_updates_.push_back({PendingUpdate::Type::Instantiate, var_idx, value});
}

void Model::enqueue_set_min(size_t var_idx, Domain::value_type new_min) {
    pending_updates_.push_back({PendingUpdate::Type::SetMin, var_idx, new_min});
}

void Model::enqueue_set_max(size_t var_idx, Domain::value_type new_max) {
    pending_updates_.push_back({PendingUpdate::Type::SetMax, var_idx, new_max});
}

void Model::enqueue_remove_value(size_t var_idx, Domain::value_type value) {
    pending_updates_.push_back({PendingUpdate::Type::RemoveValue, var_idx, value});
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
        const auto& vars = constraint->variables();

        for (const auto& var : vars) {
            // 変数の ID を直接インデックスとして使用
            size_t v_idx = var->id();
            if (v_idx < var_to_constraint_indices_.size()) {
                var_to_constraint_indices_[v_idx].push_back(c_idx);
            } else {
                std::cerr << "% [watchlist] WARNING: var " << var->name()
                          << " id=" << v_idx << " >= variables_.size()=" << variables_.size()
                          << " in constraint #" << c_idx << " (" << constraint->name() << ")\n";
            }
        }
    }
}

bool Model::prepare_propagation() {
    // 全制約の prepare_propagation を順番に実行
    // 各制約は変数の現在状態を見て内部状態を初期化し、
    // 必要に応じてドメインを絞り込む
    for (const auto& constraint : constraints_) {
        if (!constraint->prepare_propagation(*this)) {
            return false;  // 矛盾検出
        }
    }

    // prepare_propagation 後に SoA データを同期
    sync_from_domains();

    return true;
}

} // namespace sabori_csp
