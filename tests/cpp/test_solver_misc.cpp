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
    std::vector<Variable*> cells;
    for (int i = 0; i < 9; ++i) {
        auto var = model.create_variable("x" + std::to_string(i), 1, 9);
        cells.push_back(var);
    }

    // AllDifferent constraint: all cells must have different values
    auto alldiff = std::make_unique<AllDifferentConstraint>(cells);
    model.add_constraint(std::move(alldiff));

    // Row constraints: each row sums to 15
    // Row 0: x[0] + x[1] + x[2] = 15
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<Variable*>{cells[0], cells[1], cells[2]},
        15
    ));
    // Row 1: x[3] + x[4] + x[5] = 15
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<Variable*>{cells[3], cells[4], cells[5]},
        15
    ));
    // Row 2: x[6] + x[7] + x[8] = 15
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<Variable*>{cells[6], cells[7], cells[8]},
        15
    ));

    // Column constraints: each column sums to 15
    // Column 0: x[0] + x[3] + x[6] = 15
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<Variable*>{cells[0], cells[3], cells[6]},
        15
    ));
    // Column 1: x[1] + x[4] + x[7] = 15
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<Variable*>{cells[1], cells[4], cells[7]},
        15
    ));
    // Column 2: x[2] + x[5] + x[8] = 15
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<Variable*>{cells[2], cells[5], cells[8]},
        15
    ));

    // Diagonal constraints
    // Main diagonal: x[0] + x[4] + x[8] = 15
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<Variable*>{cells[0], cells[4], cells[8]},
        15
    ));
    // Anti diagonal: x[2] + x[4] + x[6] = 15
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<Variable*>{cells[2], cells[4], cells[6]},
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
    std::vector<Variable*> cells;
    for (int i = 0; i < 9; ++i) {
        auto var = model.create_variable("x" + std::to_string(i), 1, 9);
        cells.push_back(var);
    }

    // Fix specified cells via Model API
    for (const auto& [idx, val] : fixed_cells) {
        model.instantiate(0, cells[idx]->id(), val);
    }

    // AllDifferent constraint
    model.add_constraint(std::make_unique<AllDifferentConstraint>(cells));

    // Row constraints (sum = 15)
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<Variable*>{cells[0], cells[1], cells[2]}, 15));
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<Variable*>{cells[3], cells[4], cells[5]}, 15));
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<Variable*>{cells[6], cells[7], cells[8]}, 15));

    // Column constraints (sum = 15)
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<Variable*>{cells[0], cells[3], cells[6]}, 15));
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<Variable*>{cells[1], cells[4], cells[7]}, 15));
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<Variable*>{cells[2], cells[5], cells[8]}, 15));

    // Diagonal constraints (sum = 15)
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<Variable*>{cells[0], cells[4], cells[8]}, 15));
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{1, 1, 1},
        std::vector<Variable*>{cells[2], cells[4], cells[6]}, 15));

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
