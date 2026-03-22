/**
 * @file model_simplifier.cpp
 * @brief モデル簡略化クラスの実装
 */
#include "sabori_csp/model_simplifier.hpp"
#include "sabori_csp/constraints/global.hpp"
#include <iostream>
#include <cmath>
#include <numeric>
#include <algorithm>
#include <stdexcept>

namespace sabori_csp {


bool ModelSimplifier::simplify(Model& model,
                                const std::unordered_set<size_t>& protected_var_ids,
                                bool verbose) {
    substitutions_.clear();
    subst_map_.clear();
    y_vars_.clear();
    defining_constraints_.clear();

    auto var_constraints = build_var_constraints(model);
    find_elimination_candidates(model, protected_var_ids, var_constraints);

    if (substitutions_.empty()) {
        return false;
    }

    if (verbose) {
        std::cerr << "% [verbose] ModelSimplifier: eliminated "
                  << substitutions_.size() << " variable(s)\n";
        for (const auto& s : substitutions_) {
            std::cerr << "%   " << model.variable(s.x_id)->name()
                      << " -> " << s.cx << "*X + " << s.cy << "*"
                      << model.variable(s.y_id)->name() << " = " << s.rhs
                      << " (defining cst #" << s.defining_constraint_idx << ")\n";
        }
    }

    apply_substitutions(model);  // throws on UNSAT

    model.compact_constraints();
    return true;
}


std::unordered_map<size_t, std::vector<size_t>> ModelSimplifier::build_var_constraints(
    const Model& model) const {
    std::unordered_map<size_t, std::vector<size_t>> mapping;
    const auto& constraints = model.constraints();
    for (size_t c = 0; c < constraints.size(); ++c) {
        if (!constraints[c]) continue;
        const auto& var_ids = constraints[c]->var_ids_ref();
        for (size_t vid : var_ids) {
            mapping[vid].push_back(c);
        }
    }
    return mapping;
}


void ModelSimplifier::find_elimination_candidates(
    Model& model,
    const std::unordered_set<size_t>& protected_var_ids,
    const std::unordered_map<size_t, std::vector<size_t>>& var_constraints) {

    const auto& constraints = model.constraints();

    for (size_t ci = 0; ci < constraints.size(); ++ci) {
        if (!constraints[ci]) continue;

        auto* lin_eq = dynamic_cast<IntLinEqConstraint*>(constraints[ci].get());
        if (!lin_eq) continue;

        const auto& coeffs = lin_eq->coeffs();
        const auto& var_ids = lin_eq->var_ids_ref();
        int64_t target_sum = lin_eq->target_sum();

        // 非固定変数だけ集める
        std::vector<size_t> non_fixed;
        for (size_t i = 0; i < var_ids.size(); ++i) {
            if (!model.is_instantiated(var_ids[i])) {
                non_fixed.push_back(i);
            }
        }
        if (non_fixed.size() != 2) continue;

        size_t i0 = non_fixed[0], i1 = non_fixed[1];
        int64_t c0 = coeffs[i0], c1 = coeffs[i1];
        size_t v0 = var_ids[i0], v1 = var_ids[i1];

        // 固定変数の寄与をRHSから引く
        int64_t rhs_val = target_sum;
        for (size_t i = 0; i < var_ids.size(); ++i) {
            if (i == i0 || i == i1) continue;
            if (model.is_instantiated(var_ids[i])) {
                rhs_val -= coeffs[i] * model.value(var_ids[i]);
            }
        }

        // GCD 正規化
        {
            int64_t g = std::gcd(std::abs(c0), std::abs(c1));
            if (rhs_val != 0) g = std::gcd(g, std::abs(rhs_val));
            if (g > 1) {
                if (rhs_val % g != 0) continue;  // 整数解なし
                c0 /= g;
                c1 /= g;
                rhs_val /= g;
            }
        }

        // X の選択: |Cx|=1 の側を X にする
        size_t x_internal = SIZE_MAX, y_internal = SIZE_MAX;
        bool abs0_is_1 = (std::abs(c0) == 1);
        bool abs1_is_1 = (std::abs(c1) == 1);

        if (abs0_is_1 && !abs1_is_1) {
            x_internal = i0; y_internal = i1;
        } else if (!abs0_is_1 && abs1_is_1) {
            x_internal = i1; y_internal = i0;
        } else if (abs0_is_1 && abs1_is_1) {
            // 両方 |coeff|=1: is_defined_var 優先、次にドメインサイズ大きい方
            bool def0 = model.is_defined_var(v0);
            bool def1 = model.is_defined_var(v1);
            if (def0 && !def1) {
                x_internal = i0; y_internal = i1;
            } else if (!def0 && def1) {
                x_internal = i1; y_internal = i0;
            } else {
                size_t ds0 = model.var_size(v0);
                size_t ds1 = model.var_size(v1);
                if (ds0 >= ds1) { x_internal = i0; y_internal = i1; }
                else { x_internal = i1; y_internal = i0; }
            }
        } else {
            continue;  // どちらも |coeff|!=1
        }

        size_t x_id = var_ids[x_internal];
        size_t y_id = var_ids[y_internal];
        int64_t cx = (x_internal == i0) ? c0 : c1;
        int64_t cy = (y_internal == i0) ? c0 : c1;

        // 安全条件チェック（X が不適格なら X/Y スワップを試みる）
        auto is_ineligible = [&](size_t vid) {
            return protected_var_ids.count(vid) ||
                   subst_map_.count(vid) ||
                   y_vars_.count(vid);
        };

        if (is_ineligible(x_id)) {
            // Y が |coeff|=1 ならスワップ
            int64_t other_c = (x_internal == i0) ? c1 : c0;
            if (std::abs(other_c) == 1) {
                std::swap(x_id, y_id);
                std::swap(cx, cy);
            } else {
                continue;
            }
        }

        // 最終安全チェック
        if (protected_var_ids.count(x_id)) continue;
        if (subst_map_.count(x_id)) continue;
        if (y_vars_.count(x_id)) continue;
        if (subst_map_.count(y_id)) continue;  // 連鎖防止


        SubstitutionInfo info;
        info.x_id = x_id;
        info.y_id = y_id;
        info.cx = cx;
        info.cy = cy;
        info.rhs = rhs_val;
        info.defining_constraint_idx = ci;

        subst_map_[x_id] = substitutions_.size();
        substitutions_.push_back(info);
        defining_constraints_.insert(ci);
        y_vars_.insert(y_id);

        // X を defined_var としてマーク
        model.set_defined_var(x_id);
    }
}

void ModelSimplifier::apply_substitutions(Model& model) {
    const auto& constraints = model.constraints();

    for (size_t ci = 0; ci < constraints.size(); ++ci) {
        if (!constraints[ci]) continue;

        // 定義制約はそのまま残す（代入適用はスキップ）
        // X は eliminated で探索対象外だが、制約は X↔Y の伝播に必要
        if (defining_constraints_.count(ci)) {
            continue;
        }

        Constraint* cst = constraints[ci].get();

        // IntLinEqConstraint
        if (auto* p = dynamic_cast<IntLinEqConstraint*>(cst)) {
            auto coeffs = p->coeffs();  // copy
            auto var_ids = p->var_ids_ref();  // copy
            std::vector<size_t> vids(var_ids.begin(), var_ids.end());
            int64_t rhs = p->target_sum();

            if (!substitute_in_linear(coeffs, vids, rhs)) continue;

            if (vids.empty()) {
                if (rhs != 0) throw std::runtime_error("int_lin_eq: UNSAT after substitution (0 != " + std::to_string(rhs) + ")");
                model.remove_constraint(ci);
                continue;
            }

            std::vector<VariablePtr> vars;
            for (size_t vid : vids) vars.push_back(model.variable(vid));
            model.replace_constraint(ci,
                std::make_shared<IntLinEqConstraint>(std::move(coeffs), std::move(vars), rhs));
            continue;
        }

        // IntLinLeConstraint
        if (auto* p = dynamic_cast<IntLinLeConstraint*>(cst)) {
            auto coeffs = p->coeffs();
            auto var_ids = p->var_ids_ref();
            std::vector<size_t> vids(var_ids.begin(), var_ids.end());
            int64_t rhs = p->bound();

            if (!substitute_in_linear(coeffs, vids, rhs)) continue;

            if (vids.empty()) {
                if (0 > rhs) throw std::runtime_error("int_lin_le: UNSAT after substitution (0 > " + std::to_string(rhs) + ")");
                model.remove_constraint(ci);
                continue;
            }

            std::vector<VariablePtr> vars;
            for (size_t vid : vids) vars.push_back(model.variable(vid));
            model.replace_constraint(ci,
                std::make_shared<IntLinLeConstraint>(std::move(coeffs), std::move(vars), rhs));
            continue;
        }

        // IntLinNeConstraint
        if (auto* p = dynamic_cast<IntLinNeConstraint*>(cst)) {
            auto coeffs = p->coeffs();
            auto var_ids = p->var_ids_ref();
            std::vector<size_t> vids(var_ids.begin(), var_ids.end());
            int64_t rhs = p->target();

            if (!substitute_in_linear(coeffs, vids, rhs)) continue;

            if (vids.empty()) {
                if (rhs == 0) throw std::runtime_error("int_lin_ne: UNSAT after substitution (0 == 0)");
                model.remove_constraint(ci);
                continue;
            }

            std::vector<VariablePtr> vars;
            for (size_t vid : vids) vars.push_back(model.variable(vid));
            model.replace_constraint(ci,
                std::make_shared<IntLinNeConstraint>(std::move(coeffs), std::move(vars), rhs));
            continue;
        }

        // IntLinEqReifConstraint
        if (auto* p = dynamic_cast<IntLinEqReifConstraint*>(cst)) {
            auto coeffs = p->coeffs();  // linear vars only
            const auto& all_var_ids = p->var_ids_ref();
            // linear vars = first coeffs.size() entries, b = last
            std::vector<size_t> lin_vids(all_var_ids.begin(),
                                          all_var_ids.begin() + static_cast<ptrdiff_t>(coeffs.size()));
            size_t b_vid = p->b_id();
            int64_t rhs = p->target();

            if (!substitute_in_linear(coeffs, lin_vids, rhs)) continue;

            if (lin_vids.empty()) {
                if (rhs == 0) model.variable(b_vid)->remove(0);
                else model.variable(b_vid)->remove(1);
                model.remove_constraint(ci);
                continue;
            }

            std::vector<VariablePtr> vars;
            for (size_t vid : lin_vids) vars.push_back(model.variable(vid));
            model.replace_constraint(ci,
                std::make_shared<IntLinEqReifConstraint>(
                    std::move(coeffs), std::move(vars), rhs, model.variable(b_vid)));
            continue;
        }

        // IntLinNeReifConstraint
        if (auto* p = dynamic_cast<IntLinNeReifConstraint*>(cst)) {
            auto coeffs = p->coeffs();
            const auto& all_var_ids = p->var_ids_ref();
            std::vector<size_t> lin_vids(all_var_ids.begin(),
                                          all_var_ids.begin() + static_cast<ptrdiff_t>(coeffs.size()));
            size_t b_vid = p->b_id();
            int64_t rhs = p->target();

            if (!substitute_in_linear(coeffs, lin_vids, rhs)) continue;

            if (lin_vids.empty()) {
                if (rhs != 0) model.variable(b_vid)->remove(0);
                else model.variable(b_vid)->remove(1);
                model.remove_constraint(ci);
                continue;
            }

            std::vector<VariablePtr> vars;
            for (size_t vid : lin_vids) vars.push_back(model.variable(vid));
            model.replace_constraint(ci,
                std::make_shared<IntLinNeReifConstraint>(
                    std::move(coeffs), std::move(vars), rhs, model.variable(b_vid)));
            continue;
        }

        // IntLinLeReifConstraint
        if (auto* p = dynamic_cast<IntLinLeReifConstraint*>(cst)) {
            auto coeffs = p->coeffs();
            const auto& all_var_ids = p->var_ids_ref();
            std::vector<size_t> lin_vids(all_var_ids.begin(),
                                          all_var_ids.begin() + static_cast<ptrdiff_t>(coeffs.size()));
            size_t b_vid = p->b_id();
            int64_t rhs = p->bound();

            if (!substitute_in_linear(coeffs, lin_vids, rhs)) continue;

            if (lin_vids.empty()) {
                if (0 <= rhs) model.variable(b_vid)->remove(0);
                else model.variable(b_vid)->remove(1);
                model.remove_constraint(ci);
                continue;
            }

            std::vector<VariablePtr> vars;
            for (size_t vid : lin_vids) vars.push_back(model.variable(vid));
            model.replace_constraint(ci,
                std::make_shared<IntLinLeReifConstraint>(
                    std::move(coeffs), std::move(vars), rhs, model.variable(b_vid)));
            continue;
        }

        // IntLinLeImpConstraint
        if (auto* p = dynamic_cast<IntLinLeImpConstraint*>(cst)) {
            auto coeffs = p->coeffs();
            const auto& all_var_ids = p->var_ids_ref();
            std::vector<size_t> lin_vids(all_var_ids.begin(),
                                          all_var_ids.begin() + static_cast<ptrdiff_t>(coeffs.size()));
            size_t b_vid = p->b_id();
            int64_t rhs = p->bound();

            if (!substitute_in_linear(coeffs, lin_vids, rhs)) continue;

            if (lin_vids.empty()) {
                if (0 > rhs) model.variable(b_vid)->remove(1);
                model.remove_constraint(ci);
                continue;
            }

            std::vector<VariablePtr> vars;
            for (size_t vid : lin_vids) vars.push_back(model.variable(vid));
            model.replace_constraint(ci,
                std::make_shared<IntLinLeImpConstraint>(
                    std::move(coeffs), std::move(vars), rhs, model.variable(b_vid)));
            continue;
        }
    }

    // 消去変数をマーク
    for (const auto& s : substitutions_) {
        model.mark_variable_eliminated(s.x_id);
    }
}

bool ModelSimplifier::substitute_in_linear(
    std::vector<int64_t>& coeffs,
    std::vector<size_t>& var_ids,
    int64_t& rhs) const {

    bool changed = false;
    for (size_t i = 0; i < var_ids.size(); ) {
        auto it = subst_map_.find(var_ids[i]);
        if (it == subst_map_.end()) { ++i; continue; }

        changed = true;
        const auto& info = substitutions_[it->second];
        int64_t ck = coeffs[i];

        // X = (rhs_s - cy * Y) / cx
        // Substituting ck * X:
        //   ck * (rhs_s - cy * Y) / cx
        //   = ck/cx * rhs_s - ck/cx * cy * Y
        //   = ck * info.cx * info.rhs + ck * (-info.cx) * info.cy * Y
        // Wait, let me recalculate. info.cx * X + info.cy * Y = info.rhs
        // => X = (info.rhs - info.cy * Y) / info.cx
        // ck * X = ck * (info.rhs - info.cy * Y) / info.cx
        // Since |info.cx| == 1, 1/info.cx == info.cx (for cx=1 or cx=-1)
        // = ck * info.cx * info.rhs - ck * info.cx * info.cy * Y
        // RHS adjustment: -ck * info.cx * info.rhs  (move to RHS)
        // Wait, the original apply_substitutions in FznBuildContext does:
        //   y_coeff_add = -ck * info.cx * info.cy
        //   rhs_val -= ck * info.cx * info.rhs
        // Let me use the same formula.

        int64_t y_coeff_add = -ck * info.cx * info.cy;
        rhs -= ck * info.cx * info.rhs;

        // Remove X entry
        coeffs.erase(coeffs.begin() + static_cast<ptrdiff_t>(i));
        var_ids.erase(var_ids.begin() + static_cast<ptrdiff_t>(i));

        // Merge Y coefficient
        bool found_y = false;
        for (size_t j = 0; j < var_ids.size(); ++j) {
            if (var_ids[j] == info.y_id) {
                coeffs[j] += y_coeff_add;
                found_y = true;
                break;
            }
        }
        if (!found_y && y_coeff_add != 0) {
            coeffs.push_back(y_coeff_add);
            var_ids.push_back(info.y_id);
        }
        // Don't increment i - next element shifted into position
    }

    // Remove zero-coefficient entries
    if (changed) {
        size_t w = 0;
        for (size_t r = 0; r < coeffs.size(); ++r) {
            if (coeffs[r] != 0) {
                if (w != r) {
                    coeffs[w] = coeffs[r];
                    var_ids[w] = var_ids[r];
                }
                ++w;
            }
        }
        coeffs.resize(w);
        var_ids.resize(w);
    }

    return changed;
}

} // namespace sabori_csp
