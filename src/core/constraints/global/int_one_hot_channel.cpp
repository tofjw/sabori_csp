#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <cmath>

namespace sabori_csp {

namespace {

std::vector<size_t> build_var_ids(const VariablePtr& x,
                                  const std::vector<VariablePtr>& bools) {
    std::vector<size_t> ids;
    ids.reserve(1 + bools.size());
    ids.push_back(x->id());
    for (const auto& b : bools) ids.push_back(b->id());
    return ids;
}

} // namespace

IntOneHotChannelConstraint::IntOneHotChannelConstraint(
    VariablePtr x,
    std::vector<Domain::value_type> values,
    std::vector<VariablePtr> bools)
    : Constraint(build_var_ids(x, bools))
    , x_id_(x->id())
    , offset_(0)
    , contiguous_(false)
    , holes_(0)
{
    // values と bools をペアにして value 昇順にソート。aggregator から渡される
    // 場合は既にソート済みだが、直接インスタンス化するユーザにも安全に動作させる。
    std::vector<std::pair<Domain::value_type, size_t>> pairs;
    pairs.reserve(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        pairs.emplace_back(values[i], bools[i]->id());
    }
    std::sort(pairs.begin(), pairs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    values_.reserve(pairs.size());
    b_ids_.reserve(pairs.size());
    for (const auto& [v, bid] : pairs) {
        values_.push_back(v);
        b_ids_.push_back(bid);
    }

    // 連続値判定: v[0], v[0]+1, ..., v[0]+(N-1) なら添字 = v - offset_ で O(1)
    if (!values_.empty()) {
        offset_ = values_.front();
        contiguous_ = (static_cast<size_t>(values_.back() - values_.front() + 1)
                       == values_.size());
    }

    // holes_ の判定: x の構築時ドメインに含まれる値のうち、values_ に
    // 入っていないもの（= "穴"）の個数を数える。0 のとき exhaustive。
    // 「x が values_ 外を取りうる余地がいくつあるか」を表し、伝播力の
    // 弱まりがそのまま見える指標になる。
    {
        size_t covered = 0;
        for (auto v : values_) {
            if (x->domain().contains(v)) ++covered;
        }
        // x->domain().size() は x の現在の値数（= 構築時点）。values_ のうち
        // ドメイン外のエントリは "dead b"（常に 0）でカバーには寄与しない。
        size_t dom_size = x->domain().size();
        holes_ = (dom_size > covered) ? (dom_size - covered) : 0;
    }
}

std::string IntOneHotChannelConstraint::name() const {
    return "int_one_hot_channel";
}

int IntOneHotChannelConstraint::find_value_index(Domain::value_type v) const {
    if (contiguous_) {
        // O(1): v - offset_ で直接添字化。範囲外なら -1。
        auto k = v - offset_;
        if (k < 0 || static_cast<size_t>(k) >= values_.size()) return -1;
        return static_cast<int>(k);
    }
    // 非連続: values_ はソート済みなので二分探索 O(log N)
    auto it = std::lower_bound(values_.begin(), values_.end(), v);
    if (it != values_.end() && *it == v) {
        return static_cast<int>(it - values_.begin());
    }
    return -1;
}

int IntOneHotChannelConstraint::find_b_index(size_t var_id) const {
    for (size_t i = 0; i < b_ids_.size(); ++i) {
        if (b_ids_[i] == var_id) return static_cast<int>(i);
    }
    return -1;
}

std::optional<bool> IntOneHotChannelConstraint::is_satisfied(const Model& model) const {
    if (!model.is_instantiated(x_id_)) return std::nullopt;
    for (size_t b : b_ids_) {
        if (!model.is_instantiated(b)) return std::nullopt;
    }
    auto xv = model.value(x_id_);
    int matched = find_value_index(xv);
    for (size_t i = 0; i < b_ids_.size(); ++i) {
        bool expected = (static_cast<int>(i) == matched);
        bool actual = (model.value(b_ids_[i]) == 1);
        if (expected != actual) return false;
    }
    if (holes_ == 0 && matched == -1) return false;
    return true;
}

PresolveResult IntOneHotChannelConstraint::presolve(Model& model) {
    // presolve 期間中: variable->* で直接ドメイン操作する
    bool changed = false;
    auto* x_var = model.variable(x_id_);

    // 1) すでに 1 に固定された b_i があれば x = values[i] を強制し他の b_j を 0 に
    int fixed_true = -1;
    for (size_t i = 0; i < b_ids_.size(); ++i) {
        auto* bv = model.variable(b_ids_[i]);
        if (bv->is_assigned() && bv->assigned_value().value() == 1) {
            if (fixed_true >= 0) {
                // 2 個以上の b が 1 に固定 → 矛盾
                return PresolveResult::Contradiction;
            }
            fixed_true = static_cast<int>(i);
        }
    }
    if (fixed_true >= 0) {
        auto v = values_[fixed_true];
        if (!x_var->domain().contains(v)) return PresolveResult::Contradiction;
        if (!x_var->is_assigned() || x_var->assigned_value().value() != v) {
            if (!x_var->assign(v)) return PresolveResult::Contradiction;
            changed = true;
        }
        for (size_t j = 0; j < b_ids_.size(); ++j) {
            if (static_cast<int>(j) == fixed_true) continue;
            auto* bj = model.variable(b_ids_[j]);
            if (bj->is_assigned()) {
                if (bj->assigned_value().value() != 0) return PresolveResult::Contradiction;
                continue;
            }
            if (!bj->assign(0)) return PresolveResult::Contradiction;
            changed = true;
        }
    }

    // 2) b_i = 0 が固定されている → x のドメインから values[i] を除去
    for (size_t i = 0; i < b_ids_.size(); ++i) {
        auto* bv = model.variable(b_ids_[i]);
        if (bv->is_assigned() && bv->assigned_value().value() == 0) {
            auto v = values_[i];
            if (x_var->domain().contains(v)) {
                if (!x_var->remove(v)) return PresolveResult::Contradiction;
                changed = true;
            }
        }
    }

    // 3) x のドメインに含まれない values[i] に対応する b_i を 0 に固定
    for (size_t i = 0; i < b_ids_.size(); ++i) {
        auto v = values_[i];
        if (!x_var->domain().contains(v)) {
            auto* bv = model.variable(b_ids_[i]);
            if (!bv->is_assigned()) {
                if (!bv->domain().contains(0)) return PresolveResult::Contradiction;
                if (!bv->assign(0)) return PresolveResult::Contradiction;
                changed = true;
            } else if (bv->assigned_value().value() != 0) {
                return PresolveResult::Contradiction;
            }
        }
    }

    // 4) x が確定 → 該当 b_i を 1、他を 0
    if (x_var->is_assigned()) {
        auto xv = x_var->assigned_value().value();
        int matched = find_value_index(xv);
        for (size_t i = 0; i < b_ids_.size(); ++i) {
            auto* bv = model.variable(b_ids_[i]);
            int target = (static_cast<int>(i) == matched) ? 1 : 0;
            if (matched < 0 && holes_ == 0) return PresolveResult::Contradiction;
            if (bv->is_assigned()) {
                if (bv->assigned_value().value() != target) {
                    if (matched < 0 && target == 0) continue; // partial: x が values 外なら matched=-1 で b はすべて 0、ここは到達しないはず
                    return PresolveResult::Contradiction;
                }
                continue;
            }
            if (!bv->domain().contains(target)) return PresolveResult::Contradiction;
            if (!bv->assign(target)) return PresolveResult::Contradiction;
            changed = true;
        }
    }

    // 5) holes_ == 0 (exhaustive) のみ: 残り未確定 b が 1 個で、他がすべて 0
    //    → 残り 1 個を 1 に。partial coverage では x が values_ 外を取りうるので
    //    この推論は使えない。
    if (holes_ == 0 && fixed_true < 0) {
        int unassigned_idx = -1;
        size_t unassigned_count = 0;
        for (size_t i = 0; i < b_ids_.size(); ++i) {
            auto* bv = model.variable(b_ids_[i]);
            if (!bv->is_assigned()) {
                ++unassigned_count;
                unassigned_idx = static_cast<int>(i);
            } else if (bv->assigned_value().value() == 1) {
                unassigned_idx = -2;
                break;
            }
        }
        if (unassigned_idx >= 0 && unassigned_count == 1) {
            auto* bv = model.variable(b_ids_[unassigned_idx]);
            if (!bv->domain().contains(1)) return PresolveResult::Contradiction;
            if (!bv->assign(1)) return PresolveResult::Contradiction;
            // x も同時に確定
            auto v = values_[unassigned_idx];
            if (!x_var->domain().contains(v)) return PresolveResult::Contradiction;
            if (!x_var->is_assigned()) {
                if (!x_var->assign(v)) return PresolveResult::Contradiction;
            } else if (x_var->assigned_value().value() != v) {
                return PresolveResult::Contradiction;
            }
            changed = true;
        }
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool IntOneHotChannelConstraint::prepare_propagation(Model& model) {
    init_watches();
    // トレイル付きカウンタを初期化
    trail_.clear();
    uninstantiated_b_count_ = 0;
    // 状態整合チェック: presolve 段階で全部済んでいる前提だが、念のため
    // 不一致がないか最終確認のみ行う（ドメイン変更は伴わない）。
    int fixed_true = -1;
    for (size_t i = 0; i < b_ids_.size(); ++i) {
        if (model.is_instantiated(b_ids_[i])) {
            if (model.value(b_ids_[i]) == 1) {
                if (fixed_true >= 0) return false;
                fixed_true = static_cast<int>(i);
            }
        } else {
            ++uninstantiated_b_count_;
        }
        if (!model.is_defined_var(b_ids_[i]))
            assert(0);
    }
    if (model.is_defined_var(x_id_))
        assert(0);
    // model.set_no_bisect(x_id_);
    if (fixed_true >= 0) {
        if (!model.contains(x_id_, values_[fixed_true])) return false;
    }

    return true;
}

bool IntOneHotChannelConstraint::on_instantiate(Model& model, int save_point,
                                                size_t internal_var_idx,
                                                Domain::value_type value,
                                                Domain::value_type prev_min,
                                                Domain::value_type prev_max) {
    const size_t var_idx = var_id(internal_var_idx);
    if (!Constraint::on_instantiate(model, save_point, internal_var_idx, value,
                                    prev_min, prev_max)) {
        return false;
    }

    // b_i が確定したら未確定カウンタを 1 減らす（bump_activity の O(1) 化用）
    if (var_idx != x_id_) {
        int hit = find_b_index(var_idx);
        if (hit >= 0) {
            trail_.push_back({save_point, uninstantiated_b_count_});
            model.mark_constraint_dirty(model_index(), save_point);
            --uninstantiated_b_count_;
        }
    }

    if (var_idx == x_id_) {
        // x 確定 → 該当 b を 1、他を 0
        int matched = find_value_index(value);
        if (matched < 0 && holes_ == 0) return false;
        for (size_t i = 0; i < b_ids_.size(); ++i) {
            int target = (static_cast<int>(i) == matched) ? 1 : 0;
            if (model.is_instantiated(b_ids_[i])) {
                if (model.value(b_ids_[i]) != target) return false;
            } else {
                model.enqueue_instantiate(b_ids_[i], target);
            }
        }
        return true;
    }

    int bi = find_b_index(var_idx);
    if (bi < 0) return true;  // 自分の制約に関係ない変数（理論上来ない）

    if (value == 1) {
        // b_i = 1 → x = values[i]、他の b_j = 0
        auto v = values_[bi];
        if (!model.contains(x_id_, v)) return false;
        if (!model.is_instantiated(x_id_)) {
            model.enqueue_instantiate(x_id_, v);
        } else if (model.value(x_id_) != v) {
            return false;
        }
        for (size_t j = 0; j < b_ids_.size(); ++j) {
            if (static_cast<int>(j) == bi) continue;
            if (model.is_instantiated(b_ids_[j])) {
                if (model.value(b_ids_[j]) != 0) return false;
            } else {
                model.enqueue_instantiate(b_ids_[j], 0);
            }
        }
    } else {
        // b_i = 0 → x のドメインから values[i] を除去
        auto v = values_[bi];
        if (model.contains(x_id_, v)) {
            model.enqueue_remove_value(x_id_, v);
        }
        // holes_ == 0 (exhaustive) のときだけ: 全 b が 0 で確定したら矛盾、
        // 未確定が 1 個でかつ他がすべて 0 のときその 1 個を 1 にする。
        // partial coverage (holes_ > 0) では x が values_ 外の値を取り得るので
        // この推論は使えない。
        if (holes_ == 0) {
            int last_unassigned = -1;
            size_t unassigned_count = 0;
            bool any_true = false;
            for (size_t j = 0; j < b_ids_.size(); ++j) {
                if (!model.is_instantiated(b_ids_[j])) {
                    ++unassigned_count;
                    last_unassigned = static_cast<int>(j);
                } else if (model.value(b_ids_[j]) == 1) {
                    any_true = true;
                }
            }
            if (unassigned_count == 0 && !any_true) {
                // 全 b が 0 で確定 → exhaustive で矛盾
                return false;
            }
            if (unassigned_count == 1 && !any_true && last_unassigned >= 0) {
                model.enqueue_instantiate(b_ids_[last_unassigned], 1);
            }
        }
    }
    return true;
}

bool IntOneHotChannelConstraint::on_set_min(Model& model, int /*save_point*/,
                                            size_t internal_var_idx,
                                            Domain::value_type new_min,
                                            Domain::value_type /*old_min*/) {
    const size_t var_idx = var_id(internal_var_idx);
    if (var_idx != x_id_) return true;
    // x の min が上がった → values[i] < new_min の b_i を 0 に
    for (size_t i = 0; i < b_ids_.size(); ++i) {
        if (values_[i] < new_min) {
            if (model.is_instantiated(b_ids_[i])) {
                if (model.value(b_ids_[i]) != 0) return false;
            } else {
                model.enqueue_instantiate(b_ids_[i], 0);
            }
        }
    }
    return true;
}

bool IntOneHotChannelConstraint::on_set_max(Model& model, int /*save_point*/,
                                            size_t internal_var_idx,
                                            Domain::value_type new_max,
                                            Domain::value_type /*old_max*/) {
    const size_t var_idx = var_id(internal_var_idx);
    if (var_idx != x_id_) return true;
    for (size_t i = 0; i < b_ids_.size(); ++i) {
        if (values_[i] > new_max) {
            if (model.is_instantiated(b_ids_[i])) {
                if (model.value(b_ids_[i]) != 0) return false;
            } else {
                model.enqueue_instantiate(b_ids_[i], 0);
            }
        }
    }
    return true;
}

bool IntOneHotChannelConstraint::on_remove_value(Model& model, int /*save_point*/,
                                                 size_t internal_var_idx,
                                                 Domain::value_type removed_value) {
    const size_t var_idx = var_id(internal_var_idx);
    if (var_idx != x_id_) return true;
    int idx = find_value_index(removed_value);
    if (idx < 0) return true;
    auto bid = b_ids_[idx];
    if (model.is_instantiated(bid)) {
        if (model.value(bid) != 0) return false;
    } else {
        model.enqueue_instantiate(bid, 0);
    }
    return true;
}

bool IntOneHotChannelConstraint::on_final_instantiate(const Model& model) {
    auto sat = is_satisfied(model);
    return sat.value_or(false);
}

void IntOneHotChannelConstraint::bump_activity(const Model& model, size_t trigger_var_idx,
                                               double* activity, double activity_inc,
                                               bool& need_rescale, std::mt19937& rng) const {
    if (holes_ == 0) {
        bump_variable_activity(activity, x_id_, activity_inc, need_rescale, rng);
    }
    else {
        auto var_size = model.var_size(x_id_);
        double density = 0.0;
        // トレイル化したカウンタを直接参照（旧実装の O(N) ループを置換）
        size_t n_bids = uninstantiated_b_count_;
        if (trigger_var_idx != x_id_)
            n_bids++;

        if (var_size > 1) {
            density = (double)n_bids / var_size;
        }
        else {
            // estimate original var_size before instantiated
            density = (double)n_bids / (n_bids + holes_);
        }
        
        bump_variable_activity(activity, x_id_, activity_inc * density, need_rescale, rng);
    }
}

void IntOneHotChannelConstraint::init_activity(const Model& model, double* activity) const {
    auto var_size = model.var_size(x_id_);
    if (holes_ == 0) {
        if (!model.is_instantiated(x_id_)) {
            activity[x_id_] += 1.0 / var_size;
        }
    }
    else {
        double density = static_cast<double>(var_size - holes_) / var_size;
        activity[x_id_] += density / var_size;
    }
}

void IntOneHotChannelConstraint::rewind_to(int save_point) {
    while (!trail_.empty() && trail_.back().first > save_point) {
        uninstantiated_b_count_ = trail_.back().second;
        trail_.pop_back();
    }
}

} // namespace sabori_csp
