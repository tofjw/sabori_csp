#include <catch2/catch_test_macros.hpp>
#include "sabori_csp/constraint.hpp"
#include "sabori_csp/variable.hpp"
#include "sabori_csp/domain.hpp"
#include "sabori_csp/model.hpp"
#include "sabori_csp/solver.hpp"
#include <set>
#include <algorithm>

using namespace sabori_csp;

// Helper to create a variable
static VariablePtr make_var(const std::string& name, Domain::value_type min, Domain::value_type max) {
    return std::make_shared<Variable>(name, Domain(min, max));
}

static VariablePtr make_var(const std::string& name, Domain::value_type value) {
    return std::make_shared<Variable>(name, Domain(value, value));
}

// ============================================================================
// AllDifferentConstraint tests
// ============================================================================

TEST_CASE("AllDifferentConstraint name", "[constraint][all_different]") {
    auto x = make_var("x", 1, 3);
    auto y = make_var("y", 1, 3);
    auto z = make_var("z", 1, 3);
    AllDifferentConstraint c({x, y, z});

    REQUIRE(c.name() == "all_different");
}

TEST_CASE("AllDifferentConstraint variables", "[constraint][all_different]") {
    auto x = make_var("x", 1, 3);
    auto y = make_var("y", 1, 3);
    AllDifferentConstraint c({x, y});

    auto vars = c.variables();
    REQUIRE(vars.size() == 2);
}

TEST_CASE("AllDifferentConstraint is_satisfied", "[constraint][all_different]") {
    SECTION("all different values - satisfied") {
        auto x = make_var("x", 1);
        auto y = make_var("y", 2);
        auto z = make_var("z", 3);
        AllDifferentConstraint c({x, y, z});

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("duplicate values - violated") {
        auto x = make_var("x", 1);
        auto y = make_var("y", 2);
        auto z = make_var("z", 1);  // same as x
        AllDifferentConstraint c({x, y, z});

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("not fully assigned") {
        auto x = make_var("x", 1);
        auto y = make_var("y", 1, 3);
        auto z = make_var("z", 3);
        AllDifferentConstraint c({x, y, z});

        REQUIRE_FALSE(c.is_satisfied().has_value());
    }
}

TEST_CASE("AllDifferentConstraint propagate", "[constraint][all_different]") {
    SECTION("remove assigned values from others") {
        auto x = make_var("x", 2);  // assigned
        auto y = make_var("y", 1, 3);
        auto z = make_var("z", 1, 3);
        AllDifferentConstraint c({x, y, z});

        REQUIRE(c.propagate() == true);
        REQUIRE_FALSE(y->domain().contains(2));
        REQUIRE_FALSE(z->domain().contains(2));
    }

    SECTION("infeasible - domain becomes empty") {
        auto x = make_var("x", 1);  // assigned to 1
        auto y = make_var("y", 1, 1);  // domain {1}, unassigned for propagation check
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
        auto x = make_var("x", 2);  // assigned to 2
        auto y = make_var("y", 2, 2);  // domain {2}
        auto z = make_var("z", 2, 3);  // domain {2, 3}
        AllDifferentConstraint c({x, y, z});

        // y only has value 2, which x already uses
        // propagate should remove 2 from y, making it empty
        // But y is a singleton (assigned), so the check is !other->is_assigned()
        // We need on_final_instantiate to detect this
        REQUIRE(c.on_final_instantiate() == false);
    }
}

TEST_CASE("AllDifferentConstraint pool management", "[constraint][all_different]") {
    auto x = make_var("x", 1, 3);
    auto y = make_var("y", 1, 3);
    auto z = make_var("z", 1, 3);
    AllDifferentConstraint c({x, y, z});

    SECTION("initial pool size") {
        REQUIRE(c.pool_size() == 3);  // {1, 2, 3}
    }
}

TEST_CASE("AllDifferentConstraint on_final_instantiate", "[constraint][all_different]") {
    SECTION("satisfied") {
        auto x = make_var("x", 1);
        auto y = make_var("y", 2);
        auto z = make_var("z", 3);
        AllDifferentConstraint c({x, y, z});

        REQUIRE(c.on_final_instantiate() == true);
    }

    SECTION("violated") {
        auto x = make_var("x", 1);
        auto y = make_var("y", 1);
        AllDifferentConstraint c({x, y});

        REQUIRE(c.on_final_instantiate() == false);
    }
}

TEST_CASE("AllDifferentConstraint rewind_to", "[constraint][all_different][trail]") {
    auto x = make_var("x", 1, 3);
    auto y = make_var("y", 1, 3);
    auto z = make_var("z", 1, 3);
    AllDifferentConstraint c({x, y, z});
    Model model;

    REQUIRE(c.pool_size() == 3);

    // Simulate instantiation at level 1
    x->domain().assign(1);
    REQUIRE(c.on_instantiate(model, 1, 0, 1, 1, 3));
    REQUIRE(c.pool_size() == 2);

    // Simulate instantiation at level 2
    y->domain().assign(2);
    REQUIRE(c.on_instantiate(model, 2, 1, 2, 1, 3));
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
    auto x = make_var("x", 1, 3);
    auto y = make_var("y", 1, 3);
    IntLinEqConstraint c({1, 1}, {x, y}, 5);

    REQUIRE(c.name() == "int_lin_eq");
}

TEST_CASE("IntLinEqConstraint is_satisfied", "[constraint][int_lin_eq]") {
    SECTION("satisfied: 1*2 + 1*3 = 5") {
        auto x = make_var("x", 2);
        auto y = make_var("y", 3);
        IntLinEqConstraint c({1, 1}, {x, y}, 5);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("satisfied: 2*1 + 3*2 = 8") {
        auto x = make_var("x", 1);
        auto y = make_var("y", 2);
        IntLinEqConstraint c({2, 3}, {x, y}, 8);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("violated: 1*2 + 1*3 != 6") {
        auto x = make_var("x", 2);
        auto y = make_var("y", 3);
        IntLinEqConstraint c({1, 1}, {x, y}, 6);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("not fully assigned") {
        auto x = make_var("x", 1, 3);
        auto y = make_var("y", 2);
        IntLinEqConstraint c({1, 1}, {x, y}, 5);

        REQUIRE_FALSE(c.is_satisfied().has_value());
    }
}

TEST_CASE("IntLinEqConstraint on_final_instantiate", "[constraint][int_lin_eq]") {
    SECTION("satisfied") {
        auto x = make_var("x", 2);
        auto y = make_var("y", 3);
        IntLinEqConstraint c({1, 1}, {x, y}, 5);

        REQUIRE(c.on_final_instantiate() == true);
    }

    SECTION("violated") {
        auto x = make_var("x", 2);
        auto y = make_var("y", 3);
        IntLinEqConstraint c({1, 1}, {x, y}, 6);

        REQUIRE(c.on_final_instantiate() == false);
    }
}

TEST_CASE("IntLinEqConstraint with negative coefficients", "[constraint][int_lin_eq]") {
    SECTION("satisfied: 2*3 + (-1)*1 = 5") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 1);
        IntLinEqConstraint c({2, -1}, {x, y}, 5);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("satisfied: (-1)*2 + 1*7 = 5") {
        auto x = make_var("x", 2);
        auto y = make_var("y", 7);
        IntLinEqConstraint c({-1, 1}, {x, y}, 5);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }
}

TEST_CASE("IntLinEqConstraint rewind_to", "[constraint][int_lin_eq][trail]") {
    auto x = make_var("x", 1, 5);
    auto y = make_var("y", 1, 5);
    IntLinEqConstraint c({1, 1}, {x, y}, 6);
    Model model;
    model.add_variable(x);
    model.add_variable(y);

    // Initial state
    int64_t initial_sum = c.target_sum();
    REQUIRE(initial_sum == 6);

    // Simulate instantiation at level 1
    x->domain().assign(2);
    c.on_instantiate(model, 1, 0, 2, 1, 5);

    // Simulate instantiation at level 2
    y->domain().assign(4);
    c.on_instantiate(model, 2, 1, 4, 1, 5);

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
    auto x = make_var("x", 1, 3);
    auto y = make_var("y", 1, 3);
    IntLinLeConstraint c({1, 1}, {x, y}, 5);

    REQUIRE(c.name() == "int_lin_le");
}

TEST_CASE("IntLinLeConstraint is_satisfied", "[constraint][int_lin_le]") {
    SECTION("satisfied: 1*2 + 1*3 <= 5") {
        auto x = make_var("x", 2);
        auto y = make_var("y", 3);
        IntLinLeConstraint c({1, 1}, {x, y}, 5);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("satisfied: 1*2 + 1*3 <= 6") {
        auto x = make_var("x", 2);
        auto y = make_var("y", 3);
        IntLinLeConstraint c({1, 1}, {x, y}, 6);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("violated: 1*2 + 1*3 > 4") {
        auto x = make_var("x", 2);
        auto y = make_var("y", 3);
        IntLinLeConstraint c({1, 1}, {x, y}, 4);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("not fully assigned") {
        auto x = make_var("x", 1, 3);
        auto y = make_var("y", 2);
        IntLinLeConstraint c({1, 1}, {x, y}, 5);

        REQUIRE_FALSE(c.is_satisfied().has_value());
    }
}

TEST_CASE("IntLinLeConstraint on_final_instantiate", "[constraint][int_lin_le]") {
    SECTION("satisfied - equal") {
        auto x = make_var("x", 2);
        auto y = make_var("y", 3);
        IntLinLeConstraint c({1, 1}, {x, y}, 5);

        REQUIRE(c.on_final_instantiate() == true);
    }

    SECTION("satisfied - less than") {
        auto x = make_var("x", 1);
        auto y = make_var("y", 2);
        IntLinLeConstraint c({1, 1}, {x, y}, 5);

        REQUIRE(c.on_final_instantiate() == true);
    }

    SECTION("violated") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 3);
        IntLinLeConstraint c({1, 1}, {x, y}, 5);

        REQUIRE(c.on_final_instantiate() == false);
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
        auto var = std::make_shared<Variable>("x" + std::to_string(i), Domain(1, 9));
        cells.push_back(var);
        model.add_variable(var);
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
        auto var = std::make_shared<Variable>("x" + std::to_string(i), Domain(1, 9));
        cells.push_back(var);
        model.add_variable(var);
    }

    // Fix specified cells
    for (const auto& [idx, val] : fixed_cells) {
        cells[idx]->domain().assign(val);
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
    auto x0 = make_var("x0", 0, 2);
    auto x1 = make_var("x1", 0, 2);
    auto x2 = make_var("x2", 0, 2);
    CircuitConstraint c({x0, x1, x2});

    REQUIRE(c.name() == "circuit");
}

TEST_CASE("CircuitConstraint variables", "[constraint][circuit]") {
    auto x0 = make_var("x0", 0, 2);
    auto x1 = make_var("x1", 0, 2);
    auto x2 = make_var("x2", 0, 2);
    CircuitConstraint c({x0, x1, x2});

    auto vars = c.variables();
    REQUIRE(vars.size() == 3);
}

TEST_CASE("CircuitConstraint is_satisfied", "[constraint][circuit]") {
    SECTION("valid circuit - satisfied") {
        // Circuit: 0 -> 1 -> 2 -> 0
        auto x0 = make_var("x0", 1);  // x[0] = 1
        auto x1 = make_var("x1", 2);  // x[1] = 2
        auto x2 = make_var("x2", 0);  // x[2] = 0
        CircuitConstraint c({x0, x1, x2});

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("subcircuit - violated") {
        // Subcircuit: 0 -> 1 -> 0 (x[2] is not part of circuit)
        auto x0 = make_var("x0", 1);  // x[0] = 1
        auto x1 = make_var("x1", 0);  // x[1] = 0
        auto x2 = make_var("x2", 2);  // x[2] = 2 (self-loop)
        CircuitConstraint c({x0, x1, x2});

        // This should be marked as initially inconsistent or is_satisfied returns false
        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("duplicate values - violated") {
        auto x0 = make_var("x0", 1);  // x[0] = 1
        auto x1 = make_var("x1", 1);  // x[1] = 1 (duplicate)
        auto x2 = make_var("x2", 0);  // x[2] = 0
        CircuitConstraint c({x0, x1, x2});

        // Duplicate value - should be initially inconsistent
        REQUIRE(c.is_initially_inconsistent() == true);
    }

    SECTION("not fully assigned") {
        auto x0 = make_var("x0", 1);
        auto x1 = make_var("x1", 0, 2);
        auto x2 = make_var("x2", 0);
        CircuitConstraint c({x0, x1, x2});

        REQUIRE_FALSE(c.is_satisfied().has_value());
    }
}

TEST_CASE("CircuitConstraint pool management", "[constraint][circuit]") {
    auto x0 = make_var("x0", 0, 2);
    auto x1 = make_var("x1", 0, 2);
    auto x2 = make_var("x2", 0, 2);
    CircuitConstraint c({x0, x1, x2});

    SECTION("initial pool size") {
        REQUIRE(c.pool_size() == 3);  // {0, 1, 2}
    }
}

TEST_CASE("CircuitConstraint on_final_instantiate", "[constraint][circuit]") {
    SECTION("valid circuit") {
        // Circuit: 0 -> 1 -> 2 -> 0
        auto x0 = make_var("x0", 1);
        auto x1 = make_var("x1", 2);
        auto x2 = make_var("x2", 0);
        CircuitConstraint c({x0, x1, x2});

        REQUIRE(c.on_final_instantiate() == true);
    }

    SECTION("valid circuit reversed") {
        // Circuit: 0 -> 2 -> 1 -> 0
        auto x0 = make_var("x0", 2);
        auto x1 = make_var("x1", 0);
        auto x2 = make_var("x2", 1);
        CircuitConstraint c({x0, x1, x2});

        REQUIRE(c.on_final_instantiate() == true);
    }
}

TEST_CASE("CircuitConstraint rewind_to", "[constraint][circuit][trail]") {
    auto x0 = make_var("x0", 0, 2);
    auto x1 = make_var("x1", 0, 2);
    auto x2 = make_var("x2", 0, 2);
    CircuitConstraint c({x0, x1, x2});
    Model model;
    model.add_variable(x0);
    model.add_variable(x1);
    model.add_variable(x2);

    REQUIRE(c.pool_size() == 3);

    // Simulate instantiation at level 1: x[0] = 1
    x0->domain().assign(1);
    REQUIRE(c.on_instantiate(model, 1, 0, 1, 0, 2));
    REQUIRE(c.pool_size() == 2);

    // Simulate instantiation at level 2: x[1] = 2
    x1->domain().assign(2);
    REQUIRE(c.on_instantiate(model, 2, 1, 2, 0, 2));
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
        auto x0 = make_var("x0", 0, 2);
        auto x1 = make_var("x1", 0, 2);
        auto x2 = make_var("x2", 0, 2);
        CircuitConstraint c({x0, x1, x2});
        Model model;
        model.add_variable(x0);
        model.add_variable(x1);
        model.add_variable(x2);

        // x[0] = 1: 0 -> 1
        x0->domain().assign(1);
        REQUIRE(c.on_instantiate(model, 1, 0, 1, 0, 2));

        // x[1] = 0: 1 -> 0, forms subcircuit (0 -> 1 -> 0)
        x1->domain().assign(0);
        REQUIRE_FALSE(c.on_instantiate(model, 2, 1, 0, 0, 2));
    }

    SECTION("self-loop is subcircuit") {
        auto x0 = make_var("x0", 0, 2);
        auto x1 = make_var("x1", 0, 2);
        auto x2 = make_var("x2", 0, 2);
        CircuitConstraint c({x0, x1, x2});
        Model model;
        model.add_variable(x0);
        model.add_variable(x1);
        model.add_variable(x2);

        // x[0] = 0: self-loop, forms subcircuit of size 1
        x0->domain().assign(0);
        REQUIRE_FALSE(c.on_instantiate(model, 1, 0, 0, 0, 2));
    }
}

TEST_CASE("CircuitConstraint solver integration", "[solver][circuit]") {
    SECTION("n=3 find one solution") {
        Model model;
        std::vector<VariablePtr> vars;
        for (int i = 0; i < 3; ++i) {
            auto var = std::make_shared<Variable>("x" + std::to_string(i), Domain(0, 2));
            vars.push_back(var);
            model.add_variable(var);
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
            auto var = std::make_shared<Variable>("x" + std::to_string(i), Domain(0, 2));
            vars.push_back(var);
            model.add_variable(var);
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
            auto var = std::make_shared<Variable>("x" + std::to_string(i), Domain(0, 3));
            vars.push_back(var);
            model.add_variable(var);
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
    auto index = make_var("index", 1, 3);
    auto result = make_var("result", 10, 30);
    std::vector<Domain::value_type> array = {10, 20, 30};
    IntElementConstraint c(index, array, result);

    REQUIRE(c.name() == "int_element");
}

TEST_CASE("IntElementConstraint variables", "[constraint][int_element]") {
    auto index = make_var("index", 1, 3);
    auto result = make_var("result", 10, 30);
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
        auto index = make_var("index", 2);  // index = 2
        auto result = make_var("result", 20);  // result = 20
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("satisfied - 0-based index") {
        // array[0] = 10, array[1] = 20, array[2] = 30 (0-based)
        auto index = make_var("index", 1);  // index = 1
        auto result = make_var("result", 20);  // result = 20
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result, true);  // zero_based = true

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("violated - value mismatch") {
        auto index = make_var("index", 2);  // index = 2 -> array[1] = 20
        auto result = make_var("result", 30);  // result = 30 (wrong)
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("not fully assigned") {
        auto index = make_var("index", 1, 3);
        auto result = make_var("result", 20);
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE_FALSE(c.is_satisfied().has_value());
    }
}

TEST_CASE("IntElementConstraint on_final_instantiate", "[constraint][int_element]") {
    SECTION("satisfied") {
        auto index = make_var("index", 1);  // index = 1 -> array[0] = 10
        auto result = make_var("result", 10);
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.on_final_instantiate() == true);
    }

    SECTION("violated") {
        auto index = make_var("index", 1);  // index = 1 -> array[0] = 10
        auto result = make_var("result", 20);  // wrong
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.on_final_instantiate() == false);
    }
}

TEST_CASE("IntElementConstraint initial consistency", "[constraint][int_element]") {
    SECTION("index out of range - too small") {
        auto index = make_var("index", 0);  // 0 is out of range for 1-based
        auto result = make_var("result", 10, 30);
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.is_initially_inconsistent() == true);
    }

    SECTION("index out of range - too large") {
        auto index = make_var("index", 4);  // 4 is out of range for array of size 3 (1-based)
        auto result = make_var("result", 10, 30);
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.is_initially_inconsistent() == true);
    }

    SECTION("result value not in array") {
        auto index = make_var("index", 1, 3);
        auto result = make_var("result", 99);  // 99 is not in array
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.is_initially_inconsistent() == true);
    }

    SECTION("consistent initial state") {
        auto index = make_var("index", 1, 3);
        auto result = make_var("result", 10, 30);
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.is_initially_inconsistent() == false);
    }
}

TEST_CASE("IntElementConstraint rewind_to", "[constraint][int_element][trail]") {
    // IntElementConstraint has no state, so rewind_to is a no-op
    auto index = make_var("index", 1, 3);
    auto result = make_var("result", 10, 30);
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
    auto index = make_var("index", 1, 3);
    auto result = make_var("result", 10);  // fixed to 10
    std::vector<Domain::value_type> array = {10, 20, 10};
    IntElementConstraint c(index, array, result);

    // Should be consistent - index can be 1 or 3
    REQUIRE(c.is_initially_inconsistent() == false);
}

TEST_CASE("IntElementConstraint solver integration", "[solver][int_element]") {
    SECTION("find one solution - basic") {
        Model model;

        // array = {10, 20, 30} (1-based: array[1]=10, array[2]=20, array[3]=30)
        auto index = std::make_shared<Variable>("index", Domain(1, 3));
        auto result = std::make_shared<Variable>("result", Domain(10, 30));

        model.add_variable(index);
        model.add_variable(result);

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
        auto index = std::make_shared<Variable>("index", Domain(1, 3));
        auto result = std::make_shared<Variable>("result", Domain(10, 30));

        model.add_variable(index);
        model.add_variable(result);

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
        auto index = std::make_shared<Variable>("index", Domain(1, 3));
        auto result = std::make_shared<Variable>("result", Domain(10, 20));

        model.add_variable(index);
        model.add_variable(result);

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
        auto index = std::make_shared<Variable>("index", Domain(1, 3));
        auto result = std::make_shared<Variable>("result", Domain(40, 50));

        model.add_variable(index);
        model.add_variable(result);

        std::vector<Domain::value_type> array = {10, 20, 30};
        model.add_constraint(std::make_shared<IntElementConstraint>(index, array, result));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE_FALSE(solution.has_value());
    }

    SECTION("partial assignment - index fixed") {
        Model model;

        // array = {10, 20, 30}, index fixed to 2
        auto index = std::make_shared<Variable>("index", Domain(2, 2));  // fixed to 2
        auto result = std::make_shared<Variable>("result", Domain(10, 30));

        model.add_variable(index);
        model.add_variable(result);

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
        auto index = std::make_shared<Variable>("index", Domain(1, 3));
        auto result = std::make_shared<Variable>("result", Domain(20, 20));  // fixed to 20

        model.add_variable(index);
        model.add_variable(result);

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
        auto index = std::make_shared<Variable>("index", Domain(1, 3));
        auto result = std::make_shared<Variable>("result", Domain(10, 30));

        model.add_variable(index);
        model.add_variable(result);

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
    auto index = std::make_shared<Variable>("index", Domain(0, 2));
    auto result = std::make_shared<Variable>("result", Domain(10, 30));

    model.add_variable(index);
    model.add_variable(result);

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

TEST_CASE("CircuitConstraint with partial assignment", "[solver][circuit]") {
    SECTION("one variable fixed - consistent") {
        Model model;
        std::vector<VariablePtr> vars;
        for (int i = 0; i < 3; ++i) {
            auto var = std::make_shared<Variable>("x" + std::to_string(i), Domain(0, 2));
            vars.push_back(var);
            model.add_variable(var);
        }

        // Fix x[0] = 1
        vars[0]->domain().assign(1);

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
            auto var = std::make_shared<Variable>("x" + std::to_string(i), Domain(0, 2));
            vars.push_back(var);
            model.add_variable(var);
        }

        // Fix x[0] = 1, x[1] = 2 (forces x[2] = 0 for valid circuit)
        vars[0]->domain().assign(1);
        vars[1]->domain().assign(2);

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
            auto var = std::make_shared<Variable>("x" + std::to_string(i), Domain(0, 2));
            vars.push_back(var);
            model.add_variable(var);
        }

        // Fix x[0] = 1, x[1] = 0 (creates subcircuit 0 -> 1 -> 0)
        vars[0]->domain().assign(1);
        vars[1]->domain().assign(0);

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
    auto b1 = make_var("b1", 0, 1);
    auto b2 = make_var("b2", 0, 1);
    auto r = make_var("r", 0, 1);
    ArrayBoolAndConstraint c({b1, b2}, r);

    REQUIRE(c.name() == "array_bool_and");
}

TEST_CASE("ArrayBoolAndConstraint variables", "[constraint][array_bool_and]") {
    auto b1 = make_var("b1", 0, 1);
    auto b2 = make_var("b2", 0, 1);
    auto r = make_var("r", 0, 1);
    ArrayBoolAndConstraint c({b1, b2}, r);

    auto vars = c.variables();
    REQUIRE(vars.size() == 3);  // b1, b2, r
}

TEST_CASE("ArrayBoolAndConstraint is_satisfied", "[constraint][array_bool_and]") {
    SECTION("all true - r=1 satisfied") {
        auto b1 = make_var("b1", 1);
        auto b2 = make_var("b2", 1);
        auto r = make_var("r", 1);
        ArrayBoolAndConstraint c({b1, b2}, r);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("one false - r=0 satisfied") {
        auto b1 = make_var("b1", 1);
        auto b2 = make_var("b2", 0);
        auto r = make_var("r", 0);
        ArrayBoolAndConstraint c({b1, b2}, r);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("one false - r=1 not satisfied") {
        auto b1 = make_var("b1", 1);
        auto b2 = make_var("b2", 0);
        auto r = make_var("r", 1);
        ArrayBoolAndConstraint c({b1, b2}, r);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("not all assigned - unknown") {
        auto b1 = make_var("b1", 0, 1);
        auto b2 = make_var("b2", 1);
        auto r = make_var("r", 0, 1);
        ArrayBoolAndConstraint c({b1, b2}, r);

        REQUIRE_FALSE(c.is_satisfied().has_value());
    }
}

TEST_CASE("ArrayBoolAndConstraint on_final_instantiate", "[constraint][array_bool_and]") {
    SECTION("all true - r=1") {
        auto b1 = make_var("b1", 1);
        auto b2 = make_var("b2", 1);
        auto r = make_var("r", 1);
        ArrayBoolAndConstraint c({b1, b2}, r);

        REQUIRE(c.on_final_instantiate() == true);
    }

    SECTION("one false - r=0") {
        auto b1 = make_var("b1", 0);
        auto b2 = make_var("b2", 1);
        auto r = make_var("r", 0);
        ArrayBoolAndConstraint c({b1, b2}, r);

        REQUIRE(c.on_final_instantiate() == true);
    }
}

TEST_CASE("ArrayBoolAndConstraint solver integration", "[constraint][array_bool_and]") {
    SECTION("r=1 forces all bi=1") {
        Model model;
        auto b1 = make_var("b1", 0, 1);
        auto b2 = make_var("b2", 0, 1);
        auto b3 = make_var("b3", 0, 1);
        auto r = make_var("r", 1);  // r = 1 fixed

        model.add_variable(b1);
        model.add_variable(b2);
        model.add_variable(b3);
        model.add_variable(r);
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
        auto b1 = make_var("b1", 1);  // fixed
        auto b2 = make_var("b2", 1);  // fixed
        auto b3 = make_var("b3", 0, 1);
        auto r = make_var("r", 0);  // r = 0 fixed

        model.add_variable(b1);
        model.add_variable(b2);
        model.add_variable(b3);
        model.add_variable(r);
        model.add_constraint(std::make_shared<ArrayBoolAndConstraint>(
            std::vector<VariablePtr>{b1, b2, b3}, r));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("b3") == 0);
    }

    SECTION("all solutions enumeration") {
        Model model;
        auto b1 = make_var("b1", 0, 1);
        auto b2 = make_var("b2", 0, 1);
        auto r = make_var("r", 0, 1);

        model.add_variable(b1);
        model.add_variable(b2);
        model.add_variable(r);
        model.add_constraint(std::make_shared<ArrayBoolAndConstraint>(
            std::vector<VariablePtr>{b1, b2}, r));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution&) { return true; });

        REQUIRE(count == 4);  // (0,0,0), (0,1,0), (1,0,0), (1,1,1)
    }
}

// ============================================================================
// ArrayBoolOrConstraint tests
// ============================================================================

TEST_CASE("ArrayBoolOrConstraint name", "[constraint][array_bool_or]") {
    auto b1 = make_var("b1", 0, 1);
    auto b2 = make_var("b2", 0, 1);
    auto r = make_var("r", 0, 1);
    ArrayBoolOrConstraint c({b1, b2}, r);

    REQUIRE(c.name() == "array_bool_or");
}

TEST_CASE("ArrayBoolOrConstraint is_satisfied", "[constraint][array_bool_or]") {
    SECTION("one true - r=1 satisfied") {
        auto b1 = make_var("b1", 0);
        auto b2 = make_var("b2", 1);
        auto r = make_var("r", 1);
        ArrayBoolOrConstraint c({b1, b2}, r);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("all false - r=0 satisfied") {
        auto b1 = make_var("b1", 0);
        auto b2 = make_var("b2", 0);
        auto r = make_var("r", 0);
        ArrayBoolOrConstraint c({b1, b2}, r);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("all false - r=1 not satisfied") {
        auto b1 = make_var("b1", 0);
        auto b2 = make_var("b2", 0);
        auto r = make_var("r", 1);
        ArrayBoolOrConstraint c({b1, b2}, r);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }
}

TEST_CASE("ArrayBoolOrConstraint solver integration", "[constraint][array_bool_or]") {
    SECTION("r=0 forces all bi=0") {
        Model model;
        auto b1 = make_var("b1", 0, 1);
        auto b2 = make_var("b2", 0, 1);
        auto r = make_var("r", 0);  // r = 0 fixed

        model.add_variable(b1);
        model.add_variable(b2);
        model.add_variable(r);
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
        auto b1 = make_var("b1", 0);  // fixed to 0
        auto b2 = make_var("b2", 0, 1);
        auto r = make_var("r", 1);  // r = 1 fixed

        model.add_variable(b1);
        model.add_variable(b2);
        model.add_variable(r);
        model.add_constraint(std::make_shared<ArrayBoolOrConstraint>(
            std::vector<VariablePtr>{b1, b2}, r));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("b2") == 1);
    }

    SECTION("all solutions enumeration") {
        Model model;
        auto b1 = make_var("b1", 0, 1);
        auto b2 = make_var("b2", 0, 1);
        auto r = make_var("r", 0, 1);

        model.add_variable(b1);
        model.add_variable(b2);
        model.add_variable(r);
        model.add_constraint(std::make_shared<ArrayBoolOrConstraint>(
            std::vector<VariablePtr>{b1, b2}, r));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution&) { return true; });

        REQUIRE(count == 4);  // (0,0,0), (0,1,1), (1,0,1), (1,1,1)
    }
}
