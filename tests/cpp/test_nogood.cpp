#include <catch2/catch_test_macros.hpp>
#include "sabori_csp/constraints/comparison.hpp"
#include "sabori_csp/constraints/arithmetic.hpp"
#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/variable.hpp"
#include "sabori_csp/model.hpp"
#include "sabori_csp/solver.hpp"
#include <array>
#include <functional>
#include <random>

using namespace sabori_csp;

// ============================================================================
// Bound nogood propagation via RemoveValue
//
// These tests verify that propagate_bound_nogoods is called when a
// RemoveValue operation changes a variable's min or max. IntNe constraints
// use enqueue_remove_value, and with large domains (bisect mode) the solver
// learns bound nogoods (Leq/Geq). When IntNe removes a boundary value,
// the resulting bound change should trigger these learned nogoods.
// ============================================================================

TEST_CASE("Bound nogood via RemoveValue: correctness with IntNe + bisect", "[nogood][solver]") {
    // Large domains trigger bisect mode (Leq/Geq decision literals).
    // IntNe propagation uses enqueue_remove_value, which may change bounds.
    //
    // x in {1..20}, y in {1..20}, z in {1..20}
    // x != y, y != z, x + z <= 10
    //
    // Verify solution count matches with/without nogood learning.

    auto run_test = [](bool nogood_learning) -> size_t {
        Model model;
        auto x = model.create_variable("x", 1, 20);
        auto y = model.create_variable("y", 1, 20);
        auto z = model.create_variable("z", 1, 20);
        model.add_constraint(std::make_unique<IntNeConstraint>(x, y));
        model.add_constraint(std::make_unique<IntNeConstraint>(y, z));
        model.add_constraint(std::make_unique<IntLinLeConstraint>(
            std::vector<int64_t>{1, 1}, std::vector<VariablePtr>{x, z}, 10));

        Solver solver;
        solver.set_nogood_learning(nogood_learning);
        solver.set_restart_enabled(false);
        size_t count = 0;
        solver.solve_all(model, [&count](const Solution&) {
            count++;
            return true;
        });
        return count;
    };

    size_t count_on = run_test(true);
    size_t count_off = run_test(false);

    REQUIRE(count_on == count_off);
    // Sanity check: at least some solutions exist
    REQUIRE(count_on > 0);
}

TEST_CASE("Bound nogood via RemoveValue: AllDifferent + bisect", "[nogood][solver]") {
    // AllDifferent uses remove_value extensively, often removing
    // boundary values and triggering bound changes.
    //
    // x1..x4 in {1..15}, all_different(x1..x4), x1 + x4 <= 8
    //
    // Verify correctness with nogood learning through the RemoveValue path.

    auto run_test = [](bool nogood_learning) -> size_t {
        Model model;
        auto x1 = model.create_variable("x1", 1, 15);
        auto x2 = model.create_variable("x2", 1, 15);
        auto x3 = model.create_variable("x3", 1, 15);
        auto x4 = model.create_variable("x4", 1, 15);
        model.add_constraint(std::make_unique<AllDifferentConstraint>(
            std::vector<VariablePtr>{x1, x2, x3, x4}));
        model.add_constraint(std::make_unique<IntLinLeConstraint>(
            std::vector<int64_t>{1, 1}, std::vector<VariablePtr>{x1, x4}, 8));

        Solver solver;
        solver.set_nogood_learning(nogood_learning);
        solver.set_restart_enabled(false);
        size_t count = 0;
        solver.solve_all(model, [&count](const Solution&) {
            count++;
            return true;
        });
        return count;
    };

    size_t count_on = run_test(true);
    size_t count_off = run_test(false);

    REQUIRE(count_on == count_off);
    REQUIRE(count_on > 0);
}

TEST_CASE("Bound nogood via RemoveValue: optimization with IntNe + bisect", "[nogood][solver]") {
    // Optimization with large domains: bound nogoods learned from earlier
    // solutions should fire when IntNe's remove_value changes bounds.
    //
    // x in {1..30}, y in {1..30}
    // x != y, minimize (x + y)
    // Optimal: x=1 y=2 (or x=2 y=1), sum=3

    auto run_test = [](bool nogood_learning) -> int64_t {
        Model model;
        auto x = model.create_variable("x", 1, 30);
        auto y = model.create_variable("y", 1, 30);
        auto obj = model.create_variable("obj", 2, 60);
        model.add_constraint(std::make_unique<IntNeConstraint>(x, y));
        model.add_constraint(std::make_unique<IntLinEqConstraint>(
            std::vector<int64_t>{1, 1, -1},
            std::vector<VariablePtr>{x, y, obj}, 0));

        Solver solver;
        solver.set_nogood_learning(nogood_learning);
        solver.set_restart_enabled(false);
        auto result = solver.solve_optimize(model, obj->id(), true);
        REQUIRE(result.has_value());
        return result->at("obj");
    };

    auto val_on = run_test(true);
    auto val_off = run_test(false);
    REQUIRE(val_on == val_off);
    REQUIRE(val_on == 3);
}

// ============================================================================
// Conflict learning L0 (docs-dev/conflict-learning-design.md)
// ============================================================================

TEST_CASE("Learning L0: bounds_at reconstructs bounds at inference time", "[learning]") {
    Model model;
    auto* x = model.create_variable("x", 0, 10);
    model.snapshot_presolve_bounds();

    Solver solver;
    // 推論トレイルを手で構築: T0: x>=3, T1: x<=8, T2: x>=5
    solver.inference_trail_.push_back({3, static_cast<uint32_t>(x->id()), 0, 
                                       static_cast<uint32_t>(Literal::Type::Geq)});
    solver.inference_trail_.push_back({8, static_cast<uint32_t>(x->id()), 0,
                                       static_cast<uint32_t>(Literal::Type::Leq)});
    solver.inference_trail_.push_back({5, static_cast<uint32_t>(x->id()), 0,
                                       static_cast<uint32_t>(Literal::Type::Geq)});

    // T=0 時点 (エントリ適用前): presolve bounds
    auto b0 = solver.bounds_at(model, static_cast<uint32_t>(x->id()), 0);
    REQUIRE(b0.first == 0);
    REQUIRE(b0.second == 10);
    // T=1: x>=3 のみ適用済み
    auto b1 = solver.bounds_at(model, static_cast<uint32_t>(x->id()), 1);
    REQUIRE(b1.first == 3);
    REQUIRE(b1.second == 10);
    // T=2: x>=3, x<=8
    auto b2 = solver.bounds_at(model, static_cast<uint32_t>(x->id()), 2);
    REQUIRE(b2.first == 3);
    REQUIRE(b2.second == 8);
    // T=3 (全適用後): x>=5, x<=8
    auto b3 = solver.bounds_at(model, static_cast<uint32_t>(x->id()), 3);
    REQUIRE(b3.first == 5);
    REQUIRE(b3.second == 8);
}

TEST_CASE("Learning L0: solution count identical with learning on", "[learning]") {
    // ランダム区間 alldifferent で learning on の全解数がブルートフォースと一致
    // (L0 では off とビット同等のはずだが、false UNSAT ガードとして独立に検証)
    std::mt19937 rng(777);
    std::uniform_int_distribution<int64_t> lo_dist(1, 4);
    std::uniform_int_distribution<int64_t> len_dist(0, 3);

    for (int trial = 0; trial < 15; ++trial) {
        constexpr size_t kN = 5;
        int64_t lo[kN], hi[kN];
        for (size_t i = 0; i < kN; ++i) {
            lo[i] = lo_dist(rng);
            hi[i] = lo[i] + len_dist(rng);
        }
        size_t expected = 0;
        std::array<int64_t, kN> assign{};
        std::function<void(size_t)> rec = [&](size_t depth) {
            if (depth == kN) { ++expected; return; }
            for (int64_t v = lo[depth]; v <= hi[depth]; ++v) {
                bool dup = false;
                for (size_t k = 0; k < depth; ++k) if (assign[k] == v) { dup = true; break; }
                if (!dup) { assign[depth] = v; rec(depth + 1); }
            }
        };
        rec(0);

        Model model;
        std::vector<Variable*> vars;
        for (size_t i = 0; i < kN; ++i) {
            vars.push_back(model.create_variable("x" + std::to_string(i), Domain(lo[i], hi[i])));
        }
        model.add_constraint(std::make_unique<AllDifferentConstraint>(vars));

        Solver solver;
        solver.set_learning(true);
        size_t actual = 0;
        solver.solve_all(model, [&](const Solution&) { ++actual; return true; });
        INFO("trial " << trial);
        REQUIRE(actual == expected);
    }
}
