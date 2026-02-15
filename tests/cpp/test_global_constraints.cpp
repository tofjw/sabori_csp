#include <catch2/catch_test_macros.hpp>
#include "sabori_csp/constraint.hpp"
#include "sabori_csp/constraints/logical.hpp"
#include "sabori_csp/variable.hpp"
#include "sabori_csp/domain.hpp"
#include "sabori_csp/model.hpp"
#include "sabori_csp/solver.hpp"
#include <set>
#include <algorithm>

using namespace sabori_csp;

// Dummy model for propagate() calls
static Model dummy_model;

// ============================================================================
// AllDifferentConstraint tests
// ============================================================================

TEST_CASE("AllDifferentConstraint name", "[constraint][all_different]") {
    auto x = std::make_shared<Variable>("x", Domain(1, 3));
    auto y = std::make_shared<Variable>("y", Domain(1, 3));
    auto z = std::make_shared<Variable>("z", Domain(1, 3));
    AllDifferentConstraint c({x, y, z});

    REQUIRE(c.name() == "all_different");
}

TEST_CASE("AllDifferentConstraint variables", "[constraint][all_different]") {
    auto x = std::make_shared<Variable>("x", Domain(1, 3));
    auto y = std::make_shared<Variable>("y", Domain(1, 3));
    AllDifferentConstraint c({x, y});

    auto vars = c.variables();
    REQUIRE(vars.size() == 2);
}

TEST_CASE("AllDifferentConstraint is_satisfied", "[constraint][all_different]") {
    SECTION("all different values - satisfied") {
        auto x = std::make_shared<Variable>("x", Domain(1, 1));
        auto y = std::make_shared<Variable>("y", Domain(2, 2));
        auto z = std::make_shared<Variable>("z", Domain(3, 3));
        AllDifferentConstraint c({x, y, z});

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("duplicate values - violated") {
        auto x = std::make_shared<Variable>("x", Domain(1, 1));
        auto y = std::make_shared<Variable>("y", Domain(2, 2));
        auto z = std::make_shared<Variable>("z", Domain(1, 1));  // same as x
        AllDifferentConstraint c({x, y, z});

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("not fully assigned") {
        auto x = std::make_shared<Variable>("x", Domain(1, 1));
        auto y = std::make_shared<Variable>("y", Domain(1, 3));
        auto z = std::make_shared<Variable>("z", Domain(3, 3));
        AllDifferentConstraint c({x, y, z});

        REQUIRE_FALSE(c.is_satisfied().has_value());
    }
}

TEST_CASE("AllDifferentConstraint propagate", "[constraint][all_different]") {
    SECTION("remove assigned values from others") {
        auto x = std::make_shared<Variable>("x", Domain(2, 2));  // assigned
        auto y = std::make_shared<Variable>("y", Domain(1, 3));
        auto z = std::make_shared<Variable>("z", Domain(1, 3));
        AllDifferentConstraint c({x, y, z});

        REQUIRE(c.presolve(dummy_model) == true);
        REQUIRE_FALSE(y->domain().contains(2));
        REQUIRE_FALSE(z->domain().contains(2));
    }

    SECTION("infeasible - domain becomes empty") {
        auto x = std::make_shared<Variable>("x", Domain(1, 1));  // assigned to 1
        auto y = std::make_shared<Variable>("y", Domain(1, 1));  // domain {1}, unassigned for propagation check
        // Note: make_var with single value creates a singleton which is considered assigned
        // So we need a different approach to test infeasibility

        // This tests that when x=1 is assigned and y only has {1},
        // propagate will remove 1 from y making it empty
        AllDifferentConstraint c({x, y});

        // Note: Both are singletons (assigned), so propagate won't modify them
        // The infeasibility is detected by is_satisfied() instead
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("infeasible via propagation") {
        auto x = std::make_shared<Variable>("x", Domain(2, 2));  // assigned to 2
        auto y = std::make_shared<Variable>("y", Domain(2, 2));  // domain {2}
        auto z = std::make_shared<Variable>("z", Domain(2, 3));  // domain {2, 3}
        AllDifferentConstraint c({x, y, z});

        // y only has value 2, which x already uses
        // propagate should remove 2 from y, making it empty
        // But y is a singleton (assigned), so the check is !other->is_assigned()
        // We need on_final_instantiate to detect this
        REQUIRE(c.on_final_instantiate() == false);
    }
}

TEST_CASE("AllDifferentConstraint pool management", "[constraint][all_different]") {
    auto x = std::make_shared<Variable>("x", Domain(1, 3));
    auto y = std::make_shared<Variable>("y", Domain(1, 3));
    auto z = std::make_shared<Variable>("z", Domain(1, 3));
    AllDifferentConstraint c({x, y, z});

    SECTION("initial pool size") {
        REQUIRE(c.pool_size() == 3);  // {1, 2, 3}
    }
}

TEST_CASE("AllDifferentConstraint on_final_instantiate", "[constraint][all_different]") {
    SECTION("satisfied") {
        auto x = std::make_shared<Variable>("x", Domain(1, 1));
        auto y = std::make_shared<Variable>("y", Domain(2, 2));
        auto z = std::make_shared<Variable>("z", Domain(3, 3));
        AllDifferentConstraint c({x, y, z});

        REQUIRE(c.on_final_instantiate() == true);
    }

    SECTION("violated") {
        auto x = std::make_shared<Variable>("x", Domain(1, 1));
        auto y = std::make_shared<Variable>("y", Domain(1, 1));
        AllDifferentConstraint c({x, y});

        REQUIRE(c.on_final_instantiate() == false);
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
// IntLinEqConstraint tests
// ============================================================================

TEST_CASE("IntLinEqConstraint name", "[constraint][int_lin_eq]") {
    auto x = std::make_shared<Variable>("x", Domain(1, 3));
    auto y = std::make_shared<Variable>("y", Domain(1, 3));
    IntLinEqConstraint c({1, 1}, {x, y}, 5);

    REQUIRE(c.name() == "int_lin_eq");
}

TEST_CASE("IntLinEqConstraint is_satisfied", "[constraint][int_lin_eq]") {
    SECTION("satisfied: 1*2 + 1*3 = 5") {
        auto x = std::make_shared<Variable>("x", Domain(2, 2));
        auto y = std::make_shared<Variable>("y", Domain(3, 3));
        IntLinEqConstraint c({1, 1}, {x, y}, 5);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("satisfied: 2*1 + 3*2 = 8") {
        auto x = std::make_shared<Variable>("x", Domain(1, 1));
        auto y = std::make_shared<Variable>("y", Domain(2, 2));
        IntLinEqConstraint c({2, 3}, {x, y}, 8);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("violated: 1*2 + 1*3 != 6") {
        auto x = std::make_shared<Variable>("x", Domain(2, 2));
        auto y = std::make_shared<Variable>("y", Domain(3, 3));
        IntLinEqConstraint c({1, 1}, {x, y}, 6);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("not fully assigned") {
        auto x = std::make_shared<Variable>("x", Domain(1, 3));
        auto y = std::make_shared<Variable>("y", Domain(2, 2));
        IntLinEqConstraint c({1, 1}, {x, y}, 5);

        REQUIRE_FALSE(c.is_satisfied().has_value());
    }
}

TEST_CASE("IntLinEqConstraint on_final_instantiate", "[constraint][int_lin_eq]") {
    SECTION("satisfied") {
        auto x = std::make_shared<Variable>("x", Domain(2, 2));
        auto y = std::make_shared<Variable>("y", Domain(3, 3));
        IntLinEqConstraint c({1, 1}, {x, y}, 5);

        REQUIRE(c.on_final_instantiate() == true);
    }

    SECTION("violated") {
        auto x = std::make_shared<Variable>("x", Domain(2, 2));
        auto y = std::make_shared<Variable>("y", Domain(3, 3));
        IntLinEqConstraint c({1, 1}, {x, y}, 6);

        REQUIRE(c.on_final_instantiate() == false);
    }
}

TEST_CASE("IntLinEqConstraint with negative coefficients", "[constraint][int_lin_eq]") {
    SECTION("satisfied: 2*3 + (-1)*1 = 5") {
        auto x = std::make_shared<Variable>("x", Domain(3, 3));
        auto y = std::make_shared<Variable>("y", Domain(1, 1));
        IntLinEqConstraint c({2, -1}, {x, y}, 5);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("satisfied: (-1)*2 + 1*7 = 5") {
        auto x = std::make_shared<Variable>("x", Domain(2, 2));
        auto y = std::make_shared<Variable>("y", Domain(7, 7));
        IntLinEqConstraint c({-1, 1}, {x, y}, 5);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
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
    REQUIRE(c.on_final_instantiate() == true);  // 2 + 4 = 6

    // Rewind to level 0 and try different values
    c.rewind_to(0);
    // State should be restored
}

// ============================================================================
// IntLinLeConstraint tests
// ============================================================================

TEST_CASE("IntLinLeConstraint name", "[constraint][int_lin_le]") {
    auto x = std::make_shared<Variable>("x", Domain(1, 3));
    auto y = std::make_shared<Variable>("y", Domain(1, 3));
    IntLinLeConstraint c({1, 1}, {x, y}, 5);

    REQUIRE(c.name() == "int_lin_le");
}

TEST_CASE("IntLinLeConstraint is_satisfied", "[constraint][int_lin_le]") {
    SECTION("satisfied: 1*2 + 1*3 <= 5") {
        auto x = std::make_shared<Variable>("x", Domain(2, 2));
        auto y = std::make_shared<Variable>("y", Domain(3, 3));
        IntLinLeConstraint c({1, 1}, {x, y}, 5);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("satisfied: 1*2 + 1*3 <= 6") {
        auto x = std::make_shared<Variable>("x", Domain(2, 2));
        auto y = std::make_shared<Variable>("y", Domain(3, 3));
        IntLinLeConstraint c({1, 1}, {x, y}, 6);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("violated: 1*2 + 1*3 > 4") {
        auto x = std::make_shared<Variable>("x", Domain(2, 2));
        auto y = std::make_shared<Variable>("y", Domain(3, 3));
        IntLinLeConstraint c({1, 1}, {x, y}, 4);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("not fully assigned") {
        auto x = std::make_shared<Variable>("x", Domain(1, 3));
        auto y = std::make_shared<Variable>("y", Domain(2, 2));
        IntLinLeConstraint c({1, 1}, {x, y}, 5);

        REQUIRE_FALSE(c.is_satisfied().has_value());
    }
}

TEST_CASE("IntLinLeConstraint on_final_instantiate", "[constraint][int_lin_le]") {
    SECTION("satisfied - equal") {
        auto x = std::make_shared<Variable>("x", Domain(2, 2));
        auto y = std::make_shared<Variable>("y", Domain(3, 3));
        IntLinLeConstraint c({1, 1}, {x, y}, 5);

        REQUIRE(c.on_final_instantiate() == true);
    }

    SECTION("satisfied - less than") {
        auto x = std::make_shared<Variable>("x", Domain(1, 1));
        auto y = std::make_shared<Variable>("y", Domain(2, 2));
        IntLinLeConstraint c({1, 1}, {x, y}, 5);

        REQUIRE(c.on_final_instantiate() == true);
    }

    SECTION("violated") {
        auto x = std::make_shared<Variable>("x", Domain(3, 3));
        auto y = std::make_shared<Variable>("y", Domain(3, 3));
        IntLinLeConstraint c({1, 1}, {x, y}, 5);

        REQUIRE(c.on_final_instantiate() == false);
    }
}

// ============================================================================
// IntLinNeConstraint tests
// ============================================================================

TEST_CASE("IntLinNeConstraint name", "[constraint][int_lin_ne]") {
    auto x = std::make_shared<Variable>("x", Domain(1, 3));
    auto y = std::make_shared<Variable>("y", Domain(1, 3));
    IntLinNeConstraint c({1, 1}, {x, y}, 5);

    REQUIRE(c.name() == "int_lin_ne");
}

TEST_CASE("IntLinNeConstraint is_satisfied", "[constraint][int_lin_ne]") {
    SECTION("satisfied: 1*2 + 1*3 != 4") {
        auto x = std::make_shared<Variable>("x", Domain(2, 2));
        auto y = std::make_shared<Variable>("y", Domain(3, 3));
        IntLinNeConstraint c({1, 1}, {x, y}, 4);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("satisfied: 1*2 + 1*3 != 6") {
        auto x = std::make_shared<Variable>("x", Domain(2, 2));
        auto y = std::make_shared<Variable>("y", Domain(3, 3));
        IntLinNeConstraint c({1, 1}, {x, y}, 6);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("violated: 1*2 + 1*3 == 5") {
        auto x = std::make_shared<Variable>("x", Domain(2, 2));
        auto y = std::make_shared<Variable>("y", Domain(3, 3));
        IntLinNeConstraint c({1, 1}, {x, y}, 5);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("not fully assigned") {
        auto x = std::make_shared<Variable>("x", Domain(1, 3));
        auto y = std::make_shared<Variable>("y", Domain(2, 2));
        IntLinNeConstraint c({1, 1}, {x, y}, 5);

        REQUIRE_FALSE(c.is_satisfied().has_value());
    }
}

TEST_CASE("IntLinNeConstraint on_final_instantiate", "[constraint][int_lin_ne]") {
    SECTION("satisfied - not equal") {
        auto x = std::make_shared<Variable>("x", Domain(2, 2));
        auto y = std::make_shared<Variable>("y", Domain(3, 3));
        IntLinNeConstraint c({1, 1}, {x, y}, 4);

        REQUIRE(c.on_final_instantiate() == true);
    }

    SECTION("violated - equal") {
        auto x = std::make_shared<Variable>("x", Domain(2, 2));
        auto y = std::make_shared<Variable>("y", Domain(3, 3));
        IntLinNeConstraint c({1, 1}, {x, y}, 5);

        REQUIRE(c.on_final_instantiate() == false);
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
        model.add_constraint(std::make_shared<IntLinNeConstraint>(
            std::vector<int64_t>{1, 1}, std::vector<VariablePtr>{x, y}, 5));

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
        model.add_constraint(std::make_shared<IntLinNeConstraint>(
            std::vector<int64_t>{2, -1}, std::vector<VariablePtr>{x, y}, 3));

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

// ============================================================================
// Magic Square 3x3 test
// ============================================================================

TEST_CASE("Magic Square 3x3", "[solver][magic_square]") {
    /*
     * 3x3 Magic Square:
     *   x[0] x[1] x[2]
     *   x[3] x[4] x[5]
     *   x[6] x[7] x[8]
     *
     * - All cells contain distinct values from 1-9
     * - Each row, column, and diagonal sums to 15
     * - There are 8 solutions (rotations and reflections)
     */

    Model model;

    // Create 9 variables for the 3x3 grid
    std::vector<VariablePtr> cells;
    for (int i = 0; i < 9; ++i) {
        auto var = model.create_variable("x" + std::to_string(i), 1, 9);
        cells.push_back(var);
    }

    // AllDifferent constraint: all cells must have different values
    auto alldiff = std::make_shared<AllDifferentConstraint>(cells);
    model.add_constraint(alldiff);

    // Row constraints: each row sums to 15
    // Row 0: x[0] + x[1] + x[2] = 15
    model.add_constraint(std::make_shared<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<VariablePtr>{cells[0], cells[1], cells[2]},
        15
    ));
    // Row 1: x[3] + x[4] + x[5] = 15
    model.add_constraint(std::make_shared<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<VariablePtr>{cells[3], cells[4], cells[5]},
        15
    ));
    // Row 2: x[6] + x[7] + x[8] = 15
    model.add_constraint(std::make_shared<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<VariablePtr>{cells[6], cells[7], cells[8]},
        15
    ));

    // Column constraints: each column sums to 15
    // Column 0: x[0] + x[3] + x[6] = 15
    model.add_constraint(std::make_shared<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<VariablePtr>{cells[0], cells[3], cells[6]},
        15
    ));
    // Column 1: x[1] + x[4] + x[7] = 15
    model.add_constraint(std::make_shared<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<VariablePtr>{cells[1], cells[4], cells[7]},
        15
    ));
    // Column 2: x[2] + x[5] + x[8] = 15
    model.add_constraint(std::make_shared<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<VariablePtr>{cells[2], cells[5], cells[8]},
        15
    ));

    // Diagonal constraints
    // Main diagonal: x[0] + x[4] + x[8] = 15
    model.add_constraint(std::make_shared<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<VariablePtr>{cells[0], cells[4], cells[8]},
        15
    ));
    // Anti diagonal: x[2] + x[4] + x[6] = 15
    model.add_constraint(std::make_shared<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<VariablePtr>{cells[2], cells[4], cells[6]},
        15
    ));

    Solver solver;
    // Enable all advanced features
    solver.set_restart_enabled(true);
    solver.set_nogood_learning(true);
    solver.set_activity_selection(true);

    SECTION("find one solution") {
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());

        // Verify all values are 1-9 and different
        std::set<int64_t> values;
        for (int i = 0; i < 9; ++i) {
            auto val = solution->at("x" + std::to_string(i));
            REQUIRE(val >= 1);
            REQUIRE(val <= 9);
            values.insert(val);
        }
        REQUIRE(values.size() == 9);

        // Verify row sums
        REQUIRE(solution->at("x0") + solution->at("x1") + solution->at("x2") == 15);
        REQUIRE(solution->at("x3") + solution->at("x4") + solution->at("x5") == 15);
        REQUIRE(solution->at("x6") + solution->at("x7") + solution->at("x8") == 15);

        // Verify column sums
        REQUIRE(solution->at("x0") + solution->at("x3") + solution->at("x6") == 15);
        REQUIRE(solution->at("x1") + solution->at("x4") + solution->at("x7") == 15);
        REQUIRE(solution->at("x2") + solution->at("x5") + solution->at("x8") == 15);

        // Verify diagonal sums
        REQUIRE(solution->at("x0") + solution->at("x4") + solution->at("x8") == 15);
        REQUIRE(solution->at("x2") + solution->at("x4") + solution->at("x6") == 15);
    }

    SECTION("find all solutions") {
        std::vector<Solution> solutions;
        size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
            solutions.push_back(sol);
            return true;  // Continue searching
        });

        // There are exactly 8 solutions for 3x3 magic square
        REQUIRE(count == 8);
        REQUIRE(solutions.size() == 8);

        // Verify all solutions are valid and different
        std::set<std::vector<int64_t>> unique_solutions;
        for (const auto& sol : solutions) {
            std::vector<int64_t> vals;
            for (int i = 0; i < 9; ++i) {
                vals.push_back(sol.at("x" + std::to_string(i)));
            }

            // Verify row sums
            REQUIRE(vals[0] + vals[1] + vals[2] == 15);
            REQUIRE(vals[3] + vals[4] + vals[5] == 15);
            REQUIRE(vals[6] + vals[7] + vals[8] == 15);

            // Verify column sums
            REQUIRE(vals[0] + vals[3] + vals[6] == 15);
            REQUIRE(vals[1] + vals[4] + vals[7] == 15);
            REQUIRE(vals[2] + vals[5] + vals[8] == 15);

            // Verify diagonal sums
            REQUIRE(vals[0] + vals[4] + vals[8] == 15);
            REQUIRE(vals[2] + vals[4] + vals[6] == 15);

            unique_solutions.insert(vals);
        }

        // All solutions should be unique
        REQUIRE(unique_solutions.size() == 8);
    }
}

// ============================================================================
// Magic Square 3x3 with partial assignments
// ============================================================================

// Helper function to create a magic square model with optional fixed cells
static Model create_magic_square_model(
    const std::vector<std::pair<int, int64_t>>& fixed_cells = {}) {
    Model model;

    // Create 9 variables for the 3x3 grid
    std::vector<VariablePtr> cells;
    for (int i = 0; i < 9; ++i) {
        auto var = model.create_variable("x" + std::to_string(i), 1, 9);
        cells.push_back(var);
    }

    // Fix specified cells via Model API
    for (const auto& [idx, val] : fixed_cells) {
        model.instantiate(0, cells[idx]->id(), val);
    }

    // AllDifferent constraint
    model.add_constraint(std::make_shared<AllDifferentConstraint>(cells));

    // Row constraints (sum = 15)
    model.add_constraint(std::make_shared<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<VariablePtr>{cells[0], cells[1], cells[2]}, 15));
    model.add_constraint(std::make_shared<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<VariablePtr>{cells[3], cells[4], cells[5]}, 15));
    model.add_constraint(std::make_shared<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<VariablePtr>{cells[6], cells[7], cells[8]}, 15));

    // Column constraints (sum = 15)
    model.add_constraint(std::make_shared<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<VariablePtr>{cells[0], cells[3], cells[6]}, 15));
    model.add_constraint(std::make_shared<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<VariablePtr>{cells[1], cells[4], cells[7]}, 15));
    model.add_constraint(std::make_shared<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<VariablePtr>{cells[2], cells[5], cells[8]}, 15));

    // Diagonal constraints (sum = 15)
    model.add_constraint(std::make_shared<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<VariablePtr>{cells[0], cells[4], cells[8]}, 15));
    model.add_constraint(std::make_shared<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<VariablePtr>{cells[2], cells[4], cells[6]}, 15));

    return model;
}

TEST_CASE("Magic Square 3x3 - one row fixed (consistent)", "[solver][magic_square]") {
    /*
     * Valid magic square:
     *   2 7 6
     *   9 5 1
     *   4 3 8
     * Fix row 0: x[0]=2, x[1]=7, x[2]=6
     */
    auto model = create_magic_square_model({{0, 2}, {1, 7}, {2, 6}});

    Solver solver;
    auto solution = solver.solve(model);

    REQUIRE(solution.has_value());
    REQUIRE(solution->at("x0") == 2);
    REQUIRE(solution->at("x1") == 7);
    REQUIRE(solution->at("x2") == 6);
    // Verify row sums
    REQUIRE(solution->at("x3") + solution->at("x4") + solution->at("x5") == 15);
    REQUIRE(solution->at("x6") + solution->at("x7") + solution->at("x8") == 15);
}

TEST_CASE("Magic Square 3x3 - one row fixed (inconsistent)", "[solver][magic_square]") {
    /*
     * Fix row 0 with invalid values (sum != 15): x[0]=1, x[1]=2, x[2]=3 (sum=6)
     */
    auto model = create_magic_square_model({{0, 1}, {1, 2}, {2, 3}});

    Solver solver;
    auto solution = solver.solve(model);

    REQUIRE_FALSE(solution.has_value());
}

TEST_CASE("Magic Square 3x3 - two rows fixed (consistent)", "[solver][magic_square]") {
    /*
     * Valid magic square:
     *   2 7 6
     *   9 5 1
     *   4 3 8
     * Fix rows 0 and 1
     */
    auto model = create_magic_square_model({
        {0, 2}, {1, 7}, {2, 6},
        {3, 9}, {4, 5}, {5, 1}
    });

    Solver solver;
    auto solution = solver.solve(model);

    REQUIRE(solution.has_value());
    REQUIRE(solution->at("x0") == 2);
    REQUIRE(solution->at("x1") == 7);
    REQUIRE(solution->at("x2") == 6);
    REQUIRE(solution->at("x3") == 9);
    REQUIRE(solution->at("x4") == 5);
    REQUIRE(solution->at("x5") == 1);
    // Row 2 should be determined: 4, 3, 8
    REQUIRE(solution->at("x6") == 4);
    REQUIRE(solution->at("x7") == 3);
    REQUIRE(solution->at("x8") == 8);
}

TEST_CASE("Magic Square 3x3 - two rows fixed (inconsistent)", "[solver][magic_square]") {
    /*
     * Fix rows 0 and 1 with conflicting values
     * Row 0: 2 7 6 (valid, sum=15)
     * Row 1: 8 5 2 (sum=15 but conflicts with row 0's value 2)
     */
    auto model = create_magic_square_model({
        {0, 2}, {1, 7}, {2, 6},
        {3, 8}, {4, 5}, {5, 2}  // 2 is duplicated
    });

    Solver solver;
    auto solution = solver.solve(model);

    REQUIRE_FALSE(solution.has_value());
}

TEST_CASE("Magic Square 3x3 - three rows fixed (consistent)", "[solver][magic_square]") {
    /*
     * Valid magic square completely fixed:
     *   2 7 6
     *   9 5 1
     *   4 3 8
     */
    auto model = create_magic_square_model({
        {0, 2}, {1, 7}, {2, 6},
        {3, 9}, {4, 5}, {5, 1},
        {6, 4}, {7, 3}, {8, 8}
    });

    Solver solver;
    auto solution = solver.solve(model);

    REQUIRE(solution.has_value());
    REQUIRE(solution->at("x0") == 2);
    REQUIRE(solution->at("x1") == 7);
    REQUIRE(solution->at("x2") == 6);
    REQUIRE(solution->at("x3") == 9);
    REQUIRE(solution->at("x4") == 5);
    REQUIRE(solution->at("x5") == 1);
    REQUIRE(solution->at("x6") == 4);
    REQUIRE(solution->at("x7") == 3);
    REQUIRE(solution->at("x8") == 8);
}

TEST_CASE("Magic Square 3x3 - three rows fixed (inconsistent)", "[solver][magic_square]") {
    /*
     * All rows fixed but not a valid magic square:
     *   2 7 6  (sum=15)
     *   9 5 1  (sum=15)
     *   4 3 9  (sum=16, invalid; also 9 is duplicated)
     */
    auto model = create_magic_square_model({
        {0, 2}, {1, 7}, {2, 6},
        {3, 9}, {4, 5}, {5, 1},
        {6, 4}, {7, 3}, {8, 9}  // row sum=16, and 9 duplicated
    });

    Solver solver;
    auto solution = solver.solve(model);

    REQUIRE_FALSE(solution.has_value());
}

// ============================================================================
// CircuitConstraint tests
// ============================================================================

TEST_CASE("CircuitConstraint name", "[constraint][circuit]") {
    auto x0 = std::make_shared<Variable>("x0", Domain(0, 2));
    auto x1 = std::make_shared<Variable>("x1", Domain(0, 2));
    auto x2 = std::make_shared<Variable>("x2", Domain(0, 2));
    CircuitConstraint c({x0, x1, x2});

    REQUIRE(c.name() == "circuit");
}

TEST_CASE("CircuitConstraint variables", "[constraint][circuit]") {
    auto x0 = std::make_shared<Variable>("x0", Domain(0, 2));
    auto x1 = std::make_shared<Variable>("x1", Domain(0, 2));
    auto x2 = std::make_shared<Variable>("x2", Domain(0, 2));
    CircuitConstraint c({x0, x1, x2});

    auto vars = c.variables();
    REQUIRE(vars.size() == 3);
}

TEST_CASE("CircuitConstraint is_satisfied", "[constraint][circuit]") {
    SECTION("valid circuit - satisfied") {
        // Circuit: 0 -> 1 -> 2 -> 0
        auto x0 = std::make_shared<Variable>("x0", Domain(1, 1));  // x[0] = 1
        auto x1 = std::make_shared<Variable>("x1", Domain(2, 2));  // x[1] = 2
        auto x2 = std::make_shared<Variable>("x2", Domain(0, 0));  // x[2] = 0
        CircuitConstraint c({x0, x1, x2});

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("subcircuit - violated") {
        // Subcircuit: 0 -> 1 -> 0 (x[2] is not part of circuit)
        auto x0 = std::make_shared<Variable>("x0", Domain(1, 1));  // x[0] = 1
        auto x1 = std::make_shared<Variable>("x1", Domain(0, 0));  // x[1] = 0
        auto x2 = std::make_shared<Variable>("x2", Domain(2, 2));  // x[2] = 2 (self-loop)
        CircuitConstraint c({x0, x1, x2});

        // This should be marked as initially inconsistent or is_satisfied returns false
        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("duplicate values - violated") {
        auto x0 = std::make_shared<Variable>("x0", Domain(1, 1));  // x[0] = 1
        auto x1 = std::make_shared<Variable>("x1", Domain(1, 1));  // x[1] = 1 (duplicate)
        auto x2 = std::make_shared<Variable>("x2", Domain(0, 0));  // x[2] = 0
        CircuitConstraint c({x0, x1, x2});

        // Duplicate value - should be initially inconsistent
        REQUIRE(c.is_initially_inconsistent() == true);
    }

    SECTION("not fully assigned") {
        auto x0 = std::make_shared<Variable>("x0", Domain(1, 1));
        auto x1 = std::make_shared<Variable>("x1", Domain(0, 2));
        auto x2 = std::make_shared<Variable>("x2", Domain(0, 0));
        CircuitConstraint c({x0, x1, x2});

        REQUIRE_FALSE(c.is_satisfied().has_value());
    }
}

TEST_CASE("CircuitConstraint pool management", "[constraint][circuit]") {
    auto x0 = std::make_shared<Variable>("x0", Domain(0, 2));
    auto x1 = std::make_shared<Variable>("x1", Domain(0, 2));
    auto x2 = std::make_shared<Variable>("x2", Domain(0, 2));
    CircuitConstraint c({x0, x1, x2});

    SECTION("initial pool size") {
        REQUIRE(c.pool_size() == 3);  // {0, 1, 2}
    }
}

TEST_CASE("CircuitConstraint on_final_instantiate", "[constraint][circuit]") {
    SECTION("valid circuit") {
        // Circuit: 0 -> 1 -> 2 -> 0
        auto x0 = std::make_shared<Variable>("x0", Domain(1, 1));
        auto x1 = std::make_shared<Variable>("x1", Domain(2, 2));
        auto x2 = std::make_shared<Variable>("x2", Domain(0, 0));
        CircuitConstraint c({x0, x1, x2});

        REQUIRE(c.on_final_instantiate() == true);
    }

    SECTION("valid circuit reversed") {
        // Circuit: 0 -> 2 -> 1 -> 0
        auto x0 = std::make_shared<Variable>("x0", Domain(2, 2));
        auto x1 = std::make_shared<Variable>("x1", Domain(0, 0));
        auto x2 = std::make_shared<Variable>("x2", Domain(1, 1));
        CircuitConstraint c({x0, x1, x2});

        REQUIRE(c.on_final_instantiate() == true);
    }
}

TEST_CASE("CircuitConstraint rewind_to", "[constraint][circuit][trail]") {
    Model model;
    auto x0 = model.create_variable("x0", 0, 2);
    auto x1 = model.create_variable("x1", 0, 2);
    auto x2 = model.create_variable("x2", 0, 2);
    CircuitConstraint c({x0, x1, x2});

    REQUIRE(c.pool_size() == 3);

    // Simulate instantiation at level 1: x[0] = 1
    model.instantiate(1, x0->id(), 1);
    REQUIRE(c.on_instantiate(model, 1, 0, 0, 1, 0, 2));
    REQUIRE(c.pool_size() == 2);

    // Simulate instantiation at level 2: x[1] = 2
    model.instantiate(2, x1->id(), 2);
    REQUIRE(c.on_instantiate(model, 2, 1, 0, 2, 0, 2));
    REQUIRE(c.pool_size() == 1);

    // Rewind to level 1
    c.rewind_to(1);
    REQUIRE(c.pool_size() == 2);

    // Rewind to level 0
    c.rewind_to(0);
    REQUIRE(c.pool_size() == 3);
}

TEST_CASE("CircuitConstraint subcircuit detection", "[constraint][circuit]") {
    SECTION("subcircuit detected during instantiation") {
        Model model;
        auto x0 = model.create_variable("x0", 0, 2);
        auto x1 = model.create_variable("x1", 0, 2);
        auto x2 = model.create_variable("x2", 0, 2);
        CircuitConstraint c({x0, x1, x2});

        // x[0] = 1: 0 -> 1
        model.instantiate(1, x0->id(), 1);
        REQUIRE(c.on_instantiate(model, 1, 0, 0, 1, 0, 2));

        // x[1] = 0: 1 -> 0, forms subcircuit (0 -> 1 -> 0)
        model.instantiate(2, x1->id(), 0);
        REQUIRE_FALSE(c.on_instantiate(model, 2, 1, 0, 0, 0, 2));
    }

    SECTION("self-loop is subcircuit") {
        Model model;
        auto x0 = model.create_variable("x0", 0, 2);
        auto x1 = model.create_variable("x1", 0, 2);
        auto x2 = model.create_variable("x2", 0, 2);
        CircuitConstraint c({x0, x1, x2});

        // x[0] = 0: self-loop, forms subcircuit of size 1
        model.instantiate(1, x0->id(), 0);
        REQUIRE_FALSE(c.on_instantiate(model, 1, 0, 0, 0, 0, 2));
    }
}

TEST_CASE("CircuitConstraint solver integration", "[solver][circuit]") {
    SECTION("n=3 find one solution") {
        Model model;
        std::vector<VariablePtr> vars;
        for (int i = 0; i < 3; ++i) {
            auto var = model.create_variable("x" + std::to_string(i), 0, 2);
            vars.push_back(var);
        }

        model.add_constraint(std::make_shared<CircuitConstraint>(vars));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());

        // Verify it's a valid circuit
        std::vector<int64_t> values;
        for (int i = 0; i < 3; ++i) {
            values.push_back(solution->at("x" + std::to_string(i)));
        }

        // Check all different
        std::set<int64_t> unique_values(values.begin(), values.end());
        REQUIRE(unique_values.size() == 3);

        // Check forms a cycle starting from 0
        std::set<int64_t> visited;
        int64_t current = 0;
        for (int i = 0; i < 3; ++i) {
            REQUIRE(visited.find(current) == visited.end());
            visited.insert(current);
            current = values[static_cast<size_t>(current)];
        }
        REQUIRE(current == 0);  // Returns to start
    }

    SECTION("n=3 find all solutions") {
        Model model;
        std::vector<VariablePtr> vars;
        for (int i = 0; i < 3; ++i) {
            auto var = model.create_variable("x" + std::to_string(i), 0, 2);
            vars.push_back(var);
        }

        model.add_constraint(std::make_shared<CircuitConstraint>(vars));

        Solver solver;
        std::vector<Solution> solutions;
        size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        // For n=3, there are exactly 2 Hamiltonian cycles:
        // 0 -> 1 -> 2 -> 0
        // 0 -> 2 -> 1 -> 0
        REQUIRE(count == 2);
        REQUIRE(solutions.size() == 2);

        // Verify all solutions are valid and different
        std::set<std::vector<int64_t>> unique_solutions;
        for (const auto& sol : solutions) {
            std::vector<int64_t> vals;
            for (int i = 0; i < 3; ++i) {
                vals.push_back(sol.at("x" + std::to_string(i)));
            }

            // Verify forms a valid circuit
            std::set<int64_t> visited;
            int64_t current = 0;
            for (int i = 0; i < 3; ++i) {
                REQUIRE(visited.find(current) == visited.end());
                visited.insert(current);
                current = vals[static_cast<size_t>(current)];
            }
            REQUIRE(current == 0);

            unique_solutions.insert(vals);
        }

        REQUIRE(unique_solutions.size() == 2);
    }

    SECTION("n=4 find all solutions") {
        Model model;
        std::vector<VariablePtr> vars;
        for (int i = 0; i < 4; ++i) {
            auto var = model.create_variable("x" + std::to_string(i), 0, 3);
            vars.push_back(var);
        }

        model.add_constraint(std::make_shared<CircuitConstraint>(vars));

        Solver solver;
        std::vector<Solution> solutions;
        size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        // For n=4, there are exactly 6 Hamiltonian cycles
        // (n-1)!/2 = 3!/2 = 3 for undirected, but directed has (n-1)! = 6
        REQUIRE(count == 6);

        // Verify all solutions are valid
        for (const auto& sol : solutions) {
            std::vector<int64_t> vals;
            for (int i = 0; i < 4; ++i) {
                vals.push_back(sol.at("x" + std::to_string(i)));
            }

            std::set<int64_t> visited;
            int64_t current = 0;
            for (int i = 0; i < 4; ++i) {
                REQUIRE(visited.find(current) == visited.end());
                visited.insert(current);
                current = vals[static_cast<size_t>(current)];
            }
            REQUIRE(current == 0);
        }
    }
}

// ============================================================================
// IntElementConstraint tests
// ============================================================================

TEST_CASE("IntElementConstraint name", "[constraint][int_element]") {
    auto index = std::make_shared<Variable>("index", Domain(1, 3));
    auto result = std::make_shared<Variable>("result", Domain(10, 30));
    std::vector<Domain::value_type> array = {10, 20, 30};
    IntElementConstraint c(index, array, result);

    REQUIRE(c.name() == "int_element");
}

TEST_CASE("IntElementConstraint variables", "[constraint][int_element]") {
    auto index = std::make_shared<Variable>("index", Domain(1, 3));
    auto result = std::make_shared<Variable>("result", Domain(10, 30));
    std::vector<Domain::value_type> array = {10, 20, 30};
    IntElementConstraint c(index, array, result);

    auto vars = c.variables();
    REQUIRE(vars.size() == 2);
    REQUIRE(vars[0] == index);
    REQUIRE(vars[1] == result);
}

TEST_CASE("IntElementConstraint is_satisfied", "[constraint][int_element]") {
    SECTION("satisfied - 1-based index") {
        // array[1] = 10, array[2] = 20, array[3] = 30 (1-based)
        auto index = std::make_shared<Variable>("index", Domain(2, 2));  // index = 2
        auto result = std::make_shared<Variable>("result", Domain(20, 20));  // result = 20
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("satisfied - 0-based index") {
        // array[0] = 10, array[1] = 20, array[2] = 30 (0-based)
        auto index = std::make_shared<Variable>("index", Domain(1, 1));  // index = 1
        auto result = std::make_shared<Variable>("result", Domain(20, 20));  // result = 20
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result, true);  // zero_based = true

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("violated - value mismatch") {
        auto index = std::make_shared<Variable>("index", Domain(2, 2));  // index = 2 -> array[1] = 20
        auto result = std::make_shared<Variable>("result", Domain(30, 30));  // result = 30 (wrong)
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("not fully assigned") {
        auto index = std::make_shared<Variable>("index", Domain(1, 3));
        auto result = std::make_shared<Variable>("result", Domain(20, 20));
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE_FALSE(c.is_satisfied().has_value());
    }
}

TEST_CASE("IntElementConstraint on_final_instantiate", "[constraint][int_element]") {
    SECTION("satisfied") {
        auto index = std::make_shared<Variable>("index", Domain(1, 1));  // index = 1 -> array[0] = 10
        auto result = std::make_shared<Variable>("result", Domain(10, 10));
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.on_final_instantiate() == true);
    }

    SECTION("violated") {
        auto index = std::make_shared<Variable>("index", Domain(1, 1));  // index = 1 -> array[0] = 10
        auto result = std::make_shared<Variable>("result", Domain(20, 20));  // wrong
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.on_final_instantiate() == false);
    }
}

TEST_CASE("IntElementConstraint initial consistency", "[constraint][int_element]") {
    SECTION("index out of range - too small") {
        auto index = std::make_shared<Variable>("index", Domain(0, 0));  // 0 is out of range for 1-based
        auto result = std::make_shared<Variable>("result", Domain(10, 30));
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.is_initially_inconsistent() == true);
    }

    SECTION("index out of range - too large") {
        auto index = std::make_shared<Variable>("index", Domain(4, 4));  // 4 is out of range for array of size 3 (1-based)
        auto result = std::make_shared<Variable>("result", Domain(10, 30));
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.is_initially_inconsistent() == true);
    }

    SECTION("result value not in array") {
        auto index = std::make_shared<Variable>("index", Domain(1, 3));
        auto result = std::make_shared<Variable>("result", Domain(99, 99));  // 99 is not in array
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.is_initially_inconsistent() == true);
    }

    SECTION("consistent initial state") {
        auto index = std::make_shared<Variable>("index", Domain(1, 3));
        auto result = std::make_shared<Variable>("result", Domain(10, 30));
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.is_initially_inconsistent() == false);
    }
}

TEST_CASE("IntElementConstraint rewind_to", "[constraint][int_element][trail]") {
    // IntElementConstraint has no state, so rewind_to is a no-op
    auto index = std::make_shared<Variable>("index", Domain(1, 3));
    auto result = std::make_shared<Variable>("result", Domain(10, 30));
    std::vector<Domain::value_type> array = {10, 20, 30};
    IntElementConstraint c(index, array, result);

    // Should not throw or cause issues
    c.rewind_to(0);
    c.rewind_to(5);
    c.rewind_to(-1);

    // Constraint should still work
    REQUIRE(c.is_initially_inconsistent() == false);
}

TEST_CASE("IntElementConstraint with duplicate values in array", "[constraint][int_element]") {
    // array = {10, 20, 10} - value 10 appears at indices 1 and 3 (1-based)
    auto index = std::make_shared<Variable>("index", Domain(1, 3));
    auto result = std::make_shared<Variable>("result", Domain(10, 10));  // fixed to 10
    std::vector<Domain::value_type> array = {10, 20, 10};
    IntElementConstraint c(index, array, result);

    // Should be consistent - index can be 1 or 3
    REQUIRE(c.is_initially_inconsistent() == false);
}

TEST_CASE("IntElementConstraint solver integration", "[solver][int_element]") {
    SECTION("find one solution - basic") {
        Model model;

        // array = {10, 20, 30} (1-based: array[1]=10, array[2]=20, array[3]=30)
        auto index = model.create_variable("index", 1, 3);
        auto result = model.create_variable("result", 10, 30);


        std::vector<Domain::value_type> array = {10, 20, 30};
        model.add_constraint(std::make_shared<IntElementConstraint>(index, array, result));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());

        // Verify array[index] = result
        auto idx = solution->at("index");
        auto res = solution->at("result");
        REQUIRE(idx >= 1);
        REQUIRE(idx <= 3);
        REQUIRE(array[static_cast<size_t>(idx - 1)] == res);
    }

    SECTION("find all solutions - basic") {
        Model model;

        // array = {10, 20, 30}
        auto index = model.create_variable("index", 1, 3);
        auto result = model.create_variable("result", 10, 30);


        std::vector<Domain::value_type> array = {10, 20, 30};
        model.add_constraint(std::make_shared<IntElementConstraint>(index, array, result));

        Solver solver;
        std::vector<Solution> solutions;
        size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        // Should have exactly 3 solutions: (1,10), (2,20), (3,30)
        REQUIRE(count == 3);

        for (const auto& sol : solutions) {
            auto idx = sol.at("index");
            auto res = sol.at("result");
            REQUIRE(array[static_cast<size_t>(idx - 1)] == res);
        }
    }

    SECTION("find all solutions - with duplicate values") {
        Model model;

        // array = {10, 20, 10} - value 10 appears twice
        auto index = model.create_variable("index", 1, 3);
        auto result = model.create_variable("result", 10, 20);


        std::vector<Domain::value_type> array = {10, 20, 10};
        model.add_constraint(std::make_shared<IntElementConstraint>(index, array, result));

        Solver solver;
        std::vector<Solution> solutions;
        size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        // Should have exactly 3 solutions: (1,10), (2,20), (3,10)
        REQUIRE(count == 3);

        for (const auto& sol : solutions) {
            auto idx = sol.at("index");
            auto res = sol.at("result");
            REQUIRE(array[static_cast<size_t>(idx - 1)] == res);
        }
    }

    SECTION("no solution - contradictory result domain") {
        Model model;

        // array = {10, 20, 30}, but result domain is {40, 50}
        auto index = model.create_variable("index", 1, 3);
        auto result = model.create_variable("result", 40, 50);


        std::vector<Domain::value_type> array = {10, 20, 30};
        model.add_constraint(std::make_shared<IntElementConstraint>(index, array, result));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE_FALSE(solution.has_value());
    }

    SECTION("partial assignment - index fixed") {
        Model model;

        // array = {10, 20, 30}, index fixed to 2
        auto index = model.create_variable("index", 2);  // fixed to 2
        auto result = model.create_variable("result", 10, 30);


        std::vector<Domain::value_type> array = {10, 20, 30};
        model.add_constraint(std::make_shared<IntElementConstraint>(index, array, result));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("index") == 2);
        REQUIRE(solution->at("result") == 20);  // array[2-1] = 20
    }

    SECTION("partial assignment - result fixed") {
        Model model;

        // array = {10, 20, 30}, result fixed to 20
        auto index = model.create_variable("index", 1, 3);
        auto result = model.create_variable("result", 20);  // fixed to 20


        std::vector<Domain::value_type> array = {10, 20, 30};
        model.add_constraint(std::make_shared<IntElementConstraint>(index, array, result));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("index") == 2);  // only index 2 has value 20
        REQUIRE(solution->at("result") == 20);
    }

    SECTION("with other constraints - sum constraint") {
        Model model;

        // array = {10, 20, 30}
        // index + result = 22 (only solution: index=2, result=20)
        auto index = model.create_variable("index", 1, 3);
        auto result = model.create_variable("result", 10, 30);


        std::vector<Domain::value_type> array = {10, 20, 30};
        model.add_constraint(std::make_shared<IntElementConstraint>(index, array, result));
        model.add_constraint(std::make_shared<IntLinEqConstraint>(
            std::vector<int64_t>{1, 1},
            std::vector<VariablePtr>{index, result},
            22
        ));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("index") == 2);
        REQUIRE(solution->at("result") == 20);
    }
}

TEST_CASE("IntElementConstraint 0-based index", "[solver][int_element]") {
    Model model;

    // array = {10, 20, 30} (0-based: array[0]=10, array[1]=20, array[2]=30)
    auto index = model.create_variable("index", 0, 2);
    auto result = model.create_variable("result", 10, 30);


    std::vector<Domain::value_type> array = {10, 20, 30};
    model.add_constraint(std::make_shared<IntElementConstraint>(index, array, result, true));  // zero_based = true

    Solver solver;
    std::vector<Solution> solutions;
    size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    // Should have exactly 3 solutions: (0,10), (1,20), (2,30)
    REQUIRE(count == 3);

    for (const auto& sol : solutions) {
        auto idx = sol.at("index");
        auto res = sol.at("result");
        REQUIRE(array[static_cast<size_t>(idx)] == res);  // 0-based, no offset
    }
}

TEST_CASE("IntElementConstraint bounds propagation", "[solver][int_element]") {
    SECTION("forward: index bounds narrow result bounds") {
        Model model;

        // array = {5, 100, 3, 50, 7} (1-based)
        // index ∈ [2, 4] → array[1..3] = {100, 3, 50}
        // result should be bounded to [3, 100]
        auto index = model.create_variable("index", 2, 4);
        auto result = model.create_variable("result", 0, 200);

        std::vector<Domain::value_type> array = {5, 100, 3, 50, 7};
        model.add_constraint(std::make_shared<IntElementConstraint>(index, array, result));

        Solver solver;
        std::vector<Solution> solutions;
        size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        REQUIRE(count == 3);
        for (const auto& sol : solutions) {
            auto idx = sol.at("index");
            auto res = sol.at("result");
            REQUIRE(array[static_cast<size_t>(idx - 1)] == res);
        }
    }

    SECTION("reverse: result.min narrows index bounds") {
        Model model;

        // array = {1, 2, 3, 10, 20} (1-based)
        // result ∈ [10, 100]
        // Only index 4 and 5 have values >= 10
        auto index = model.create_variable("index", 1, 5);
        auto result = model.create_variable("result", 10, 100);

        std::vector<Domain::value_type> array = {1, 2, 3, 10, 20};
        model.add_constraint(std::make_shared<IntElementConstraint>(index, array, result));

        Solver solver;
        std::vector<Solution> solutions;
        size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        // Only (4, 10) and (5, 20) are valid
        REQUIRE(count == 2);
        for (const auto& sol : solutions) {
            auto idx = sol.at("index");
            auto res = sol.at("result");
            REQUIRE(array[static_cast<size_t>(idx - 1)] == res);
            REQUIRE(res >= 10);
        }
    }

    SECTION("reverse: result.max narrows index bounds") {
        Model model;

        // array = {100, 50, 3, 2, 1} (1-based)
        // result ∈ [0, 3]
        // Only index 3, 4, 5 have values <= 3
        auto index = model.create_variable("index", 1, 5);
        auto result = model.create_variable("result", 0, 3);

        std::vector<Domain::value_type> array = {100, 50, 3, 2, 1};
        model.add_constraint(std::make_shared<IntElementConstraint>(index, array, result));

        Solver solver;
        std::vector<Solution> solutions;
        size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        // Only (3, 3), (4, 2), (5, 1) are valid
        REQUIRE(count == 3);
        for (const auto& sol : solutions) {
            auto idx = sol.at("index");
            auto res = sol.at("result");
            REQUIRE(array[static_cast<size_t>(idx - 1)] == res);
            REQUIRE(res <= 3);
        }
    }

    SECTION("unsatisfiable via bounds") {
        Model model;

        // array = {1, 2, 3} (1-based)
        // result ∈ [10, 20] → no element has value >= 10
        auto index = model.create_variable("index", 1, 3);
        auto result = model.create_variable("result", 10, 20);

        std::vector<Domain::value_type> array = {1, 2, 3};
        model.add_constraint(std::make_shared<IntElementConstraint>(index, array, result));

        Solver solver;
        auto solution = solver.solve(model);
        REQUIRE_FALSE(solution.has_value());
    }

    SECTION("chain propagation with int_lin_eq") {
        Model model;

        // array = {10, 20, 30, 40, 50} (1-based)
        // result + extra = 60
        // extra ∈ [20, 30] → result ∈ [30, 40]
        // Only index 3 (30) and 4 (40) are valid
        auto index = model.create_variable("index", 1, 5);
        auto result = model.create_variable("result", 0, 100);
        auto extra = model.create_variable("extra", 20, 30);

        std::vector<Domain::value_type> array = {10, 20, 30, 40, 50};
        model.add_constraint(std::make_shared<IntElementConstraint>(index, array, result));
        model.add_constraint(std::make_shared<IntLinEqConstraint>(
            std::vector<int64_t>{1, 1},
            std::vector<VariablePtr>{result, extra},
            60
        ));

        Solver solver;
        std::vector<Solution> solutions;
        size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        // result=30, extra=30 (index=3) and result=40, extra=20 (index=4)
        REQUIRE(count == 2);
        for (const auto& sol : solutions) {
            auto idx = sol.at("index");
            auto res = sol.at("result");
            auto ext = sol.at("extra");
            REQUIRE(array[static_cast<size_t>(idx - 1)] == res);
            REQUIRE(res + ext == 60);
        }
    }

    SECTION("0-based bounds propagation") {
        Model model;

        // array = {5, 100, 3, 50, 7} (0-based)
        // index ∈ [1, 3] → array[1..3] = {100, 3, 50}
        auto index = model.create_variable("index", 1, 3);
        auto result = model.create_variable("result", 0, 200);

        std::vector<Domain::value_type> array = {5, 100, 3, 50, 7};
        model.add_constraint(std::make_shared<IntElementConstraint>(index, array, result, true));

        Solver solver;
        std::vector<Solution> solutions;
        size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        REQUIRE(count == 3);
        for (const auto& sol : solutions) {
            auto idx = sol.at("index");
            auto res = sol.at("result");
            REQUIRE(array[static_cast<size_t>(idx)] == res);
        }
    }

    SECTION("reverse: result.min with index starting at 0-based 0 (p_max_ boundary)") {
        // array[0] >= result.min のとき、p_max_ 二分探索が mid=0 の else 分岐に入る
        // 修正前は size_t アンダーフロー (right = 0 - 1 = SIZE_MAX) で segfault
        Model model;

        // array = {10, 5, 3, 8} (1-based: index 1..4)
        // result ∈ [4, 10] → array[0]=10 >= 4 なので p_max_[0]=10 >= 4
        // → p_max_ 二分探索で mid=0, else 分岐 (right = mid - 1)
        auto index = model.create_variable("index", 1, 4);
        auto result = model.create_variable("result", 4, 10);

        std::vector<Domain::value_type> array = {10, 5, 3, 8};
        model.add_constraint(std::make_shared<IntElementConstraint>(index, array, result));

        Solver solver;
        std::vector<Solution> solutions;
        size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        // array[1]=10, array[4]=8 are in [4,10]; array[2]=5 is in [4,10] too
        // array[3]=3 is NOT in [4,10]
        REQUIRE(count == 3);
        for (const auto& sol : solutions) {
            auto idx = sol.at("index");
            auto res = sol.at("result");
            REQUIRE(array[static_cast<size_t>(idx - 1)] == res);
            REQUIRE(res >= 4);
            REQUIRE(res <= 10);
        }
    }

    SECTION("reverse: result.max with index starting at 0-based 0 (p_min_ boundary)") {
        // array[0] <= result.max のとき、p_min_ 二分探索が mid=0 の else 分岐に入る
        Model model;

        // array = {3, 50, 100, 5} (1-based: index 1..4)
        // result ∈ [1, 5] → array[0]=3 <= 5 なので p_min_[0]=3 <= 5
        // → p_min_ 二分探索で mid=0, else 分岐 (right = mid - 1)
        auto index = model.create_variable("index", 1, 4);
        auto result = model.create_variable("result", 1, 5);

        std::vector<Domain::value_type> array = {3, 50, 100, 5};
        model.add_constraint(std::make_shared<IntElementConstraint>(index, array, result));

        Solver solver;
        std::vector<Solution> solutions;
        size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        // array[1]=3, array[4]=5 are in [1,5]; array[2]=50, array[3]=100 are NOT
        REQUIRE(count == 2);
        for (const auto& sol : solutions) {
            auto idx = sol.at("index");
            auto res = sol.at("result");
            REQUIRE(array[static_cast<size_t>(idx - 1)] == res);
            REQUIRE(res <= 5);
        }
    }

    SECTION("single element array") {
        // n=1: 全ての二分探索が lo_0=0, hi_0=0 で mid=0 を使う
        Model model;

        auto index = model.create_variable("index", 1, 1);
        auto result = model.create_variable("result", 0, 100);

        std::vector<Domain::value_type> array = {42};
        model.add_constraint(std::make_shared<IntElementConstraint>(index, array, result));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("index") == 1);
        REQUIRE(solution->at("result") == 42);
    }

    SECTION("0-based single element with chain constraint") {
        // 0-based + n=1 + 連鎖伝播
        Model model;

        auto index = model.create_variable("index", 0, 0);
        auto result = model.create_variable("result", 0, 100);
        auto other = model.create_variable("other", 0, 100);

        std::vector<Domain::value_type> array = {7};
        model.add_constraint(std::make_shared<IntElementConstraint>(index, array, result, true));
        // result + other = 10 → result=7, other=3
        model.add_constraint(std::make_shared<IntLinEqConstraint>(
            std::vector<int64_t>{1, 1},
            std::vector<VariablePtr>{result, other},
            10
        ));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("result") == 7);
        REQUIRE(solution->at("other") == 3);
    }

    SECTION("two element array - reverse propagation at boundaries") {
        // n=2: 二分探索のエッジケース (left=0, right=1 → mid=0)
        Model model;

        // array = {100, 1} (1-based)
        // result ∈ [50, 200] → only index 1 has value >= 50
        auto index = model.create_variable("index", 1, 2);
        auto result = model.create_variable("result", 50, 200);

        std::vector<Domain::value_type> array = {100, 1};
        model.add_constraint(std::make_shared<IntElementConstraint>(index, array, result));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("index") == 1);
        REQUIRE(solution->at("result") == 100);
    }
}

TEST_CASE("CircuitConstraint with partial assignment", "[solver][circuit]") {
    SECTION("one variable fixed - consistent") {
        Model model;
        std::vector<VariablePtr> vars;
        for (int i = 0; i < 3; ++i) {
            auto var = model.create_variable("x" + std::to_string(i), 0, 2);
            vars.push_back(var);
        }

        // Fix x[0] = 1
        model.instantiate(0, vars[0]->id(), 1);

        model.add_constraint(std::make_shared<CircuitConstraint>(vars));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("x0") == 1);

        // Verify valid circuit
        std::vector<int64_t> values = {
            solution->at("x0"),
            solution->at("x1"),
            solution->at("x2")
        };

        std::set<int64_t> visited;
        int64_t current = 0;
        for (int i = 0; i < 3; ++i) {
            REQUIRE(visited.find(current) == visited.end());
            visited.insert(current);
            current = values[static_cast<size_t>(current)];
        }
        REQUIRE(current == 0);
    }

    SECTION("two variables fixed - consistent") {
        Model model;
        std::vector<VariablePtr> vars;
        for (int i = 0; i < 3; ++i) {
            auto var = model.create_variable("x" + std::to_string(i), 0, 2);
            vars.push_back(var);
        }

        // Fix x[0] = 1, x[1] = 2 (forces x[2] = 0 for valid circuit)
        model.instantiate(0, vars[0]->id(), 1);
        model.instantiate(0, vars[1]->id(), 2);

        model.add_constraint(std::make_shared<CircuitConstraint>(vars));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("x0") == 1);
        REQUIRE(solution->at("x1") == 2);
        REQUIRE(solution->at("x2") == 0);
    }

    SECTION("partial assignment creates subcircuit - inconsistent") {
        Model model;
        std::vector<VariablePtr> vars;
        for (int i = 0; i < 3; ++i) {
            auto var = model.create_variable("x" + std::to_string(i), 0, 2);
            vars.push_back(var);
        }

        // Fix x[0] = 1, x[1] = 0 (creates subcircuit 0 -> 1 -> 0)
        model.instantiate(0, vars[0]->id(), 1);
        model.instantiate(0, vars[1]->id(), 0);

        auto constraint = std::make_shared<CircuitConstraint>(vars);
        model.add_constraint(constraint);

        // Should be initially inconsistent
        REQUIRE(constraint->is_initially_inconsistent() == true);
    }
}

// ============================================================================
// ArrayBoolAndConstraint tests
// ============================================================================

TEST_CASE("ArrayBoolAndConstraint name", "[constraint][array_bool_and]") {
    auto b1 = std::make_shared<Variable>("b1", Domain(0, 1));
    auto b2 = std::make_shared<Variable>("b2", Domain(0, 1));
    auto r = std::make_shared<Variable>("r", Domain(0, 1));
    ArrayBoolAndConstraint c({b1, b2}, r);

    REQUIRE(c.name() == "array_bool_and");
}

TEST_CASE("ArrayBoolAndConstraint variables", "[constraint][array_bool_and]") {
    auto b1 = std::make_shared<Variable>("b1", Domain(0, 1));
    auto b2 = std::make_shared<Variable>("b2", Domain(0, 1));
    auto r = std::make_shared<Variable>("r", Domain(0, 1));
    ArrayBoolAndConstraint c({b1, b2}, r);

    auto vars = c.variables();
    REQUIRE(vars.size() == 3);  // b1, b2, r
}

TEST_CASE("ArrayBoolAndConstraint is_satisfied", "[constraint][array_bool_and]") {
    SECTION("all true - r=1 satisfied") {
        auto b1 = std::make_shared<Variable>("b1", Domain(1, 1));
        auto b2 = std::make_shared<Variable>("b2", Domain(1, 1));
        auto r = std::make_shared<Variable>("r", Domain(1, 1));
        ArrayBoolAndConstraint c({b1, b2}, r);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("one false - r=0 satisfied") {
        auto b1 = std::make_shared<Variable>("b1", Domain(1, 1));
        auto b2 = std::make_shared<Variable>("b2", Domain(0, 0));
        auto r = std::make_shared<Variable>("r", Domain(0, 0));
        ArrayBoolAndConstraint c({b1, b2}, r);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("one false - r=1 not satisfied") {
        auto b1 = std::make_shared<Variable>("b1", Domain(1, 1));
        auto b2 = std::make_shared<Variable>("b2", Domain(0, 0));
        auto r = std::make_shared<Variable>("r", Domain(1, 1));
        ArrayBoolAndConstraint c({b1, b2}, r);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("not all assigned - unknown") {
        auto b1 = std::make_shared<Variable>("b1", Domain(0, 1));
        auto b2 = std::make_shared<Variable>("b2", Domain(1, 1));
        auto r = std::make_shared<Variable>("r", Domain(0, 1));
        ArrayBoolAndConstraint c({b1, b2}, r);

        REQUIRE_FALSE(c.is_satisfied().has_value());
    }
}

TEST_CASE("ArrayBoolAndConstraint on_final_instantiate", "[constraint][array_bool_and]") {
    SECTION("all true - r=1") {
        auto b1 = std::make_shared<Variable>("b1", Domain(1, 1));
        auto b2 = std::make_shared<Variable>("b2", Domain(1, 1));
        auto r = std::make_shared<Variable>("r", Domain(1, 1));
        ArrayBoolAndConstraint c({b1, b2}, r);

        REQUIRE(c.on_final_instantiate() == true);
    }

    SECTION("one false - r=0") {
        auto b1 = std::make_shared<Variable>("b1", Domain(0, 0));
        auto b2 = std::make_shared<Variable>("b2", Domain(1, 1));
        auto r = std::make_shared<Variable>("r", Domain(0, 0));
        ArrayBoolAndConstraint c({b1, b2}, r);

        REQUIRE(c.on_final_instantiate() == true);
    }
}

TEST_CASE("ArrayBoolAndConstraint solver integration", "[constraint][array_bool_and]") {
    SECTION("r=1 forces all bi=1") {
        Model model;
        auto b1 = model.create_variable("b1", 0, 1);
        auto b2 = model.create_variable("b2", 0, 1);
        auto b3 = model.create_variable("b3", 0, 1);
        auto r = model.create_variable("r", 1);  // r = 1 fixed

        model.add_constraint(std::make_shared<ArrayBoolAndConstraint>(
            std::vector<VariablePtr>{b1, b2, b3}, r));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("b1") == 1);
        REQUIRE(solution->at("b2") == 1);
        REQUIRE(solution->at("b3") == 1);
        REQUIRE(solution->at("r") == 1);
    }

    SECTION("r=0, b1=b2=1 forces b3=0") {
        Model model;
        auto b1 = model.create_variable("b1", 1);  // fixed
        auto b2 = model.create_variable("b2", 1);  // fixed
        auto b3 = model.create_variable("b3", 0, 1);
        auto r = model.create_variable("r", 0);  // r = 0 fixed

        model.add_constraint(std::make_shared<ArrayBoolAndConstraint>(
            std::vector<VariablePtr>{b1, b2, b3}, r));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("b3") == 0);
    }

    SECTION("all solutions enumeration") {
        Model model;
        auto b1 = model.create_variable("b1", 0, 1);
        auto b2 = model.create_variable("b2", 0, 1);
        auto r = model.create_variable("r", 0, 1);

        model.add_constraint(std::make_shared<ArrayBoolAndConstraint>(
            std::vector<VariablePtr>{b1, b2}, r));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution&) { return true; });

        REQUIRE(count == 4);  // (0,0,0), (0,1,0), (1,0,0), (1,1,1)
    }

    SECTION("all bi=1, r=0 is UNSAT (propagation bug regression)") {
        Model model;
        auto b1 = model.create_variable("b1", 1);  // fixed to 1
        auto b2 = model.create_variable("b2", 1);  // fixed to 1
        auto b3 = model.create_variable("b3", 1);  // fixed to 1
        auto r = model.create_variable("r", 0);     // fixed to 0

        model.add_constraint(std::make_shared<ArrayBoolAndConstraint>(
            std::vector<VariablePtr>{b1, b2, b3}, r));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE_FALSE(solution.has_value());  // UNSAT
    }

    SECTION("all bi=1 forces r=1 even when r is free") {
        Model model;
        auto b1 = model.create_variable("b1", 1);  // fixed to 1
        auto b2 = model.create_variable("b2", 1);  // fixed to 1
        auto b3 = model.create_variable("b3", 1);  // fixed to 1
        auto r = model.create_variable("r", 0, 1);

        model.add_constraint(std::make_shared<ArrayBoolAndConstraint>(
            std::vector<VariablePtr>{b1, b2, b3}, r));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("r") == 1);
    }
}

// ============================================================================
// ArrayBoolOrConstraint tests
// ============================================================================

TEST_CASE("ArrayBoolOrConstraint name", "[constraint][array_bool_or]") {
    auto b1 = std::make_shared<Variable>("b1", Domain(0, 1));
    auto b2 = std::make_shared<Variable>("b2", Domain(0, 1));
    auto r = std::make_shared<Variable>("r", Domain(0, 1));
    ArrayBoolOrConstraint c({b1, b2}, r);

    REQUIRE(c.name() == "array_bool_or");
}

TEST_CASE("ArrayBoolOrConstraint is_satisfied", "[constraint][array_bool_or]") {
    SECTION("one true - r=1 satisfied") {
        auto b1 = std::make_shared<Variable>("b1", Domain(0, 0));
        auto b2 = std::make_shared<Variable>("b2", Domain(1, 1));
        auto r = std::make_shared<Variable>("r", Domain(1, 1));
        ArrayBoolOrConstraint c({b1, b2}, r);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("all false - r=0 satisfied") {
        auto b1 = std::make_shared<Variable>("b1", Domain(0, 0));
        auto b2 = std::make_shared<Variable>("b2", Domain(0, 0));
        auto r = std::make_shared<Variable>("r", Domain(0, 0));
        ArrayBoolOrConstraint c({b1, b2}, r);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("all false - r=1 not satisfied") {
        auto b1 = std::make_shared<Variable>("b1", Domain(0, 0));
        auto b2 = std::make_shared<Variable>("b2", Domain(0, 0));
        auto r = std::make_shared<Variable>("r", Domain(1, 1));
        ArrayBoolOrConstraint c({b1, b2}, r);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }
}

TEST_CASE("ArrayBoolOrConstraint solver integration", "[constraint][array_bool_or]") {
    SECTION("r=0 forces all bi=0") {
        Model model;
        auto b1 = model.create_variable("b1", 0, 1);
        auto b2 = model.create_variable("b2", 0, 1);
        auto r = model.create_variable("r", 0);  // r = 0 fixed

        model.add_constraint(std::make_shared<ArrayBoolOrConstraint>(
            std::vector<VariablePtr>{b1, b2}, r));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("b1") == 0);
        REQUIRE(solution->at("b2") == 0);
    }

    SECTION("r=1, b1=0 forces b2=1 (2WL propagation)") {
        Model model;
        auto b1 = model.create_variable("b1", 0);  // fixed to 0
        auto b2 = model.create_variable("b2", 0, 1);
        auto r = model.create_variable("r", 1);  // r = 1 fixed

        model.add_constraint(std::make_shared<ArrayBoolOrConstraint>(
            std::vector<VariablePtr>{b1, b2}, r));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("b2") == 1);
    }

    SECTION("all solutions enumeration") {
        Model model;
        auto b1 = model.create_variable("b1", 0, 1);
        auto b2 = model.create_variable("b2", 0, 1);
        auto r = model.create_variable("r", 0, 1);

        model.add_constraint(std::make_shared<ArrayBoolOrConstraint>(
            std::vector<VariablePtr>{b1, b2}, r));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution&) { return true; });

        REQUIRE(count == 4);  // (0,0,0), (0,1,1), (1,0,1), (1,1,1)
    }

    SECTION("all bi=0, r=1 is UNSAT (propagation bug regression)") {
        Model model;
        auto b1 = model.create_variable("b1", 0);  // fixed to 0
        auto b2 = model.create_variable("b2", 0);  // fixed to 0
        auto b3 = model.create_variable("b3", 0);  // fixed to 0
        auto r = model.create_variable("r", 1);     // fixed to 1

        model.add_constraint(std::make_shared<ArrayBoolOrConstraint>(
            std::vector<VariablePtr>{b1, b2, b3}, r));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE_FALSE(solution.has_value());  // UNSAT
    }

    SECTION("r=1, b1=b2=0 forces b3=1") {
        Model model;
        auto b1 = model.create_variable("b1", 0);  // fixed to 0
        auto b2 = model.create_variable("b2", 0);  // fixed to 0
        auto b3 = model.create_variable("b3", 0, 1);
        auto r = model.create_variable("r", 1);     // fixed to 1

        model.add_constraint(std::make_shared<ArrayBoolOrConstraint>(
            std::vector<VariablePtr>{b1, b2, b3}, r));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("b3") == 1);
    }
}

// ============================================================================
// BoolClauseConstraint tests
// ============================================================================

TEST_CASE("BoolClauseConstraint name", "[constraint][bool_clause]") {
    auto p1 = std::make_shared<Variable>("p1", Domain(0, 1));
    auto n1 = std::make_shared<Variable>("n1", Domain(0, 1));
    BoolClauseConstraint c({p1}, {n1});

    REQUIRE(c.name() == "bool_clause");
}

TEST_CASE("BoolClauseConstraint is_satisfied", "[constraint][bool_clause]") {
    SECTION("positive literal true - satisfied") {
        // p1 ∨ ¬n1, p1=1 → satisfied
        auto p1 = std::make_shared<Variable>("p1", Domain(1, 1));
        auto n1 = std::make_shared<Variable>("n1", Domain(1, 1));
        BoolClauseConstraint c({p1}, {n1});

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("negative literal false - satisfied") {
        // p1 ∨ ¬n1, p1=0, n1=0 → satisfied (because ¬n1 = true)
        auto p1 = std::make_shared<Variable>("p1", Domain(0, 0));
        auto n1 = std::make_shared<Variable>("n1", Domain(0, 0));
        BoolClauseConstraint c({p1}, {n1});

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("all falsified - not satisfied") {
        // p1 ∨ ¬n1, p1=0, n1=1 → not satisfied
        auto p1 = std::make_shared<Variable>("p1", Domain(0, 0));
        auto n1 = std::make_shared<Variable>("n1", Domain(1, 1));
        BoolClauseConstraint c({p1}, {n1});

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("undetermined") {
        auto p1 = std::make_shared<Variable>("p1", Domain(0, 1));
        auto n1 = std::make_shared<Variable>("n1", Domain(0, 1));
        BoolClauseConstraint c({p1}, {n1});

        REQUIRE_FALSE(c.is_satisfied().has_value());
    }
}

TEST_CASE("BoolClauseConstraint unit propagation", "[constraint][bool_clause]") {
    SECTION("single positive literal - forces true") {
        // p1 (clause with only positive), p1 must be 1
        Model model;
        auto p1 = model.create_variable("p1", 0, 1);

        model.add_constraint(std::make_shared<BoolClauseConstraint>(
            std::vector<VariablePtr>{p1}, std::vector<VariablePtr>{}));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("p1") == 1);
    }

    SECTION("single negative literal - forces false") {
        // ¬n1 (clause with only negative), n1 must be 0
        Model model;
        auto n1 = model.create_variable("n1", 0, 1);

        model.add_constraint(std::make_shared<BoolClauseConstraint>(
            std::vector<VariablePtr>{}, std::vector<VariablePtr>{n1}));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("n1") == 0);
    }

    SECTION("2WL propagation - last unset literal") {
        // p1 ∨ p2 ∨ p3, p1=0, p2=0 → p3 must be 1
        Model model;
        auto p1 = model.create_variable("p1", 0);  // fixed to 0
        auto p2 = model.create_variable("p2", 0);  // fixed to 0
        auto p3 = model.create_variable("p3", 0, 1);

        model.add_constraint(std::make_shared<BoolClauseConstraint>(
            std::vector<VariablePtr>{p1, p2, p3}, std::vector<VariablePtr>{}));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("p3") == 1);
    }

    SECTION("mixed clause propagation") {
        // p1 ∨ ¬n1 ∨ ¬n2, p1=0, n1=1 → n2 must be 0
        Model model;
        auto p1 = model.create_variable("p1", 0);  // fixed to 0
        auto n1 = model.create_variable("n1", 1);  // fixed to 1 (so ¬n1=0)
        auto n2 = model.create_variable("n2", 0, 1);

        model.add_constraint(std::make_shared<BoolClauseConstraint>(
            std::vector<VariablePtr>{p1}, std::vector<VariablePtr>{n1, n2}));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("n2") == 0);
    }
}

TEST_CASE("BoolClauseConstraint solver integration", "[constraint][bool_clause]") {
    SECTION("empty clause is unsatisfiable") {
        Model model;
        model.add_constraint(std::make_shared<BoolClauseConstraint>(
            std::vector<VariablePtr>{}, std::vector<VariablePtr>{}));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE_FALSE(solution.has_value());
    }

    SECTION("conflicting clauses") {
        // p1 AND ¬p1 is unsatisfiable
        Model model;
        auto p1 = model.create_variable("p1", 0, 1);

        // clause 1: p1 (p1 must be true)
        model.add_constraint(std::make_shared<BoolClauseConstraint>(
            std::vector<VariablePtr>{p1}, std::vector<VariablePtr>{}));
        // clause 2: ¬p1 (p1 must be false)
        model.add_constraint(std::make_shared<BoolClauseConstraint>(
            std::vector<VariablePtr>{}, std::vector<VariablePtr>{p1}));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE_FALSE(solution.has_value());
    }

    SECTION("all solutions for simple clause") {
        // p1 ∨ p2 has 3 solutions: (0,1), (1,0), (1,1)
        Model model;
        auto p1 = model.create_variable("p1", 0, 1);
        auto p2 = model.create_variable("p2", 0, 1);

        model.add_constraint(std::make_shared<BoolClauseConstraint>(
            std::vector<VariablePtr>{p1, p2}, std::vector<VariablePtr>{}));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution&) { return true; });

        REQUIRE(count == 3);
    }

    SECTION("backtracking with multiple clauses") {
        // (p1 ∨ p2) ∧ (¬p1 ∨ p3) ∧ (¬p2 ∨ ¬p3)
        // Solutions: (0,1,0), (1,0,1) = 2 solutions
        Model model;
        auto p1 = model.create_variable("p1", 0, 1);
        auto p2 = model.create_variable("p2", 0, 1);
        auto p3 = model.create_variable("p3", 0, 1);


        // p1 ∨ p2
        model.add_constraint(std::make_shared<BoolClauseConstraint>(
            std::vector<VariablePtr>{p1, p2}, std::vector<VariablePtr>{}));
        // ¬p1 ∨ p3 (pos={p3}, neg={p1})
        model.add_constraint(std::make_shared<BoolClauseConstraint>(
            std::vector<VariablePtr>{p3}, std::vector<VariablePtr>{p1}));
        // ¬p2 ∨ ¬p3 (pos={}, neg={p2, p3})
        model.add_constraint(std::make_shared<BoolClauseConstraint>(
            std::vector<VariablePtr>{}, std::vector<VariablePtr>{p2, p3}));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution&) { return true; });

        REQUIRE(count == 2);
    }
}

// ============================================================================
// ArrayIntMaximumConstraint tests
// ============================================================================

TEST_CASE("ArrayIntMaximumConstraint name", "[constraint][array_int_maximum]") {
    auto m = std::make_shared<Variable>("m", Domain(1, 5));
    auto x1 = std::make_shared<Variable>("x1", Domain(1, 5));
    auto x2 = std::make_shared<Variable>("x2", Domain(1, 5));
    ArrayIntMaximumConstraint c(m, {x1, x2});

    REQUIRE(c.name() == "array_int_maximum");
}

TEST_CASE("ArrayIntMaximumConstraint is_satisfied", "[constraint][array_int_maximum]") {
    SECTION("satisfied when m equals max") {
        auto m = std::make_shared<Variable>("m", Domain(3, 3));
        auto x1 = std::make_shared<Variable>("x1", Domain(1, 1));
        auto x2 = std::make_shared<Variable>("x2", Domain(3, 3));
        auto x3 = std::make_shared<Variable>("x3", Domain(2, 2));
        ArrayIntMaximumConstraint c(m, {x1, x2, x3});

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("not satisfied when m differs from max") {
        auto m = std::make_shared<Variable>("m", Domain(2, 2));
        auto x1 = std::make_shared<Variable>("x1", Domain(1, 1));
        auto x2 = std::make_shared<Variable>("x2", Domain(3, 3));
        ArrayIntMaximumConstraint c(m, {x1, x2});

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("undetermined when variables unassigned") {
        auto m = std::make_shared<Variable>("m", Domain(1, 5));
        auto x1 = std::make_shared<Variable>("x1", Domain(1, 5));
        auto x2 = std::make_shared<Variable>("x2", Domain(1, 5));
        ArrayIntMaximumConstraint c(m, {x1, x2});

        REQUIRE_FALSE(c.is_satisfied().has_value());
    }
}

TEST_CASE("ArrayIntMaximumConstraint solver integration", "[constraint][array_int_maximum]") {
    SECTION("finds maximum value") {
        Model model;
        auto m = model.create_variable("m", 1, 10);
        auto x1 = model.create_variable("x1", 3);  // fixed
        auto x2 = model.create_variable("x2", 7);  // fixed
        auto x3 = model.create_variable("x3", 5);  // fixed

        model.add_constraint(std::make_shared<ArrayIntMaximumConstraint>(m, std::vector<VariablePtr>{x1, x2, x3}));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("m") == 7);
    }

    SECTION("propagates constraints correctly") {
        Model model;
        auto m = model.create_variable("m", 5);  // m fixed to 5
        auto x1 = model.create_variable("x1", 1, 10);
        auto x2 = model.create_variable("x2", 1, 10);

        model.add_constraint(std::make_shared<ArrayIntMaximumConstraint>(m, std::vector<VariablePtr>{x1, x2}));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("m") == 5);
        // At least one of x1, x2 must be 5
        REQUIRE((solution->at("x1") == 5 || solution->at("x2") == 5));
        // Both must be <= 5
        REQUIRE(solution->at("x1") <= 5);
        REQUIRE(solution->at("x2") <= 5);
    }

    SECTION("detects unsatisfiable") {
        Model model;
        auto m = model.create_variable("m", 3);  // m fixed to 3
        auto x1 = model.create_variable("x1", 5, 10);  // x1 >= 5 > m
        auto x2 = model.create_variable("x2", 1, 2);

        model.add_constraint(std::make_shared<ArrayIntMaximumConstraint>(m, std::vector<VariablePtr>{x1, x2}));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE_FALSE(solution.has_value());
    }
}

// ============================================================================
// ArrayIntMinimumConstraint tests
// ============================================================================

TEST_CASE("ArrayIntMinimumConstraint name", "[constraint][array_int_minimum]") {
    auto m = std::make_shared<Variable>("m", Domain(1, 5));
    auto x1 = std::make_shared<Variable>("x1", Domain(1, 5));
    auto x2 = std::make_shared<Variable>("x2", Domain(1, 5));
    ArrayIntMinimumConstraint c(m, {x1, x2});

    REQUIRE(c.name() == "array_int_minimum");
}

TEST_CASE("ArrayIntMinimumConstraint is_satisfied", "[constraint][array_int_minimum]") {
    SECTION("satisfied when m equals min") {
        auto m = std::make_shared<Variable>("m", Domain(1, 1));
        auto x1 = std::make_shared<Variable>("x1", Domain(1, 1));
        auto x2 = std::make_shared<Variable>("x2", Domain(3, 3));
        auto x3 = std::make_shared<Variable>("x3", Domain(2, 2));
        ArrayIntMinimumConstraint c(m, {x1, x2, x3});

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("not satisfied when m differs from min") {
        auto m = std::make_shared<Variable>("m", Domain(2, 2));
        auto x1 = std::make_shared<Variable>("x1", Domain(1, 1));
        auto x2 = std::make_shared<Variable>("x2", Domain(3, 3));
        ArrayIntMinimumConstraint c(m, {x1, x2});

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }
}

TEST_CASE("ArrayIntMinimumConstraint solver integration", "[constraint][array_int_minimum]") {
    SECTION("finds minimum value") {
        Model model;
        auto m = model.create_variable("m", 1, 10);
        auto x1 = model.create_variable("x1", 3);  // fixed
        auto x2 = model.create_variable("x2", 7);  // fixed
        auto x3 = model.create_variable("x3", 5);  // fixed

        model.add_constraint(std::make_shared<ArrayIntMinimumConstraint>(m, std::vector<VariablePtr>{x1, x2, x3}));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("m") == 3);
    }

    SECTION("propagates constraints correctly") {
        Model model;
        auto m = model.create_variable("m", 5);  // m fixed to 5
        auto x1 = model.create_variable("x1", 1, 10);
        auto x2 = model.create_variable("x2", 1, 10);

        model.add_constraint(std::make_shared<ArrayIntMinimumConstraint>(m, std::vector<VariablePtr>{x1, x2}));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("m") == 5);
        // At least one of x1, x2 must be 5
        REQUIRE((solution->at("x1") == 5 || solution->at("x2") == 5));
        // Both must be >= 5
        REQUIRE(solution->at("x1") >= 5);
        REQUIRE(solution->at("x2") >= 5);
    }
}

// ============================================================================
// IntTimesConstraint tests
// ============================================================================

TEST_CASE("IntTimesConstraint name", "[constraint][int_times]") {
    auto x = std::make_shared<Variable>("x", Domain(1, 5));
    auto y = std::make_shared<Variable>("y", Domain(1, 5));
    auto z = std::make_shared<Variable>("z", Domain(1, 25));
    IntTimesConstraint c(x, y, z);

    REQUIRE(c.name() == "int_times");
}

TEST_CASE("IntTimesConstraint is_satisfied", "[constraint][int_times]") {
    SECTION("satisfied: 2 * 3 = 6") {
        auto x = std::make_shared<Variable>("x", Domain(2, 2));
        auto y = std::make_shared<Variable>("y", Domain(3, 3));
        auto z = std::make_shared<Variable>("z", Domain(6, 6));
        IntTimesConstraint c(x, y, z);

        auto satisfied = c.is_satisfied();
        REQUIRE(satisfied.has_value());
        REQUIRE(satisfied.value() == true);
    }

    SECTION("not satisfied: 2 * 3 != 5") {
        auto x = std::make_shared<Variable>("x", Domain(2, 2));
        auto y = std::make_shared<Variable>("y", Domain(3, 3));
        auto z = std::make_shared<Variable>("z", Domain(5, 5));
        IntTimesConstraint c(x, y, z);

        auto satisfied = c.is_satisfied();
        REQUIRE(satisfied.has_value());
        REQUIRE(satisfied.value() == false);
    }

    SECTION("unassigned") {
        auto x = std::make_shared<Variable>("x", Domain(1, 5));
        auto y = std::make_shared<Variable>("y", Domain(1, 5));
        auto z = std::make_shared<Variable>("z", Domain(1, 25));
        IntTimesConstraint c(x, y, z);

        REQUIRE_FALSE(c.is_satisfied().has_value());
    }
}

TEST_CASE("IntTimesConstraint solver integration", "[constraint][int_times]") {
    SECTION("basic multiplication") {
        Model model;
        auto x = model.create_variable("x", 3);  // x = 3
        auto y = model.create_variable("y", 4);  // y = 4
        auto z = model.create_variable("z", 1, 20);  // z to be determined

        model.add_constraint(std::make_shared<IntTimesConstraint>(x, y, z));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("x") == 3);
        REQUIRE(solution->at("y") == 4);
        REQUIRE(solution->at("z") == 12);  // 3 * 4 = 12
    }

    SECTION("find factors") {
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 1, 10);
        auto z = model.create_variable("z", 12);  // z = 12

        model.add_constraint(std::make_shared<IntTimesConstraint>(x, y, z));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution& sol) {
            REQUIRE(sol.at("x") * sol.at("y") == 12);
            return true;
        });

        // Factors of 12 in range 1-10: (1,12 no), (2,6), (3,4), (4,3), (6,2), (12,1 no)
        // Valid: (2,6), (3,4), (4,3), (6,2)
        REQUIRE(count == 4);
    }

    SECTION("multiplication by zero") {
        Model model;
        auto x = model.create_variable("x", 0);  // x = 0
        auto y = model.create_variable("y", 1, 5);
        auto z = model.create_variable("z", 0, 10);

        model.add_constraint(std::make_shared<IntTimesConstraint>(x, y, z));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("z") == 0);  // 0 * y = 0
    }

    SECTION("unsatisfiable") {
        Model model;
        auto x = model.create_variable("x", 2);  // x = 2
        auto y = model.create_variable("y", 3);  // y = 3
        auto z = model.create_variable("z", 5);  // z = 5, but 2 * 3 = 6

        model.add_constraint(std::make_shared<IntTimesConstraint>(x, y, z));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE_FALSE(solution.has_value());
    }

    SECTION("negative numbers") {
        Model model;
        auto x = model.create_variable("x", -3);  // x = -3
        auto y = model.create_variable("y", 4);   // y = 4
        auto z = model.create_variable("z", -20, 20);

        model.add_constraint(std::make_shared<IntTimesConstraint>(x, y, z));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("z") == -12);  // -3 * 4 = -12
    }
}

TEST_CASE("IntTimesConstraint sign combinations", "[constraint][int_times]") {
    SECTION("(+) * (+) = (+)") {
        Model model;
        auto x = model.create_variable("x", 3);
        auto y = model.create_variable("y", 4);
        auto z = model.create_variable("z", -100, 100);
        model.add_constraint(std::make_shared<IntTimesConstraint>(x, y, z));

        Solver solver;
        auto solution = solver.solve(model);
        REQUIRE(solution.has_value());
        REQUIRE(solution->at("z") == 12);
    }

    SECTION("(+) * (-) = (-)") {
        Model model;
        auto x = model.create_variable("x", 3);
        auto y = model.create_variable("y", -4);
        auto z = model.create_variable("z", -100, 100);
        model.add_constraint(std::make_shared<IntTimesConstraint>(x, y, z));

        Solver solver;
        auto solution = solver.solve(model);
        REQUIRE(solution.has_value());
        REQUIRE(solution->at("z") == -12);
    }

    SECTION("(-) * (+) = (-)") {
        Model model;
        auto x = model.create_variable("x", -3);
        auto y = model.create_variable("y", 4);
        auto z = model.create_variable("z", -100, 100);
        model.add_constraint(std::make_shared<IntTimesConstraint>(x, y, z));

        Solver solver;
        auto solution = solver.solve(model);
        REQUIRE(solution.has_value());
        REQUIRE(solution->at("z") == -12);
    }

    SECTION("(-) * (-) = (+)") {
        Model model;
        auto x = model.create_variable("x", -3);
        auto y = model.create_variable("y", -4);
        auto z = model.create_variable("z", -100, 100);
        model.add_constraint(std::make_shared<IntTimesConstraint>(x, y, z));

        Solver solver;
        auto solution = solver.solve(model);
        REQUIRE(solution.has_value());
        REQUIRE(solution->at("z") == 12);
    }

    SECTION("find factors of positive with negative range") {
        Model model;
        auto x = model.create_variable("x", -6, 6);
        auto y = model.create_variable("y", -6, 6);
        auto z = model.create_variable("z", 12);
        model.add_constraint(std::make_shared<IntTimesConstraint>(x, y, z));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution& sol) {
            REQUIRE(sol.at("x") * sol.at("y") == 12);
            return true;
        });
        // (2,6), (3,4), (4,3), (6,2), (-2,-6), (-3,-4), (-4,-3), (-6,-2)
        REQUIRE(count == 8);
    }

    SECTION("find factors of negative with mixed range") {
        Model model;
        auto x = model.create_variable("x", -6, 6);
        auto y = model.create_variable("y", -6, 6);
        auto z = model.create_variable("z", -12);
        model.add_constraint(std::make_shared<IntTimesConstraint>(x, y, z));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution& sol) {
            REQUIRE(sol.at("x") * sol.at("y") == -12);
            return true;
        });
        // (2,-6), (3,-4), (4,-3), (6,-2), (-2,6), (-3,4), (-4,3), (-6,2)
        REQUIRE(count == 8);
    }

    SECTION("zero with mixed range") {
        Model model;
        auto x = model.create_variable("x", -2, 2);
        auto y = model.create_variable("y", -2, 2);
        auto z = model.create_variable("z", 0);
        model.add_constraint(std::make_shared<IntTimesConstraint>(x, y, z));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution& sol) {
            REQUIRE(sol.at("x") * sol.at("y") == 0);
            return true;
        });
        // x=0 with y in {-2,-1,0,1,2}: 5 solutions
        // y=0 with x in {-2,-1,1,2}: 4 solutions (x=0,y=0 already counted)
        // Total: 9
        REQUIRE(count == 9);
    }
}

// ============================================================================
// TableConstraint tests
// ============================================================================

TEST_CASE("TableConstraint name", "[constraint][table]") {
    auto x = std::make_shared<Variable>("x", Domain(1, 3));
    auto y = std::make_shared<Variable>("y", Domain(1, 3));
    // table: (1,2), (2,3), (3,1)
    TableConstraint c({x, y}, {1,2, 2,3, 3,1});
    REQUIRE(c.name() == "table_int");
}

TEST_CASE("TableConstraint variables", "[constraint][table]") {
    auto x = std::make_shared<Variable>("x", Domain(1, 3));
    auto y = std::make_shared<Variable>("y", Domain(1, 3));
    TableConstraint c({x, y}, {1,2, 2,3, 3,1});
    auto vars = c.variables();
    REQUIRE(vars.size() == 2);
}

TEST_CASE("TableConstraint is_satisfied", "[constraint][table]") {
    SECTION("satisfied - tuple in table") {
        auto x = std::make_shared<Variable>("x", Domain(1, 1));
        auto y = std::make_shared<Variable>("y", Domain(2, 2));
        TableConstraint c({x, y}, {1,2, 2,3, 3,1});
        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("violated - tuple not in table") {
        auto x = std::make_shared<Variable>("x", Domain(1, 1));
        auto y = std::make_shared<Variable>("y", Domain(3, 3));
        TableConstraint c({x, y}, {1,2, 2,3, 3,1});
        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("unknown - not fully assigned") {
        auto x = std::make_shared<Variable>("x", Domain(1, 3));
        auto y = std::make_shared<Variable>("y", Domain(2, 2));
        TableConstraint c({x, y}, {1,2, 2,3, 3,1});
        REQUIRE_FALSE(c.is_satisfied().has_value());
    }
}

TEST_CASE("TableConstraint presolve", "[constraint][table]") {
    SECTION("removes values not in any tuple") {
        auto x = std::make_shared<Variable>("x", Domain(1, 5));
        auto y = std::make_shared<Variable>("y", Domain(1, 5));
        // table: (1,2), (2,3)
        TableConstraint c({x, y}, {1,2, 2,3});
        REQUIRE(c.presolve(dummy_model) == true);

        // x should only have {1,2}, y should only have {2,3}
        REQUIRE(x->domain().contains(1));
        REQUIRE(x->domain().contains(2));
        REQUIRE_FALSE(x->domain().contains(3));
        REQUIRE_FALSE(x->domain().contains(4));
        REQUIRE_FALSE(x->domain().contains(5));

        REQUIRE(y->domain().contains(2));
        REQUIRE(y->domain().contains(3));
        REQUIRE_FALSE(y->domain().contains(1));
        REQUIRE_FALSE(y->domain().contains(4));
    }
}

TEST_CASE("TableConstraint empty table", "[constraint][table]") {
    auto x = std::make_shared<Variable>("x", Domain(1, 3));
    auto y = std::make_shared<Variable>("y", Domain(1, 3));
    TableConstraint c({x, y}, {});
    REQUIRE(c.is_initially_inconsistent() == true);
}

TEST_CASE("TableConstraint with Solver", "[constraint][table][solver]") {
    SECTION("find all solutions for 2-var table") {
        // table: (1,2), (2,3), (3,1)
        Model model;
        auto x = model.create_variable("x", 1, 3);
        auto y = model.create_variable("y", 1, 3);
        model.add_constraint(std::make_shared<TableConstraint>(
            std::vector<VariablePtr>{x, y},
            std::vector<Domain::value_type>{1,2, 2,3, 3,1}));

        Solver solver;
        std::vector<Solution> solutions;
        size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        REQUIRE(count == 3);
        // Verify each solution is a valid tuple
        for (const auto& sol : solutions) {
            auto sx = sol.at("x");
            auto sy = sol.at("y");
            bool valid = (sx == 1 && sy == 2) ||
                         (sx == 2 && sy == 3) ||
                         (sx == 3 && sy == 1);
            REQUIRE(valid);
        }
    }

    SECTION("3-var table") {
        // table: (1,2,3), (3,1,2), (2,3,1)
        Model model;
        auto x = model.create_variable("x", 1, 3);
        auto y = model.create_variable("y", 1, 3);
        auto z = model.create_variable("z", 1, 3);
        model.add_constraint(std::make_shared<TableConstraint>(
            std::vector<VariablePtr>{x, y, z},
            std::vector<Domain::value_type>{1,2,3, 3,1,2, 2,3,1}));

        Solver solver;
        std::vector<Solution> solutions;
        size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        REQUIRE(count == 3);
    }

    SECTION("UNSAT: table + conflicting constraint") {
        // table: (1,1), (2,2) but x != y
        Model model;
        auto x = model.create_variable("x", 1, 2);
        auto y = model.create_variable("y", 1, 2);
        model.add_constraint(std::make_shared<TableConstraint>(
            std::vector<VariablePtr>{x, y},
            std::vector<Domain::value_type>{1,1, 2,2}));
        model.add_constraint(std::make_shared<IntNeConstraint>(x, y));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution&) {
            return true;
        });

        REQUIRE(count == 0);
    }

    SECTION("large table (>64 tuples)") {
        // Create a table with all pairs (i,j) where i+j is even, i,j in [1,10]
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 1, 10);

        std::vector<Domain::value_type> tuples;
        int expected_count = 0;
        for (int i = 1; i <= 10; ++i) {
            for (int j = 1; j <= 10; ++j) {
                if ((i + j) % 2 == 0) {
                    tuples.push_back(i);
                    tuples.push_back(j);
                    ++expected_count;
                }
            }
        }

        model.add_constraint(std::make_shared<TableConstraint>(
            std::vector<VariablePtr>{x, y}, tuples));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution&) {
            return true;
        });

        REQUIRE(count == static_cast<size_t>(expected_count));
    }
}

// ============================================================================
// CountEqConstraint tests
// ============================================================================

TEST_CASE("CountEqConstraint name", "[constraint][count_eq]") {
    auto x1 = std::make_shared<Variable>("x1", Domain(1, 3));
    auto x2 = std::make_shared<Variable>("x2", Domain(1, 3));
    auto c = std::make_shared<Variable>("c", Domain(0, 2));
    CountEqConstraint cst({x1, x2}, 2, c);

    REQUIRE(cst.name() == "count_eq");
}

TEST_CASE("CountEqConstraint variables", "[constraint][count_eq]") {
    auto x1 = std::make_shared<Variable>("x1", Domain(1, 3));
    auto x2 = std::make_shared<Variable>("x2", Domain(1, 3));
    auto c = std::make_shared<Variable>("c", Domain(0, 2));
    CountEqConstraint cst({x1, x2}, 2, c);

    auto vars = cst.variables();
    REQUIRE(vars.size() == 3);  // x1, x2, c
}

TEST_CASE("CountEqConstraint is_satisfied", "[constraint][count_eq]") {
    SECTION("satisfied - count matches") {
        auto x1 = std::make_shared<Variable>("x1", Domain(2, 2));
        auto x2 = std::make_shared<Variable>("x2", Domain(1, 1));
        auto x3 = std::make_shared<Variable>("x3", Domain(2, 2));
        auto c = std::make_shared<Variable>("c", Domain(2, 2));
        CountEqConstraint cst({x1, x2, x3}, 2, c);

        REQUIRE(cst.is_satisfied().has_value());
        REQUIRE(cst.is_satisfied().value() == true);
    }

    SECTION("violated - count does not match") {
        auto x1 = std::make_shared<Variable>("x1", Domain(2, 2));
        auto x2 = std::make_shared<Variable>("x2", Domain(1, 1));
        auto x3 = std::make_shared<Variable>("x3", Domain(2, 2));
        auto c = std::make_shared<Variable>("c", Domain(1, 1));
        CountEqConstraint cst({x1, x2, x3}, 2, c);

        REQUIRE(cst.is_satisfied().has_value());
        REQUIRE(cst.is_satisfied().value() == false);
    }

    SECTION("not fully assigned") {
        auto x1 = std::make_shared<Variable>("x1", Domain(1, 3));
        auto x2 = std::make_shared<Variable>("x2", Domain(2, 2));
        auto c = std::make_shared<Variable>("c", Domain(0, 2));
        CountEqConstraint cst({x1, x2}, 2, c);

        REQUIRE_FALSE(cst.is_satisfied().has_value());
    }
}

TEST_CASE("CountEqConstraint presolve", "[constraint][count_eq]") {
    SECTION("c bounds tightened") {
        auto x1 = std::make_shared<Variable>("x1", Domain(2, 2));  // definite: target=2
        auto x2 = std::make_shared<Variable>("x2", Domain(1, 3));  // possible
        auto x3 = std::make_shared<Variable>("x3", Domain(3, 3));  // not possible (no 2)
        auto c = std::make_shared<Variable>("c", Domain(0, 3));
        CountEqConstraint cst({x1, x2, x3}, 2, c);

        REQUIRE(cst.presolve(dummy_model) == true);
        // definite=1, possible=1 → c ∈ [1, 2]
        REQUIRE(c->min() == 1);
        REQUIRE(c->max() == 2);
    }

    SECTION("all must be target when c forces it") {
        auto x1 = std::make_shared<Variable>("x1", Domain(1, 3));
        auto x2 = std::make_shared<Variable>("x2", Domain(1, 3));
        auto x3 = std::make_shared<Variable>("x3", Domain(1, 3));
        auto c = std::make_shared<Variable>("c", Domain(3, 3));
        CountEqConstraint cst({x1, x2, x3}, 2, c);

        REQUIRE(cst.presolve(dummy_model) == true);
        // c=3, definite=0, possible=3 → c.min(3) == def+poss(3) → all forced to 2
        REQUIRE(x1->min() == 2);
        REQUIRE(x1->max() == 2);
        REQUIRE(x2->min() == 2);
        REQUIRE(x2->max() == 2);
        REQUIRE(x3->min() == 2);
        REQUIRE(x3->max() == 2);
    }

    SECTION("none can be target when c=0") {
        auto x1 = std::make_shared<Variable>("x1", Domain(1, 3));
        auto x2 = std::make_shared<Variable>("x2", Domain(1, 3));
        auto c = std::make_shared<Variable>("c", Domain(0, 0));
        CountEqConstraint cst({x1, x2}, 2, c);

        REQUIRE(cst.presolve(dummy_model) == true);
        // c=0, definite=0 → c.max(0)==definite(0) → remove target from all
        REQUIRE_FALSE(x1->domain().contains(2));
        REQUIRE_FALSE(x2->domain().contains(2));
    }

    SECTION("infeasible - not enough possible") {
        auto x1 = std::make_shared<Variable>("x1", Domain(3, 3));
        auto x2 = std::make_shared<Variable>("x2", Domain(3, 3));
        auto c = std::make_shared<Variable>("c", Domain(2, 2));
        CountEqConstraint cst({x1, x2}, 2, c);

        // definite=0, possible=0, c=2 → impossible
        REQUIRE(cst.presolve(dummy_model) == false);
    }
}

TEST_CASE("CountEqConstraint solver integration", "[constraint][count_eq]") {
    SECTION("all solutions count") {
        // x1 in {1,2}, x2 in {1,2}, target=2, c in {0,1,2}
        // Solutions: (1,1,0), (1,2,1), (2,1,1), (2,2,2) → 4 solutions
        Model model;
        auto x1 = model.create_variable("x1", 1, 2);
        auto x2 = model.create_variable("x2", 1, 2);
        auto c = model.create_variable("c", 0, 2);
        model.add_constraint(std::make_shared<CountEqConstraint>(
            std::vector<VariablePtr>{x1, x2}, 2, c));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution&) {
            return true;
        });

        REQUIRE(count == 4);
    }

    SECTION("with fixed count") {
        // x1 in {1,2,3}, x2 in {1,2,3}, x3 in {1,2,3}, target=1, c=2
        // Exactly 2 of 3 vars must be 1. Remaining can be 2 or 3.
        // Choose which 2: C(3,2)=3 combos, each combo the non-1 has 2 choices
        // Total: 3 * 2 = 6
        Model model;
        auto x1 = model.create_variable("x1", 1, 3);
        auto x2 = model.create_variable("x2", 1, 3);
        auto x3 = model.create_variable("x3", 1, 3);
        auto c = model.create_variable("c", 2, 2);
        model.add_constraint(std::make_shared<CountEqConstraint>(
            std::vector<VariablePtr>{x1, x2, x3}, 1, c));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution&) {
            return true;
        });

        REQUIRE(count == 6);
    }
}
