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
