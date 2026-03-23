#include "sabori_csp/fzn/model.hpp"
#include "fzn_build_context.hpp"
#include "constraint_registry.hpp"
#include <algorithm>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <unordered_set>

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

std::unique_ptr<sabori_csp::Model> Model::to_model(bool verbose, bool use_gac) const {
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
                // lb == ub の変数は実質固定値なのでエイリアス化しない
                // （ドメイン制約が正規変数に伝播されないバグを防ぐ）
                auto b_it = var_decls_.find(b_name);
                auto i_it = var_decls_.find(i_name);
                if (b_it != var_decls_.end() && i_it != var_decls_.end() &&
                    !b_it->second.fixed_value && !i_it->second.fixed_value &&
                    b_it->second.lb != b_it->second.ub && i_it->second.lb != i_it->second.ub) {
                    alias_map[i_name] = b_name;
                }
            }
        }
    }

    // Create variables
    for (const auto& [name, decl] : var_decls_) {
        // エイリアス対象の変数はスキップ
        if (alias_map.count(name) || decl.alias_target) {
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
        if (decl.is_defined_var && !decl.fixed_value) {
            model->set_defined_var(var->id());
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

    // Handle var aliases (e.g., var 0..10000: objective = X_INTRODUCED_1111_)
    for (const auto& [name, decl] : var_decls_) {
        if (decl.alias_target) {
            auto it = var_map.find(*decl.alias_target);
            if (it != var_map.end()) {
                var_map[name] = it->second;
                model->add_variable_alias(name, it->second->id());
            }
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

    // ビルドコンテキスト構築
    FznBuildContext ctx{model.get(), var_map, var_decls_, array_decls_,
                        constant_arrays, alias_map, verbose, use_gac};

    // FlatZinc アノテーション由来の is_defined_var 集合を記録
    // （ヒューリスティックで追加したものと区別するため）
    std::unordered_set<size_t> original_defined_vars;
    for (size_t i = 0; i < model->variables().size(); ++i) {
        if (model->is_defined_var(i)) {
            original_defined_vars.insert(i);
        }
    }

    // Create constraints via registry
    ConstraintRegistry registry;
    register_all_constraints(registry);

    for (size_t constraint_idx = 0; constraint_idx < constraint_decls_.size(); ++constraint_idx) {
        const auto& decl = constraint_decls_[constraint_idx];

        auto result = registry.create(decl.name, decl, ctx);
        if (result.has_value() && *result) {
            if (decl.line > 0) {
                (*result)->set_label(decl.name + ":L" + std::to_string(decl.line));
            }
            model->add_constraint(std::move(*result));
        }
    }

    return model;
}

} // namespace fzn
} // namespace sabori_csp
