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

    for (size_t i = 0; i < variables.size(); ++i) {
        if (model.is_eliminated(i)) continue;
        if (model.is_defined_var(i) || model.is_instantiated(i)) {
            defined_vars.push_back(i);
        } else {
            var_order_.push_back(i);
        }
    }
    decision_var_end_ = var_order_.size();
    var_order_.insert(var_order_.end(), defined_vars.begin(), defined_vars.end());
    std::shuffle(var_order_.begin(), var_order_.begin() + decision_var_end_, rng);
    std::shuffle(var_order_.begin() + decision_var_end_, var_order_.end(), rng);
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
    defined_unassigned_end_ = n;
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
    } else {
        if (pos < defined_unassigned_end_) {
            --defined_unassigned_end_;
            std::swap(var_order_[pos], var_order_[defined_unassigned_end_]);
            var_position_[var_order_[pos]] = pos;
            var_position_[var_order_[defined_unassigned_end_]] = defined_unassigned_end_;
        }
    }
}

void VariableSelector::restore_decision_end(size_t new_end) {
    decision_unassigned_end_ = new_end;
}

void VariableSelector::restore_defined_end(size_t new_end) {
    defined_unassigned_end_ = new_end;
}

size_t VariableSelector::select(const Model& model,
                                 const std::vector<double>& activity,
                                 const std::vector<int>& temporal_activity,
                                 const Bloom512& ng_usage_bloom,
                                 bool activity_first,
                                 std::mt19937& rng) {
    // コミュニティローテーション: リスタート後の最初の1回だけ優先変数を使用
    if (community_first_var_ != SIZE_MAX) {
        size_t var = community_first_var_;
        community_first_var_ = SIZE_MAX;
        if (!model.is_instantiated(var)) {
            return var;
        }
    }

    // 線形スキャン: decision vars → defined vars
    size_t result = select_linear(model, activity, temporal_activity, ng_usage_bloom, activity_first,
                                  rng, 0, decision_unassigned_end_);
    if (result != SIZE_MAX) return result;
    return select_linear(model, activity, temporal_activity, ng_usage_bloom, activity_first,
                         rng, decision_var_end_, defined_unassigned_end_);
}

size_t VariableSelector::select_linear(const Model& model,
                                        const std::vector<double>& activity,
                                        const std::vector<int>& temporal_activity,
                                        const Bloom512& ng_usage_bloom,
                                        bool activity_first,
                                        std::mt19937& rng,
                                        size_t begin, size_t end) {
    size_t n = end - begin;
    if (n == 0) return SIZE_MAX;

    size_t best_idx = SIZE_MAX;
    size_t min_domain_size = SIZE_MAX;
    double best_activity = -1.0;
    int best_temporal = -1;
    int best_ng_overlap = -1;
    bool use_bloom = !ng_usage_bloom.empty();

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
                else if (domain_size == min_domain_size) {
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

void VariableSelector::select_restart_pivot(const Model& model,
                                             const std::vector<double>& activity,
                                             const CommunityAnalysis& community_analysis,
                                             size_t restart_count) {
    community_first_var_ = SIZE_MAX;

    if (community_analysis.is_enabled()) {
        // コミュニティベースのローテーション
        const auto& tops = community_analysis.top_communities(5);
        if (!tops.empty()) {
            size_t target_comm = tops[restart_count % tops.size()];
            const auto& vars = community_analysis.community_vars(target_comm);
            size_t best_size = SIZE_MAX;
            double best_act = -1.0;
            for (size_t v : vars) {
                if (!model.is_instantiated(v)) {
                    size_t ds = static_cast<size_t>(model.var_max(v) - model.var_min(v) + 1);
                    if (ds < best_size || (ds == best_size && activity[v] > best_act)) {
                        best_size = ds;
                        best_act = activity[v];
                        community_first_var_ = v;
                    }
                }
            }
        }
    } else if (decision_var_end_ > 0) {
        // コミュニティ分析なし: var_order_ を均等グループに分割してローテーション
        constexpr size_t num_groups = 5;
        size_t group_size = (decision_var_end_ + num_groups - 1) / num_groups;
        size_t group_idx = restart_count % num_groups;
        size_t begin = group_idx * group_size;
        size_t end = std::min(begin + group_size, decision_var_end_);

        size_t best_size = SIZE_MAX;
        double best_act = -1.0;
        for (size_t k = begin; k < end; ++k) {
            size_t v = var_order_[k];
            if (!model.is_instantiated(v)) {
                size_t ds = static_cast<size_t>(model.var_max(v) - model.var_min(v) + 1);
                if (ds < best_size || (ds == best_size && activity[v] > best_act)) {
                    best_size = ds;
                    best_act = activity[v];
                    community_first_var_ = v;
                }
            }
        }
    }
}

void VariableSelector::shuffle(std::mt19937& rng) {
    std::shuffle(var_order_.begin(), var_order_.begin() + decision_var_end_, rng);
    std::shuffle(var_order_.begin() + decision_var_end_, var_order_.end(), rng);
}

} // namespace sabori_csp
