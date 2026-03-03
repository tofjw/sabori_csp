/**
 * @file arithmetic.hpp
 * @brief 算術制約クラス (int_times, int_div, int_mod, int_plus, int_abs)
 */
#ifndef SABORI_CSP_CONSTRAINTS_ARITHMETIC_HPP
#define SABORI_CSP_CONSTRAINTS_ARITHMETIC_HPP

#include "sabori_csp/constraint.hpp"

namespace sabori_csp {

/**
 * @brief int_times制約: x * y = z
 */
class IntTimesConstraint : public Constraint {
public:
    IntTimesConstraint(VariablePtr x, VariablePtr y, VariablePtr z);

    std::string name() const override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;
    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;
    bool on_final_instantiate(const Model& model) override;

    bool on_last_uninstantiated(Model& model, int save_point,
                                size_t last_var_internal_idx) override;

private:
    size_t x_id_, y_id_, z_id_;

    bool propagate_bounds(Model& model);
};

/**
 * @brief int_abs制約: |x| = y
 */
class IntAbsConstraint : public Constraint {
public:
    IntAbsConstraint(VariablePtr x, VariablePtr y);

    std::string name() const override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;
    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;
    bool on_final_instantiate(const Model& model) override;

private:
    size_t x_id_, y_id_;

    bool propagate_bounds(Model& model);
};

/**
 * @brief int_mod制約: x mod y = z (truncated division)
 */
class IntModConstraint : public Constraint {
public:
    IntModConstraint(VariablePtr x, VariablePtr y, VariablePtr z);

    std::string name() const override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;
    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;
    bool on_final_instantiate(const Model& model) override;

    bool on_last_uninstantiated(Model& model, int save_point,
                                size_t last_var_internal_idx) override;

private:
    size_t x_id_, y_id_, z_id_;

    bool propagate_bounds(Model& model);
};

} // namespace sabori_csp

#endif // SABORI_CSP_CONSTRAINTS_ARITHMETIC_HPP
