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

std::unique_ptr<sabori_csp::Model> Model::to_model(bool verbose) const {
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

    // Phase 2: 2変数 int_lin_eq の代入マップ構築
    std::map<std::string, SubstInfo> subst_map;
    std::unordered_set<size_t> defining_constraint_indices;
    std::unordered_set<std::string> subst_y_vars;  // 連鎖防止

    // 出力変数・目的関数変数は消去禁止
    std::unordered_set<std::string> protected_vars;
    for (const auto& [name, vdecl] : var_decls_) {
        if (vdecl.is_output) protected_vars.insert(name);
    }
    if (solve_decl_.kind != SolveKind::Satisfy && !solve_decl_.objective_var.empty()) {
        protected_vars.insert(solve_decl_.objective_var);
    }
    for (const auto& [arr_name, arr_decl] : array_decls_) {
        if (arr_decl.is_output) {
            for (const auto& elem : arr_decl.elements) protected_vars.insert(elem);
        }
    }

    // 変数ごとの制約出現回数を事前カウント
    constexpr size_t kMaxConstraintCountForElimination = 2;
    std::unordered_map<std::string, size_t> var_constraint_count;
    for (const auto& cdecl : constraint_decls_) {
        for (const auto& arg : cdecl.args) {
            if (std::holds_alternative<std::string>(arg)) {
                const auto& s = std::get<std::string>(arg);
                if (var_decls_.count(s)) {
                    ++var_constraint_count[s];
                } else {
                    auto ait = array_decls_.find(s);
                    if (ait != array_decls_.end()) {
                        for (const auto& elem : ait->second.elements) {
                            ++var_constraint_count[elem];
                        }
                    }
                }
            } else if (std::holds_alternative<std::vector<std::string>>(arg)) {
                for (const auto& elem : std::get<std::vector<std::string>>(arg)) {
                    ++var_constraint_count[elem];
                }
            }
        }
    }

    for (size_t ci = 0; ci < constraint_decls_.size(); ++ci) {
        const auto& cdecl = constraint_decls_[ci];
        if (cdecl.name != "int_lin_eq" || cdecl.args.size() != 3) continue;
        if (!std::holds_alternative<Domain::value_type>(cdecl.args[2])) continue;

        // 係数と変数名を取得
        std::vector<int64_t> cs;
        std::vector<std::string> vns;
        if (std::holds_alternative<std::vector<Domain::value_type>>(cdecl.args[0])) {
            for (auto v : std::get<std::vector<Domain::value_type>>(cdecl.args[0]))
                cs.push_back(v);
        } else continue;
        if (std::holds_alternative<std::vector<std::string>>(cdecl.args[1])) {
            vns = std::get<std::vector<std::string>>(cdecl.args[1]);
        } else if (std::holds_alternative<std::string>(cdecl.args[1])) {
            const auto& aname = std::get<std::string>(cdecl.args[1]);
            auto ait = array_decls_.find(aname);
            if (ait != array_decls_.end()) vns = ait->second.elements;
            else continue;
        } else continue;

        if (cs.size() != vns.size()) continue;

        // 非固定変数だけ集める
        std::vector<size_t> non_fixed;
        for (size_t i = 0; i < vns.size(); ++i) {
            auto vit = var_decls_.find(vns[i]);
            if (vit != var_decls_.end() && vit->second.fixed_value) continue;
            // alias の解決先も確認
            if (alias_map.count(vns[i])) {
                auto ait2 = var_decls_.find(alias_map.at(vns[i]));
                if (ait2 != var_decls_.end() && ait2->second.fixed_value) continue;
            }
            non_fixed.push_back(i);
        }
        if (non_fixed.size() != 2) continue;

        size_t i0 = non_fixed[0], i1 = non_fixed[1];
        int64_t c0 = cs[i0], c1 = cs[i1];
        const std::string& n0 = vns[i0];
        const std::string& n1 = vns[i1];

        // 固定変数の寄与をRHSから引く
        int64_t rhs_val = std::get<Domain::value_type>(cdecl.args[2]);
        for (size_t i = 0; i < vns.size(); ++i) {
            if (i == i0 || i == i1) continue;
            auto vit = var_decls_.find(vns[i]);
            std::string resolved = vns[i];
            if (alias_map.count(vns[i])) resolved = alias_map.at(vns[i]);
            auto rit = var_decls_.find(resolved);
            if (rit != var_decls_.end() && rit->second.fixed_value) {
                rhs_val -= cs[i] * (*rit->second.fixed_value);
            }
        }

        // GCD 正規化: c0, c1, rhs_val を GCD で割る
        {
            int64_t g = std::gcd(std::abs(c0), std::abs(c1));
            if (rhs_val != 0) g = std::gcd(g, std::abs(rhs_val));
            if (g > 1) {
                if (rhs_val % g != 0) continue;  // 整数解なし
                c0 /= g; cs[i0] = c0;
                c1 /= g; cs[i1] = c1;
                rhs_val /= g;
            }
        }

        // X の選択: |Cx|=1 の側を X にする
        // 両方 |coeff|=1 の場合は is_defined_var アノテーション優先、次にドメインサイズ大きい方
        size_t x_idx = SIZE_MAX, y_idx = SIZE_MAX;
        bool abs0_is_1 = (std::abs(c0) == 1);
        bool abs1_is_1 = (std::abs(c1) == 1);
        if (abs0_is_1 && !abs1_is_1) {
            x_idx = i0; y_idx = i1;
        } else if (!abs0_is_1 && abs1_is_1) {
            x_idx = i1; y_idx = i0;
        } else if (abs0_is_1 && abs1_is_1) {
            // 両方 |coeff|=1
            auto vit0 = var_decls_.find(n0);
            auto vit1 = var_decls_.find(n1);
            bool def0 = (vit0 != var_decls_.end() && vit0->second.is_defined_var);
            bool def1 = (vit1 != var_decls_.end() && vit1->second.is_defined_var);
            if (def0 && !def1) {
                x_idx = i0; y_idx = i1;
            } else if (!def0 && def1) {
                x_idx = i1; y_idx = i0;
            } else {
                // ドメインサイズで判断
                size_t ds0 = 0, ds1 = 0;
                auto mit0 = var_map.find(n0);
                auto mit1 = var_map.find(n1);
                if (mit0 != var_map.end()) ds0 = mit0->second->domain().size();
                if (mit1 != var_map.end()) ds1 = mit1->second->domain().size();
                if (ds0 >= ds1) { x_idx = i0; y_idx = i1; }
                else { x_idx = i1; y_idx = i0; }
            }
        } else {
            continue;  // どちらも |coeff|!=1
        }

        // 安全条件チェック（X が不適格なら X/Y スワップを試みる）
        if (protected_vars.count(vns[x_idx]) || subst_map.count(vns[x_idx]) ||
            subst_y_vars.count(vns[x_idx]) || alias_map.count(vns[x_idx])) {
            // X が不適格 → Y が |coeff|=1 ならスワップ
            if (std::abs(cs[y_idx]) == 1) {
                std::swap(x_idx, y_idx);
            } else {
                continue;
            }
        }
        const std::string& x_name = vns[x_idx];
        const std::string& y_name = vns[y_idx];
        int64_t cx = cs[x_idx];
        int64_t cy = cs[y_idx];
        if (protected_vars.count(x_name)) continue;
        if (subst_map.count(x_name)) continue;
        if (subst_y_vars.count(x_name)) continue;
        if (subst_map.count(y_name)) continue;  // 連鎖防止
        if (alias_map.count(x_name)) continue;
        // X の制約出現数が上限を超える場合はスキップ
        {
            auto cit = var_constraint_count.find(x_name);
            if (cit != var_constraint_count.end() && cit->second > kMaxConstraintCountForElimination) continue;
        }

        subst_map[x_name] = SubstInfo{y_name, cx, cy, rhs_val};
        defining_constraint_indices.insert(ci);
        subst_y_vars.insert(y_name);

        // X を defined_var としてマーク
        auto xit = var_map.find(x_name);
        if (xit != var_map.end()) {
            model->set_defined_var(xit->second->id());
        }
    }

    if (verbose && !subst_map.empty()) {
        std::cerr << "% [verbose] int_lin_eq substitution: eliminated "
                  << subst_map.size() << " variable(s)\n";
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
                        constant_arrays, subst_map, alias_map,
                        false, verbose};

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
        ctx.is_defining = defining_constraint_indices.count(constraint_idx) > 0;
        const auto& decl = constraint_decls_[constraint_idx];

        auto result = registry.create(decl.name, decl, ctx);
        if (result.has_value() && *result) {
            model->add_constraint(std::move(*result));
        }
    }

    return model;
}

} // namespace fzn
} // namespace sabori_csp

