#include <catch2/catch_test_macros.hpp>
#include "fzn_corpus_util.hpp"
#include <iostream>
#include <string>
#include <vector>

using namespace sabori_csp;
using namespace sabori_csp::test_fzn;

// ============================================================================
// 解の固定再解ゲート（偽 UNSAT / 偽の解の検出）
//
// golden master はソルバー自身が記録した出力との byte 照合なので、記録時点で
// 解を落としていればそのまま「正解」になり、偽 UNSAT を見逃す。実際
// bounds-only Domain の remove_below/remove_above 偽 UNSAT（3b1de3b）は golden を
// 素通りし、並列検証で「正解を固定した fzn を -j1 が UNSAT と判定」して見つかった。
//
// ここでは golden コーパスの全 fzn について、ソルバーが出した各解の値を
// int_eq/bool_eq 制約として元の fzn に追加し、解き直して SAT になることを要求する。
//   - 解の値が固定された状態の presolve / 伝播経路（探索時とは別経路）が
//     その解を棄却したら偽 UNSAT のバグ
//   - 出した解が実際には制約違反なら、固定後の伝播で UNSAT になる（偽の解）
// ============================================================================

namespace {

/// 1 fzn あたりの再解数の上限（全解が多い fzn は等間隔に間引く。最後の解は別枠で必ず含める）
constexpr size_t kMaxResolvePerFzn = 32;

std::vector<Solution> collect_solutions(const fzn::Model& fm) {
    std::vector<Solution> sols;
    auto model = build_model(fm);
    if (!model) return sols;
    Solver s;
    auto collect = [&sols](const Solution& sol) { sols.push_back(sol); return true; };
    const auto& sd = fm.solve_decl();
    if (sd.kind == fzn::SolveKind::Satisfy) {
        s.solve_all(*model, collect);
    } else {
        size_t obj = model->find_variable_index(sd.objective_var);
        s.solve_optimize(*model, obj, sd.kind == fzn::SolveKind::Minimize, collect);
    }
    return sols;
}

/// fzn 宣言にある非定数変数のうち、解に値があるものを int_eq/bool_eq で固定した fzn。
/// 補助変数（"__" 始まり）は fzn 上に存在しないので対象外。
fzn::Model fix_solution(const fzn::Model& fm, const Solution& sol, size_t& n_fixed) {
    fzn::Model fixed = fm;
    n_fixed = 0;
    for (const auto& [name, decl] : fm.var_decls()) {
        if (decl.fixed_value) continue;
        auto it = sol.find(name);
        if (it == sol.end()) continue;
        fzn::ConstraintDecl c;
        c.name = decl.is_bool ? "bool_eq" : "int_eq";
        c.args = {name, it->second};
        fixed.add_constraint_decl(std::move(c));
        ++n_fixed;
    }
    return fixed;
}

} // namespace

TEST_CASE("resolve: 各解の値を固定して解き直すと SAT", "[fzn][resolve]") {
    auto corpus = read_corpus();
    REQUIRE(corpus.size() > 200);

    size_t n_resolved = 0;
    for (const auto& rel : corpus) {
        CAPTURE(rel);
        auto fm = fzn::parse_file(std::string(SABORI_SOURCE_DIR) + "/" + rel);
        REQUIRE(fm);
        auto sols = collect_solutions(*fm);
        if (sols.empty()) continue;

        // 間引いても最後の解（最適化なら最適解）は必ず含める。remove_value.fzn の
        // bounds-only 偽 UNSAT は 266 個の改善解のうち最適解でしか踏まない
        size_t step = (sols.size() + kMaxResolvePerFzn - 1) / kMaxResolvePerFzn;
        std::vector<size_t> picks;
        for (size_t i = 0; i < sols.size(); i += step) picks.push_back(i);
        if (picks.back() != sols.size() - 1) picks.push_back(sols.size() - 1);
        for (size_t i : picks) {
            CAPTURE(i);
            size_t n_fixed = 0;
            auto fixed = fix_solution(*fm, sols[i], n_fixed);
            REQUIRE(n_fixed > 0);

            auto model = build_model(fixed);
            CHECK(model != nullptr);  // 簡約段階での棄却
            if (!model) continue;
            Solver s;
            auto re = s.solve(*model);
            CHECK(re.has_value());
            if (!re) continue;
            // 固定した変数の値がそのまま返ること
            for (const auto& [name, decl] : fm->var_decls()) {
                if (decl.fixed_value) continue;
                auto a = sols[i].find(name);
                auto b = re->find(name);
                if (a == sols[i].end() || b == re->end()) continue;
                CAPTURE(name);
                CHECK(a->second == b->second);
            }
            ++n_resolved;
        }
    }
    std::cout << "[fzn resolve] " << n_resolved << " solutions re-solved\n";
}
