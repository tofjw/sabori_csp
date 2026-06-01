/**
 * @file variable_selector.cpp
 * @brief 変数選択クラスの実装
 */
#include "sabori_csp/variable_selector.hpp"

#include <algorithm>

namespace sabori_csp {

void VariableSelector::build_order(const Model& model, std::mt19937& rng) {
    const auto& variables = model.variables();

    var_order_.clear();
    var_order_.reserve(variables.size());
    std::vector<size_t> defined_vars;
    std::vector<size_t> unconstrained_vars;

    for (size_t i = 0; i < variables.size(); ++i) {
        if (model.is_eliminated(i)) continue;
        // 制約に参照されない変数は探索に影響しない（FlatZinc の output 用ダミー
        // bool 等）。決定/活性化ヒューリスティクスを希釈しないよう最後尾へ。
        if (model.constraints_for_var(i).empty()) {
            unconstrained_vars.push_back(i);
        } else if (model.is_defined_var(i) || model.is_instantiated(i)) {
            defined_vars.push_back(i);
        } else {
            var_order_.push_back(i);
        }
    }
    decision_var_end_ = var_order_.size();
    var_order_.insert(var_order_.end(), defined_vars.begin(), defined_vars.end());
    defined_var_end_ = var_order_.size();
    var_order_.insert(var_order_.end(), unconstrained_vars.begin(), unconstrained_vars.end());
    std::shuffle(var_order_.begin(), var_order_.begin() + decision_var_end_, rng);
    std::shuffle(var_order_.begin() + decision_var_end_, var_order_.begin() + defined_var_end_, rng);
    std::shuffle(var_order_.begin() + defined_var_end_, var_order_.end(), rng);
}

void VariableSelector::init_tracking(const Model& model) {
    const size_t n = var_order_.size();
    var_position_.assign(model.variables().size(), SIZE_MAX);
    for (size_t k = 0; k < n; ++k) {
        var_position_[var_order_[k]] = k;
    }
    // Decision vars: 割当済みを後方へ
    decision_unassigned_end_ = decision_var_end_;
    for (size_t k = 0; k < decision_unassigned_end_; ) {
        if (model.is_instantiated(var_order_[k])) {
            --decision_unassigned_end_;
            std::swap(var_order_[k], var_order_[decision_unassigned_end_]);
            var_position_[var_order_[k]] = k;
            var_position_[var_order_[decision_unassigned_end_]] = decision_unassigned_end_;
        } else {
            ++k;
        }
    }
    // Defined vars: 同様
    defined_unassigned_end_ = defined_var_end_;
    for (size_t k = decision_var_end_; k < defined_unassigned_end_; ) {
        if (model.is_instantiated(var_order_[k])) {
            --defined_unassigned_end_;
            std::swap(var_order_[k], var_order_[defined_unassigned_end_]);
            var_position_[var_order_[k]] = k;
            var_position_[var_order_[defined_unassigned_end_]] = defined_unassigned_end_;
        } else {
            ++k;
        }
    }
    // Unconstrained vars: 同様（最後尾ゾーン）
    unconstrained_unassigned_end_ = n;
    for (size_t k = defined_var_end_; k < unconstrained_unassigned_end_; ) {
        if (model.is_instantiated(var_order_[k])) {
            --unconstrained_unassigned_end_;
            std::swap(var_order_[k], var_order_[unconstrained_unassigned_end_]);
            var_position_[var_order_[k]] = k;
            var_position_[var_order_[unconstrained_unassigned_end_]] = unconstrained_unassigned_end_;
        } else {
            ++k;
        }
    }
}

void VariableSelector::mark_assigned(size_t var_idx) {
    if (var_idx >= var_position_.size()) return;
    size_t pos = var_position_[var_idx];
    if (pos == SIZE_MAX) return;  // eliminated variable
    if (pos < decision_var_end_) {
        if (pos < decision_unassigned_end_) {
            --decision_unassigned_end_;
            std::swap(var_order_[pos], var_order_[decision_unassigned_end_]);
            var_position_[var_order_[pos]] = pos;
            var_position_[var_order_[decision_unassigned_end_]] = decision_unassigned_end_;
        }
    } else if (pos < defined_var_end_) {
        if (pos < defined_unassigned_end_) {
            --defined_unassigned_end_;
            std::swap(var_order_[pos], var_order_[defined_unassigned_end_]);
            var_position_[var_order_[pos]] = pos;
            var_position_[var_order_[defined_unassigned_end_]] = defined_unassigned_end_;
        }
    } else {
        if (pos < unconstrained_unassigned_end_) {
            --unconstrained_unassigned_end_;
            std::swap(var_order_[pos], var_order_[unconstrained_unassigned_end_]);
            var_position_[var_order_[pos]] = pos;
            var_position_[var_order_[unconstrained_unassigned_end_]] = unconstrained_unassigned_end_;
        }
    }
}

void VariableSelector::restore_decision_end(size_t new_end) {
    decision_unassigned_end_ = new_end;
}

void VariableSelector::restore_defined_end(size_t new_end) {
    defined_unassigned_end_ = new_end;
}

void VariableSelector::restore_unconstrained_end(size_t new_end) {
    unconstrained_unassigned_end_ = new_end;
}

size_t VariableSelector::select(const Model& model,
                                 const std::vector<double>& activity,
                                 const std::vector<int>& temporal_activity,
                                 const Bloom512& ng_usage_bloom,
                                 bool activity_first,
                                 std::mt19937& rng,
                                 const CommunityAnalysis* community_analysis) {
    // コミュニティローテーション: リスタート後の最初の1回だけ優先変数を使用
    if (community_first_var_ != SIZE_MAX) {
        size_t var = community_first_var_;
        community_first_var_ = SIZE_MAX;
        if (!model.is_instantiated(var)) {
            return var;
        }
    }

    // タイブレークで使う「直近判定変数のコミュニティ」を 1 度だけ算出
    size_t target_community = SIZE_MAX;
    if (community_analysis != nullptr && community_analysis->is_enabled()) {
        target_community = community_analysis->last_decision_community();
    }

    // 線形スキャン: decision vars → defined vars → unconstrained vars
    size_t result = select_linear(model, activity, temporal_activity, ng_usage_bloom, activity_first,
                                  rng, 0, decision_unassigned_end_, community_analysis, target_community);
    if (result != SIZE_MAX) return result;
    result = select_linear(model, activity, temporal_activity, ng_usage_bloom, activity_first,
                           rng, decision_var_end_, defined_unassigned_end_, community_analysis, target_community);
    if (result != SIZE_MAX) return result;
    return select_linear(model, activity, temporal_activity, ng_usage_bloom, activity_first,
                         rng, defined_var_end_, unconstrained_unassigned_end_, community_analysis, target_community);
}

size_t VariableSelector::select_linear(const Model& model,
                                        const std::vector<double>& activity,
                                        const std::vector<int>& temporal_activity,
                                        const Bloom512& ng_usage_bloom,
                                        bool activity_first,
                                        std::mt19937& rng,
                                        size_t begin, size_t end,
                                        const CommunityAnalysis* community_analysis,
                                        size_t target_community) {
    size_t n = end - begin;
    if (n == 0) return SIZE_MAX;

    size_t best_idx = SIZE_MAX;
    size_t min_domain_size = SIZE_MAX;
    double best_activity = -1.0;
    int best_temporal = -1;
    int best_ng_overlap = -1;
    bool use_bloom = !ng_usage_bloom.empty();
    // 同一コミュニティ優先タイブレーク: ターゲットがある時のみ有効
    const std::vector<size_t>* var_community = nullptr;
    if (community_analysis != nullptr && community_analysis->is_enabled()
        && target_community != SIZE_MAX) {
        var_community = &community_analysis->structure().community;
    }

    size_t start = rng() % n;
    size_t tie_count = 0;
    for (size_t j = 0; j < n; ++j) {
        size_t k = begin + (start + j) % n;
        size_t i = var_order_[k];
        if (model.is_instantiated(i)) continue;
        size_t domain_size = model.var_size(i);
        int ta = temporal_activity[i];
        bool better = false;
        bool tied = false;
        if (ta > best_temporal) {
            better = true;
        } else if (ta == best_temporal) {
            if (activity_first) {
                if (activity[i] > best_activity) {
                    better = true;
                } else if (activity[i] == best_activity && domain_size < min_domain_size) {
                    better = true;
                } else if (activity[i] == best_activity && domain_size == min_domain_size) {
                    tied = true;
                }
            } else {
                if (domain_size < min_domain_size) {
                    better = true;
                } else if (domain_size == min_domain_size && activity[i] > best_activity) {
                    better = true;
                } else if (domain_size == min_domain_size && activity[i] == best_activity) {
                    tied = true;
                }
            }
        }

        if (tied && use_bloom) {
            int ng_overlap = (model.var_ng_bloom(i) & ng_usage_bloom).popcount();
            if (ng_overlap > best_ng_overlap) {
                better = true;
                tied = false;
            } else if (ng_overlap < best_ng_overlap) {
                tied = false;
            }
        }

        if (better) {
            best_idx = i;
            min_domain_size = domain_size;
            best_activity = activity[i];
            best_temporal = ta;
            if (use_bloom) {
                best_ng_overlap = (model.var_ng_bloom(i) & ng_usage_bloom).popcount();
            }
            tie_count = 1;
        } else if (tied) {
            ++tie_count;
            if (rng() % tie_count == 0) {
                best_idx = i;
            }
        }
    }
    return best_idx;
}

namespace {
// 候補集合から activity 比例で 1 つ確率的に選ぶ。
// activity がすべて 0 の場合は一様ランダム。
size_t pick_pivot(const std::vector<size_t>& candidates,
                   const std::vector<double>& activity,
                   std::mt19937& rng) {
    if (candidates.empty()) return SIZE_MAX;

    // 未使用 (activity==0) の変数があればそれらを優先
    std::vector<size_t> untouched;
    double min_activity = 0.0;
    for (size_t v : candidates) {
        if (activity[v] == 0.0)
            continue;

        if (min_activity == 0.0) {
            min_activity = activity[v];
        }
        else if (activity[v] < min_activity) {
            min_activity = activity[v];
        }
    }
    if (min_activity == 0.0) {
        std::uniform_int_distribution<size_t> iud(0, candidates.size() - 1);
        return candidates[iud(rng)];
    }

    double total = 0.0;
    for (size_t v : candidates) {
        total += (activity[v] == 0.0 ? min_activity : activity[v]);
    }

    std::uniform_real_distribution<double> ud(0.0, total);
    double r = ud(rng);

    double cur = 0.0;
    for (size_t v : candidates) {
        cur += (activity[v] == 0.0 ? min_activity : activity[v]);
        if (cur > r)
            return v;
    }
    return candidates[candidates.size() - 1];
}
}  // namespace

void VariableSelector::select_restart_pivot(const Model& model,
                                             const std::vector<double>& activity,
                                             std::mt19937& rng) {
    // 未割当の決定変数のうち最小ドメインサイズ (MRV) のものを集めて
    // activity 重み付きランダムに 1 つ選ぶ。
    size_t best_size = SIZE_MAX;
    std::vector<size_t> candidates;
    for (size_t k = 0; k < decision_var_end_; ++k) {
        size_t v = var_order_[k];
        if (model.is_instantiated(v)) continue;
        // size_t ds = static_cast<size_t>(model.var_max(v) - model.var_min(v) + 1);
        size_t ds = static_cast<size_t>(model.var_size(v));
        if (ds < best_size) {
            best_size = ds;
            candidates.clear();
            candidates.push_back(v);
        } else if (ds == best_size) {
            candidates.push_back(v);
        }
    }
    community_first_var_ = pick_pivot(candidates, activity, rng);
}

void VariableSelector::shuffle(std::mt19937& rng) {
    std::shuffle(var_order_.begin(), var_order_.begin() + decision_var_end_, rng);
    std::shuffle(var_order_.begin() + decision_var_end_, var_order_.begin() + defined_var_end_, rng);
    std::shuffle(var_order_.begin() + defined_var_end_, var_order_.end(), rng);
}

} // namespace sabori_csp
