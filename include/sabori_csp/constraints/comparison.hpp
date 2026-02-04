/**
 * @file comparison.hpp
 * @brief 比較制約クラス (int_eq, int_ne, int_lt, int_le)
 */
#ifndef SABORI_CSP_CONSTRAINTS_COMPARISON_HPP
#define SABORI_CSP_CONSTRAINTS_COMPARISON_HPP

#include "sabori_csp/constraint.hpp"

namespace sabori_csp {

/**
 * @brief int_eq制約: x == y
 */
class IntEqConstraint : public Constraint {
public:
    IntEqConstraint(VariablePtr x, VariablePtr y);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool propagate(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

protected:
    void check_initial_consistency() override;

private:
    VariablePtr x_;
    VariablePtr y_;
};

/**
 * @brief int_eq_reif制約: (x == y) <-> b
 */
class IntEqReifConstraint : public Constraint {
public:
    IntEqReifConstraint(VariablePtr x, VariablePtr y, VariablePtr b);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool propagate(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

protected:
    void check_initial_consistency() override;

private:
    VariablePtr x_;
    VariablePtr y_;
    VariablePtr b_;
};

/**
 * @brief int_ne制約: x != y
 */
class IntNeConstraint : public Constraint {
public:
    IntNeConstraint(VariablePtr x, VariablePtr y);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool propagate(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

protected:
    void check_initial_consistency() override;

private:
    VariablePtr x_;
    VariablePtr y_;
};

/**
 * @brief int_ne_reif制約: (x != y) <-> b
 */
class IntNeReifConstraint : public Constraint {
public:
    IntNeReifConstraint(VariablePtr x, VariablePtr y, VariablePtr b);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool propagate(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

protected:
    void check_initial_consistency() override;

private:
    VariablePtr x_;
    VariablePtr y_;
    VariablePtr b_;
};

/**
 * @brief int_lt制約: x < y
 */
class IntLtConstraint : public Constraint {
public:
    IntLtConstraint(VariablePtr x, VariablePtr y);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool propagate(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

protected:
    void check_initial_consistency() override;

private:
    VariablePtr x_;
    VariablePtr y_;
};

/**
 * @brief int_le制約: x <= y
 */
class IntLeConstraint : public Constraint {
public:
    IntLeConstraint(VariablePtr x, VariablePtr y);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool propagate(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

protected:
    void check_initial_consistency() override;

private:
    VariablePtr x_;
    VariablePtr y_;
};

/**
 * @brief int_le_reif制約: (x <= y) <-> b
 */
class IntLeReifConstraint : public Constraint {
public:
    IntLeReifConstraint(VariablePtr x, VariablePtr y, VariablePtr b);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool propagate(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

protected:
    void check_initial_consistency() override;

private:
    VariablePtr x_;
    VariablePtr y_;
    VariablePtr b_;
};

/**
 * @brief int_max制約: max(x, y) = m
 *
 * 2変数の最大値制約。m は x と y の最大値に等しい。
 */
class IntMaxConstraint : public Constraint {
public:
    IntMaxConstraint(VariablePtr x, VariablePtr y, VariablePtr m);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool propagate(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

protected:
    void check_initial_consistency() override;

private:
    VariablePtr x_;
    VariablePtr y_;
    VariablePtr m_;
};

} // namespace sabori_csp

#endif // SABORI_CSP_CONSTRAINTS_COMPARISON_HPP
