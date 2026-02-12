#include "sabori_csp/fzn/model.hpp"
#include <algorithm>
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

    // Phase 0: bool2int をスキャンしてエイリアスマップを構築
    // alias_map: int変数名 → bool変数名（正規変数）
    std::map<std::string, std::string> alias_map;
    for (const auto& decl : constraint_decls_) {
        if (decl.name == "bool2int" && decl.args.size() == 2) {
            // bool2int(b, i): b が正規変数、i がエイリアス
            if (std::holds_alternative<std::string>(decl.args[0]) &&
                std::holds_alternative<std::string>(decl.args[1])) {
                const auto& b_name = std::get<std::string>(decl.args[0]);
                const auto& i_name = std::get<std::string>(decl.args[1]);
                // 両方が固定値でない場合のみエイリアス化
                auto b_it = var_decls_.find(b_name);
                auto i_it = var_decls_.find(i_name);
                if (b_it != var_decls_.end() && i_it != var_decls_.end() &&
                    !b_it->second.fixed_value && !i_it->second.fixed_value) {
                    alias_map[i_name] = b_name;
                }
            }
        }
    }

    // Create variables
    for (const auto& [name, decl] : var_decls_) {
        // エイリアス対象の変数はスキップ
        if (alias_map.count(name)) {
            continue;
        }
        VariablePtr var;
        if (decl.fixed_value) {
            var = model->create_variable(name, *decl.fixed_value);
        } else if (!decl.domain_values.empty()) {
            // Set domain: var {1,3,5}: x;
            var = model->create_variable(name, decl.domain_values);
        } else {
            var = model->create_variable(name, decl.lb, decl.ub);
        }
        var_map[name] = var;
    }

    // エイリアスを var_map に登録
    for (const auto& [alias_name, canonical_name] : alias_map) {
        auto it = var_map.find(canonical_name);
        if (it != var_map.end()) {
            var_map[alias_name] = it->second;
            model->add_variable_alias(alias_name, it->second->id());
        }
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

    // Helper to get or create variable by name (handles __inline_ prefix for literals)
    auto get_var_by_name = [&](const std::string& name) -> VariablePtr {
        // First check if variable already exists
        auto it = var_map.find(name);
        if (it != var_map.end()) {
            return it->second;
        }
        // Check if it's an inline literal that needs to be created
        if (name.rfind("__inline_", 0) == 0) {
            // Extract the value from the name
            std::string val_str = name.substr(9);  // strlen("__inline_") = 9
            int64_t val = std::stoll(val_str);
            // Create a constant variable
            auto var = model->create_variable(name, val);
            var_map[name] = var;
            return var;
        }
        throw std::runtime_error("Unknown variable: " + name);
    };

    // Helper to get or create variable from argument
    auto get_var = [&](const ConstraintArg& arg) -> VariablePtr {
        if (std::holds_alternative<std::string>(arg)) {
            const auto& name = std::get<std::string>(arg);
            return get_var_by_name(name);
        } else if (std::holds_alternative<Domain::value_type>(arg)) {
            // Create a constant variable
            auto val = std::get<Domain::value_type>(arg);
            static int const_counter = 0;
            std::string name = "__const_" + std::to_string(const_counter++);
            auto var = model->create_variable(name, val);
            var_map[name] = var;
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
#if 1/* broken */
            if (decl.args.size() != 3) {
                throw std::runtime_error("int_eq_reif requires 3 arguments");
            }
            auto x = get_var(decl.args[0]);
            auto y = get_var(decl.args[1]);
            auto b = get_var(decl.args[2]);
            constraint = std::make_shared<IntEqReifConstraint>(x, y, b);
#endif
        } else if (decl.name == "int_ne") {
            if (decl.args.size() != 2) {
                throw std::runtime_error("int_ne requires 2 arguments");
            }
            auto x = get_var(decl.args[0]);
            auto y = get_var(decl.args[1]);
            constraint = std::make_shared<IntNeConstraint>(x, y);
        } else if (decl.name == "int_ne_reif") {
#if 1
            if (decl.args.size() != 3) {
                throw std::runtime_error("int_ne_reif requires 3 arguments");
            }
            auto x = get_var(decl.args[0]);
            auto y = get_var(decl.args[1]);
            auto b = get_var(decl.args[2]);
            constraint = std::make_shared<IntNeReifConstraint>(x, y, b);
#endif
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
                vars.push_back(get_var_by_name(name));
            }
            constraint = std::make_shared<AllDifferentConstraint>(vars);
        } else if (decl.name == "circuit" || decl.name == "fzn_circuit") {
            if (decl.args.size() != 1) {
                throw std::runtime_error("circuit requires 1 argument (array)");
            }
            const auto var_names = resolve_var_array(decl.args[0]);
            std::vector<VariablePtr> vars;
            for (const auto& name : var_names) {
                vars.push_back(get_var_by_name(name));
            }
            constraint = std::make_shared<CircuitConstraint>(vars);
        } else if (decl.name == "int_lin_eq") {
#if 1
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
#endif
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
#if 1
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
#endif
        } else if (decl.name == "int_lin_ne_reif") {
            if (decl.args.size() != 4) {
                throw std::runtime_error("int_lin_ne_reif requires 4 arguments (coeffs, vars, target, b)");
            }
            if (!std::holds_alternative<Domain::value_type>(decl.args[2])) {
                throw std::runtime_error("int_lin_ne_reif: third argument must be an integer");
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
                    throw std::runtime_error("Unknown variable in int_lin_ne_reif: " + name);
                }
                vars.push_back(it->second);
            }
            constraint = std::make_shared<IntLinNeReifConstraint>(coeffs, vars, target, b);
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
            if (decl.args.size() != 2) {
                throw std::runtime_error("bool2int requires 2 arguments");
            }
            // エイリアス化済みの場合は制約不要
            if (std::holds_alternative<std::string>(decl.args[1]) &&
                alias_map.count(std::get<std::string>(decl.args[1]))) {
                constraint = nullptr;
            } else {
                // 定数ケース等: 従来通り IntEqConstraint
                auto b = get_var(decl.args[0]);
                auto i = get_var(decl.args[1]);
                constraint = std::make_shared<IntEqConstraint>(b, i);
            }
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
#if 1
            // array_bool_and([b1, b2, ..., bn], r) means r = b1 ∧ b2 ∧ ... ∧ bn
            if (decl.args.size() != 2) {
                throw std::runtime_error("array_bool_and requires 2 arguments");
            }
            const auto var_names = resolve_var_array(decl.args[0]);
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
#endif
        } else if (decl.name == "array_bool_or") {
            // array_bool_or([b1, b2, ..., bn], r) means r = b1 ∨ b2 ∨ ... ∨ bn
            if (decl.args.size() != 2) {
                throw std::runtime_error("array_bool_or requires 2 arguments");
            }
            const auto var_names = resolve_var_array(decl.args[0]);
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
#if 1 /* broken */
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
#endif
        } else if (decl.name == "bool_not") {
#if 1
            // bool_not(a, b) means ¬a = b (i.e., a + b = 1)
            if (decl.args.size() != 2) {
                throw std::runtime_error("bool_not requires 2 arguments");
            }
            auto a = get_var(decl.args[0]);
            auto b = get_var(decl.args[1]);
            constraint = std::make_shared<BoolNotConstraint>(a, b);
#endif
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
                index_var = model->create_variable(name, val);
                var_map[name] = index_var;
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
                result_var = model->create_variable(name, val);
                var_map[name] = result_var;
            } else {
                throw std::runtime_error("int_element: third argument must be a variable or integer");
            }

            // FlatZinc uses 1-based indexing by default
            constraint = std::make_shared<IntElementConstraint>(index_var, array, result_var, false);
        } else if (decl.name == "array_var_int_element" || decl.name == "array_var_bool_element") {
#if 1 /* broken */
            // array_var_int_element(index, array, result) where array contains variables
            // array_var_bool_element is the same but with bool variables (0-1 domain)
            if (decl.args.size() != 3) {
                throw std::runtime_error(decl.name + " requires 3 arguments (index, array, result)");
            }
            // index variable
            VariablePtr index_var = get_var(decl.args[0]);

            // array of variables (may contain __inline_ literals)
            const auto var_names = resolve_var_array(decl.args[1]);
            std::vector<VariablePtr> array_vars;
            for (const auto& vname : var_names) {
                array_vars.push_back(get_var_by_name(vname));
            }

            // result variable
            VariablePtr result_var = get_var(decl.args[2]);

            // FlatZinc uses 1-based indexing by default
            constraint = std::make_shared<ArrayVarIntElementConstraint>(
                index_var, array_vars, result_var, false);
#endif
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
        } else if (decl.name == "array_int_minimum") {
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
        } else if (decl.name == "int_min") {
            // int_min(x, y, m) means min(x, y) = m
            if (decl.args.size() != 3) {
                throw std::runtime_error("int_min requires 3 arguments (x, y, m)");
            }
            auto x = get_var(decl.args[0]);
            auto y = get_var(decl.args[1]);
            auto m = get_var(decl.args[2]);
            constraint = std::make_shared<IntMinConstraint>(x, y, m);
        } else if (decl.name == "int_times") {
            // int_times(x, y, z) means x * y = z
            if (decl.args.size() != 3) {
                throw std::runtime_error("int_times requires 3 arguments (x, y, z)");
            }
            auto x = get_var(decl.args[0]);
            auto y = get_var(decl.args[1]);
            auto z = get_var(decl.args[2]);
            constraint = std::make_shared<IntTimesConstraint>(x, y, z);
        } else if (decl.name == "int_abs") {
            // int_abs(x, y) means |x| = y
            if (decl.args.size() != 2) {
                throw std::runtime_error("int_abs requires 2 arguments (x, y)");
            }
            auto x = get_var(decl.args[0]);
            auto y = get_var(decl.args[1]);
            constraint = std::make_shared<IntAbsConstraint>(x, y);
        } else if (decl.name == "int_max") {
            // int_max(x, y, m) means max(x, y) = m
            if (decl.args.size() != 3) {
                throw std::runtime_error("int_max requires 3 arguments (x, y, m)");
            }
            auto x = get_var(decl.args[0]);
            auto y = get_var(decl.args[1]);
            auto m = get_var(decl.args[2]);
            constraint = std::make_shared<IntMaxConstraint>(x, y, m);
        } else if (decl.name == "table_int" || decl.name == "sabori_table_int") {
            if (decl.args.size() != 2) {
                throw std::runtime_error("table_int requires 2 arguments (vars, tuples)");
            }
            const auto var_names = resolve_var_array(decl.args[0]);
            const auto tuples = resolve_int_array(decl.args[1]);
            std::vector<VariablePtr> vars;
            for (const auto& name : var_names) {
                vars.push_back(get_var_by_name(name));
            }
            constraint = std::make_shared<TableConstraint>(vars, tuples);
        } else if (decl.name == "set_in") {
            // set_in(x, lb..ub) means x must be in range [lb, ub]
            if (decl.args.size() != 2) {
                throw std::runtime_error("set_in requires 2 arguments");
            }
            auto x = get_var(decl.args[0]);
            if (std::holds_alternative<IntRange>(decl.args[1])) {
                const auto& range = std::get<IntRange>(decl.args[1]);
                // Add constraints: x >= lb AND x <= ub
                // Create constant variables for bounds
                auto lb_var = model->create_variable("__set_in_lb_" + x->name(), range.lb);
                auto ub_var = model->create_variable("__set_in_ub_" + x->name(), range.ub);
                // x >= lb (equivalent to lb <= x)
                model->add_constraint(std::make_shared<IntLeConstraint>(lb_var, x));
                // x <= ub
                model->add_constraint(std::make_shared<IntLeConstraint>(x, ub_var));
                constraint = nullptr; // Already added
            } else if (std::holds_alternative<std::vector<Domain::value_type>>(decl.args[1])) {
                const auto& values = std::get<std::vector<Domain::value_type>>(decl.args[1]);
                // Remove values from x's domain that are not in the set
                auto domain_vals = x->domain().values();
                for (auto v : domain_vals) {
                    if (std::find(values.begin(), values.end(), v) == values.end()) {
                        x->domain().remove(v);
                    }
                }
                constraint = nullptr; // Already handled
            } else {
                throw std::runtime_error("set_in requires range or set argument");
            }
        } else if (decl.name == "set_in_reif") {
            // set_in_reif(x, S, b) means b ↔ (x ∈ S)
            if (decl.args.size() != 3) {
                throw std::runtime_error("set_in_reif requires 3 arguments");
            }
            auto x = get_var(decl.args[0]);
            auto b = get_var(decl.args[2]);

            if (std::holds_alternative<IntRange>(decl.args[1])) {
                // Range case: b ↔ (lb <= x <= ub)
                const auto& range = std::get<IntRange>(decl.args[1]);
                static int set_in_reif_counter = 0;
                int id = set_in_reif_counter++;
                auto lb_var = model->create_variable("__sir_lb_" + std::to_string(id), range.lb);
                auto ub_var = model->create_variable("__sir_ub_" + std::to_string(id), range.ub);
                auto b1 = model->create_variable("__sir_b1_" + std::to_string(id), 0, 1);
                auto b2 = model->create_variable("__sir_b2_" + std::to_string(id), 0, 1);
                // b1 = (lb <= x)
                model->add_constraint(std::make_shared<IntLeReifConstraint>(lb_var, x, b1));
                // b2 = (x <= ub)
                model->add_constraint(std::make_shared<IntLeReifConstraint>(x, ub_var, b2));
                // b = b1 ∧ b2
                model->add_constraint(std::make_shared<ArrayBoolAndConstraint>(
                    std::vector<VariablePtr>{b1, b2}, b));
                constraint = nullptr;
            } else if (std::holds_alternative<std::vector<Domain::value_type>>(decl.args[1])) {
                // Set literal case: b ↔ (x = v1 ∨ x = v2 ∨ ... ∨ vn)
                const auto& values = std::get<std::vector<Domain::value_type>>(decl.args[1]);
                static int set_in_reif_set_counter = 0;
                int id = set_in_reif_set_counter++;
                std::vector<VariablePtr> bool_vars;
                for (size_t i = 0; i < values.size(); ++i) {
                    auto vi_var = model->create_variable(
                        "__sir_v_" + std::to_string(id) + "_" + std::to_string(i), values[i]);
                    auto bi = model->create_variable(
                        "__sir_bi_" + std::to_string(id) + "_" + std::to_string(i), 0, 1);
                    // bi = (x == vi)
                    model->add_constraint(std::make_shared<IntEqReifConstraint>(x, vi_var, bi));
                    bool_vars.push_back(bi);
                }
                // b = b1 ∨ b2 ∨ ... ∨ bn
                model->add_constraint(std::make_shared<ArrayBoolOrConstraint>(bool_vars, b));
                constraint = nullptr;
            } else {
                throw std::runtime_error("set_in_reif requires range or set argument");
            }
        } else {
            // Unknown constraint - error
            throw std::runtime_error("Unsupported constraint: " + decl.name);
        }

        if (constraint) {
            model->add_constraint(constraint);
        }
    }

    return model;
}

} // namespace fzn
} // namespace sabori_csp
