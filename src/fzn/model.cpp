#include "sabori_csp/fzn/model.hpp"
#include <stdexcept>

namespace sabori_csp {
namespace fzn {

void Model::add_var_decl(VarDecl decl) {
    var_decls_[decl.name] = std::move(decl);
}

void Model::add_array_decl(ArrayDecl decl) {
    array_decls_[decl.name] = std::move(decl);
}

void Model::add_constraint_decl(ConstraintDecl decl) {
    constraint_decls_.push_back(std::move(decl));
}

void Model::set_solve_decl(SolveDecl decl) {
    solve_decl_ = std::move(decl);
}

std::vector<std::string> Model::output_vars() const {
    std::vector<std::string> result;
    for (const auto& [name, decl] : var_decls_) {
        if (decl.is_output) {
            result.push_back(name);
        }
    }
    return result;
}

std::vector<std::string> Model::output_arrays() const {
    std::vector<std::string> result;
    for (const auto& [name, decl] : array_decls_) {
        if (decl.is_output) {
            result.push_back(name);
        }
    }
    return result;
}

std::unique_ptr<sabori_csp::Model> Model::to_model() const {
    auto model = std::make_unique<sabori_csp::Model>();
    std::map<std::string, VariablePtr> var_map;

    // Create variables
    for (const auto& [name, decl] : var_decls_) {
        Domain domain;
        if (decl.fixed_value) {
            domain = Domain(*decl.fixed_value, *decl.fixed_value);
        } else {
            domain = Domain(decl.lb, decl.ub);
        }
        auto var = std::make_shared<Variable>(name, domain);
        var_map[name] = var;
        model->add_variable(var);
    }

    // Helper to get or create variable from argument
    auto get_var = [&](const ConstraintArg& arg) -> VariablePtr {
        if (std::holds_alternative<std::string>(arg)) {
            const auto& name = std::get<std::string>(arg);
            auto it = var_map.find(name);
            if (it != var_map.end()) {
                return it->second;
            }
            throw std::runtime_error("Unknown variable: " + name);
        } else if (std::holds_alternative<Domain::value_type>(arg)) {
            // Create a constant variable
            auto val = std::get<Domain::value_type>(arg);
            static int const_counter = 0;
            std::string name = "__const_" + std::to_string(const_counter++);
            auto var = std::make_shared<Variable>(name, Domain(val, val));
            var_map[name] = var;
            model->add_variable(var);
            return var;
        }
        throw std::runtime_error("Invalid constraint argument");
    };

    // Create constraints
    for (const auto& decl : constraint_decls_) {
        ConstraintPtr constraint;

        if (decl.name == "int_eq") {
            if (decl.args.size() != 2) {
                throw std::runtime_error("int_eq requires 2 arguments");
            }
            auto x = get_var(decl.args[0]);
            auto y = get_var(decl.args[1]);
            constraint = std::make_shared<IntEqConstraint>(x, y);
        } else if (decl.name == "int_eq_reif") {
            if (decl.args.size() != 3) {
                throw std::runtime_error("int_eq_reif requires 3 arguments");
            }
            auto x = get_var(decl.args[0]);
            auto y = get_var(decl.args[1]);
            auto b = get_var(decl.args[2]);
            constraint = std::make_shared<IntEqReifConstraint>(x, y, b);
        } else if (decl.name == "int_ne") {
            if (decl.args.size() != 2) {
                throw std::runtime_error("int_ne requires 2 arguments");
            }
            auto x = get_var(decl.args[0]);
            auto y = get_var(decl.args[1]);
            constraint = std::make_shared<IntNeConstraint>(x, y);
        } else if (decl.name == "int_lt") {
            if (decl.args.size() != 2) {
                throw std::runtime_error("int_lt requires 2 arguments");
            }
            auto x = get_var(decl.args[0]);
            auto y = get_var(decl.args[1]);
            constraint = std::make_shared<IntLtConstraint>(x, y);
        } else if (decl.name == "int_le") {
            if (decl.args.size() != 2) {
                throw std::runtime_error("int_le requires 2 arguments");
            }
            auto x = get_var(decl.args[0]);
            auto y = get_var(decl.args[1]);
            constraint = std::make_shared<IntLeConstraint>(x, y);
        } else if (decl.name == "all_different_int" || decl.name == "alldifferent_int") {
            if (decl.args.size() != 1) {
                throw std::runtime_error("all_different_int requires 1 argument (array)");
            }
            if (!std::holds_alternative<std::vector<std::string>>(decl.args[0])) {
                throw std::runtime_error("all_different_int argument must be an array of variables");
            }
            const auto& var_names = std::get<std::vector<std::string>>(decl.args[0]);
            std::vector<VariablePtr> vars;
            for (const auto& name : var_names) {
                auto it = var_map.find(name);
                if (it == var_map.end()) {
                    throw std::runtime_error("Unknown variable in all_different_int: " + name);
                }
                vars.push_back(it->second);
            }
            constraint = std::make_shared<AllDifferentConstraint>(vars);
        } else if (decl.name == "circuit") {
            if (decl.args.size() != 1) {
                throw std::runtime_error("circuit requires 1 argument (array)");
            }
            if (!std::holds_alternative<std::vector<std::string>>(decl.args[0])) {
                throw std::runtime_error("circuit argument must be an array of variables");
            }
            const auto& var_names = std::get<std::vector<std::string>>(decl.args[0]);
            std::vector<VariablePtr> vars;
            for (const auto& name : var_names) {
                auto it = var_map.find(name);
                if (it == var_map.end()) {
                    throw std::runtime_error("Unknown variable in circuit: " + name);
                }
                vars.push_back(it->second);
            }
            constraint = std::make_shared<CircuitConstraint>(vars);
        } else if (decl.name == "int_lin_eq") {
            if (decl.args.size() != 3) {
                throw std::runtime_error("int_lin_eq requires 3 arguments (coeffs, vars, sum)");
            }
            if (!std::holds_alternative<std::vector<Domain::value_type>>(decl.args[0])) {
                throw std::runtime_error("int_lin_eq: first argument must be an array of integers");
            }
            if (!std::holds_alternative<std::vector<std::string>>(decl.args[1])) {
                throw std::runtime_error("int_lin_eq: second argument must be an array of variables");
            }
            if (!std::holds_alternative<Domain::value_type>(decl.args[2])) {
                throw std::runtime_error("int_lin_eq: third argument must be an integer");
            }
            const auto& coeffs_raw = std::get<std::vector<Domain::value_type>>(decl.args[0]);
            const auto& var_names = std::get<std::vector<std::string>>(decl.args[1]);
            auto sum = std::get<Domain::value_type>(decl.args[2]);

            std::vector<int64_t> coeffs(coeffs_raw.begin(), coeffs_raw.end());
            std::vector<VariablePtr> vars;
            for (const auto& name : var_names) {
                auto it = var_map.find(name);
                if (it == var_map.end()) {
                    throw std::runtime_error("Unknown variable in int_lin_eq: " + name);
                }
                vars.push_back(it->second);
            }
            constraint = std::make_shared<IntLinEqConstraint>(coeffs, vars, sum);
        } else if (decl.name == "int_lin_le") {
            if (decl.args.size() != 3) {
                throw std::runtime_error("int_lin_le requires 3 arguments (coeffs, vars, bound)");
            }
            if (!std::holds_alternative<std::vector<Domain::value_type>>(decl.args[0])) {
                throw std::runtime_error("int_lin_le: first argument must be an array of integers");
            }
            if (!std::holds_alternative<std::vector<std::string>>(decl.args[1])) {
                throw std::runtime_error("int_lin_le: second argument must be an array of variables");
            }
            if (!std::holds_alternative<Domain::value_type>(decl.args[2])) {
                throw std::runtime_error("int_lin_le: third argument must be an integer");
            }
            const auto& coeffs_raw = std::get<std::vector<Domain::value_type>>(decl.args[0]);
            const auto& var_names = std::get<std::vector<std::string>>(decl.args[1]);
            auto bound = std::get<Domain::value_type>(decl.args[2]);

            std::vector<int64_t> coeffs(coeffs_raw.begin(), coeffs_raw.end());
            std::vector<VariablePtr> vars;
            for (const auto& name : var_names) {
                auto it = var_map.find(name);
                if (it == var_map.end()) {
                    throw std::runtime_error("Unknown variable in int_lin_le: " + name);
                }
                vars.push_back(it->second);
            }
            constraint = std::make_shared<IntLinLeConstraint>(coeffs, vars, bound);
        } else if (decl.name == "array_int_element" || decl.name == "int_element") {
            if (decl.args.size() != 3) {
                throw std::runtime_error("int_element requires 3 arguments (index, array, result)");
            }
            // index variable
            VariablePtr index_var;
            if (std::holds_alternative<std::string>(decl.args[0])) {
                const auto& name = std::get<std::string>(decl.args[0]);
                auto it = var_map.find(name);
                if (it == var_map.end()) {
                    throw std::runtime_error("Unknown variable in int_element: " + name);
                }
                index_var = it->second;
            } else if (std::holds_alternative<Domain::value_type>(decl.args[0])) {
                auto val = std::get<Domain::value_type>(decl.args[0]);
                static int idx_const_counter = 0;
                std::string name = "__idx_const_" + std::to_string(idx_const_counter++);
                index_var = std::make_shared<Variable>(name, Domain(val, val));
                var_map[name] = index_var;
                model->add_variable(index_var);
            } else {
                throw std::runtime_error("int_element: first argument must be a variable or integer");
            }

            // array of integers - can be inline array or array name reference
            std::vector<Domain::value_type> array;
            if (std::holds_alternative<std::vector<Domain::value_type>>(decl.args[1])) {
                array = std::get<std::vector<Domain::value_type>>(decl.args[1]);
            } else if (std::holds_alternative<std::string>(decl.args[1])) {
                // Resolve array name to array values
                const auto& array_name = std::get<std::string>(decl.args[1]);
                auto arr_it = array_decls_.find(array_name);
                if (arr_it == array_decls_.end()) {
                    throw std::runtime_error("Unknown array in int_element: " + array_name);
                }
                const auto& arr_decl = arr_it->second;
                // Get values from each element's VarDecl
                for (const auto& elem_name : arr_decl.elements) {
                    auto var_it = var_decls_.find(elem_name);
                    if (var_it == var_decls_.end()) {
                        throw std::runtime_error("Unknown array element in int_element: " + elem_name);
                    }
                    if (!var_it->second.fixed_value) {
                        throw std::runtime_error("int_element: array element must be a constant: " + elem_name);
                    }
                    array.push_back(*var_it->second.fixed_value);
                }
            } else {
                throw std::runtime_error("int_element: second argument must be an array of integers or array name");
            }

            // result variable
            VariablePtr result_var;
            if (std::holds_alternative<std::string>(decl.args[2])) {
                const auto& name = std::get<std::string>(decl.args[2]);
                auto it = var_map.find(name);
                if (it == var_map.end()) {
                    throw std::runtime_error("Unknown variable in int_element: " + name);
                }
                result_var = it->second;
            } else if (std::holds_alternative<Domain::value_type>(decl.args[2])) {
                auto val = std::get<Domain::value_type>(decl.args[2]);
                static int res_const_counter = 0;
                std::string name = "__res_const_" + std::to_string(res_const_counter++);
                result_var = std::make_shared<Variable>(name, Domain(val, val));
                var_map[name] = result_var;
                model->add_variable(result_var);
            } else {
                throw std::runtime_error("int_element: third argument must be a variable or integer");
            }

            // FlatZinc uses 1-based indexing by default
            constraint = std::make_shared<IntElementConstraint>(index_var, array, result_var, false);
        } else {
            // Unknown constraint - skip for now (skeleton implementation)
            continue;
        }

        if (constraint) {
            model->add_constraint(constraint);
        }
    }

    return model;
}

} // namespace fzn
} // namespace sabori_csp
