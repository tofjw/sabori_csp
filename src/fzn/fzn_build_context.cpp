#include "fzn_build_context.hpp"
#include <stdexcept>

namespace sabori_csp {
namespace fzn {

VariablePtr FznBuildContext::get_var(const ConstraintArg& arg) {
    if (std::holds_alternative<std::string>(arg)) {
        const auto& name = std::get<std::string>(arg);
        return get_var_by_name(name);
    } else if (std::holds_alternative<Domain::value_type>(arg)) {
        auto val = std::get<Domain::value_type>(arg);
        static int const_counter = 0;
        std::string name = "__const_" + std::to_string(const_counter++);
        auto var = model->create_variable(name, val);
        var_map[name] = var;
        return var;
    }
    throw std::runtime_error("Invalid constraint argument");
}

VariablePtr FznBuildContext::get_var_by_name(const std::string& name) {
    auto it = var_map.find(name);
    if (it != var_map.end()) {
        return it->second;
    }
    if (name.rfind("__inline_", 0) == 0) {
        std::string val_str = name.substr(9);  // strlen("__inline_") = 9
        int64_t val = std::stoll(val_str);
        auto var = model->create_variable(name, val);
        var_map[name] = var;
        return var;
    }
    throw std::runtime_error("Unknown variable: " + name);
}

std::vector<std::string> FznBuildContext::resolve_var_array(const ConstraintArg& arg) const {
    if (std::holds_alternative<std::vector<std::string>>(arg)) {
        return std::get<std::vector<std::string>>(arg);
    } else if (std::holds_alternative<std::string>(arg)) {
        const auto& name = std::get<std::string>(arg);
        auto it = array_decls.find(name);
        if (it != array_decls.end()) {
            return it->second.elements;
        }
        throw std::runtime_error("Unknown variable array: " + name);
    } else if (std::holds_alternative<std::vector<Domain::value_type>>(arg)) {
        const auto& int_vec = std::get<std::vector<Domain::value_type>>(arg);
        if (int_vec.empty()) {
            return {};
        }
        throw std::runtime_error("Expected array of variables, got array of integers");
    }
    throw std::runtime_error("Expected array of variables");
}

std::vector<Domain::value_type> FznBuildContext::resolve_int_array(const ConstraintArg& arg) const {
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
    if (std::holds_alternative<std::vector<std::string>>(arg)) {
        const auto& str_vec = std::get<std::vector<std::string>>(arg);
        std::vector<Domain::value_type> result;
        result.reserve(str_vec.size());
        for (const auto& s : str_vec) {
            if (s.rfind("__inline_", 0) == 0) {
                result.push_back(std::stoll(s.substr(9)));
            } else {
                throw std::runtime_error("Expected integer array, got variable name: " + s);
            }
        }
        return result;
    }
    throw std::runtime_error("Expected array of integers");
}

} // namespace fzn
} // namespace sabori_csp
