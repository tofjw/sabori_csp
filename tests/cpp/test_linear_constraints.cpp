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
// IntLinEqConstraint tests
// ============================================================================

TEST_CASE("IntLinEqConstraint name", "[constraint][int_lin_eq]") {
    Model model;
    auto* x = model.create_variable("x", Domain(1, 3));
    auto* y = model.create_variable("y", Domain(1, 3));
    IntLinEqConstraint c({1, 1}, {x, y}, 5);

    REQUIRE(c.name() == "int_lin_eq");
}

TEST_CASE("IntLinEqConstraint is_satisfied", "[constraint][int_lin_eq]") {
    SECTION("satisfied: 1*2 + 1*3 = 5") {
        Model model;
        auto* x = model.create_variable("x", Domain(2, 2));
        auto* y = model.create_variable("y", Domain(3, 3));
        IntLinEqConstraint c({1, 1}, {x, y}, 5);

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == true);
    }

    SECTION("satisfied: 2*1 + 3*2 = 8") {
        Model model;
        auto* x = model.create_variable("x", Domain(1, 1));
        auto* y = model.create_variable("y", Domain(2, 2));
        IntLinEqConstraint c({2, 3}, {x, y}, 8);

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == true);
    }

    SECTION("violated: 1*2 + 1*3 != 6") {
        Model model;
        auto* x = model.create_variable("x", Domain(2, 2));
        auto* y = model.create_variable("y", Domain(3, 3));
        IntLinEqConstraint c({1, 1}, {x, y}, 6);

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == false);
    }

    SECTION("not fully assigned") {
        Model model;
        auto* x = model.create_variable("x", Domain(1, 3));
        auto* y = model.create_variable("y", Domain(2, 2));
        IntLinEqConstraint c({1, 1}, {x, y}, 5);

        REQUIRE_FALSE(c.is_satisfied(model).has_value());
    }
}

TEST_CASE("IntLinEqConstraint on_final_instantiate", "[constraint][int_lin_eq]") {
    SECTION("satisfied") {
        Model model;
        auto x = model.create_variable("x", 2, 2);
        auto y = model.create_variable("y", 3, 3);
        IntLinEqConstraint c({1, 1}, {x, y}, 5);

        REQUIRE(c.on_final_instantiate(model) == true);
    }

    SECTION("violated") {
        Model model;
        auto x = model.create_variable("x", 2, 2);
        auto y = model.create_variable("y", 3, 3);
        IntLinEqConstraint c({1, 1}, {x, y}, 6);

        REQUIRE(c.on_final_instantiate(model) == false);
    }
}

TEST_CASE("IntLinEqConstraint with negative coefficients", "[constraint][int_lin_eq]") {
    SECTION("satisfied: 2*3 + (-1)*1 = 5") {
        Model model;
        auto* x = model.create_variable("x", Domain(3, 3));
        auto* y = model.create_variable("y", Domain(1, 1));
        IntLinEqConstraint c({2, -1}, {x, y}, 5);

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == true);
    }

    SECTION("satisfied: (-1)*2 + 1*7 = 5") {
        Model model;
        auto* x = model.create_variable("x", Domain(2, 2));
        auto* y = model.create_variable("y", Domain(7, 7));
        IntLinEqConstraint c({-1, 1}, {x, y}, 5);

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == true);
    }
}

TEST_CASE("IntLinEqConstraint rewind_to", "[constraint][int_lin_eq][trail]") {
    Model model;
    auto x = model.create_variable("x", 1, 5);
    auto y = model.create_variable("y", 1, 5);
    IntLinEqConstraint c({1, 1}, {x, y}, 6);
    c.prepare_propagation(model);

    // Initial state
    int64_t initial_sum = c.target_sum();
    REQUIRE(initial_sum == 6);

    // Simulate instantiation at level 1
    model.instantiate(1, x->id(), 2);
    c.on_instantiate(model, 1, 0, 0, 2, 1, 5);

    // Simulate instantiation at level 2
    model.instantiate(2, y->id(), 4);
    c.on_instantiate(model, 2, 1, 0, 4, 1, 5);

    // Verify final state
    REQUIRE(c.on_final_instantiate(model) == true);  // 2 + 4 = 6

    // Rewind to level 0 and try different values
    c.rewind_to(0);
    // State should be restored
}

// ============================================================================
// IntLinEqConstraint ±1 coefficient specialization tests
// ============================================================================

TEST_CASE("IntLinEqConstraint unit coeffs: all +1, solve_all", "[constraint][int_lin_eq][unit_coeffs]") {
    // x + y + z = 6, x,y,z in [1,3]
    // 7 solutions: permutations of (1,2,3) = 6, plus (2,2,2) = 1
    Model model;
    auto x = model.create_variable("x", 1, 3);
    auto y = model.create_variable("y", 1, 3);
    auto z = model.create_variable("z", 1, 3);
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1}, std::vector<Variable*>{x, y, z}, 6));

    Solver solver;
    std::vector<Solution> solutions;
    size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    REQUIRE(count == 7);
    for (const auto& sol : solutions) {
        REQUIRE(sol.at("x") + sol.at("y") + sol.at("z") == 6);
    }
}

TEST_CASE("IntLinEqConstraint unit coeffs: mixed +1/-1, solve_all", "[constraint][int_lin_eq][unit_coeffs]") {
    // x - y + z = 3, x,y,z in [1,4]
    // Enumerate all valid solutions by brute force and compare
    Model model;
    auto x = model.create_variable("x", 1, 4);
    auto y = model.create_variable("y", 1, 4);
    auto z = model.create_variable("z", 1, 4);
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{1, -1, 1}, std::vector<Variable*>{x, y, z}, 3));

    Solver solver;
    std::vector<Solution> solutions;
    size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    // Count expected solutions: x - y + z = 3, x,y,z in [1,4]
    size_t expected = 0;
    for (int xi = 1; xi <= 4; ++xi)
        for (int yi = 1; yi <= 4; ++yi)
            for (int zi = 1; zi <= 4; ++zi)
                if (xi - yi + zi == 3) ++expected;

    REQUIRE(count == expected);
    for (const auto& sol : solutions) {
        REQUIRE(sol.at("x") - sol.at("y") + sol.at("z") == 3);
    }
}

TEST_CASE("IntLinEqConstraint unit coeffs: all -1", "[constraint][int_lin_eq][unit_coeffs]") {
    // -x - y - z = -6, x,y,z in [1,3]  (same as x+y+z=6, 7 solutions)
    Model model;
    auto x = model.create_variable("x", 1, 3);
    auto y = model.create_variable("y", 1, 3);
    auto z = model.create_variable("z", 1, 3);
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{-1, -1, -1}, std::vector<Variable*>{x, y, z}, -6));

    Solver solver;
    std::vector<Solution> solutions;
    size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    REQUIRE(count == 7);
    for (const auto& sol : solutions) {
        REQUIRE(sol.at("x") + sol.at("y") + sol.at("z") == 6);
    }
}

TEST_CASE("IntLinEqConstraint unit coeffs: UNSAT detection", "[constraint][int_lin_eq][unit_coeffs]") {
    // x + y + z = 100, x,y,z in [1,3] => max=9 < 100 => UNSAT
    Model model;
    auto x = model.create_variable("x", 1, 3);
    auto y = model.create_variable("y", 1, 3);
    auto z = model.create_variable("z", 1, 3);
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1}, std::vector<Variable*>{x, y, z}, 100));

    Solver solver;
    size_t count = solver.solve_all(model, [](const Solution&) { return true; });
    REQUIRE(count == 0);
}

TEST_CASE("IntLinEqConstraint unit coeffs: many variables", "[constraint][int_lin_eq][unit_coeffs]") {
    // x1+x2+x3+x4+x5 = 10, xi in [1,4]
    // Verify solution count matches brute force
    Model model;
    std::vector<Variable*> vars;
    for (int i = 0; i < 5; ++i) {
        vars.push_back(model.create_variable("x" + std::to_string(i), 1, 4));
    }
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1, 1, 1}, vars, 10));

    Solver solver;
    std::vector<Solution> solutions;
    size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    // Brute force count
    size_t expected = 0;
    for (int a=1;a<=4;++a) for(int b=1;b<=4;++b) for(int c=1;c<=4;++c)
        for(int d=1;d<=4;++d) for(int e=1;e<=4;++e)
            if(a+b+c+d+e==10) ++expected;

    REQUIRE(count == expected);
    for (const auto& sol : solutions) {
        int sum = 0;
        for (int i = 0; i < 5; ++i) sum += sol.at("x" + std::to_string(i));
        REQUIRE(sum == 10);
    }
}

TEST_CASE("IntLinEqConstraint unit coeffs: bounds propagation tightens domains", "[constraint][int_lin_eq][unit_coeffs]") {
    // x + y + z = 15, x in [1,10], y in [1,10], z in [1,10]
    // Presolve should tighten: min of each = 15 - 10 - 10 = -5 -> clamped to 1
    //                          max of each = 15 - 1 - 1 = 13 -> clamped to 10
    // With 3 vars: each min >= 15 - 10 - 10 = -5 (no change), max <= 15 - 1 - 1 = 13 (no change)
    // Let's use tighter domains: x in [1,5], y in [1,5], z in [1,5], sum=12
    // each min >= 12 - 5 - 5 = 2, max <= 12 - 1 - 1 = 10 -> clamped to 5
    Model model;
    auto x = model.create_variable("x", 1, 5);
    auto y = model.create_variable("y", 1, 5);
    auto z = model.create_variable("z", 1, 5);
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1}, std::vector<Variable*>{x, y, z}, 12));

    // Presolve should tighten lower bounds to 2
    auto result = model.constraints()[0]->presolve(model);
    REQUIRE(result == PresolveResult::Changed);
    REQUIRE(x->min() >= 2);
    REQUIRE(y->min() >= 2);
    REQUIRE(z->min() >= 2);

    // All solutions should sum to 12
    Solver solver;
    std::vector<Solution> solutions;
    solver.solve_all(model, [&solutions](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    for (const auto& sol : solutions) {
        REQUIRE(sol.at("x") + sol.at("y") + sol.at("z") == 12);
    }
}

TEST_CASE("IntLinEqConstraint unit coeffs: negative coeffs bounds propagation", "[constraint][int_lin_eq][unit_coeffs]") {
    // x - y = 0 with x in [1,5], y in [1,5] => x == y
    // Should have 5 solutions: (1,1),(2,2),(3,3),(4,4),(5,5)
    Model model;
    auto x = model.create_variable("x", 1, 5);
    auto y = model.create_variable("y", 1, 5);
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{1, -1}, std::vector<Variable*>{x, y}, 0));

    Solver solver;
    std::vector<Solution> solutions;
    size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    REQUIRE(count == 5);
    for (const auto& sol : solutions) {
        REQUIRE(sol.at("x") == sol.at("y"));
    }
}

TEST_CASE("IntLinEqConstraint unit coeffs: mixed with non-unit falls back to general", "[constraint][int_lin_eq]") {
    // 2*x + 1*y = 7, x in [1,3], y in [1,5]
    // Not all unit coeffs, should use general path (regression test)
    Model model;
    auto x = model.create_variable("x", 1, 3);
    auto y = model.create_variable("y", 1, 5);
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{2, 1}, std::vector<Variable*>{x, y}, 7));

    Solver solver;
    std::vector<Solution> solutions;
    size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    // x=1,y=5; x=2,y=3; x=3,y=1
    REQUIRE(count == 3);
    for (const auto& sol : solutions) {
        REQUIRE(2 * sol.at("x") + sol.at("y") == 7);
    }
}

// ============================================================================
// IntLinLeConstraint tests
// ============================================================================

TEST_CASE("IntLinLeConstraint name", "[constraint][int_lin_le]") {
    Model model;
    auto* x = model.create_variable("x", Domain(1, 3));
    auto* y = model.create_variable("y", Domain(1, 3));
    IntLinLeConstraint c({1, 1}, {x, y}, 5);

    REQUIRE(c.name() == "int_lin_le");
}

TEST_CASE("IntLinLeConstraint is_satisfied", "[constraint][int_lin_le]") {
    SECTION("satisfied: 1*2 + 1*3 <= 5") {
        Model model;
        auto* x = model.create_variable("x", Domain(2, 2));
        auto* y = model.create_variable("y", Domain(3, 3));
        IntLinLeConstraint c({1, 1}, {x, y}, 5);

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == true);
    }

    SECTION("satisfied: 1*2 + 1*3 <= 6") {
        Model model;
        auto* x = model.create_variable("x", Domain(2, 2));
        auto* y = model.create_variable("y", Domain(3, 3));
        IntLinLeConstraint c({1, 1}, {x, y}, 6);

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == true);
    }

    SECTION("violated: 1*2 + 1*3 > 4") {
        Model model;
        auto* x = model.create_variable("x", Domain(2, 2));
        auto* y = model.create_variable("y", Domain(3, 3));
        IntLinLeConstraint c({1, 1}, {x, y}, 4);

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == false);
    }

    SECTION("not fully assigned") {
        Model model;
        auto* x = model.create_variable("x", Domain(1, 3));
        auto* y = model.create_variable("y", Domain(2, 2));
        IntLinLeConstraint c({1, 1}, {x, y}, 5);

        REQUIRE_FALSE(c.is_satisfied(model).has_value());
    }
}

TEST_CASE("IntLinLeConstraint on_final_instantiate", "[constraint][int_lin_le]") {
    SECTION("satisfied - equal") {
        Model model;
        auto x = model.create_variable("x", 2, 2);
        auto y = model.create_variable("y", 3, 3);
        IntLinLeConstraint c({1, 1}, {x, y}, 5);

        REQUIRE(c.on_final_instantiate(model) == true);
    }

    SECTION("satisfied - less than") {
        Model model;
        auto x = model.create_variable("x", 1, 1);
        auto y = model.create_variable("y", 2, 2);
        IntLinLeConstraint c({1, 1}, {x, y}, 5);

        REQUIRE(c.on_final_instantiate(model) == true);
    }

    SECTION("violated") {
        Model model;
        auto x = model.create_variable("x", 3, 3);
        auto y = model.create_variable("y", 3, 3);
        IntLinLeConstraint c({1, 1}, {x, y}, 5);

        REQUIRE(c.on_final_instantiate(model) == false);
    }
}

// ============================================================================
// IntLinNeConstraint tests
// ============================================================================

TEST_CASE("IntLinNeConstraint name", "[constraint][int_lin_ne]") {
    Model model;
    auto* x = model.create_variable("x", Domain(1, 3));
    auto* y = model.create_variable("y", Domain(1, 3));
    IntLinNeConstraint c({1, 1}, {x, y}, 5);

    REQUIRE(c.name() == "int_lin_ne");
}

TEST_CASE("IntLinNeConstraint is_satisfied", "[constraint][int_lin_ne]") {
    SECTION("satisfied: 1*2 + 1*3 != 4") {
        Model model;
        auto* x = model.create_variable("x", Domain(2, 2));
        auto* y = model.create_variable("y", Domain(3, 3));
        IntLinNeConstraint c({1, 1}, {x, y}, 4);

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == true);
    }

    SECTION("satisfied: 1*2 + 1*3 != 6") {
        Model model;
        auto* x = model.create_variable("x", Domain(2, 2));
        auto* y = model.create_variable("y", Domain(3, 3));
        IntLinNeConstraint c({1, 1}, {x, y}, 6);

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == true);
    }

    SECTION("violated: 1*2 + 1*3 == 5") {
        Model model;
        auto* x = model.create_variable("x", Domain(2, 2));
        auto* y = model.create_variable("y", Domain(3, 3));
        IntLinNeConstraint c({1, 1}, {x, y}, 5);

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == false);
    }

    SECTION("not fully assigned") {
        Model model;
        auto* x = model.create_variable("x", Domain(1, 3));
        auto* y = model.create_variable("y", Domain(2, 2));
        IntLinNeConstraint c({1, 1}, {x, y}, 5);

        REQUIRE_FALSE(c.is_satisfied(model).has_value());
    }
}

TEST_CASE("IntLinNeConstraint on_final_instantiate", "[constraint][int_lin_ne]") {
    SECTION("satisfied - not equal") {
        Model model;
        auto x = model.create_variable("x", 2, 2);
        auto y = model.create_variable("y", 3, 3);
        IntLinNeConstraint c({1, 1}, {x, y}, 4);

        REQUIRE(c.on_final_instantiate(model) == true);
    }

    SECTION("violated - equal") {
        Model model;
        auto x = model.create_variable("x", 2, 2);
        auto y = model.create_variable("y", 3, 3);
        IntLinNeConstraint c({1, 1}, {x, y}, 5);

        REQUIRE(c.on_final_instantiate(model) == false);
    }
}

TEST_CASE("IntLinNeConstraint with Solver", "[constraint][int_lin_ne][solver]") {
    SECTION("simple: x + y != 5 with x,y in [1,3]") {
        // x + y != 5 with x,y in {1,2,3}
        // Valid combinations: (1,1)=2, (1,2)=3, (1,3)=4, (2,1)=3, (2,2)=4, (3,1)=4, (3,3)=6
        // Invalid: (2,3)=5, (3,2)=5
        Model model;
        auto x = model.create_variable("x", 1, 3);
        auto y = model.create_variable("y", 1, 3);
        model.add_constraint(std::make_unique<IntLinNeConstraint>(
            std::vector<int64_t>{1, 1}, std::vector<Variable*>{x, y}, 5));

        Solver solver;
        std::vector<Solution> solutions;
        size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        // 9 total - 2 invalid = 7 solutions
        REQUIRE(count == 7);
        REQUIRE(solutions.size() == 7);

        // Verify (2,3) and (3,2) are not in solutions
        for (const auto& sol : solutions) {
            int sum = sol.at("x") + sol.at("y");
            REQUIRE(sum != 5);
        }
    }

    SECTION("coefficient test: 2*x - y != 3 with x,y in [1,3]") {
        // 2*x - y != 3
        // 2*1 - y != 3 => y != -1 (always satisfied for y in [1,3])
        // 2*2 - y != 3 => y != 1
        // 2*3 - y != 3 => y != 3
        // Invalid: (2,1)=3, (3,3)=3
        Model model;
        auto x = model.create_variable("x", 1, 3);
        auto y = model.create_variable("y", 1, 3);
        model.add_constraint(std::make_unique<IntLinNeConstraint>(
            std::vector<int64_t>{2, -1}, std::vector<Variable*>{x, y}, 3));

        Solver solver;
        std::vector<Solution> solutions;
        size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        // 9 total - 2 invalid = 7 solutions
        REQUIRE(count == 7);
        REQUIRE(solutions.size() == 7);

        // Verify constraints
        for (const auto& sol : solutions) {
            int val = 2 * sol.at("x") - sol.at("y");
            REQUIRE(val != 3);
        }
    }
}
