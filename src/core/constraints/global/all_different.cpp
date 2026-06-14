#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <cmath>
#include <set>

namespace sabori_csp {

namespace {

// López-Ortiz bounds(Z) フィルタ用の経路操作（union-find 風のリンク配列）
inline int bz_pathmax(const std::vector<int>& a, int x) {
    while (a[x] > x) x = a[x];
    return x;
}

inline int bz_pathmin(const std::vector<int>& a, int x) {
    while (a[x] < x) x = a[x];
    return x;
}

inline void bz_pathset(std::vector<int>& a, int start, int end, int to) {
    int next = start;
    while (next != end) {
        int prev = next;
        next = a[prev];
        a[prev] = to;
    }
}

}  // namespace

// ============================================================================
// AllDifferentConstraint implementation
// ============================================================================

AllDifferentConstraint::AllDifferentConstraint(std::vector<VariablePtr> vars)
    : Constraint(extract_var_ids(vars))
    , unfixed_count_(0) {
    // 全変数の値の和集合をプールとして構築
    std::set<Domain::value_type> all_values;
    for (const auto& var : vars) {
        var->domain().for_each_value([&](auto v) { all_values.insert(v); });
    }
    pool_.assign(std::vector<Domain::value_type>(all_values.begin(), all_values.end()));

    // 既に確定している変数の値をプールから削除 + 未確定カウント初期化
    for (const auto& var : vars) {
        if (var->is_assigned()) {
            pool_.remove(var->assigned_value().value());
        } else {
            ++unfixed_count_;
        }
    }

    // 初期整合性チェック
}

std::string AllDifferentConstraint::name() const {
    return "all_different";
}

bool AllDifferentConstraint::prepare_propagation(Model& model) {
    // presolve 後の変数状態に基づいてプールと未確定カウントを再構築
    pool_.reset_all_active();
    unfixed_count_ = 0;
    pool_trail_.clear();
    ++bz_epoch_;  // 前回探索のエコー検出エントリを無効化

    for (size_t i = 0; i < var_ids_.size(); ++i) {
        auto vid = var_ids_[i];
        if (model.is_instantiated(vid)) {
            if (!pool_.remove(model.value(vid))) {
                // 値がプールにない = 重複
                return false;
            }
        } else {
            ++unfixed_count_;
            model.set_no_bisect(vid);
        }
    }

    if (unfixed_count_ > pool_.size()) return false;

    return true;
}

PresolveResult AllDifferentConstraint::presolve(Model& model) {
    bool changed = false;
    // 確定した変数の値を他の変数から削除
    for (size_t i = 0; i < var_ids_.size(); ++i) {
        auto* var = model.variable(var_ids_[i]);
        if (var->is_assigned()) {
            auto val = var->assigned_value().value();
            for (size_t j = 0; j < var_ids_.size(); ++j) {
                if (j != i) {
                    auto* other = model.variable(var_ids_[j]);
                    if (!other->is_assigned()) {
                        if (other->domain().contains(val)) {
                            other->remove(val);
                            changed = true;
                        }
                        if (other->domain().empty()) {
                            return PresolveResult::Contradiction;
                        }
                    }
                }
            }
        }
    }

    // bounds(Z) フィルタ: Hall interval による bounds の絞り込み
    const size_t n = var_ids_.size();
    if (n >= 2) {
        bz_min_.resize(n);
        bz_max_.resize(n);
        bz_newmin_.resize(n);
        bz_newmax_.resize(n);
        for (size_t i = 0; i < n; ++i) {
            auto* var = model.variable(var_ids_[i]);
            bz_min_[i] = var->min();
            bz_max_[i] = var->max();
            bz_newmin_[i] = bz_min_[i];
            bz_newmax_[i] = bz_max_[i];
        }
        bool bz_changed = false;
        if (!run_bounds_filter(n, bz_changed)) {
            return PresolveResult::Contradiction;
        }
        if (bz_changed) {
            for (size_t i = 0; i < n; ++i) {
                auto* var = model.variable(var_ids_[i]);
                if (bz_newmin_[i] > bz_min_[i] && !var->remove_below(bz_newmin_[i])) {
                    return PresolveResult::Contradiction;
                }
                if (bz_newmax_[i] < bz_max_[i] && !var->remove_above(bz_newmax_[i])) {
                    return PresolveResult::Contradiction;
                }
            }
            changed = true;
        }
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool AllDifferentConstraint::on_instantiate(Model& model, int save_point,
					    size_t var_idx, size_t internal_var_idx, Domain::value_type value,
					    Domain::value_type prev_min,
					    Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value, prev_min, prev_max)) {
      return false;
    }

    // プールから値を削除
    if (!pool_.contains(value)) {
        // 既にプールにない = 他の変数が使用済み
        return false;
    }

    // Trail に保存
    if (pool_trail_.save_if_needed(save_point, {pool_.active_count(), unfixed_count_})) {
        model.mark_constraint_dirty(model_index(), save_point);
    }

    // プールから削除
    pool_.remove(value);

    // 未確定カウントをデクリメント（O(1)）
    --unfixed_count_;

    // 鳩の巣原理: 未確定変数 > 利用可能な値 なら矛盾
    if (unfixed_count_ > pool_.size()) {
        return false;
    }

    // 確定した値を他の未確定変数のドメインから削除
    for (size_t i = 0; i < var_ids_.size(); ++i) {
        auto vid = var_ids_[i];
        if (!model.is_instantiated(vid) && model.contains(vid, value)) {
            model.enqueue_remove_value(vid, value);
        }
    }

    // 確定で区間 [value, value] が固定された → bounds(Z) フィルタをバッチ登録
    // （イベントキューが空になった時点で propagate_batch が1回実行される）
    model.schedule_constraint_batch(model_index());

    if (unfixed_count_ <= 1) {
      size_t last_idx = SIZE_MAX;
      if (!model.is_instantiated(var_ids_[watch1()])) {
	last_idx = watch1();
      }
      if (!model.is_instantiated(var_ids_[watch2()])) {
	last_idx = watch2();
      }

      if (last_idx == SIZE_MAX) {
        return on_final_instantiate(model);
      }
      else {
	// 残り1変数になったら on_last_uninstantiated を呼び出す
	return on_last_uninstantiated(model, save_point, last_idx);
      }
    }

    return true;
}

bool AllDifferentConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
						    size_t last_var_internal_idx) {
    auto last_var_id = var_ids_[last_var_internal_idx];

    // 既に確定している場合は整合性チェックのみ
    if (model.is_instantiated(last_var_id)) {
        auto val = model.value(last_var_id);
        // その値がプールに残っているか（他の変数と重複していないか）
        return pool_.contains(val);
    }

    // 利用可能な値が1つだけなら自動決定
    if (pool_.size() == 1) {
        Domain::value_type remaining_value = pool_.value_at(0);
        model.enqueue_instantiate(last_var_id, remaining_value);
    }
    // 利用可能な値が0なら矛盾
    else if (pool_.size() == 0) {
        return false;
    }
    // 複数の値が利用可能な場合は、ドメインを絞り込む
    // 注: ここでは単純化のため、確定のみを行う

    return true;
}

bool AllDifferentConstraint::on_final_instantiate(const Model& model) {
    // 全変数が異なる値を持つか確認
    std::set<Domain::value_type> used_values;
    for (size_t i = 0; i < var_ids_.size(); ++i) {
        auto val = model.value(var_ids_[i]);
        if (used_values.count(val) > 0) {
            return false;
        }
        used_values.insert(val);
    }
    return true;
}

bool AllDifferentConstraint::run_bounds_filter(size_t n, bool& changed) {
    changed = false;
    if (n < 2) return true;

    if (bz_minsorted_.size() != n) {
        bz_minsorted_.resize(n);
        bz_maxsorted_.resize(n);
        for (size_t i = 0; i < n; ++i) {
            bz_minsorted_[i] = i;
            bz_maxsorted_[i] = i;
        }
        bz_minrank_.resize(n);
        bz_maxrank_.resize(n);
        bz_bounds_.resize(2 * n + 2);
        bz_t_.resize(2 * n + 2);
        bz_h_.resize(2 * n + 2);
        bz_d_.resize(2 * n + 2);
    }

    // ソート順は呼び出し間でほぼ保存されるので、前回の順列を初期値に使う
    std::sort(bz_minsorted_.begin(), bz_minsorted_.end(),
              [this](size_t a, size_t b) { return bz_min_[a] < bz_min_[b]; });
    std::sort(bz_maxsorted_.begin(), bz_maxsorted_.end(),
              [this](size_t a, size_t b) { return bz_max_[a] < bz_max_[b]; });

    // 全区間の端点 {min_i, max_i+1} をマージして臨界値列 bounds[1..nb] を構築
    Domain::value_type minv = bz_min_[bz_minsorted_[0]];
    Domain::value_type maxv = bz_max_[bz_maxsorted_[0]] + 1;
    Domain::value_type last = minv - 2;
    bz_bounds_[0] = last;
    int nb = 0;
    {
        size_t i = 0, j = 0;
        for (;;) {
            if (i < n && minv <= maxv) {
                if (minv != last) bz_bounds_[++nb] = last = minv;
                bz_minrank_[bz_minsorted_[i]] = nb;
                if (++i < n) minv = bz_min_[bz_minsorted_[i]];
            } else {
                if (maxv != last) bz_bounds_[++nb] = last = maxv;
                bz_maxrank_[bz_maxsorted_[j]] = nb;
                if (++j == n) break;
                maxv = bz_max_[bz_maxsorted_[j]] + 1;
            }
        }
    }
    bz_bounds_[nb + 1] = bz_bounds_[nb] + 2;

    // ---- 下限フィルタ: max 昇順に区間を挿入し、Hall interval で min を押し上げる ----
    for (int k = 1; k <= nb + 1; ++k) {
        bz_t_[k] = k - 1;
        bz_h_[k] = k - 1;
        bz_d_[k] = bz_bounds_[k] - bz_bounds_[k - 1];
    }
    for (size_t idx = 0; idx < n; ++idx) {
        size_t vi = bz_maxsorted_[idx];
        int x = bz_minrank_[vi];
        int y = bz_maxrank_[vi];
        int z = bz_pathmax(bz_t_, x + 1);
        int j = bz_t_[z];
        if (--bz_d_[z] == 0) {
            bz_t_[z] = z + 1;
            z = bz_pathmax(bz_t_, z + 1);
            bz_t_[z] = j;
        }
        bz_pathset(bz_t_, x + 1, z, z);
        if (bz_d_[z] < bz_bounds_[z] - bz_bounds_[y]) {
            return false;  // Hall interval の容量超過 = 矛盾
        }
        if (bz_h_[x] > x) {
            int w = bz_pathmax(bz_h_, bz_h_[x]);
            bz_newmin_[vi] = bz_bounds_[w];
            bz_pathset(bz_h_, x, w, w);
            changed = true;
        }
        if (bz_d_[z] == bz_bounds_[z] - bz_bounds_[y]) {
            bz_pathset(bz_h_, bz_h_[y], j - 1, y);
            bz_h_[y] = j - 1;
        }
    }

    // ---- 上限フィルタ: min 降順に区間を挿入し、Hall interval で max を押し下げる ----
    for (int k = 0; k <= nb; ++k) {
        bz_t_[k] = k + 1;
        bz_h_[k] = k + 1;
        bz_d_[k] = bz_bounds_[k + 1] - bz_bounds_[k];
    }
    for (size_t idx = n; idx-- > 0;) {
        size_t vi = bz_minsorted_[idx];
        int x = bz_maxrank_[vi];
        int y = bz_minrank_[vi];
        int z = bz_pathmin(bz_t_, x - 1);
        int j = bz_t_[z];
        if (--bz_d_[z] == 0) {
            bz_t_[z] = z - 1;
            z = bz_pathmin(bz_t_, z - 1);
            bz_t_[z] = j;
        }
        bz_pathset(bz_t_, x - 1, z, z);
        if (bz_d_[z] < bz_bounds_[y] - bz_bounds_[z]) {
            return false;
        }
        if (bz_h_[x] < x) {
            int w = bz_pathmin(bz_h_, bz_h_[x]);
            bz_newmax_[vi] = bz_bounds_[w] - 1;
            bz_pathset(bz_h_, x, w, w);
            changed = true;
        }
        if (bz_d_[z] == bz_bounds_[y] - bz_bounds_[z]) {
            bz_pathset(bz_h_, bz_h_[y], j + 1, y);
            bz_h_[y] = j + 1;
        }
    }

    return true;
}

bool AllDifferentConstraint::propagate_bounds_z(Model& model, int save_point) {
    if (!bounds_z_enabled_) return true;
    const size_t n = var_ids_.size();
    if (n < 2 || unfixed_count_ < 2) return true;

    bz_min_.resize(n);
    bz_max_.resize(n);
    bz_newmin_.resize(n);
    bz_newmax_.resize(n);
    for (size_t i = 0; i < n; ++i) {
        bz_min_[i] = model.var_min(var_ids_[i]);
        bz_max_[i] = model.var_max(var_ids_[i]);
        bz_newmin_[i] = bz_min_[i];
        bz_newmax_[i] = bz_max_[i];
    }

    bool changed = false;
    if (!run_bounds_filter(n, changed)) return false;
    if (!changed) return true;

    if (bz_expected_min_.size() != n) {
        bz_expected_min_.assign(n, 0);
        bz_expected_max_.assign(n, 0);
        bz_expected_min_epoch_.assign(n, 0);
        bz_expected_max_epoch_.assign(n, 0);
    }

    // rewind_to() で epoch を無効化できるよう dirty マークしておく
    model.mark_constraint_dirty(model_index(), save_point);

    for (size_t i = 0; i < n; ++i) {
        auto vid = var_ids_[i];
        if (bz_newmin_[i] > bz_newmax_[i]) return false;
        if (bz_newmin_[i] > bz_min_[i]) {
            bz_expected_min_[i] = bz_newmin_[i];
            bz_expected_min_epoch_[i] = bz_epoch_;
            model.enqueue_set_min(vid, bz_newmin_[i]);
        }
        if (bz_newmax_[i] < bz_max_[i]) {
            bz_expected_max_[i] = bz_newmax_[i];
            bz_expected_max_epoch_[i] = bz_epoch_;
            model.enqueue_set_max(vid, bz_newmax_[i]);
        }
    }
    return true;
}

bool AllDifferentConstraint::check_hall_pair(Model& model, size_t trigger_var_idx) {
    auto& vd = model.var_data(trigger_var_idx);
    if (vd.size != 2) return true;

    auto v1 = vd.min;
    auto v2 = vd.max;

    // 同じ {v1, v2} ドメインを持つ未確定変数を数える
    size_t match_count = 0;
    for (size_t i = 0; i < var_ids_.size(); ++i) {
        auto vid = var_ids_[i];
        if (model.is_instantiated(vid)) continue;
        auto& d = model.var_data(vid);
        if (d.size == 2 && d.min == v1 && d.max == v2) {
            ++match_count;
        }
    }

    if (match_count >= 3) {
        return false;
    }

    if (match_count == 2) {
        // 残りの変数から v1, v2 を削除
        for (size_t i = 0; i < var_ids_.size(); ++i) {
            auto vid = var_ids_[i];
            if (model.is_instantiated(vid)) continue;
            auto& d = model.var_data(vid);
            if (d.size == 2 && d.min == v1 && d.max == v2) continue;
            if (model.contains(vid, v1)) {
                model.enqueue_remove_value(vid, v1);
            }
            if (model.contains(vid, v2)) {
                model.enqueue_remove_value(vid, v2);
            }
        }
    }

    return true;
}

bool AllDifferentConstraint::on_remove_value(Model& model, int save_point,
                                              size_t var_idx, size_t internal_var_idx,
                                              Domain::value_type removed_value) {
    return check_hall_pair(model, var_idx);
}

bool AllDifferentConstraint::on_set_min(Model& model, int save_point,
                                         size_t var_idx, size_t internal_var_idx,
                                         Domain::value_type new_min,
                                         Domain::value_type old_min) {
    if (!check_hall_pair(model, var_idx)) return false;
    // 自分が enqueue した set_min のエコーならフィルタ済みなのでスキップ
    if (internal_var_idx < bz_expected_min_epoch_.size() &&
        bz_expected_min_epoch_[internal_var_idx] == bz_epoch_ &&
        bz_expected_min_[internal_var_idx] == new_min) {
        bz_expected_min_epoch_[internal_var_idx] = 0;
        return true;
    }
    model.schedule_constraint_batch(model_index());
    return true;
}

bool AllDifferentConstraint::on_set_max(Model& model, int save_point,
                                         size_t var_idx, size_t internal_var_idx,
                                         Domain::value_type new_max,
                                         Domain::value_type old_max) {
    if (!check_hall_pair(model, var_idx)) return false;
    if (internal_var_idx < bz_expected_max_epoch_.size() &&
        bz_expected_max_epoch_[internal_var_idx] == bz_epoch_ &&
        bz_expected_max_[internal_var_idx] == new_max) {
        bz_expected_max_epoch_[internal_var_idx] = 0;
        return true;
    }
    model.schedule_constraint_batch(model_index());
    return true;
}

bool AllDifferentConstraint::propagate_batch(Model& model, int save_point) {
    return propagate_bounds_z(model, save_point);
}

void AllDifferentConstraint::rewind_to(int save_point) {
    pool_trail_.rewind_to(save_point, [&](const TrailEntry& entry) {
        pool_.restore_active_count(entry.old_pool_n);
        unfixed_count_ = entry.old_unfixed_count;
    });
    // バックトラック後はエコー検出用の expected エントリを一括無効化
    ++bz_epoch_;
}

void AllDifferentConstraint::bump_activity(const Model& model, size_t trigger_var_idx,
                                           double* activity, double activity_inc,
                                           bool& need_rescale, std::mt19937& rng) const {
    if (model.is_instantiated(trigger_var_idx)) {
        auto val = model.value(trigger_var_idx);
        if (pool_.consumed(val)) {
            size_t count = 0;
            for (size_t vid : var_ids_) {
                if (model.contains(vid, val)) {
                    ++count;
                }
            }

            if (count > 0) {
                double inc = activity_inc / count;
                for (size_t vid : var_ids_) {
                    if (model.contains(vid, val)) {
                        double a = inc / model.var_size(vid);
                        bump_variable_activity(activity, vid, a, need_rescale, rng);
                    }
                }
            }
        }
    }
    else {
        Constraint::bump_activity(model, trigger_var_idx, activity, activity_inc, need_rescale, rng);
    }
}

void AllDifferentConstraint::init_activity(const Model& model, double* activity) const {
    size_t n = 0;
    for (size_t vid : var_ids_) {
        if (!model.is_instantiated(vid)) ++n;
    }
    if (n <= 1) return;
    double nm1 = static_cast<double>(n - 1);
    for (size_t vid : var_ids_) {
        if (!model.is_instantiated(vid)) {
            size_t d = model.var_size(vid);
            if (d > 1) {
                double dd = static_cast<double>(d);
                activity[vid] += nm1 * (std::log(dd) - std::log(dd - 1.0));
            }
        }
    }
}

}  // namespace sabori_csp
