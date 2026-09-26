/**
 * @file fzn_corpus_util.hpp
 * @brief golden コーパスの fzn を in-process で解くテストの共通部
 */
#pragma once

#include "fzn_parser.hpp"
#include "sabori_csp/model.hpp"
#include "sabori_csp/model_simplifier.hpp"
#include "sabori_csp/solver.hpp"
#include <fstream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace sabori_csp {
namespace test_fzn {

/// tests/golden/corpus.txt の fzn（ROOT 相対パス）一覧。
inline std::vector<std::string> read_corpus() {
    std::vector<std::string> paths;
    std::ifstream in(std::string(SABORI_SOURCE_DIR) + "/tests/golden/corpus.txt");
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) paths.push_back(line);
    }
    return paths;
}

/// fzn → Model（fzn_sabori の既定経路と同じく ModelSimplifier まで適用）。
/// 簡約で確定 UNSAT になった場合は nullptr。
inline std::unique_ptr<Model> build_model(const fzn::Model& fm) {
    auto model = fm.to_model();
    std::unordered_set<size_t> protected_ids;
    if (fm.solve_decl().kind != fzn::SolveKind::Satisfy &&
        !fm.solve_decl().objective_var.empty()) {
        size_t idx = model->find_variable_index(fm.solve_decl().objective_var);
        if (idx != SIZE_MAX) protected_ids.insert(idx);
    }
    ModelSimplifier simplifier;
    simplifier.simplify(*model, protected_ids, false);
    if (simplifier.is_infeasible()) return nullptr;
    return model;
}

/// 補助変数のうち定数用（__const_ 等）と set_in_reif 分解用（__sir_）は、名前の通し番号が
/// プロセス内 static カウンタ由来で、同じ fzn を 2 回構築すると名前がずれる。解から取り除く
/// （定数は比較する意味がなく、__sir_ の値の食い違いは統計の一致検査で捕まる）。
inline Solution normalize(const Solution& sol) {
    Solution out;
    for (const auto& [name, v] : sol) {
        if (name.rfind("__const_", 0) == 0 || name.rfind("__idx_const_", 0) == 0 ||
            name.rfind("__res_const_", 0) == 0 || name.rfind("__sir_", 0) == 0) {
            continue;
        }
        out.emplace(name, v);
    }
    return out;
}

} // namespace test_fzn
} // namespace sabori_csp
