#include "sabori_csp/model.hpp"
#include <stdexcept>
#include <algorithm>
#include <limits>

namespace sabori_csp {

size_t Model::add_variable(VariablePtr var) {
    size_t id = variables_.size();
    name_to_id_[var->name()] = id;
    variables_.push_back(var);

    // SoA データを更新
    const auto& domain = var->domain();
    mins_.push_back(domain.min().value_or(0));
    maxs_.push_back(domain.max().value_or(0));
    sizes_.push_back(domain.size());

    // 重複保存防止用
    last_saved_level_.push_back(-1);

    return id;
}

void Model::add_constraint(ConstraintPtr constraint) {
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

bool Model::is_instantiated(size_t var_idx) const {
    return sizes_[var_idx] == 1;
}

Domain::value_type Model::value(size_t var_idx) const {
    if (!is_instantiated(var_idx)) {
        throw std::runtime_error("Variable is not instantiated");
    }
    return mins_[var_idx];
}

bool Model::contains(size_t var_idx, Domain::value_type val) const {
    // TODO: min <= val && val <= min であることを先に確認する
    return variables_[var_idx]->domain().contains(val);
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

    // Sparse Set から new_min 未満の値を除外
    // TODO: 毎回真面目にメンテすると遅いので、飛ばす
    auto& domain = variables_[var_idx]->domain();
    auto& vals = domain.values_ref();
    size_t n = domain.n();
    size_t i = 0;
    Domain::value_type current_min = std::numeric_limits<Domain::value_type>::max();

    while (i < n) {
        if (vals[i] < new_min) {
            domain.swap_at(i, n - 1);
            --n;
        } else {
            current_min = std::min(current_min, vals[i]);
            ++i;
        }
    }

    if (n == 0) {
        return false;
    }

    domain.set_n(n);
    domain.set_min_cache(current_min);
    mins_[var_idx] = current_min;
    sizes_[var_idx] = n;
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

    // Sparse Set から new_max より大きい値を除外
    // TODO: 毎回真面目にメンテすると遅いので、飛ばす
    auto& domain = variables_[var_idx]->domain();
    auto& vals = domain.values_ref();
    size_t n = domain.n();
    size_t i = 0;
    Domain::value_type current_max = std::numeric_limits<Domain::value_type>::min();

    while (i < n) {
        if (vals[i] > new_max) {
            domain.swap_at(i, n - 1);
            --n;
        } else {
            current_max = std::max(current_max, vals[i]);
            ++i;
        }
    }

    if (n == 0) {
        return false;
    }

    domain.set_n(n);
    domain.set_max_cache(current_max);
    maxs_[var_idx] = current_max;
    sizes_[var_idx] = n;
    return true;
}

bool Model::remove_value(int save_point, size_t var_idx, Domain::value_type val) {
    auto& domain = variables_[var_idx]->domain();
    auto& sparse = domain.sparse_ref();
    auto it = sparse.find(val);

    if (it == sparse.end() || it->second >= domain.n()) {
        return true;  // 既に無い
    }

    save_var_state(save_point, var_idx);

    size_t idx = it->second;
    domain.swap_at(idx, domain.n() - 1);
    size_t new_n = domain.n() - 1;
    domain.set_n(new_n);

    if (new_n == 0) {
        sizes_[var_idx] = 0;
        return false;
    }

    // min/max の更新が必要な場合
    if (val == mins_[var_idx] || val == maxs_[var_idx]) {
        domain.update_bounds();
        mins_[var_idx] = domain.min().value();
        maxs_[var_idx] = domain.max().value();
    }
    sizes_[var_idx] = new_n;
    return true;
}

bool Model::instantiate(int save_point, size_t var_idx, Domain::value_type val) {
    auto& domain = variables_[var_idx]->domain();
    auto& sparse = domain.sparse_ref();
    auto it = sparse.find(val);

    // TODO: min <= val && val <= min であることを先に確認する

    if (it == sparse.end() || it->second >= domain.n()) {
        return false;  // ドメインに無い
    }

    save_var_state(save_point, var_idx);

    size_t idx = it->second;
    domain.swap_at(idx, 0);
    domain.set_n(1);
    domain.set_min_cache(val);
    domain.set_max_cache(val);

    mins_[var_idx] = val;
    maxs_[var_idx] = val;
    sizes_[var_idx] = 1;
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

size_t Model::var_trail_size() const {
    return var_trail_.size();
}

size_t Model::constraint_trail_size() const {
    return constraint_trail_.size();
}

void Model::sync_from_domains() {
    for (size_t i = 0; i < variables_.size(); ++i) {
        const auto& domain = variables_[i]->domain();
        mins_[i] = domain.min().value_or(0);
        maxs_[i] = domain.max().value_or(0);
        sizes_[i] = domain.size();
    }
}

void Model::sync_to_domains() {
    // SoA データから Domain オブジェクトを更新
    // 現在は Domain 側が正の情報源なので、特に処理しない
}

void Model::enqueue_instantiate(size_t var_idx, Domain::value_type value) {
    // 重複チェック
    for (const auto& [idx, val] : pending_instantiations_) {
        if (idx == var_idx) {
            return;
        }
    }
    pending_instantiations_.push_back({var_idx, value});
}

const std::vector<std::pair<size_t, Domain::value_type>>& Model::pending_instantiations() const {
    return pending_instantiations_;
}

void Model::clear_pending_instantiations() {
    pending_instantiations_.clear();
}

} // namespace sabori_csp
