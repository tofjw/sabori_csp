#include "sabori_csp/fzn/model.hpp"
#include "fzn_build_context.hpp"
#include "constraint_registry.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <set>
#include <stdexcept>
#include <unordered_map>
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

    // Phase 0.6: 制約 hash-consing（SABORI_DEDUP=0 で無効化、既定有効）
    //   (a) 完全重複制約の除去（C ∧ C = C なので常に健全）
    //   (b) 同一条件 *_reif の b 統合: b1 ↔ C と b2 ↔ C なら全解で b1 = b2
    //       なので b2 を b1 のエイリアスにして後発制約を削除。
    //       *_imp は b → C の片方向で b1 = b2 が帰結しないため対象外
    // 物量根拠: presolve_volume_probe.py (stripboard/community-detection で
    // 変数の 11-12%、harmony で制約の 16%)。重複 reif は
    // OneHotChannelAggregator の「重複を含むグループは集約しない」条件も
    // 解除する。
    std::vector<bool> skip_constraint(constraint_decls_.size(), false);
    {
        const char* dedup_env = std::getenv("SABORI_DEDUP");
        const bool dedup_enabled = !(dedup_env && dedup_env[0] == '0');

        // alias 連鎖を根まで解決（bool2int 由来 + 本パスで追加される分）
        auto resolve = [&alias_map](const std::string& n) -> const std::string& {
            const std::string* cur = &n;
            for (int i = 0; i < 16; ++i) {
                auto it = alias_map.find(*cur);
                if (it == alias_map.end()) break;
                cur = &it->second;
            }
            return *cur;
        };
        // b 統合に使える bool 変数か（bool2int と同じ縮退ガード + 目的変数除外）
        auto mergeable_bool = [&](const std::string& name) {
            auto it = var_decls_.find(name);
            return it != var_decls_.end() && it->second.is_bool &&
                   !it->second.fixed_value && !it->second.alias_target &&
                   it->second.lb != it->second.ub &&
                   name != solve_decl_.objective_var;
        };
        auto serialize = [&](const ConstraintDecl& d, bool drop_last) {
            std::string key = d.name;
            const size_t n = d.args.size() - (drop_last ? 1 : 0);
            for (size_t i = 0; i < n; ++i) {
                key += '\x01';
                const auto& a = d.args[i];
                if (const auto* v = std::get_if<Domain::value_type>(&a)) {
                    key += 'I';
                    key += std::to_string(*v);
                } else if (const auto* s = std::get_if<std::string>(&a)) {
                    key += 'S';
                    key += resolve(*s);
                } else if (const auto* vi =
                               std::get_if<std::vector<Domain::value_type>>(&a)) {
                    key += 'A';
                    for (auto x : *vi) {
                        key += std::to_string(x);
                        key += ',';
                    }
                } else if (const auto* vs =
                               std::get_if<std::vector<std::string>>(&a)) {
                    key += 'V';
                    for (const auto& e : *vs) {
                        key += resolve(e);
                        key += ',';
                    }
                } else if (const auto* r = std::get_if<IntRange>(&a)) {
                    key += 'R';
                    key += std::to_string(r->lb);
                    key += "..";
                    key += std::to_string(r->ub);
                }
            }
            return key;
        };

        if (dedup_enabled) {
            std::unordered_map<std::string, size_t> full_keys;   // 完全一致
            std::unordered_map<std::string, size_t> reif_conds;  // 条件部一致
            size_t dup_rows = 0, reif_merged = 0;
            for (size_t i = 0; i < constraint_decls_.size(); ++i) {
                const auto& d = constraint_decls_[i];
                auto [fit, fresh] = full_keys.try_emplace(serialize(d, false), i);
                if (!fresh) {
                    skip_constraint[i] = true;
                    ++dup_rows;
                    continue;
                }
                // reif の b 統合（末尾引数が bool 変数名のときのみ）
                const bool is_reif =
                    d.name.size() > 5 &&
                    d.name.compare(d.name.size() - 5, 5, "_reif") == 0;
                if (!is_reif || d.args.empty()) continue;
                const auto* b2 = std::get_if<std::string>(&d.args.back());
                if (!b2) continue;
                auto [rit, cond_fresh] =
                    reif_conds.try_emplace(serialize(d, true), i);
                if (cond_fresh) continue;
                const auto& first = constraint_decls_[rit->second];
                const auto* b1 = std::get_if<std::string>(&first.args.back());
                if (!b1) continue;
                const std::string b1r = resolve(*b1);
                const std::string b2r = resolve(*b2);
                if (b1r == b2r) {  // 実質完全重複
                    skip_constraint[i] = true;
                    ++dup_rows;
                    continue;
                }
                if (mergeable_bool(b1r) && mergeable_bool(b2r)) {
                    alias_map[b2r] = b1r;
                    skip_constraint[i] = true;
                    ++reif_merged;
                }
            }
            if (verbose && (dup_rows > 0 || reif_merged > 0)) {
                std::cerr << "% [verbose] constraint dedup: " << dup_rows
                          << " duplicate row(s) removed, " << reif_merged
                          << " reif bool(s) merged\n";
            }
        }
    }

    // 固定値 var_decl の eager 生成スキップ判定に使う: 出力対象の配列の
    // 要素は解出力が sol を名前引きするので必ず実体化する。
    std::set<std::string> output_elems;
    for (const auto& [aname, arr_decl] : array_decls_) {
        if (!arr_decl.is_output) continue;
        for (const auto& elem : arr_decl.elements) {
            output_elems.insert(elem);
        }
    }

    // Create variables
    //
    // 固定値 (par) の var_decl はここでは生成せず、制約から名前参照された
    // ときに FznBuildContext::get_var_by_name が遅延実体化する。
    // par 配列（テーブルデータや線形係数）は要素ごとに固定値 var_decl として
    // パースされるため、eager 生成すると定数セル数ぶんの Variable が
    // モデルに積まれる（mznc2025 groupsplitter で 483万個 → presolve/探索
    // 基盤全体が定数スケールで死ぬ）。定数は resolve_int_array 経由で
    // 消費されるのが大半で、Variable 実体が要るのは名前参照時のみ。
    // 例外として出力対象（is_output / 出力配列の要素）と目的変数は実体化する。
    for (const auto& [name, decl] : var_decls_) {
        // エイリアス対象の変数はスキップ
        if (alias_map.count(name) || decl.alias_target) {
            continue;
        }
        if (decl.fixed_value && !decl.is_output && !output_elems.count(name) &&
            name != solve_decl_.objective_var) {
            continue;  // 遅延実体化に委ねる
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
    // （reif b 統合で bool2int の正規変数がさらにエイリアス化されうるので
    //   連鎖を根まで解決してから引く）
    for (const auto& [alias_name, canonical_name] : alias_map) {
        const std::string* root = &canonical_name;
        for (int g = 0; g < 16; ++g) {
            auto next = alias_map.find(*root);
            if (next == alias_map.end()) break;
            root = &next->second;
        }
        auto it = var_map.find(*root);
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
    for (const auto& d : constraint_decls_) {
        if (d.name.rfind("fzn_diffn", 0) == 0) {
            ++ctx.diffn_decl_count;
        }
    }

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
        if (skip_constraint[constraint_idx]) {
            continue;  // hash-consing で重複と判定済み
        }
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
