#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/variable.hpp"
#include "sabori_csp/model.hpp"
#include <unordered_map>

namespace sabori_csp {

// ============================================================================
// LinearConstraintBase implementation
// ============================================================================

std::vector<VariablePtr> LinearConstraintBase::aggregate_terms(
        const std::vector<int64_t>& coeffs,
        const std::vector<VariablePtr>& vars) {
    // 同一変数の係数を集約
    std::unordered_map<Variable*, int64_t> aggregated;
    for (size_t i = 0; i < vars.size(); ++i) {
        aggregated[vars[i]] += coeffs[i];
    }

    // 一意な変数リストと係数リストを再構築（係数0の項を除外）
    std::vector<VariablePtr> unique_vars;
    for (const auto& [var_ptr, coeff] : aggregated) {
        if (coeff == 0) continue;
        unique_vars.push_back(var_ptr);
        coeffs_.push_back(coeff);
    }
    return unique_vars;
}

void LinearConstraintBase::prune_sum_le(Model& model, int64_t bound,
                                        int64_t fixed_sum, int64_t min_rem,
                                        size_t skip_idx) const {
    const int64_t slack = bound - fixed_sum;
    const size_t n = coeffs_.size();
    for (size_t j = 0; j < n; ++j) {
        if (j == skip_idx) continue;
        size_t vid = var_ids_[j];
        if (model.is_instantiated(vid)) continue;

        int64_t c = coeffs_[j];

        // rest_min = min_rem minus j's min contribution
        int64_t rest_min = (c >= 0)
            ? min_rem - c * model.var_min(vid)
            : min_rem - c * model.var_max(vid);
        int64_t available = slack - rest_min;  // c * x_j <= available

        if (c > 0) {
            int64_t new_max = available / c;  // floor division (available >= 0)
            if (new_max < model.var_max(vid)) {
                model.enqueue_set_max(vid, new_max);
            }
        } else {
            int64_t abs_c = -c;
            // c * x_j <= available → x_j >= ceil(-available / abs_c)
            int64_t new_min;
            if (available >= 0) {
                new_min = -(available / abs_c);
            } else {
                new_min = ((-available) + abs_c - 1) / abs_c;
            }
            if (new_min > model.var_min(vid)) {
                model.enqueue_set_min(vid, new_min);
            }
        }
    }
}

}  // namespace sabori_csp
