#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/variable.hpp"
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

}  // namespace sabori_csp
