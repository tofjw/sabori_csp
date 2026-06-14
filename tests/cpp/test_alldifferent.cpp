#include <catch2/catch_test_macros.hpp>
#include "sabori_csp/constraint.hpp"
#include "sabori_csp/constraints/logical.hpp"
#include "sabori_csp/constraints/all_different_gac.hpp"
#include "sabori_csp/variable.hpp"
#include "sabori_csp/domain.hpp"
#include "sabori_csp/model.hpp"
#include "sabori_csp/solver.hpp"
#include <set>
#include <tuple>
#include <algorithm>
#include <iostream>

using namespace sabori_csp;

// ============================================================================
// AllDifferentConstraint tests
// ============================================================================

TEST_CASE("AllDifferentConstraint name", "[constraint][all_different]") {
    Model model;
    auto* x = model.create_variable("x", Domain(1, 3));
    auto* y = model.create_variable("y", Domain(1, 3));
    auto* z = model.create_variable("z", Domain(1, 3));
    AllDifferentConstraint c({x, y, z});

    REQUIRE(c.name() == "all_different");
}

TEST_CASE("AllDifferentConstraint variables", "[constraint][all_different]") {
    Model model;
    auto* x = model.create_variable("x", Domain(1, 3));
    auto* y = model.create_variable("y", Domain(1, 3));
    AllDifferentConstraint c({x, y});

    auto& vars = c.var_ids_ref();
    REQUIRE(vars.size() == 2);
}

TEST_CASE("AllDifferentConstraint is_satisfied", "[constraint][all_different]") {
    SECTION("all different values - satisfied") {
        Model model;
        auto* x = model.create_variable("x", Domain(1, 1));
        auto* y = model.create_variable("y", Domain(2, 2));
        auto* z = model.create_variable("z", Domain(3, 3));
        AllDifferentConstraint c({x, y, z});

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == true);
    }

    SECTION("duplicate values - violated") {
        Model model;
        auto* x = model.create_variable("x", Domain(1, 1));
        auto* y = model.create_variable("y", Domain(2, 2));
        auto* z = model.create_variable("z", Domain(1, 1));  // same as x
        AllDifferentConstraint c({x, y, z});

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == false);
    }

    SECTION("not fully assigned") {
        Model model;
        auto* x = model.create_variable("x", Domain(1, 1));
        auto* y = model.create_variable("y", Domain(1, 3));
        auto* z = model.create_variable("z", Domain(3, 3));
        AllDifferentConstraint c({x, y, z});

        REQUIRE_FALSE(c.is_satisfied(model).has_value());
    }
}

TEST_CASE("AllDifferentConstraint propagate", "[constraint][all_different]") {
    SECTION("remove assigned values from others") {
        Model model;
        auto x = model.create_variable("x", 2, 2);  // assigned
        auto y = model.create_variable("y", 1, 3);
        auto z = model.create_variable("z", 1, 3);
        AllDifferentConstraint c({x, y, z});

        REQUIRE(c.presolve(model) != PresolveResult::Contradiction);
        REQUIRE_FALSE(y->domain().contains(2));
        REQUIRE_FALSE(z->domain().contains(2));
    }

    SECTION("infeasible - domain becomes empty") {
        Model model;
        auto* x = model.create_variable("x", Domain(1, 1));  // assigned to 1
        auto* y = model.create_variable("y", Domain(1, 1));  // domain {1}, unassigned for propagation check
        // Note: make_var with single value creates a singleton which is considered assigned
        // So we need a different approach to test infeasibility

        // This tests that when x=1 is assigned and y only has {1},
        // propagate will remove 1 from y making it empty
        AllDifferentConstraint c({x, y});

        // Note: Both are singletons (assigned), so propagate won't modify them
        // The infeasibility is detected by is_satisfied() instead
        REQUIRE(c.is_satisfied(model).value() == false);
    }

    SECTION("infeasible via propagation") {
        Model model;
        auto x = model.create_variable("x", 2, 2);  // assigned to 2
        auto y = model.create_variable("y", 2, 2);  // domain {2}
        auto z = model.create_variable("z", 2, 3);  // domain {2, 3}
        AllDifferentConstraint c({x, y, z});

        REQUIRE(c.on_final_instantiate(model) == false);
    }
}

TEST_CASE("AllDifferentConstraint pool management", "[constraint][all_different]") {
    Model model;
    auto* x = model.create_variable("x", Domain(1, 3));
    auto* y = model.create_variable("y", Domain(1, 3));
    auto* z = model.create_variable("z", Domain(1, 3));
    AllDifferentConstraint c({x, y, z});

    SECTION("initial pool size") {
        REQUIRE(c.pool_size() == 3);  // {1, 2, 3}
    }
}

TEST_CASE("AllDifferentConstraint on_final_instantiate", "[constraint][all_different]") {
    SECTION("satisfied") {
        Model model;
        auto x = model.create_variable("x", 1, 1);
        auto y = model.create_variable("y", 2, 2);
        auto z = model.create_variable("z", 3, 3);
        AllDifferentConstraint c({x, y, z});

        REQUIRE(c.on_final_instantiate(model) == true);
    }

    SECTION("violated") {
        Model model;
        auto x = model.create_variable("x", 1, 1);
        auto y = model.create_variable("y", 1, 1);
        AllDifferentConstraint c({x, y});

        REQUIRE(c.on_final_instantiate(model) == false);
    }
}

TEST_CASE("AllDifferentConstraint rewind_to", "[constraint][all_different][trail]") {
    Model model;
    auto x = model.create_variable("x", 1, 3);
    auto y = model.create_variable("y", 1, 3);
    auto z = model.create_variable("z", 1, 3);
    AllDifferentConstraint c({x, y, z});

    REQUIRE(c.pool_size() == 3);

    // Simulate instantiation at level 1
    model.instantiate(1, x->id(), 1);
    REQUIRE(c.on_instantiate(model, 1, 0, 0, 1, 1, 3));
    REQUIRE(c.pool_size() == 2);

    // Simulate instantiation at level 2
    model.instantiate(2, y->id(), 2);
    REQUIRE(c.on_instantiate(model, 2, 1, 0, 2, 1, 3));
    REQUIRE(c.pool_size() == 1);

    // Rewind to level 1
    c.rewind_to(1);
    REQUIRE(c.pool_size() == 2);

    // Rewind to level 0
    c.rewind_to(0);
    REQUIRE(c.pool_size() == 3);
}

// ============================================================================
// AllDifferentConstraint bounds(Z) (López-Ortiz et al.) tests
// ============================================================================

TEST_CASE("AllDifferentConstraint bounds(Z) Hall interval presolve", "[constraint][all_different][bounds_z]") {
    // x1,x2 ∈ [1,2] が Hall interval → x3.min は 3 に、さらに x4.min は 4 に
    Model model;
    auto* x1 = model.create_variable("x1", Domain(1, 2));
    auto* x2 = model.create_variable("x2", Domain(1, 2));
    auto* x3 = model.create_variable("x3", Domain(1, 3));
    auto* x4 = model.create_variable("x4", Domain(1, 4));
    AllDifferentConstraint c({x1, x2, x3, x4});

    REQUIRE(c.presolve(model) == PresolveResult::Changed);
    REQUIRE(x3->min() == 3);
    REQUIRE(x4->min() == 4);
}

TEST_CASE("AllDifferentConstraint bounds(Z) upper bound pruning", "[constraint][all_different][bounds_z]") {
    // x1,x2 ∈ [3,4] が Hall interval → x3.max は 2 に
    Model model;
    auto* x1 = model.create_variable("x1", Domain(3, 4));
    auto* x2 = model.create_variable("x2", Domain(3, 4));
    auto* x3 = model.create_variable("x3", Domain(2, 4));
    AllDifferentConstraint c({x1, x2, x3});

    REQUIRE(c.presolve(model) == PresolveResult::Changed);
    REQUIRE(x3->max() == 2);
}

TEST_CASE("AllDifferentConstraint bounds(Z) pigeonhole infeasibility", "[constraint][all_different][bounds_z]") {
    // 3変数が [1,2] → 鳩の巣で矛盾
    Model model;
    auto* x1 = model.create_variable("x1", Domain(1, 2));
    auto* x2 = model.create_variable("x2", Domain(1, 2));
    auto* x3 = model.create_variable("x3", Domain(1, 2));
    AllDifferentConstraint c({x1, x2, x3});

    REQUIRE(c.presolve(model) == PresolveResult::Contradiction);
}

TEST_CASE("AllDifferentConstraint bounds(Z) solve_all correctness", "[constraint][all_different][bounds_z]") {
    // Hall interval 越しの全解列挙が正しい解数を返すこと
    Model model;
    auto* x1 = model.create_variable("x1", Domain(1, 2));
    auto* x2 = model.create_variable("x2", Domain(1, 2));
    auto* x3 = model.create_variable("x3", Domain(1, 3));
    auto* x4 = model.create_variable("x4", Domain(1, 4));

    model.add_constraint(std::make_unique<AllDifferentConstraint>(
        std::vector<Variable*>{x1, x2, x3, x4}));

    Solver solver;
    auto solutions = std::vector<Solution>();
    solver.solve_all(model, [&](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    // x1,x2 は {1,2} の順列、x3=3, x4=4 の 2 解のみ
    REQUIRE(solutions.size() == 2);
    for (const auto& sol : solutions) {
        REQUIRE(sol.at("x3") == 3);
        REQUIRE(sol.at("x4") == 4);
    }
}

TEST_CASE("AllDifferentConstraint bounds(Z) randomized solution count", "[constraint][all_different][bounds_z]") {
    // ランダムな区間ドメインで全解数をブルートフォースと比較
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int64_t> lo_dist(1, 4);
    std::uniform_int_distribution<int64_t> len_dist(0, 3);

    for (int trial = 0; trial < 30; ++trial) {
        constexpr size_t kN = 5;
        int64_t lo[kN], hi[kN];
        for (size_t i = 0; i < kN; ++i) {
            lo[i] = lo_dist(rng);
            hi[i] = lo[i] + len_dist(rng);
        }

        // ブルートフォースで解数を数える
        size_t expected = 0;
        std::array<int64_t, kN> assign{};
        std::function<void(size_t)> enumerate = [&](size_t depth) {
            if (depth == kN) {
                ++expected;
                return;
            }
            for (int64_t v = lo[depth]; v <= hi[depth]; ++v) {
                bool dup = false;
                for (size_t k = 0; k < depth; ++k) {
                    if (assign[k] == v) { dup = true; break; }
                }
                if (!dup) {
                    assign[depth] = v;
                    enumerate(depth + 1);
                }
            }
        };
        enumerate(0);

        Model model;
        std::vector<Variable*> vars;
        for (size_t i = 0; i < kN; ++i) {
            vars.push_back(model.create_variable("x" + std::to_string(i),
                                                 Domain(lo[i], hi[i])));
        }
        model.add_constraint(std::make_unique<AllDifferentConstraint>(vars));

        Solver solver;
        size_t actual = 0;
        solver.solve_all(model, [&](const Solution&) {
            ++actual;
            return true;
        });

        INFO("trial " << trial);
        REQUIRE(actual == expected);
    }
}

// ============================================================================
// AllDifferentConstraint GAC (Régin's algorithm) tests
// ============================================================================

TEST_CASE("AllDifferentConstraint GAC Hall set detection", "[constraint][all_different][gac]") {
    // vars x1={1,2}, x2={1,2}, x3={1,2,3}, x4={1,2,3,4}
    // Hall set {x1,x2}→{1,2} should prune 1,2 from x3 and x4
    Model model;
    auto* x1 = model.create_variable("x1", Domain(std::vector<int64_t>{1, 2}));
    auto* x2 = model.create_variable("x2", Domain(std::vector<int64_t>{1, 2}));
    auto* x3 = model.create_variable("x3", Domain(std::vector<int64_t>{1, 2, 3}));
    auto* x4 = model.create_variable("x4", Domain(std::vector<int64_t>{1, 2, 3, 4}));

    model.add_constraint(std::make_unique<AllDifferentGACConstraint>(
        std::vector<Variable*>{x1, x2, x3, x4}));

    Solver solver;
    auto solutions = std::vector<Solution>();
    solver.solve_all(model, [&](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    // All solutions must have x3 ∈ {3,4} and x4 ∈ {3,4} (but only one of each)
    // Actually x3 can only be 3 (since D(x3)={1,2,3} and 1,2 pruned → {3})
    // x4 can be 3 or 4 but with x3=3, x4=4
    // Total solutions: x1∈{1,2}, x2∈{2,1} (2 permutations), x3=3, x4=4 → 2 solutions
    REQUIRE(solutions.size() == 2);
    for (const auto& sol : solutions) {
        REQUIRE(sol.at("x3") == 3);
        REQUIRE(sol.at("x4") == 4);
    }
}

TEST_CASE("AllDifferentConstraint GAC infeasibility detection", "[constraint][all_different][gac]") {
    // 4 vars all with domain {1,2,3} → pigeonhole: impossible
    Model model;
    auto* x1 = model.create_variable("x1", Domain(std::vector<int64_t>{1, 2, 3}));
    auto* x2 = model.create_variable("x2", Domain(std::vector<int64_t>{1, 2, 3}));
    auto* x3 = model.create_variable("x3", Domain(std::vector<int64_t>{1, 2, 3}));
    auto* x4 = model.create_variable("x4", Domain(std::vector<int64_t>{1, 2, 3}));

    model.add_constraint(std::make_unique<AllDifferentGACConstraint>(
        std::vector<Variable*>{x1, x2, x3, x4}));

    Solver solver;
    auto result = solver.solve(model);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("AllDifferentConstraint GAC correctness: 5 vars", "[constraint][all_different][gac]") {
    // 5 vars with domains that GAC can prune
    // x1={1,2}, x2={1,2}, x3={1,2,3}, x4={3,4}, x5={3,4,5}
    // Hall set {x1,x2}→{1,2} prunes 1,2 from x3 → x3={3}
    // Then Hall set {x3,x4} where x3={3}, x4={3,4} → x4 must be 4
    // x5={3,4,5}, 3 and 4 pruned → x5={5}
    Model model;
    auto* x1 = model.create_variable("x1", Domain(std::vector<int64_t>{1, 2}));
    auto* x2 = model.create_variable("x2", Domain(std::vector<int64_t>{1, 2}));
    auto* x3 = model.create_variable("x3", Domain(std::vector<int64_t>{1, 2, 3}));
    auto* x4 = model.create_variable("x4", Domain(std::vector<int64_t>{3, 4}));
    auto* x5 = model.create_variable("x5", Domain(std::vector<int64_t>{3, 4, 5}));

    model.add_constraint(std::make_unique<AllDifferentGACConstraint>(
        std::vector<Variable*>{x1, x2, x3, x4, x5}));

    Solver solver;
    auto solutions = std::vector<Solution>();
    solver.solve_all(model, [&](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    // x3=3, x4=4, x5=5, x1∈{1,2}, x2∈{2,1} → 2 solutions
    REQUIRE(solutions.size() == 2);
    for (const auto& sol : solutions) {
        REQUIRE(sol.at("x3") == 3);
        REQUIRE(sol.at("x4") == 4);
        REQUIRE(sol.at("x5") == 5);
    }
}

TEST_CASE("AllDifferentGACConstraint disabled for small constraints", "[constraint][all_different][gac]") {
    // 3 vars: GAC should be disabled (unfixed_count_ < 4), fall back to forward check + hall pair
    Model model;
    auto* x = model.create_variable("x", Domain(1, 3));
    auto* y = model.create_variable("y", Domain(1, 3));
    auto* z = model.create_variable("z", Domain(1, 3));

    model.add_constraint(std::make_unique<AllDifferentGACConstraint>(
        std::vector<Variable*>{x, y, z}));

    Solver solver;
    auto solutions = std::vector<Solution>();
    solver.solve_all(model, [&](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    // 3! = 6 solutions
    REQUIRE(solutions.size() == 6);
}

// ============================================================================
// AllDifferentGACConstraint: 全解列挙による正確性テスト
// ============================================================================

// ヘルパー: 全解がすべて all_different を満たしているか検証
static void verify_all_different_solutions(const std::vector<Solution>& solutions,
                                            const std::vector<std::string>& var_names) {
    for (size_t s = 0; s < solutions.size(); ++s) {
        std::set<int64_t> vals;
        for (const auto& name : var_names) {
            vals.insert(solutions[s].at(name));
        }
        REQUIRE(vals.size() == var_names.size());
    }
}

TEST_CASE("AllDifferentGAC: complete solution count for 4 vars 1..4",
          "[constraint][all_different][gac]") {
    // 4 vars each with domain {1,2,3,4} → 4! = 24 solutions
    Model model;
    auto* x1 = model.create_variable("x1", Domain(std::vector<int64_t>{1,2,3,4}));
    auto* x2 = model.create_variable("x2", Domain(std::vector<int64_t>{1,2,3,4}));
    auto* x3 = model.create_variable("x3", Domain(std::vector<int64_t>{1,2,3,4}));
    auto* x4 = model.create_variable("x4", Domain(std::vector<int64_t>{1,2,3,4}));

    model.add_constraint(std::make_unique<AllDifferentGACConstraint>(
        std::vector<Variable*>{x1, x2, x3, x4}));

    Solver solver;
    std::vector<Solution> solutions;
    solver.solve_all(model, [&](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    REQUIRE(solutions.size() == 24);
    verify_all_different_solutions(solutions, {"x1","x2","x3","x4"});
}

TEST_CASE("AllDifferentGAC: no false UNSAT with asymmetric domains",
          "[constraint][all_different][gac]") {
    // Asymmetric domains that have valid solutions but require careful SCC handling
    // x1={1,2}, x2={2,3}, x3={3,4}, x4={4,5}
    // All solutions exist (chain-like domains)
    Model model;
    auto* x1 = model.create_variable("x1", Domain(std::vector<int64_t>{1,2}));
    auto* x2 = model.create_variable("x2", Domain(std::vector<int64_t>{2,3}));
    auto* x3 = model.create_variable("x3", Domain(std::vector<int64_t>{3,4}));
    auto* x4 = model.create_variable("x4", Domain(std::vector<int64_t>{4,5}));

    model.add_constraint(std::make_unique<AllDifferentGACConstraint>(
        std::vector<Variable*>{x1, x2, x3, x4}));

    Solver solver;
    std::vector<Solution> solutions;
    solver.solve_all(model, [&](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    // Brute force verify
    int expected_asym = 0;
    std::vector<std::vector<int64_t>> dom_asym = {{1,2},{2,3},{3,4},{4,5}};
    for (auto a : dom_asym[0]) for (auto b : dom_asym[1])
      for (auto c : dom_asym[2]) for (auto d : dom_asym[3]) {
        std::set<int64_t> s{a,b,c,d};
        if (s.size() == 4) expected_asym++;
      }
    REQUIRE(solutions.size() == static_cast<size_t>(expected_asym));
    verify_all_different_solutions(solutions, {"x1","x2","x3","x4"});
}

TEST_CASE("AllDifferentGAC: no over-pruning with overlapping domains",
          "[constraint][all_different][gac]") {
    // All vars share many values — GAC must not over-prune
    // x1..x4 ∈ {1,2,3,4,5} → C(5,4)*4! = 5*24 = 120 solutions
    Model model;
    auto* x1 = model.create_variable("x1", Domain(std::vector<int64_t>{1,2,3,4,5}));
    auto* x2 = model.create_variable("x2", Domain(std::vector<int64_t>{1,2,3,4,5}));
    auto* x3 = model.create_variable("x3", Domain(std::vector<int64_t>{1,2,3,4,5}));
    auto* x4 = model.create_variable("x4", Domain(std::vector<int64_t>{1,2,3,4,5}));

    model.add_constraint(std::make_unique<AllDifferentGACConstraint>(
        std::vector<Variable*>{x1, x2, x3, x4}));

    Solver solver;
    std::vector<Solution> solutions;
    solver.solve_all(model, [&](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    REQUIRE(solutions.size() == 120);
    verify_all_different_solutions(solutions, {"x1","x2","x3","x4"});
}

TEST_CASE("AllDifferentGAC: nested Hall sets",
          "[constraint][all_different][gac]") {
    // x1={1,2}, x2={1,2}, x3={2,3}, x4={2,3}, x5={1,2,3,4,5}
    // Hall set {x1,x2}→{1,2} and {x3,x4}→{2,3} interact
    // After pruning: x5 must not have 1,2,3 → x5 ∈ {4,5}
    // But x3,x4 can use 2, so actually {1,2} consumed by x1,x2,
    // and x3,x4 must use {3, ?} — but x3,x4 ∈ {2,3} with 2 consumed → x3=x4=3 impossible
    // Actually: x1,x2 take {1,2}. x3,x4 ∈ {2,3} but 2 is taken → x3=x4=3 conflict →
    // Wait, {2,3} minus 2 pruned? Not exactly — let me think again.
    // Hall set {x1,x2}→{1,2}: prune 1,2 from x3,x4,x5
    // x3: {2,3}→{3}, x4: {2,3}→{3} → x3=x4=3 conflict → UNSAT
    Model model;
    auto* x1 = model.create_variable("x1", Domain(std::vector<int64_t>{1,2}));
    auto* x2 = model.create_variable("x2", Domain(std::vector<int64_t>{1,2}));
    auto* x3 = model.create_variable("x3", Domain(std::vector<int64_t>{2,3}));
    auto* x4 = model.create_variable("x4", Domain(std::vector<int64_t>{2,3}));
    auto* x5 = model.create_variable("x5", Domain(std::vector<int64_t>{1,2,3,4,5}));

    model.add_constraint(std::make_unique<AllDifferentGACConstraint>(
        std::vector<Variable*>{x1, x2, x3, x4, x5}));

    Solver solver;
    auto result = solver.solve(model);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("AllDifferentGAC: non-contiguous domains",
          "[constraint][all_different][gac]") {
    // Domains with gaps — tests val_to_vars mapping correctness
    // x1={1,10}, x2={10,20}, x3={20,30}, x4={1,30}
    Model model;
    auto* x1 = model.create_variable("x1", Domain(std::vector<int64_t>{1,10}));
    auto* x2 = model.create_variable("x2", Domain(std::vector<int64_t>{10,20}));
    auto* x3 = model.create_variable("x3", Domain(std::vector<int64_t>{20,30}));
    auto* x4 = model.create_variable("x4", Domain(std::vector<int64_t>{1,30}));

    model.add_constraint(std::make_unique<AllDifferentGACConstraint>(
        std::vector<Variable*>{x1, x2, x3, x4}));

    Solver solver;
    std::vector<Solution> solutions;
    solver.solve_all(model, [&](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    // Brute force verify
    int expected_noncont = 0;
    std::vector<std::vector<int64_t>> dom_nc = {{1,10},{10,20},{20,30},{1,30}};
    for (auto a : dom_nc[0]) for (auto b : dom_nc[1]) for (auto c : dom_nc[2]) for (auto d : dom_nc[3]) {
        std::set<int64_t> s{a,b,c,d};
        if (s.size() == 4) expected_noncont++;
    }
    REQUIRE(solutions.size() == static_cast<size_t>(expected_noncont));
    verify_all_different_solutions(solutions, {"x1","x2","x3","x4"});
}

TEST_CASE("AllDifferentGAC: single solution forced by domains",
          "[constraint][all_different][gac]") {
    // Each variable forced to a unique value
    // x1={1}, x2={2}, x3={3}, x4={4} — but these are singletons (instantiated)
    // Use slightly larger domains where GAC can reduce to 1 solution
    // x1={1,2}, x2={2,3}, x3={3,4}, x4={4,1}
    // Circular: solutions are (1,2,3,4) and (2,3,4,1) → 2 solutions
    Model model;
    auto* x1 = model.create_variable("x1", Domain(std::vector<int64_t>{1,2}));
    auto* x2 = model.create_variable("x2", Domain(std::vector<int64_t>{2,3}));
    auto* x3 = model.create_variable("x3", Domain(std::vector<int64_t>{3,4}));
    auto* x4 = model.create_variable("x4", Domain(std::vector<int64_t>{4,1}));

    model.add_constraint(std::make_unique<AllDifferentGACConstraint>(
        std::vector<Variable*>{x1, x2, x3, x4}));

    Solver solver;
    std::vector<Solution> solutions;
    solver.solve_all(model, [&](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    REQUIRE(solutions.size() == 2);
    verify_all_different_solutions(solutions, {"x1","x2","x3","x4"});
}

TEST_CASE("AllDifferentGAC: mixed fixed and unfixed variables",
          "[constraint][all_different][gac]") {
    // Some vars already fixed, others need GAC filtering
    // x1=1 (fixed), x2={1,2,3,4,5}, x3={1,2,3,4,5}, x4={1,2,3,4,5}, x5={1,2,3,4,5}
    // GAC should prune 1 from x2..x5, remaining: each has {2,3,4,5}
    // Solutions: 4! = 24
    Model model;
    auto* x1 = model.create_variable("x1", Domain(std::vector<int64_t>{1}));
    auto* x2 = model.create_variable("x2", Domain(std::vector<int64_t>{1,2,3,4,5}));
    auto* x3 = model.create_variable("x3", Domain(std::vector<int64_t>{1,2,3,4,5}));
    auto* x4 = model.create_variable("x4", Domain(std::vector<int64_t>{1,2,3,4,5}));
    auto* x5 = model.create_variable("x5", Domain(std::vector<int64_t>{1,2,3,4,5}));

    model.add_constraint(std::make_unique<AllDifferentGACConstraint>(
        std::vector<Variable*>{x1, x2, x3, x4, x5}));

    Solver solver;
    std::vector<Solution> solutions;
    solver.solve_all(model, [&](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    REQUIRE(solutions.size() == 24);
    verify_all_different_solutions(solutions, {"x1","x2","x3","x4","x5"});
}

TEST_CASE("AllDifferentGAC: pigeonhole UNSAT with 5 vars 4 values",
          "[constraint][all_different][gac]") {
    // 5 vars, domain {1..4} → impossible (pigeonhole)
    Model model;
    auto* x1 = model.create_variable("x1", Domain(std::vector<int64_t>{1,2,3,4}));
    auto* x2 = model.create_variable("x2", Domain(std::vector<int64_t>{1,2,3,4}));
    auto* x3 = model.create_variable("x3", Domain(std::vector<int64_t>{1,2,3,4}));
    auto* x4 = model.create_variable("x4", Domain(std::vector<int64_t>{1,2,3,4}));
    auto* x5 = model.create_variable("x5", Domain(std::vector<int64_t>{1,2,3,4}));

    model.add_constraint(std::make_unique<AllDifferentGACConstraint>(
        std::vector<Variable*>{x1, x2, x3, x4, x5}));

    Solver solver;
    auto result = solver.solve(model);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("AllDifferentGAC: partial pigeonhole detected by GAC",
          "[constraint][all_different][gac]") {
    // x1={1,2}, x2={1,2}, x3={1,2}, x4={3,4,5,6}
    // Hall set {x1,x2,x3}→{1,2}: only 2 values for 3 vars → UNSAT
    // GAC should detect this without backtracking
    Model model;
    auto* x1 = model.create_variable("x1", Domain(std::vector<int64_t>{1,2}));
    auto* x2 = model.create_variable("x2", Domain(std::vector<int64_t>{1,2}));
    auto* x3 = model.create_variable("x3", Domain(std::vector<int64_t>{1,2}));
    auto* x4 = model.create_variable("x4", Domain(std::vector<int64_t>{3,4,5,6}));

    model.add_constraint(std::make_unique<AllDifferentGACConstraint>(
        std::vector<Variable*>{x1, x2, x3, x4}));

    Solver solver;
    auto result = solver.solve(model);
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("AllDifferentGAC: 6 vars stress test solution count",
          "[constraint][all_different][gac]") {
    // 6 vars with various domain sizes
    // x1={1,2,3}, x2={2,3,4}, x3={3,4,5}, x4={4,5,6}, x5={1,5,6}, x6={1,2,6}
    Model model;
    auto* x1 = model.create_variable("x1", Domain(std::vector<int64_t>{1,2,3}));
    auto* x2 = model.create_variable("x2", Domain(std::vector<int64_t>{2,3,4}));
    auto* x3 = model.create_variable("x3", Domain(std::vector<int64_t>{3,4,5}));
    auto* x4 = model.create_variable("x4", Domain(std::vector<int64_t>{4,5,6}));
    auto* x5 = model.create_variable("x5", Domain(std::vector<int64_t>{1,5,6}));
    auto* x6 = model.create_variable("x6", Domain(std::vector<int64_t>{1,2,6}));

    model.add_constraint(std::make_unique<AllDifferentGACConstraint>(
        std::vector<Variable*>{x1, x2, x3, x4, x5, x6}));

    Solver solver;
    std::vector<Solution> solutions;
    solver.solve_all(model, [&](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    // Verify all solutions are valid (all different)
    REQUIRE(solutions.size() > 0);
    verify_all_different_solutions(solutions, {"x1","x2","x3","x4","x5","x6"});

    // Brute force count: enumerate all valid assignments
    int expected = 0;
    std::vector<std::vector<int64_t>> domains = {
        {1,2,3}, {2,3,4}, {3,4,5}, {4,5,6}, {1,5,6}, {1,2,6}
    };
    for (auto a : domains[0])
      for (auto b : domains[1])
        for (auto c : domains[2])
          for (auto d : domains[3])
            for (auto e : domains[4])
              for (auto f : domains[5]) {
                std::set<int64_t> s{a,b,c,d,e,f};
                if (s.size() == 6) expected++;
              }
    REQUIRE(solutions.size() == static_cast<size_t>(expected));
}

TEST_CASE("AllDifferentGAC: negative values",
          "[constraint][all_different][gac]") {
    // Test with negative domain values
    Model model;
    auto* x1 = model.create_variable("x1", Domain(std::vector<int64_t>{-2,-1}));
    auto* x2 = model.create_variable("x2", Domain(std::vector<int64_t>{-1,0}));
    auto* x3 = model.create_variable("x3", Domain(std::vector<int64_t>{0,1}));
    auto* x4 = model.create_variable("x4", Domain(std::vector<int64_t>{1,2}));

    model.add_constraint(std::make_unique<AllDifferentGACConstraint>(
        std::vector<Variable*>{x1, x2, x3, x4}));

    Solver solver;
    std::vector<Solution> solutions;
    solver.solve_all(model, [&](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    // Brute force verify
    int expected_neg = 0;
    std::vector<std::vector<int64_t>> dom_neg = {{-2,-1},{-1,0},{0,1},{1,2}};
    for (auto a : dom_neg[0]) for (auto b : dom_neg[1])
      for (auto c : dom_neg[2]) for (auto d : dom_neg[3]) {
        std::set<int64_t> s{a,b,c,d};
        if (s.size() == 4) expected_neg++;
      }
    REQUIRE(solutions.size() == static_cast<size_t>(expected_neg));
    verify_all_different_solutions(solutions, {"x1","x2","x3","x4"});
}

TEST_CASE("AllDifferentGAC: multiple constraints interaction",
          "[constraint][all_different][gac]") {
    // Two overlapping all_different constraints
    // alldiff(x1,x2,x3,x4), alldiff(x3,x4,x5,x6)
    // x1..x6 ∈ {1,2,3,4}
    Model model;
    auto* x1 = model.create_variable("x1", Domain(std::vector<int64_t>{1,2,3,4}));
    auto* x2 = model.create_variable("x2", Domain(std::vector<int64_t>{1,2,3,4}));
    auto* x3 = model.create_variable("x3", Domain(std::vector<int64_t>{1,2,3,4}));
    auto* x4 = model.create_variable("x4", Domain(std::vector<int64_t>{1,2,3,4}));
    auto* x5 = model.create_variable("x5", Domain(std::vector<int64_t>{1,2,3,4}));
    auto* x6 = model.create_variable("x6", Domain(std::vector<int64_t>{1,2,3,4}));

    model.add_constraint(std::make_unique<AllDifferentGACConstraint>(
        std::vector<Variable*>{x1, x2, x3, x4}));
    model.add_constraint(std::make_unique<AllDifferentGACConstraint>(
        std::vector<Variable*>{x3, x4, x5, x6}));

    Solver solver;
    std::vector<Solution> solutions;
    solver.solve_all(model, [&](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    // All solutions must satisfy both constraints
    for (const auto& sol : solutions) {
        std::set<int64_t> s1{sol.at("x1"), sol.at("x2"), sol.at("x3"), sol.at("x4")};
        std::set<int64_t> s2{sol.at("x3"), sol.at("x4"), sol.at("x5"), sol.at("x6")};
        REQUIRE(s1.size() == 4);
        REQUIRE(s2.size() == 4);
    }

    // Brute force count
    int expected = 0;
    for (int a=1;a<=4;a++) for (int b=1;b<=4;b++) for (int c=1;c<=4;c++)
      for (int d=1;d<=4;d++) for (int e=1;e<=4;e++) for (int f=1;f<=4;f++) {
        std::set<int> s1{a,b,c,d}, s2{c,d,e,f};
        if (s1.size()==4 && s2.size()==4) expected++;
      }
    REQUIRE(solutions.size() == static_cast<size_t>(expected));
}

TEST_CASE("AllDifferentGAC: exactly at threshold (4 vars)",
          "[constraint][all_different][gac]") {
    // GAC activates at unfixed_count_ >= 4, test boundary
    // x1={1,2,3,4}, x2={1,2,3,4}, x3={1,2}, x4={1,2}
    // Hall set {x3,x4}→{1,2}: prune 1,2 from x1,x2 → x1,x2 ∈ {3,4}
    // Total: x1∈{3,4}, x2∈{3,4}\{x1}, x3∈{1,2}, x4∈{1,2}\{x3}
    // = 2*1*2*1 = 4 solutions
    Model model;
    auto* x1 = model.create_variable("x1", Domain(std::vector<int64_t>{1,2,3,4}));
    auto* x2 = model.create_variable("x2", Domain(std::vector<int64_t>{1,2,3,4}));
    auto* x3 = model.create_variable("x3", Domain(std::vector<int64_t>{1,2}));
    auto* x4 = model.create_variable("x4", Domain(std::vector<int64_t>{1,2}));

    model.add_constraint(std::make_unique<AllDifferentGACConstraint>(
        std::vector<Variable*>{x1, x2, x3, x4}));

    Solver solver;
    std::vector<Solution> solutions;
    solver.solve_all(model, [&](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    REQUIRE(solutions.size() == 4);
    verify_all_different_solutions(solutions, {"x1","x2","x3","x4"});

    // Verify Hall set pruning worked: x1 and x2 must be in {3,4}
    for (const auto& sol : solutions) {
        REQUIRE(sol.at("x1") >= 3);
        REQUIRE(sol.at("x2") >= 3);
    }
}

TEST_CASE("AllDifferentGAC: free values (more values than vars)",
          "[constraint][all_different][gac]") {
    // Tests BFS from free value nodes
    // x1={1,2}, x2={1,2}, x3={1,2,3}, x4={1,2,3}
    // Values 1,2 form Hall set for x1,x2 → prune from x3,x4
    // x3={3}, x4={3} → conflict → UNSAT? No wait...
    // x1,x2 each take one of {1,2}, x3=3, x4=3 → x3=x4=3 conflict → UNSAT
    // Let me use: x1={1,2}, x2={1,2}, x3={1,2,3,4}, x4={1,2,3,4}
    // Hall {x1,x2}→{1,2}: x3,x4 ∈ {3,4}
    // Solutions: 2*1*2*1 = 4
    Model model;
    auto* x1 = model.create_variable("x1", Domain(std::vector<int64_t>{1,2}));
    auto* x2 = model.create_variable("x2", Domain(std::vector<int64_t>{1,2}));
    auto* x3 = model.create_variable("x3", Domain(std::vector<int64_t>{1,2,3,4}));
    auto* x4 = model.create_variable("x4", Domain(std::vector<int64_t>{1,2,3,4}));

    model.add_constraint(std::make_unique<AllDifferentGACConstraint>(
        std::vector<Variable*>{x1, x2, x3, x4}));

    Solver solver;
    std::vector<Solution> solutions;
    solver.solve_all(model, [&](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    REQUIRE(solutions.size() == 4);
    verify_all_different_solutions(solutions, {"x1","x2","x3","x4"});
}

TEST_CASE("AllDifferentGAC: large sparse domains",
          "[constraint][all_different][gac]") {
    // Sparse domains with large value gaps
    // x1={100,200}, x2={200,300}, x3={300,400}, x4={400,100}
    // Same structure as circular test but with large values
    Model model;
    auto* x1 = model.create_variable("x1", Domain(std::vector<int64_t>{100,200}));
    auto* x2 = model.create_variable("x2", Domain(std::vector<int64_t>{200,300}));
    auto* x3 = model.create_variable("x3", Domain(std::vector<int64_t>{300,400}));
    auto* x4 = model.create_variable("x4", Domain(std::vector<int64_t>{400,100}));

    model.add_constraint(std::make_unique<AllDifferentGACConstraint>(
        std::vector<Variable*>{x1, x2, x3, x4}));

    Solver solver;
    std::vector<Solution> solutions;
    solver.solve_all(model, [&](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    // (100,200,300,400) and (200,300,400,100)
    REQUIRE(solutions.size() == 2);
    verify_all_different_solutions(solutions, {"x1","x2","x3","x4"});
}
