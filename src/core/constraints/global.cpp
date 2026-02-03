#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <set>
#include <limits>

namespace sabori_csp {

// ============================================================================
// AllDifferentConstraint implementation
// ============================================================================

AllDifferentConstraint::AllDifferentConstraint(std::vector<VariablePtr> vars)
    : Constraint(vars)
    , pool_n_(0)
    , unfixed_count_(0) {
    // 変数ポインタ → 内部インデックスマップを構築
    for (size_t i = 0; i < vars_.size(); ++i) {
        var_ptr_to_idx_[vars_[i].get()] = i;
    }

    // 全変数の値の和集合をプールとして構築
    std::set<Domain::value_type> all_values;
    for (const auto& var : vars) {
        for (auto v : var->domain().values()) {
            all_values.insert(v);
        }
    }

    pool_values_.assign(all_values.begin(), all_values.end());
    pool_n_ = pool_values_.size();
    for (size_t i = 0; i < pool_n_; ++i) {
        pool_sparse_[pool_values_[i]] = i;
    }

    // 既に確定している変数の値をプールから削除 + 未確定カウント初期化
    for (const auto& var : vars) {
        if (var->is_assigned()) {
            auto val = var->assigned_value().value();
            auto it = pool_sparse_.find(val);
            if (it != pool_sparse_.end() && it->second < pool_n_) {
                // Sparse Set から削除（スワップ）
                size_t idx = it->second;
                size_t last_idx = pool_n_ - 1;
                Domain::value_type last_val = pool_values_[last_idx];

                pool_values_[idx] = last_val;
                pool_values_[last_idx] = val;
                pool_sparse_[last_val] = idx;
                pool_sparse_[val] = last_idx;
                --pool_n_;
            }
        } else {
            ++unfixed_count_;
        }
    }

    // 初期整合性チェック
    check_initial_consistency();
}

std::string AllDifferentConstraint::name() const {
    return "all_different";
}

std::vector<VariablePtr> AllDifferentConstraint::variables() const {
    return vars_;
}

std::optional<bool> AllDifferentConstraint::is_satisfied() const {
    std::set<Domain::value_type> used_values;
    for (const auto& var : vars_) {
        if (!var->is_assigned()) {
            return std::nullopt;
        }
        auto val = var->assigned_value().value();
        if (used_values.count(val) > 0) {
            return false;
        }
        used_values.insert(val);
    }
    return true;
}

bool AllDifferentConstraint::propagate() {
    // 確定した変数の値を他の変数から削除
    for (const auto& var : vars_) {
        if (var->is_assigned()) {
            auto val = var->assigned_value().value();
            for (const auto& other : vars_) {
                if (other != var && !other->is_assigned()) {
                    other->domain().remove(val);
                    if (other->domain().empty()) {
                        return false;
                    }
                }
            }
        }
    }
    return true;
}

bool AllDifferentConstraint::remove_from_pool(int save_point, Domain::value_type value) {
    auto it = pool_sparse_.find(value);
    if (it == pool_sparse_.end() || it->second >= pool_n_) {
        return false;  // 既にプールにない
    }

    // Trail に保存（同一レベルでも複数回保存する可能性あり）
    if (pool_trail_.empty() || pool_trail_.back().first != save_point) {
        pool_trail_.push_back({save_point, {pool_n_, unfixed_count_}});
    }

    // Sparse Set から削除（スワップ）
    size_t idx = it->second;
    size_t last_idx = pool_n_ - 1;
    Domain::value_type last_val = pool_values_[last_idx];

    pool_values_[idx] = last_val;
    pool_values_[last_idx] = value;
    pool_sparse_[last_val] = idx;
    pool_sparse_[value] = last_idx;
    --pool_n_;

    return true;
}

bool AllDifferentConstraint::on_instantiate(Model& model, int save_point,
                                             size_t /*var_idx*/, Domain::value_type value,
                                             Domain::value_type /*prev_min*/,
                                             Domain::value_type /*prev_max*/) {
    // 基底クラスの 2WL 処理は省略（AllDifferent は全変数を監視）

    // プールから値を削除
    auto it = pool_sparse_.find(value);
    if (it == pool_sparse_.end() || it->second >= pool_n_) {
        // 既にプールにない = 他の変数が使用済み
        return false;
    }

    // Trail に保存
    if (pool_trail_.empty() || pool_trail_.back().first != save_point) {
        pool_trail_.push_back({save_point, {pool_n_, unfixed_count_}});
    }

    // プールから削除
    size_t idx = it->second;
    size_t last_idx = pool_n_ - 1;
    Domain::value_type last_val = pool_values_[last_idx];

    pool_values_[idx] = last_val;
    pool_values_[last_idx] = value;
    pool_sparse_[last_val] = idx;
    pool_sparse_[value] = last_idx;
    --pool_n_;

    // 未確定カウントをデクリメント（O(1)）
    --unfixed_count_;

    // 鳩の巣原理: 未確定変数 > 利用可能な値 なら矛盾
    if (unfixed_count_ > pool_n_) {
        return false;
    }

    // 残り1変数になったら on_last_uninstantiated を呼び出す
    if (unfixed_count_ == 1) {
        size_t last_idx = find_last_uninstantiated();
        if (last_idx != SIZE_MAX) {
            return on_last_uninstantiated(model, save_point, last_idx);
        }
    } else if (unfixed_count_ == 0) {
        return on_final_instantiate();
    }

    return true;
}

bool AllDifferentConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                                      size_t last_var_internal_idx) {
    auto& last_var = vars_[last_var_internal_idx];

    // 既に確定している場合は整合性チェックのみ
    if (last_var->is_assigned()) {
        auto val = last_var->assigned_value().value();
        // その値がプールに残っているか（他の変数と重複していないか）
        auto it = pool_sparse_.find(val);
        return (it != pool_sparse_.end() && it->second < pool_n_);
    }

    // 利用可能な値が1つだけなら自動決定
    if (pool_n_ == 1) {
        Domain::value_type remaining_value = pool_values_[0];
        // モデル内の変数インデックスを特定
        for (size_t model_idx = 0; model_idx < model.variables().size(); ++model_idx) {
            if (model.variable(model_idx) == last_var) {
                model.enqueue_instantiate(model_idx, remaining_value);
                break;
            }
        }
    }
    // 利用可能な値が0なら矛盾
    else if (pool_n_ == 0) {
        return false;
    }
    // 複数の値が利用可能な場合は、ドメインを絞り込む
    // 注: ここでは単純化のため、確定のみを行う

    return true;
}

void AllDifferentConstraint::check_initial_consistency() {
    // 既に確定している変数の値が重複していれば矛盾
    std::set<Domain::value_type> used_values;
    for (const auto& var : vars_) {
        if (var->is_assigned()) {
            auto val = var->assigned_value().value();
            if (used_values.count(val) > 0) {
                set_initially_inconsistent(true);
                return;
            }
            used_values.insert(val);
        }
    }

    // 鳩の巣原理: 未確定変数の数 > 利用可能な値の数 なら矛盾
    if (unfixed_count_ > pool_n_) {
        set_initially_inconsistent(true);
    }
}

bool AllDifferentConstraint::on_final_instantiate() {
    // 全変数が異なる値を持つか確認
    std::set<Domain::value_type> used_values;
    for (const auto& var : vars_) {
        auto val = var->assigned_value().value();
        if (used_values.count(val) > 0) {
            return false;
        }
        used_values.insert(val);
    }
    return true;
}

void AllDifferentConstraint::rewind_to(int save_point) {
    while (!pool_trail_.empty() && pool_trail_.back().first > save_point) {
        const auto& entry = pool_trail_.back().second;
        pool_n_ = entry.old_pool_n;
        unfixed_count_ = entry.old_unfixed_count;
        pool_trail_.pop_back();
    }
}

// ============================================================================
// IntLinEqConstraint implementation
// ============================================================================

IntLinEqConstraint::IntLinEqConstraint(std::vector<int64_t> coeffs,
                                         std::vector<VariablePtr> vars,
                                         int64_t target_sum)
    : Constraint(vars)
    , target_sum_(target_sum)
    , current_fixed_sum_(0)
    , min_rem_potential_(0)
    , max_rem_potential_(0)
    , unfixed_count_(0) {
    // 同一変数の係数を集約
    std::unordered_map<Variable*, int64_t> aggregated;
    for (size_t i = 0; i < vars.size(); ++i) {
        aggregated[vars[i].get()] += coeffs[i];
    }

    // 一意な変数リストと係数リストを再構築
    vars_.clear();
    coeffs_.clear();
    for (const auto& [var_ptr, coeff] : aggregated) {
        // shared_ptr を探す
        for (const auto& var : vars) {
            if (var.get() == var_ptr) {
                vars_.push_back(var);
                coeffs_.push_back(coeff);
                break;
            }
        }
    }

    // 変数ポインタ → 内部インデックスマップを構築
    for (size_t i = 0; i < vars_.size(); ++i) {
        var_ptr_to_idx_[vars_[i].get()] = i;
    }

    // 初期ポテンシャルを計算（既に確定している変数は fixed_sum に加算）
    for (size_t i = 0; i < vars_.size(); ++i) {
        int64_t c = coeffs_[i];

        if (vars_[i]->is_assigned()) {
            // 既に確定している変数
            current_fixed_sum_ += c * vars_[i]->assigned_value().value();
        } else {
            // 未確定の変数
            ++unfixed_count_;
            auto min_val = vars_[i]->domain().min().value();
            auto max_val = vars_[i]->domain().max().value();

            if (c >= 0) {
                min_rem_potential_ += c * min_val;
                max_rem_potential_ += c * max_val;
            } else {
                min_rem_potential_ += c * max_val;
                max_rem_potential_ += c * min_val;
            }
        }
    }

    // 2WL を再初期化
    init_watches();

    // 初期整合性チェック
    check_initial_consistency();
}

std::string IntLinEqConstraint::name() const {
    return "int_lin_eq";
}

std::vector<VariablePtr> IntLinEqConstraint::variables() const {
    return vars_;
}

std::optional<bool> IntLinEqConstraint::is_satisfied() const {
    int64_t sum = 0;
    for (size_t i = 0; i < vars_.size(); ++i) {
        if (!vars_[i]->is_assigned()) {
            return std::nullopt;
        }
        sum += coeffs_[i] * vars_[i]->assigned_value().value();
    }
    return sum == target_sum_;
}

bool IntLinEqConstraint::propagate() {
    // 簡易的な bounds propagation
    // TODO: より効率的な実装
    return true;
}

bool IntLinEqConstraint::can_assign(size_t internal_idx, Domain::value_type value,
                                     Domain::value_type prev_min,
                                     Domain::value_type prev_max) const {
    int64_t c = coeffs_[internal_idx];

    // 1. 自分が value を取った時、他が「最小」でも target を超えないか？
    int64_t potential_min;
    if (c >= 0) {
        potential_min = current_fixed_sum_ + (min_rem_potential_ - c * prev_min) + c * value;
    } else {
        potential_min = current_fixed_sum_ + (min_rem_potential_ - c * prev_max) + c * value;
    }
    if (potential_min > target_sum_) {
        return false;
    }

    // 2. 自分が value を取った時、他が「最大」でも target に届くか？
    int64_t potential_max;
    if (c >= 0) {
        potential_max = current_fixed_sum_ + (max_rem_potential_ - c * prev_max) + c * value;
    } else {
        potential_max = current_fixed_sum_ + (max_rem_potential_ - c * prev_min) + c * value;
    }
    if (potential_max < target_sum_) {
        return false;
    }

    return true;
}

bool IntLinEqConstraint::on_instantiate(Model& model, int save_point,
                                          size_t var_idx, Domain::value_type value,
                                          Domain::value_type prev_min,
                                          Domain::value_type prev_max) {
    // モデルから変数ポインタを取得し、O(1) で内部インデックスを特定
    Variable* var_ptr = model.variable(var_idx).get();
    auto it = var_ptr_to_idx_.find(var_ptr);
    if (it == var_ptr_to_idx_.end()) {
        // この制約に関係ない変数
        return true;
    }
    size_t internal_idx = it->second;

    // Look-ahead チェック
    if (!can_assign(internal_idx, value, prev_min, prev_max)) {
        return false;
    }

    // Trail に保存
    if (trail_.empty() || trail_.back().first != save_point) {
        trail_.push_back({save_point, {current_fixed_sum_, min_rem_potential_, max_rem_potential_, unfixed_count_}});
    }

    // 差分更新
    int64_t c = coeffs_[internal_idx];
    current_fixed_sum_ += c * value;
    if (c >= 0) {
        min_rem_potential_ -= c * prev_min;
        max_rem_potential_ -= c * prev_max;
    } else {
        min_rem_potential_ -= c * prev_max;
        max_rem_potential_ -= c * prev_min;
    }

    // 未確定カウントをデクリメント（O(1)）
    --unfixed_count_;

    // 残り1変数になったら on_last_uninstantiated を呼び出す
    if (unfixed_count_ == 1) {
        size_t last_idx = find_last_uninstantiated();
        if (last_idx != SIZE_MAX) {
            return on_last_uninstantiated(model, save_point, last_idx);
        }
    } else if (unfixed_count_ == 0) {
        return on_final_instantiate();
    }

    return true;
}

bool IntLinEqConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                                  size_t last_var_internal_idx) {
    int64_t last_coeff = coeffs_[last_var_internal_idx];
    int64_t remaining = target_sum_ - current_fixed_sum_;
    auto& last_var = vars_[last_var_internal_idx];

    // 既に確定している場合は整合性チェックのみ
    if (last_var->is_assigned()) {
        int64_t actual = last_var->assigned_value().value();
        return (last_coeff * actual == remaining);
    }

    // 係数で割り切れる場合のみ値を決定
    if (remaining % last_coeff == 0) {
        int64_t required_value = remaining / last_coeff;

        if (last_var->domain().contains(required_value)) {
            // モデル内の変数インデックスを特定
            for (size_t model_idx = 0; model_idx < model.variables().size(); ++model_idx) {
                if (model.variable(model_idx) == last_var) {
                    model.enqueue_instantiate(model_idx, required_value);
                    break;
                }
            }
        } else {
            // 確定する値がドメインに含まれない
            return false;
        }
    } else {
        // 割り切れなければ制約は満たせない
        return false;
    }

    return true;
}

void IntLinEqConstraint::check_initial_consistency() {
    // 確定済み + 残りの最小/最大ポテンシャルをチェック
    // current_fixed_sum_ + min_rem_potential_ > target_sum_ または
    // current_fixed_sum_ + max_rem_potential_ < target_sum_ なら矛盾
    int64_t total_min = current_fixed_sum_ + min_rem_potential_;
    int64_t total_max = current_fixed_sum_ + max_rem_potential_;
    if (total_min > target_sum_ || total_max < target_sum_) {
        set_initially_inconsistent(true);
    }
}

bool IntLinEqConstraint::on_final_instantiate() {
    int64_t sum = 0;
    for (size_t i = 0; i < vars_.size(); ++i) {
        sum += coeffs_[i] * vars_[i]->assigned_value().value();
    }
    return sum == target_sum_;
}

void IntLinEqConstraint::rewind_to(int save_point) {
    while (!trail_.empty() && trail_.back().first > save_point) {
        const auto& entry = trail_.back().second;
        current_fixed_sum_ = entry.fixed_sum;
        min_rem_potential_ = entry.min_pot;
        max_rem_potential_ = entry.max_pot;
        unfixed_count_ = entry.unfixed_count;
        trail_.pop_back();
    }
}

// ============================================================================
// IntLinLeConstraint implementation
// ============================================================================

IntLinLeConstraint::IntLinLeConstraint(std::vector<int64_t> coeffs,
                                         std::vector<VariablePtr> vars,
                                         int64_t bound)
    : Constraint(vars)
    , coeffs_(std::move(coeffs))
    , bound_(bound)
    , current_fixed_sum_(0)
    , min_rem_potential_(0) {
    // 変数ポインタ → 内部インデックスマップを構築
    for (size_t i = 0; i < vars_.size(); ++i) {
        var_ptr_to_idx_[vars_[i].get()] = i;
    }

    // 初期ポテンシャルを計算（既に確定している変数は fixed_sum に加算）
    for (size_t i = 0; i < vars_.size(); ++i) {
        int64_t c = coeffs_[i];

        if (vars_[i]->is_assigned()) {
            // 既に確定している変数
            current_fixed_sum_ += c * vars_[i]->assigned_value().value();
        } else {
            // 未確定の変数
            auto min_val = vars_[i]->domain().min().value();
            auto max_val = vars_[i]->domain().max().value();

            if (c >= 0) {
                min_rem_potential_ += c * min_val;
            } else {
                min_rem_potential_ += c * max_val;
            }
        }
    }

    // 初期整合性チェック
    check_initial_consistency();
}

std::string IntLinLeConstraint::name() const {
    return "int_lin_le";
}

std::vector<VariablePtr> IntLinLeConstraint::variables() const {
    return vars_;
}

std::optional<bool> IntLinLeConstraint::is_satisfied() const {
    int64_t sum = 0;
    for (size_t i = 0; i < vars_.size(); ++i) {
        if (!vars_[i]->is_assigned()) {
            return std::nullopt;
        }
        sum += coeffs_[i] * vars_[i]->assigned_value().value();
    }
    return sum <= bound_;
}

bool IntLinLeConstraint::propagate() {
    return true;
}

bool IntLinLeConstraint::on_instantiate(Model& model, int save_point,
                                          size_t var_idx, Domain::value_type value,
                                          Domain::value_type prev_min,
                                          Domain::value_type prev_max) {
    // モデルから変数ポインタを取得し、O(1) で内部インデックスを特定
    Variable* var_ptr = model.variable(var_idx).get();
    auto it = var_ptr_to_idx_.find(var_ptr);
    if (it == var_ptr_to_idx_.end()) {
        // この制約に関係ない変数
        return true;
    }
    size_t internal_idx = it->second;

    int64_t c = coeffs_[internal_idx];

    // Trail に保存
    if (trail_.empty() || trail_.back().first != save_point) {
        trail_.push_back({save_point, {current_fixed_sum_, min_rem_potential_}});
    }

    // 差分更新
    current_fixed_sum_ += c * value;
    if (c >= 0) {
        min_rem_potential_ -= c * prev_min;
    } else {
        min_rem_potential_ -= c * prev_max;
    }

    // 下限チェック: 現在の確定和 + 残りの最小 > bound なら矛盾
    if (current_fixed_sum_ + min_rem_potential_ > bound_) {
        return false;
    }

    return true;
}

bool IntLinLeConstraint::on_final_instantiate() {
    int64_t sum = 0;
    for (size_t i = 0; i < vars_.size(); ++i) {
        sum += coeffs_[i] * vars_[i]->assigned_value().value();
    }
    return sum <= bound_;
}

void IntLinLeConstraint::rewind_to(int save_point) {
    while (!trail_.empty() && trail_.back().first > save_point) {
        const auto& entry = trail_.back().second;
        current_fixed_sum_ = entry.fixed_sum;
        min_rem_potential_ = entry.min_pot;
        trail_.pop_back();
    }
}

void IntLinLeConstraint::check_initial_consistency() {
    // 確定済み + 残りの最小ポテンシャルが bound を超えるなら矛盾
    if (current_fixed_sum_ + min_rem_potential_ > bound_) {
        set_initially_inconsistent(true);
    }
}

// ============================================================================
// CircuitConstraint implementation
// ============================================================================

CircuitConstraint::CircuitConstraint(std::vector<VariablePtr> vars)
    : Constraint(vars)
    , n_(vars.size())
    , head_(n_)
    , tail_(n_)
    , size_(n_, 1)
    , in_degree_(n_, 0)
    , unfixed_count_(0)
    , pool_n_(n_) {
    // 初期状態: 各ノードは長さ1のパス（自分自身が root）
    for (size_t i = 0; i < n_; ++i) {
        head_[i] = i;
        tail_[i] = i;
    }

    // プール初期化（0 から n-1 の値）
    pool_.resize(n_);
    for (size_t i = 0; i < n_; ++i) {
        pool_[i] = static_cast<Domain::value_type>(i);
        pool_idx_[static_cast<Domain::value_type>(i)] = i;
    }

    // 変数ポインタ → インデックス マップを構築
    for (size_t i = 0; i < vars_.size(); ++i) {
        var_ptr_to_idx_[vars_[i].get()] = i;
    }

    // 既に確定している変数のパス結合と入次数を設定 + 未確定カウント
    for (size_t i = 0; i < n_; ++i) {
        if (vars_[i]->is_assigned()) {
            auto val = vars_[i]->assigned_value().value();
            size_t j = static_cast<size_t>(val);

            // 入次数チェック
            if (in_degree_[j] > 0) {
                set_initially_inconsistent(true);
                return;
            }

            size_t h1 = find(i);
            size_t h2 = find(j);

            if (h1 == h2) {
                // 閉路形成
                if (size_[h1] < n_) {
                    // サブサーキット
                    set_initially_inconsistent(true);
                    return;
                }
            } else {
                // パス結合: h1 → ... → i → j → ... → t2
                size_t t2 = tail_[h2];
                tail_[h1] = t2;
                head_[h2] = h1;
                size_[h1] += size_[h2];
            }

            in_degree_[j] = 1;
            remove_from_pool(val);
        } else {
            ++unfixed_count_;
        }
    }

    // 初期整合性チェック
    check_initial_consistency();
}

std::string CircuitConstraint::name() const {
    return "circuit";
}

std::vector<VariablePtr> CircuitConstraint::variables() const {
    return vars_;
}

std::optional<bool> CircuitConstraint::is_satisfied() const {
    // 全変数が確定していなければ nullopt
    std::vector<Domain::value_type> values;
    for (const auto& var : vars_) {
        if (!var->is_assigned()) {
            return std::nullopt;
        }
        values.push_back(var->assigned_value().value());
    }

    // AllDifferent チェック
    std::set<Domain::value_type> unique_values(values.begin(), values.end());
    if (unique_values.size() != n_) {
        return false;
    }

    // 閉路チェック: ノード 0 から始めて全ノードを訪問できるか
    std::set<size_t> visited;
    size_t current = 0;
    for (size_t step = 0; step < n_; ++step) {
        if (visited.count(current) > 0) {
            return false;  // 途中で既訪問ノードに戻った（サブサーキット）
        }
        visited.insert(current);
        current = static_cast<size_t>(values[current]);
    }

    // 全ノード訪問後、ノード 0 に戻るか
    return current == 0 && visited.size() == n_;
}

bool CircuitConstraint::propagate() {
    // レガシー propagate: 特に何もしない（on_instantiate で処理）
    return true;
}

size_t CircuitConstraint::find(size_t i) const {
    // Follow parent pointers to root (no path compression to keep rewind simple)
    while (head_[i] != i) {
        i = head_[i];
    }
    return i;
}

void CircuitConstraint::remove_from_pool(Domain::value_type value) {
    auto it = pool_idx_.find(value);
    if (it == pool_idx_.end() || it->second >= pool_n_) {
        return;  // 既にプールにない
    }

    // Sparse Set から削除（スワップ）
    size_t idx = it->second;
    size_t last_idx = pool_n_ - 1;
    Domain::value_type last_value = pool_[last_idx];

    pool_[idx] = last_value;
    pool_[last_idx] = value;
    pool_idx_[last_value] = idx;
    pool_idx_[value] = last_idx;
    --pool_n_;
}

bool CircuitConstraint::on_instantiate(Model& model, int save_point,
                                        size_t var_idx, Domain::value_type value,
                                        Domain::value_type /*prev_min*/,
                                        Domain::value_type /*prev_max*/) {
    // モデルから変数ポインタを取得し、O(1) で内部インデックスを特定
    Variable* var_ptr = model.variable(var_idx).get();
    auto it = var_ptr_to_idx_.find(var_ptr);
    if (it == var_ptr_to_idx_.end()) {
        // この制約に関係ない変数
        return true;
    }
    size_t internal_idx = it->second;

    size_t i = internal_idx;
    size_t j = static_cast<size_t>(value);

    // AllDifferent チェック: ノード j の入次数が既に 1 なら重複
    if (in_degree_[j] > 0) {
        return false;
    }

    // i と j を含むパスの root を見つける
    size_t h1 = find(i);
    size_t h2 = find(j);

    // サブサーキット検出: 同じパス内なら閉路形成
    if (h1 == h2) {
        // 閉路が形成される
        if (size_[h1] < n_) {
            // サブサーキット（全ノードを含まない閉路）
            return false;
        }
        // size == n なら正当な完全閉路
        // 入次数とプールを更新して trail に記録
        size_t old_pool_n = pool_n_;
        size_t old_unfixed_count = unfixed_count_;
        in_degree_[j] = 1;
        remove_from_pool(value);
        --unfixed_count_;

        TrailEntry entry{0, 0, 0, 0, value, old_pool_n, old_unfixed_count, false};
        trail_.push_back({save_point, entry});

        // 残り1変数チェック（O(1)）
        if (unfixed_count_ == 1) {
            size_t last_idx = find_last_uninstantiated();
            if (last_idx != SIZE_MAX) {
                if (!on_last_uninstantiated(model, save_point, last_idx)) {
                    return false;
                }
            }
        } else if (unfixed_count_ == 0) {
            return on_final_instantiate();
        }

        return true;
    }

    // パス結合: h1 → ... → i → j → ... → t2
    size_t t2 = tail_[h2];  // h2 の root のパスの末尾

    // trail に記録 (h2 の親を h1 に変更する前の状態)
    size_t old_tail_h1 = tail_[h1];
    size_t old_size_h1 = size_[h1];
    size_t old_pool_n = pool_n_;
    size_t old_unfixed_count = unfixed_count_;

    TrailEntry entry{h1, old_tail_h1, h2, old_size_h1, value, old_pool_n, old_unfixed_count, true};
    trail_.push_back({save_point, entry});

    // 更新: h2 のパスを h1 のパスに統合
    tail_[h1] = t2;
    head_[h2] = h1;  // h2 の親を h1 に (Union-Find の union)
    size_[h1] += size_[h2];
    in_degree_[j] = 1;  // ノード j の入次数をインクリメント
    remove_from_pool(value);  // 値 j をプールから削除
    --unfixed_count_;

    // 鳩の巣原理: 未確定変数 > 利用可能な値 なら矛盾
    if (unfixed_count_ > pool_n_) {
        return false;
    }

    // 残り1変数チェック（O(1)）
    if (unfixed_count_ == 1) {
        size_t last_idx = find_last_uninstantiated();
        if (last_idx != SIZE_MAX) {
            if (!on_last_uninstantiated(model, save_point, last_idx)) {
                return false;
            }
        }
    } else if (unfixed_count_ == 0) {
        return on_final_instantiate();
    }

    return true;
}

bool CircuitConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                                  size_t last_var_internal_idx) {
    auto& last_var = vars_[last_var_internal_idx];

    // 既に確定している場合は整合性チェックのみ
    if (last_var->is_assigned()) {
        auto val = last_var->assigned_value().value();
        size_t j = static_cast<size_t>(val);
        // その値がプールに残っているか（他の変数と重複していないか）
        auto it = pool_idx_.find(val);
        return (it != pool_idx_.end() && it->second < pool_n_) && in_degree_[j] == 0;
    }

    // 利用可能な値が1つだけなら自動決定
    if (pool_n_ == 1) {
        Domain::value_type remaining_value = pool_[0];
        // モデル内の変数インデックスを特定
        for (size_t model_idx = 0; model_idx < model.variables().size(); ++model_idx) {
            if (model.variable(model_idx) == last_var) {
                model.enqueue_instantiate(model_idx, remaining_value);
                break;
            }
        }
    }
    // 利用可能な値が0なら矛盾
    else if (pool_n_ == 0) {
        return false;
    }

    return true;
}

bool CircuitConstraint::on_final_instantiate() {
    // 全ての変数が確定したときの最終確認: 単一のハミルトン閉路を形成しているか
    // ノード 0 を含むパスの root を取得し、そのサイズが n であれば OK
    size_t h0 = find(0);
    return size_[h0] == n_;
}

void CircuitConstraint::rewind_to(int save_point) {
    while (!trail_.empty() && trail_.back().first > save_point) {
        const auto& entry = trail_.back().second;

        // 入次数を戻す
        size_t j = static_cast<size_t>(entry.j);
        in_degree_[j] = 0;

        // プールを戻す
        pool_n_ = entry.old_pool_n;

        // 未確定カウントを戻す
        unfixed_count_ = entry.old_unfixed_count;

        // パス結合の場合のみ head/tail/size を戻す
        if (entry.is_merge) {
            tail_[entry.h1] = entry.old_tail_h1;
            head_[entry.h2] = entry.h2;  // h2 を root に戻す
            size_[entry.h1] = entry.old_size_h1;
        }

        trail_.pop_back();
    }
}

void CircuitConstraint::check_initial_consistency() {
    // 既に初期矛盾が設定されている場合はスキップ
    if (is_initially_inconsistent()) {
        return;
    }

    // 鳩の巣原理: 未確定変数 > 利用可能な値 なら矛盾
    if (unfixed_count_ > pool_n_) {
        set_initially_inconsistent(true);
    }
}

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

bool IntElementConstraint::propagate() {
    // 簡易伝播: 特に何もしない（on_instantiate で処理）
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
            // モデル内の変数インデックスを特定して enqueue
            for (size_t model_idx = 0; model_idx < model.variables().size(); ++model_idx) {
                if (model.variable(model_idx) == result_var_) {
                    model.enqueue_instantiate(model_idx, expected_result);
                    break;
                }
            }
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
            for (size_t model_idx = 0; model_idx < model.variables().size(); ++model_idx) {
                if (model.variable(model_idx) == index_var_) {
                    model.enqueue_instantiate(model_idx, candidates[0]);
                    break;
                }
            }
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

} // namespace sabori_csp
