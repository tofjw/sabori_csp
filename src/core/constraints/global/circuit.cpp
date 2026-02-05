#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <set>
#include <limits>

namespace sabori_csp {

// ============================================================================
// CircuitConstraint implementation
// ============================================================================

CircuitConstraint::CircuitConstraint(std::vector<VariablePtr> vars)
    : Constraint(vars)
    , n_(vars.size())
    , base_offset_(0)
    , head_(n_)
    , tail_(n_)
    , size_(n_, 1)
    , in_degree_(n_, 0)
    , unfixed_count_(0)
    , pool_n_(n_) {
    // ベースオフセットを検出（全変数の値域のグローバル最小値から）
    // FlatZinc の circuit は通常 1-based（値の範囲は 1..n）
    if (!vars.empty()) {
        Domain::value_type global_min = std::numeric_limits<Domain::value_type>::max();
        for (const auto& v : vars) {
            if (!v->domain().empty()) {
                global_min = std::min(global_min, v->min());
            }
        }
        if (global_min != std::numeric_limits<Domain::value_type>::max()) {
            base_offset_ = global_min;
        }
    }

    // 初期状態: 各ノードは長さ1のパス（自分自身が root）
    for (size_t i = 0; i < n_; ++i) {
        head_[i] = i;
        tail_[i] = i;
    }

    // プール初期化（0 から n-1 の内部インデックス）
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
            // 値を内部インデックス（0-based）に変換
            size_t j = static_cast<size_t>(val - base_offset_);

            // 範囲チェック
            if (j >= n_) {
                set_initially_inconsistent(true);
                return;
            }

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
            // 内部インデックスをプールから削除
            remove_from_pool(static_cast<Domain::value_type>(j));
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
        // 値を内部インデックスに変換
        current = static_cast<size_t>(values[current] - base_offset_);
    }

    // 全ノード訪問後、ノード 0 に戻るか
    return current == 0 && visited.size() == n_;
}

bool CircuitConstraint::propagate(Model& /*model*/) {
    // 初期伝播: 特に何もしない（on_instantiate で処理）
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
    // 値を内部インデックス（0-based）に変換
    size_t j = static_cast<size_t>(value - base_offset_);

    // 範囲チェック
    if (j >= n_) {
        return false;
    }

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
        remove_from_pool(static_cast<Domain::value_type>(j));
        --unfixed_count_;

        // TrailEntryのjは内部インデックスを保存
        TrailEntry entry{0, 0, 0, 0, static_cast<Domain::value_type>(j), old_pool_n, old_unfixed_count, false};
        trail_.push_back({save_point, entry});
        model.mark_constraint_dirty(model_index(), save_point);

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

    // TrailEntryのjは内部インデックスを保存
    TrailEntry entry{h1, old_tail_h1, h2, old_size_h1, static_cast<Domain::value_type>(j), old_pool_n, old_unfixed_count, true};
    trail_.push_back({save_point, entry});
    model.mark_constraint_dirty(model_index(), save_point);

    // 更新: h2 のパスを h1 のパスに統合
    tail_[h1] = t2;
    head_[h2] = h1;  // h2 の親を h1 に (Union-Find の union)
    size_[h1] += size_[h2];
    in_degree_[j] = 1;  // ノード j の入次数をインクリメント
    remove_from_pool(static_cast<Domain::value_type>(j));  // 内部インデックス j をプールから削除
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
        // 値を内部インデックスに変換
        size_t j = static_cast<size_t>(val - base_offset_);
        if (j >= n_) {
            return false;
        }
        // その値がプールに残っているか（他の変数と重複していないか）
        // プールには内部インデックスが格納されている
        auto it = pool_idx_.find(static_cast<Domain::value_type>(j));
        return (it != pool_idx_.end() && it->second < pool_n_) && in_degree_[j] == 0;
    }

    // 利用可能な値が1つだけなら自動決定
    if (pool_n_ == 1) {
        // プールには内部インデックスが格納されているので、元の値に戻す
        Domain::value_type remaining_value = pool_[0] + base_offset_;
        model.enqueue_instantiate(last_var->id(), remaining_value);
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

}  // namespace sabori_csp

