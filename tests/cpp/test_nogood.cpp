#include <catch2/catch_test_macros.hpp>
#include "sabori_csp/constraints/comparison.hpp"
#include "sabori_csp/constraints/arithmetic.hpp"
#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/variable.hpp"
#include "sabori_csp/model.hpp"
#include "sabori_csp/solver.hpp"

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
        model.add_constraint(std::make_shared<IntNeConstraint>(x, y));
        model.add_constraint(std::make_shared<IntNeConstraint>(y, z));
        model.add_constraint(std::make_shared<IntLinLeConstraint>(
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
        model.add_constraint(std::make_shared<AllDifferentConstraint>(
            std::vector<VariablePtr>{x1, x2, x3, x4}));
        model.add_constraint(std::make_shared<IntLinLeConstraint>(
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
        model.add_constraint(std::make_shared<IntNeConstraint>(x, y));
        model.add_constraint(std::make_shared<IntLinEqConstraint>(
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
