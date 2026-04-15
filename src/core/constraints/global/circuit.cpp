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
    : Constraint(extract_var_ids(vars))
    , n_(var_ids_.size())
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

    // 既に確定している変数のパス結合と入次数を設定 + 未確定カウント
    for (size_t i = 0; i < n_; ++i) {
        if (vars[i]->is_assigned()) {
            auto val = vars[i]->assigned_value().value();
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

}

std::string CircuitConstraint::name() const {
    return "circuit";
}

bool CircuitConstraint::prepare_propagation(Model& model) {
    // presolve 後の変数状態に基づいて内部状態を完全に再構築
    for (size_t i = 0; i < n_; ++i) {
        head_[i] = i;
        tail_[i] = i;
        size_[i] = 1;
        in_degree_[i] = 0;
    }
    unfixed_count_ = 0;
    pool_n_ = n_;
    for (size_t i = 0; i < n_; ++i) {
        pool_[i] = static_cast<Domain::value_type>(i);
        pool_idx_[static_cast<Domain::value_type>(i)] = i;
    }
    trail_.clear();

    for (size_t i = 0; i < n_; ++i) {
        if (model.is_instantiated(var_ids_[i])) {
            auto val = model.value(var_ids_[i]);
            size_t j = static_cast<size_t>(val - base_offset_);
            if (j >= n_) return false;
            if (in_degree_[j] > 0) return false;

            size_t h1 = find(i);
            size_t h2 = find(j);
            if (h1 == h2) {
                if (size_[h1] < n_) return false;
            } else {
                size_t t2 = tail_[h2];
                tail_[h1] = t2;
                head_[h2] = h1;
                size_[h1] += size_[h2];
            }
            in_degree_[j] = 1;
            remove_from_pool(static_cast<Domain::value_type>(j));
        } else {
            ++unfixed_count_;
        }
    }

    if (unfixed_count_ > pool_n_) return false;

    return true;
}

PresolveResult CircuitConstraint::presolve(Model& model) {
    // 初期伝播: 特に何もしない（on_instantiate で処理）
    return PresolveResult::Unchanged;
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
                                        size_t var_idx, size_t internal_var_idx,
                                        Domain::value_type value,
                                        Domain::value_type /*prev_min*/,
                                        Domain::value_type /*prev_max*/) {
    size_t i = internal_var_idx;
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
            size_t last_idx = find_last_uninstantiated(model);
            if (last_idx != SIZE_MAX) {
                if (!on_last_uninstantiated(model, save_point, last_idx)) {
                    return false;
                }
            }
        } else if (unfixed_count_ == 0) {
            return on_final_instantiate(model);
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
        size_t last_idx = find_last_uninstantiated(model);
        if (last_idx != SIZE_MAX) {
            if (!on_last_uninstantiated(model, save_point, last_idx)) {
                return false;
            }
        }
    } else if (unfixed_count_ == 0) {
        return on_final_instantiate(model);
    }

    return true;
}

bool CircuitConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                                  size_t last_var_internal_idx) {
    auto last_var_id = var_ids_[last_var_internal_idx];

    // 既に確定している場合は整合性チェックのみ
    if (model.is_instantiated(last_var_id)) {
        auto val = model.value(last_var_id);
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
        model.enqueue_instantiate(last_var_id, remaining_value);
    }
    // 利用可能な値が0なら矛盾
    else if (pool_n_ == 0) {
        return false;
    }

    return true;
}

bool CircuitConstraint::on_final_instantiate(const Model& model) {
    (void)model;
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

void CircuitConstraint::bump_activity(const Model& model, size_t trigger_var_idx,
                                       double* activity, double activity_inc,
                                       bool& need_rescale, std::mt19937& rng) const {
    if (!model.is_instantiated(trigger_var_idx)) return;

    auto trigger_val = model.value(trigger_var_idx);

    // 原因となる変数と衝突している変数の 2つがあるはず
    const double inc = 0.5 * activity_inc;

    for (size_t vid : var_ids_) {
        if (!model.is_instantiated(vid)) continue;
        if (model.value(vid) == trigger_val) {
            bump_variable_activity(activity, vid, inc, need_rescale, rng);
        }
    }
}

// ============================================================================

}  // namespace sabori_csp

