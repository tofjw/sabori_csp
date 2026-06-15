// Find-all (solve_all) enumeration consistency tests.
//
// Design-review safety net (refactoring-plan-2026-06.md, Phase 6 "find_all 解列挙
// 設計レビュー"). solve_all has TWO enumeration engines selected by
// restart_enabled_:
//   - restart_enabled_ = true (default): search_with_restart + solution NoGoods.
//     Each found solution is blocked by a permanent NoGood, then search restarts
//     from root. Completeness/duplicate-freeness depends on the NoGood correctly
//     blocking exactly the found solution (the 2026-06-10 root-instantiated-watch
//     infinite-loop bug lived here) and on the unit-NoGood degeneration path.
//   - restart_enabled_ = false: run_search explicit-stack DFS Enumerate mode — a
//     classical, trivially-complete enumerator with no NoGood dependence.
//
// golden master only checks the restart path against its own frozen output, never
// against an independent oracle. These tests cross-check the two engines (and an
// analytic count where known) and assert NO DUPLICATES, which directly guards the
// "re-find the same solution forever" failure mode.
#include <catch2/catch_test_macros.hpp>
#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/variable.hpp"
#include "sabori_csp/model.hpp"
#include "sabori_csp/solver.hpp"
#include <functional>
#include <optional>
#include <set>
#include <vector>

using namespace sabori_csp;

namespace {

using Row = std::vector<int64_t>;

// Build a fresh model, returning the variables to record (in column order).
using Builder = std::function<std::vector<Variable*>(Model&)>;

// Enumerate all solutions with a given restart setting; return every reported row
// (NOT deduplicated, so callers can detect duplicate reports).
std::vector<Row> enumerate(const Builder& build, bool restart) {
    Model model;
    auto vars = build(model);
    std::vector<std::string> names;
    names.reserve(vars.size());
    for (auto* v : vars) names.push_back(v->name());

    Solver solver;
    solver.set_restart_enabled(restart);
    std::vector<Row> rows;
    solver.solve_all(model, [&](const Solution& sol) {
        Row row;
        row.reserve(names.size());
        for (const auto& n : names) row.push_back(sol.at(n));
        rows.push_back(std::move(row));
        return true;
    });
    return rows;
}

// Cross-check both engines against each other (and an optional analytic count).
// Asserts: each engine reports no duplicate, and both report the identical SET.
void check(const Builder& build, std::optional<size_t> expected_count = std::nullopt) {
    auto with_restart = enumerate(build, true);
    auto no_restart = enumerate(build, false);

    std::set<Row> set_restart(with_restart.begin(), with_restart.end());
    std::set<Row> set_dfs(no_restart.begin(), no_restart.end());

    // No engine may report the same full assignment twice.
    REQUIRE(with_restart.size() == set_restart.size());
    REQUIRE(no_restart.size() == set_dfs.size());

    // Both engines must enumerate exactly the same solution set.
    REQUIRE(set_restart == set_dfs);

    if (expected_count) {
        REQUIRE(set_restart.size() == *expected_count);
    }
}

}  // namespace

TEST_CASE("find_all: single free variable (unit solution NoGoods)", "[solver][find_all]") {
    // One search variable, no constraints: every solution NoGood degenerates to a
    // single literal (unit NoGood). Exercises the unit-NoGood enumeration path.
    check([](Model& m) {
        return std::vector<Variable*>{m.create_variable("x", 0, 4)};
    }, 5);
}

TEST_CASE("find_all: permutations via all_different", "[solver][find_all]") {
    // x,y,z over {0,1,2} all-different -> 3! = 6 permutations. Multiple search vars,
    // many solution NoGoods accumulate.
    check([](Model& m) {
        auto x = m.create_variable("x", 0, 2);
        auto y = m.create_variable("y", 0, 2);
        auto z = m.create_variable("z", 0, 2);
        m.add_constraint(std::make_unique<AllDifferentConstraint>(
            std::vector<Variable*>{x, y, z}));
        return std::vector<Variable*>{x, y, z};
    }, 6);
}

TEST_CASE("find_all: all_different over 4 vars (24 solutions)", "[solver][find_all]") {
    // Stress NoGood accumulation: 4! = 24 permutations, each blocked by a permanent
    // solution NoGood.
    check([](Model& m) {
        std::vector<Variable*> v;
        for (int i = 0; i < 4; ++i) v.push_back(m.create_variable("v" + std::to_string(i), 0, 3));
        m.add_constraint(std::make_unique<AllDifferentConstraint>(v));
        return v;
    }, 24);
}

TEST_CASE("find_all: constant variable excluded from NoGoods", "[solver][find_all]") {
    // A singleton (initial_range == 1) variable is filtered out of solution
    // literals. The free variable must still enumerate fully and the constant must
    // be reported in every solution.
    check([](Model& m) {
        auto c = m.create_variable("c", 7, 7);   // constant
        auto x = m.create_variable("x", 0, 3);   // free
        return std::vector<Variable*>{c, x};
    }, 4);
}

TEST_CASE("find_all: linear equality with several solutions", "[solver][find_all]") {
    // x + y + z = 6 over {1..3}^3. Root propagation may tighten domains; the two
    // engines must still agree on the full set.
    check([](Model& m) {
        auto x = m.create_variable("x", 1, 3);
        auto y = m.create_variable("y", 1, 3);
        auto z = m.create_variable("z", 1, 3);
        m.add_constraint(std::make_unique<IntLinEqConstraint>(
            std::vector<int64_t>{1, 1, 1}, std::vector<Variable*>{x, y, z}, 6));
        return std::vector<Variable*>{x, y, z};
    });
}

TEST_CASE("find_all: unique solution forced by propagation", "[solver][find_all]") {
    // x + y = 2 over {0,1}^2 -> only (1,1). Presolve/root propagation may fully
    // instantiate before any decision; enumeration must report exactly one solution
    // and terminate (the "all assigned after backtrack" termination path).
    check([](Model& m) {
        auto x = m.create_variable("x", 0, 1);
        auto y = m.create_variable("y", 0, 1);
        m.add_constraint(std::make_unique<IntLinEqConstraint>(
            std::vector<int64_t>{1, 1}, std::vector<Variable*>{x, y}, 2));
        return std::vector<Variable*>{x, y};
    }, 1);
}

TEST_CASE("find_all: unsatisfiable model enumerates nothing", "[solver][find_all]") {
    // x + y = 5 over {0,1}^2 is UNSAT. Both engines report zero solutions.
    check([](Model& m) {
        auto x = m.create_variable("x", 0, 1);
        auto y = m.create_variable("y", 0, 1);
        m.add_constraint(std::make_unique<IntLinEqConstraint>(
            std::vector<int64_t>{1, 1}, std::vector<Variable*>{x, y}, 5));
        return std::vector<Variable*>{x, y};
    }, 0);
}
