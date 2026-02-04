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

bool Model::set_var_upper_bound(const std::string& name, Domain::value_type ub) {
    auto it = var_decls_.find(name);
    if (it == var_decls_.end()) {
        return false;
    }
    if (ub < it->second.lb) {
        return false;  // Would make domain empty
    }
    it->second.ub = ub;
    return true;
}

bool Model::set_var_lower_bound(const std::string& name, Domain::value_type lb) {
    auto it = var_decls_.find(name);
    if (it == var_decls_.end()) {
        return false;
    }
    if (lb > it->second.ub) {
        return false;  // Would make domain empty
    }
    it->second.lb = lb;
    return true;
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

    // Build a map of constant arrays (arrays of par int)
    std::map<std::string, std::vector<Domain::value_type>> constant_arrays;
    for (const auto& [name, arr_decl] : array_decls_) {
        // Check if this is a constant array (all elements are fixed)
        bool is_constant = true;
        std::vector<Domain::value_type> values;
        for (const auto& elem : arr_decl.elements) {
            auto it = var_decls_.find(elem);
            if (it != var_decls_.end() && it->second.fixed_value) {
                values.push_back(*it->second.fixed_value);
            } else {
                is_constant = false;
                break;
            }
        }
        if (is_constant && !values.empty()) {
            constant_arrays[name] = values;
        }
    }

    // Helper to resolve an argument that could be an array reference or inline array
    auto resolve_int_array = [&](const ConstraintArg& arg) -> std::vector<Domain::value_type> {
        if (std::holds_alternative<std::vector<Domain::value_type>>(arg)) {
            return std::get<std::vector<Domain::value_type>>(arg);
        } else if (std::holds_alternative<std::string>(arg)) {
            const auto& name = std::get<std::string>(arg);
            auto it = constant_arrays.find(name);
            if (it != constant_arrays.end()) {
                return it->second;
            }
            throw std::runtime_error("Unknown constant array: " + name);
        }
        throw std::runtime_error("Expected array of integers");
    };

    // Helper to resolve an argument that could be an array reference or inline variable list
    auto resolve_var_array = [&](const ConstraintArg& arg) -> std::vector<std::string> {
        if (std::holds_alternative<std::vector<std::string>>(arg)) {
            return std::get<std::vector<std::string>>(arg);
        } else if (std::holds_alternative<std::string>(arg)) {
            const auto& name = std::get<std::string>(arg);
            auto it = array_decls_.find(name);
            if (it != array_decls_.end()) {
                return it->second.elements;
            }
            throw std::runtime_error("Unknown variable array: " + name);
        } else if (std::holds_alternative<std::vector<Domain::value_type>>(arg)) {
            // Empty array [] is parsed as vector<int64_t>{}
            const auto& int_vec = std::get<std::vector<Domain::value_type>>(arg);
            if (int_vec.empty()) {
                return {};  // Empty variable array
            }
            throw std::runtime_error("Expected array of variables, got array of integers");
        }
        throw std::runtime_error("Expected array of variables");
    };

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
        } else if (decl.name == "int_le_reif") {
            if (decl.args.size() != 3) {
                throw std::runtime_error("int_le_reif requires 3 arguments");
            }
            auto x = get_var(decl.args[0]);
            auto y = get_var(decl.args[1]);
            auto b = get_var(decl.args[2]);
            constraint = std::make_shared<IntLeReifConstraint>(x, y, b);
        } else if (decl.name == "all_different_int" || decl.name == "alldifferent_int" ||
                   decl.name == "fzn_all_different_int") {
            if (decl.args.size() != 1) {
                throw std::runtime_error("all_different_int requires 1 argument (array)");
            }
            const auto var_names = resolve_var_array(decl.args[0]);
            std::vector<VariablePtr> vars;
            for (const auto& name : var_names) {
                auto it = var_map.find(name);
                if (it == var_map.end()) {
                    throw std::runtime_error("Unknown variable in all_different_int: " + name);
                }
                vars.push_back(it->second);
            }
            constraint = std::make_shared<AllDifferentConstraint>(vars);
        } else if (decl.name == "circuit" || decl.name == "fzn_circuit") {
            if (decl.args.size() != 1) {
                throw std::runtime_error("circuit requires 1 argument (array)");
            }
            const auto var_names = resolve_var_array(decl.args[0]);
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
            if (!std::holds_alternative<Domain::value_type>(decl.args[2])) {
                throw std::runtime_error("int_lin_eq: third argument must be an integer");
            }
            const auto coeffs_raw = resolve_int_array(decl.args[0]);
            const auto var_names = resolve_var_array(decl.args[1]);
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
            if (!std::holds_alternative<Domain::value_type>(decl.args[2])) {
                throw std::runtime_error("int_lin_le: third argument must be an integer");
            }
            const auto coeffs_raw = resolve_int_array(decl.args[0]);
            const auto var_names = resolve_var_array(decl.args[1]);
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
        } else if (decl.name == "int_lin_eq_reif") {
            if (decl.args.size() != 4) {
                throw std::runtime_error("int_lin_eq_reif requires 4 arguments (coeffs, vars, target, b)");
            }
            if (!std::holds_alternative<Domain::value_type>(decl.args[2])) {
                throw std::runtime_error("int_lin_eq_reif: third argument must be an integer");
            }
            const auto coeffs_raw = resolve_int_array(decl.args[0]);
            const auto var_names = resolve_var_array(decl.args[1]);
            auto target = std::get<Domain::value_type>(decl.args[2]);
            auto b = get_var(decl.args[3]);

            std::vector<int64_t> coeffs(coeffs_raw.begin(), coeffs_raw.end());
            std::vector<VariablePtr> vars;
            for (const auto& name : var_names) {
                auto it = var_map.find(name);
                if (it == var_map.end()) {
                    throw std::runtime_error("Unknown variable in int_lin_eq_reif: " + name);
                }
                vars.push_back(it->second);
            }
            constraint = std::make_shared<IntLinEqReifConstraint>(coeffs, vars, target, b);
        } else if (decl.name == "int_lin_le_reif") {
            if (decl.args.size() != 4) {
                throw std::runtime_error("int_lin_le_reif requires 4 arguments (coeffs, vars, bound, b)");
            }
            if (!std::holds_alternative<Domain::value_type>(decl.args[2])) {
                throw std::runtime_error("int_lin_le_reif: third argument must be an integer");
            }
            const auto coeffs_raw = resolve_int_array(decl.args[0]);
            const auto var_names = resolve_var_array(decl.args[1]);
            auto bound = std::get<Domain::value_type>(decl.args[2]);
            auto b = get_var(decl.args[3]);

            std::vector<int64_t> coeffs(coeffs_raw.begin(), coeffs_raw.end());
            std::vector<VariablePtr> vars;
            for (const auto& name : var_names) {
                auto it = var_map.find(name);
                if (it == var_map.end()) {
                    throw std::runtime_error("Unknown variable in int_lin_le_reif: " + name);
                }
                vars.push_back(it->second);
            }
            constraint = std::make_shared<IntLinLeReifConstraint>(coeffs, vars, bound, b);
        } else if (decl.name == "int_lin_le_imp") {
            if (decl.args.size() != 4) {
                throw std::runtime_error("int_lin_le_imp requires 4 arguments (coeffs, vars, bound, b)");
            }
            if (!std::holds_alternative<Domain::value_type>(decl.args[2])) {
                throw std::runtime_error("int_lin_le_imp: third argument must be an integer");
            }
            const auto coeffs_raw = resolve_int_array(decl.args[0]);
            const auto var_names = resolve_var_array(decl.args[1]);
            auto bound = std::get<Domain::value_type>(decl.args[2]);
            auto b = get_var(decl.args[3]);

            std::vector<int64_t> coeffs(coeffs_raw.begin(), coeffs_raw.end());
            std::vector<VariablePtr> vars;
            for (const auto& name : var_names) {
                auto it = var_map.find(name);
                if (it == var_map.end()) {
                    throw std::runtime_error("Unknown variable in int_lin_le_imp: " + name);
                }
                vars.push_back(it->second);
            }
            constraint = std::make_shared<IntLinLeImpConstraint>(coeffs, vars, bound, b);
        } else if (decl.name == "int_lin_ne") {
            if (decl.args.size() != 3) {
                throw std::runtime_error("int_lin_ne requires 3 arguments (coeffs, vars, target)");
            }
            if (!std::holds_alternative<Domain::value_type>(decl.args[2])) {
                throw std::runtime_error("int_lin_ne: third argument must be an integer");
            }
            const auto coeffs_raw = resolve_int_array(decl.args[0]);
            const auto var_names = resolve_var_array(decl.args[1]);
            auto target = std::get<Domain::value_type>(decl.args[2]);

            std::vector<int64_t> coeffs(coeffs_raw.begin(), coeffs_raw.end());
            std::vector<VariablePtr> vars;
            for (const auto& name : var_names) {
                auto it = var_map.find(name);
                if (it == var_map.end()) {
                    throw std::runtime_error("Unknown variable in int_lin_ne: " + name);
                }
                vars.push_back(it->second);
            }
            constraint = std::make_shared<IntLinNeConstraint>(coeffs, vars, target);
        // ========================================
        // Bool constraints (aliases for int constraints with 0-1 variables)
        // ========================================
        } else if (decl.name == "bool2int") {
            // bool2int(b, i) means b <-> i, where b is bool and i is int
            // Since both are 0-1 integer variables internally, this is just int_eq
            if (decl.args.size() != 2) {
                throw std::runtime_error("bool2int requires 2 arguments");
            }
            auto b = get_var(decl.args[0]);
            auto i = get_var(decl.args[1]);
            constraint = std::make_shared<IntEqConstraint>(b, i);
        } else if (decl.name == "bool_eq") {
            // bool_eq(a, b) is equivalent to int_eq(a, b) for 0-1 variables
            if (decl.args.size() != 2) {
                throw std::runtime_error("bool_eq requires 2 arguments");
            }
            auto x = get_var(decl.args[0]);
            auto y = get_var(decl.args[1]);
            constraint = std::make_shared<IntEqConstraint>(x, y);
        } else if (decl.name == "bool_ne") {
            // bool_ne(a, b) is equivalent to int_ne(a, b) for 0-1 variables
            if (decl.args.size() != 2) {
                throw std::runtime_error("bool_ne requires 2 arguments");
            }
            auto x = get_var(decl.args[0]);
            auto y = get_var(decl.args[1]);
            constraint = std::make_shared<IntNeConstraint>(x, y);
        } else if (decl.name == "bool_lt") {
            // bool_lt(a, b) is equivalent to int_lt(a, b) for 0-1 variables
            // a < b with a,b in {0,1} means a=0, b=1
            if (decl.args.size() != 2) {
                throw std::runtime_error("bool_lt requires 2 arguments");
            }
            auto x = get_var(decl.args[0]);
            auto y = get_var(decl.args[1]);
            constraint = std::make_shared<IntLtConstraint>(x, y);
        } else if (decl.name == "bool_le") {
            // bool_le(a, b) is equivalent to int_le(a, b) for 0-1 variables
            // a <= b means not(a) or b, i.e., a implies b
            if (decl.args.size() != 2) {
                throw std::runtime_error("bool_le requires 2 arguments");
            }
            auto x = get_var(decl.args[0]);
            auto y = get_var(decl.args[1]);
            constraint = std::make_shared<IntLeConstraint>(x, y);
        } else if (decl.name == "bool_eq_reif") {
            // bool_eq_reif(a, b, r) is equivalent to int_eq_reif(a, b, r)
            if (decl.args.size() != 3) {
                throw std::runtime_error("bool_eq_reif requires 3 arguments");
            }
            auto x = get_var(decl.args[0]);
            auto y = get_var(decl.args[1]);
            auto r = get_var(decl.args[2]);
            constraint = std::make_shared<IntEqReifConstraint>(x, y, r);
        } else if (decl.name == "bool_le_reif") {
            // bool_le_reif(a, b, r) is equivalent to int_le_reif(a, b, r)
            if (decl.args.size() != 3) {
                throw std::runtime_error("bool_le_reif requires 3 arguments");
            }
            auto x = get_var(decl.args[0]);
            auto y = get_var(decl.args[1]);
            auto r = get_var(decl.args[2]);
            constraint = std::make_shared<IntLeReifConstraint>(x, y, r);
        } else if (decl.name == "bool_lin_eq") {
            // bool_lin_eq(coeffs, vars, sum) is equivalent to int_lin_eq
            if (decl.args.size() != 3) {
                throw std::runtime_error("bool_lin_eq requires 3 arguments");
            }
            if (!std::holds_alternative<std::vector<Domain::value_type>>(decl.args[0])) {
                throw std::runtime_error("bool_lin_eq: first argument must be an array of integers");
            }
            if (!std::holds_alternative<std::vector<std::string>>(decl.args[1])) {
                throw std::runtime_error("bool_lin_eq: second argument must be an array of variables");
            }
            if (!std::holds_alternative<Domain::value_type>(decl.args[2])) {
                throw std::runtime_error("bool_lin_eq: third argument must be an integer");
            }
            const auto& coeffs_raw = std::get<std::vector<Domain::value_type>>(decl.args[0]);
            const auto& var_names = std::get<std::vector<std::string>>(decl.args[1]);
            auto sum = std::get<Domain::value_type>(decl.args[2]);

            std::vector<int64_t> coeffs(coeffs_raw.begin(), coeffs_raw.end());
            std::vector<VariablePtr> vars;
            for (const auto& name : var_names) {
                auto it = var_map.find(name);
                if (it == var_map.end()) {
                    throw std::runtime_error("Unknown variable in bool_lin_eq: " + name);
                }
                vars.push_back(it->second);
            }
            constraint = std::make_shared<IntLinEqConstraint>(coeffs, vars, sum);
        } else if (decl.name == "bool_lin_le") {
            // bool_lin_le(coeffs, vars, bound) is equivalent to int_lin_le
            if (decl.args.size() != 3) {
                throw std::runtime_error("bool_lin_le requires 3 arguments");
            }
            if (!std::holds_alternative<std::vector<Domain::value_type>>(decl.args[0])) {
                throw std::runtime_error("bool_lin_le: first argument must be an array of integers");
            }
            if (!std::holds_alternative<std::vector<std::string>>(decl.args[1])) {
                throw std::runtime_error("bool_lin_le: second argument must be an array of variables");
            }
            if (!std::holds_alternative<Domain::value_type>(decl.args[2])) {
                throw std::runtime_error("bool_lin_le: third argument must be an integer");
            }
            const auto& coeffs_raw = std::get<std::vector<Domain::value_type>>(decl.args[0]);
            const auto& var_names = std::get<std::vector<std::string>>(decl.args[1]);
            auto bound = std::get<Domain::value_type>(decl.args[2]);

            std::vector<int64_t> coeffs(coeffs_raw.begin(), coeffs_raw.end());
            std::vector<VariablePtr> vars;
            for (const auto& name : var_names) {
                auto it = var_map.find(name);
                if (it == var_map.end()) {
                    throw std::runtime_error("Unknown variable in bool_lin_le: " + name);
                }
                vars.push_back(it->second);
            }
            constraint = std::make_shared<IntLinLeConstraint>(coeffs, vars, bound);
        } else if (decl.name == "array_bool_and") {
            // array_bool_and([b1, b2, ..., bn], r) means r = b1 ∧ b2 ∧ ... ∧ bn
            if (decl.args.size() != 2) {
                throw std::runtime_error("array_bool_and requires 2 arguments");
            }
            if (!std::holds_alternative<std::vector<std::string>>(decl.args[0])) {
                throw std::runtime_error("array_bool_and: first argument must be an array of variables");
            }
            const auto& var_names = std::get<std::vector<std::string>>(decl.args[0]);
            std::vector<VariablePtr> vars;
            for (const auto& name : var_names) {
                auto it = var_map.find(name);
                if (it == var_map.end()) {
                    throw std::runtime_error("Unknown variable in array_bool_and: " + name);
                }
                vars.push_back(it->second);
            }
            auto r = get_var(decl.args[1]);
            constraint = std::make_shared<ArrayBoolAndConstraint>(vars, r);
        } else if (decl.name == "array_bool_or") {
            // array_bool_or([b1, b2, ..., bn], r) means r = b1 ∨ b2 ∨ ... ∨ bn
            if (decl.args.size() != 2) {
                throw std::runtime_error("array_bool_or requires 2 arguments");
            }
            if (!std::holds_alternative<std::vector<std::string>>(decl.args[0])) {
                throw std::runtime_error("array_bool_or: first argument must be an array of variables");
            }
            const auto& var_names = std::get<std::vector<std::string>>(decl.args[0]);
            std::vector<VariablePtr> vars;
            for (const auto& name : var_names) {
                auto it = var_map.find(name);
                if (it == var_map.end()) {
                    throw std::runtime_error("Unknown variable in array_bool_or: " + name);
                }
                vars.push_back(it->second);
            }
            auto r = get_var(decl.args[1]);
            constraint = std::make_shared<ArrayBoolOrConstraint>(vars, r);
        } else if (decl.name == "bool_clause") {
            // bool_clause([pos], [neg]) means ∨(pos) ∨ ∨(¬neg)
            if (decl.args.size() != 2) {
                throw std::runtime_error("bool_clause requires 2 arguments");
            }
            const auto pos_names = resolve_var_array(decl.args[0]);
            const auto neg_names = resolve_var_array(decl.args[1]);
            std::vector<VariablePtr> pos_vars;
            std::vector<VariablePtr> neg_vars;
            for (const auto& name : pos_names) {
                auto it = var_map.find(name);
                if (it == var_map.end()) {
                    throw std::runtime_error("Unknown variable in bool_clause: " + name);
                }
                pos_vars.push_back(it->second);
            }
            for (const auto& name : neg_names) {
                auto it = var_map.find(name);
                if (it == var_map.end()) {
                    throw std::runtime_error("Unknown variable in bool_clause: " + name);
                }
                neg_vars.push_back(it->second);
            }
            constraint = std::make_shared<BoolClauseConstraint>(pos_vars, neg_vars);
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
        } else if (decl.name == "array_int_maximum") {
            // array_int_maximum(m, [x1, x2, ...]) means m = max(x1, x2, ...)
            if (decl.args.size() != 2) {
                throw std::runtime_error("array_int_maximum requires 2 arguments (max_var, array)");
            }
            auto m = get_var(decl.args[0]);
            const auto var_names = resolve_var_array(decl.args[1]);
            std::vector<VariablePtr> vars;
            for (const auto& name : var_names) {
                auto it = var_map.find(name);
                if (it == var_map.end()) {
                    throw std::runtime_error("Unknown variable in array_int_maximum: " + name);
                }
                vars.push_back(it->second);
            }
            constraint = std::make_shared<ArrayIntMaximumConstraint>(m, vars);
        } else if (decl.name == "array_int_minimum" || decl.name == "int_min") {
            // array_int_minimum(m, [x1, x2, ...]) means m = min(x1, x2, ...)
            if (decl.args.size() != 2) {
                throw std::runtime_error("array_int_minimum requires 2 arguments (min_var, array)");
            }
            auto m = get_var(decl.args[0]);
            const auto var_names = resolve_var_array(decl.args[1]);
            std::vector<VariablePtr> vars;
            for (const auto& name : var_names) {
                auto it = var_map.find(name);
                if (it == var_map.end()) {
                    throw std::runtime_error("Unknown variable in array_int_minimum: " + name);
                }
                vars.push_back(it->second);
            }
            constraint = std::make_shared<ArrayIntMinimumConstraint>(m, vars);
        } else if (decl.name == "int_times") {
            // int_times(x, y, z) means x * y = z
            if (decl.args.size() != 3) {
                throw std::runtime_error("int_times requires 3 arguments (x, y, z)");
            }
            auto x = get_var(decl.args[0]);
            auto y = get_var(decl.args[1]);
            auto z = get_var(decl.args[2]);
            constraint = std::make_shared<IntTimesConstraint>(x, y, z);
        } else if (decl.name == "int_max") {
            // int_max(x, y, m) means max(x, y) = m
            if (decl.args.size() != 3) {
                throw std::runtime_error("int_max requires 3 arguments (x, y, m)");
            }
            auto x = get_var(decl.args[0]);
            auto y = get_var(decl.args[1]);
            auto m = get_var(decl.args[2]);
            constraint = std::make_shared<IntMaxConstraint>(x, y, m);
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
