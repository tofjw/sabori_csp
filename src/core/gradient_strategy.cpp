#include "sabori_csp/gradient_strategy.hpp"
#include "sabori_csp/model.hpp"

#include <limits>

namespace sabori_csp {

namespace {
// 「未割当」を表す番兵値（Solver::kNoValue と同一）
constexpr GradientStrategy::value_type kNoValue =
    std::numeric_limits<GradientStrategy::value_type>::min();
} // namespace

void GradientStrategy::rebuild_eligible(const Model& model) {
    const auto& variables = model.variables();
    eligible_vars_.clear();
    for (size_t vi = 0; vi < variables.size(); ++vi) {
        if (model.is_eliminated(vi))
            continue;
        if (model.is_defined_var(vi))
            continue;
        if (model.is_instantiated(vi))
            continue;
        // ある程度範囲が広いものに限る
        if (model.presolve_max(vi) - model.presolve_min(vi) < 50)
            continue;
        eligible_vars_.push_back(vi);
    }
}

void GradientStrategy::compute(const Model& model,
                               const std::vector<value_type>& current_best,
                               const std::vector<double>& activity,
                               std::mt19937& rng) {
    // porbe の時は勾配が有効にすると少し改善することがある
    double min_activity = -1.0;
    size_t max_var_size = 0;
    if (!eligible_vars_.empty() && !prev_solution_.empty()) {
        const auto& variables = model.variables();
        if (gradient_.empty()) {
            gradient_.assign(variables.size(), 0.0);
        }
        for (size_t vi : eligible_vars_) {
            if (prev_solution_[vi] != kNoValue &&
                current_best[vi] != kNoValue) {
                double delta = static_cast<double>(current_best[vi] - prev_solution_[vi]);
                if (delta < 0)
                    gradient_[vi] = -1.0;
                else if (delta > 0.0)
                    gradient_[vi] = 1.0;
                else
                    gradient_[vi] = 0.0;
            }
        }
        std::uniform_int_distribution<size_t> idist(0, eligible_vars_.size() - 1);
        size_t start_idx = idist(rng);
        for (size_t i = 0; i < eligible_vars_.size(); i++) {
            auto idx = (start_idx + i) % eligible_vars_.size();
            size_t vi = eligible_vars_[idx];
            double g = gradient_[vi];
            if (g != 0.0
                && !(g < 0.0 && current_best[vi] == model.presolve_min(vi))
                && !(g > 0.0 && current_best[vi] == model.presolve_max(vi))) {
                if ((min_activity < 0 || activity[vi] < min_activity)
                    || (activity[vi] == min_activity && max_var_size < model.var_size(vi))) {
                    var_idx_ = vi;
                    direction_ = (g > 0.0) ? +1 : -1;
                    ref_val_ = current_best[vi];
                    min_activity = activity[vi];
                    max_var_size = model.var_size(vi);
                }
            }
        }
    }
}

} // namespace sabori_csp
