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
// IntElementConstraint tests
// ============================================================================

TEST_CASE("IntElementConstraint name", "[constraint][int_element]") {
    Model model;
    auto* index = model.create_variable("index", Domain(1, 3));
    auto* result = model.create_variable("result", Domain(10, 30));
    std::vector<Domain::value_type> array = {10, 20, 30};
    IntElementConstraint c(index, array, result);

    REQUIRE(c.name() == "int_element");
}

TEST_CASE("IntElementConstraint variables", "[constraint][int_element]") {
    Model model;
    auto* index = model.create_variable("index", Domain(1, 3));
    auto* result = model.create_variable("result", Domain(10, 30));
    std::vector<Domain::value_type> array = {10, 20, 30};
    IntElementConstraint c(index, array, result);

    auto& vars = c.var_ids_ref();
    REQUIRE(vars.size() == 2);
    REQUIRE(vars[0] == index->id());
    REQUIRE(vars[1] == result->id());
}

TEST_CASE("IntElementConstraint is_satisfied", "[constraint][int_element]") {
    SECTION("satisfied - 1-based index") {
        // array[1] = 10, array[2] = 20, array[3] = 30 (1-based)
        Model model;
        auto* index = model.create_variable("index", Domain(2, 2));  // index = 2
        auto* result = model.create_variable("result", Domain(20, 20));  // result = 20
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == true);
    }

    SECTION("satisfied - 0-based index") {
        // array[0] = 10, array[1] = 20, array[2] = 30 (0-based)
        Model model;
        auto* index = model.create_variable("index", Domain(1, 1));  // index = 1
        auto* result = model.create_variable("result", Domain(20, 20));  // result = 20
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result, true);  // zero_based = true

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == true);
    }

    SECTION("violated - value mismatch") {
        Model model;
        auto* index = model.create_variable("index", Domain(2, 2));  // index = 2 -> array[1] = 20
        auto* result = model.create_variable("result", Domain(30, 30));  // result = 30 (wrong)
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == false);
    }

    SECTION("not fully assigned") {
        Model model;
        auto* index = model.create_variable("index", Domain(1, 3));
        auto* result = model.create_variable("result", Domain(20, 20));
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE_FALSE(c.is_satisfied(model).has_value());
    }
}

TEST_CASE("IntElementConstraint on_final_instantiate", "[constraint][int_element]") {
    SECTION("satisfied") {
        Model model;
        auto index = model.create_variable("index", 1, 1);  // index = 1 -> array[0] = 10
        auto result = model.create_variable("result", 10, 10);
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.on_final_instantiate(model) == true);
    }

    SECTION("violated") {
        Model model;
        auto index = model.create_variable("index", 1, 1);  // index = 1 -> array[0] = 10
        auto result = model.create_variable("result", 20, 20);  // wrong
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.on_final_instantiate(model) == false);
    }
}

TEST_CASE("IntElementConstraint initial consistency", "[constraint][int_element]") {
    SECTION("index out of range - too small") {
        Model model;
        auto* index = model.create_variable("index", Domain(0, 0));  // 0 is out of range for 1-based
        auto* result = model.create_variable("result", Domain(10, 30));
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.presolve(model) == PresolveResult::Contradiction);
    }

    SECTION("index out of range - too large") {
        Model model;
        auto* index = model.create_variable("index", Domain(4, 4));  // 4 is out of range for array of size 3 (1-based)
        auto* result = model.create_variable("result", Domain(10, 30));
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.presolve(model) == PresolveResult::Contradiction);
    }

    SECTION("result value not in array") {
        Model model;
        auto* index = model.create_variable("index", Domain(1, 3));
        auto* result = model.create_variable("result", Domain(99, 99));  // 99 is not in array
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.presolve(model) == PresolveResult::Contradiction);
    }

    SECTION("consistent initial state") {
        Model model;
        auto* index = model.create_variable("index", Domain(1, 3));
        auto* result = model.create_variable("result", Domain(10, 30));
        std::vector<Domain::value_type> array = {10, 20, 30};
        IntElementConstraint c(index, array, result);

        REQUIRE(c.presolve(model) != PresolveResult::Contradiction);
    }
}

TEST_CASE("IntElementConstraint rewind_to", "[constraint][int_element][trail]") {
    // IntElementConstraint has no state, so rewind_to is a no-op
    Model model;
    auto* index = model.create_variable("index", Domain(1, 3));
    auto* result = model.create_variable("result", Domain(10, 30));
    std::vector<Domain::value_type> array = {10, 20, 30};
    IntElementConstraint c(index, array, result);

    // Should not throw or cause issues
    c.rewind_to(0);
    c.rewind_to(5);
    c.rewind_to(-1);

    // Constraint should still be in a consistent state
    REQUIRE(c.prepare_propagation(model) == true);
}

TEST_CASE("IntElementConstraint with duplicate values in array", "[constraint][int_element]") {
    // array = {10, 20, 10} - value 10 appears at indices 1 and 3 (1-based)
    Model model;
    auto* index = model.create_variable("index", Domain(1, 3));
    auto* result = model.create_variable("result", Domain(10, 10));  // fixed to 10
    std::vector<Domain::value_type> array = {10, 20, 10};
    IntElementConstraint c(index, array, result);

    // Should be consistent - index can be 1 or 3
    REQUIRE(c.prepare_propagation(model) == true);
}

TEST_CASE("IntElementConstraint solver integration", "[solver][int_element]") {
    SECTION("find one solution - basic") {
        Model model;

        // array = {10, 20, 30} (1-based: array[1]=10, array[2]=20, array[3]=30)
        auto index = model.create_variable("index", 1, 3);
        auto result = model.create_variable("result", 10, 30);


        std::vector<Domain::value_type> array = {10, 20, 30};
        model.add_constraint(std::make_unique<IntElementConstraint>(index, array, result));

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
        model.add_constraint(std::make_unique<IntElementConstraint>(index, array, result));

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
        model.add_constraint(std::make_unique<IntElementConstraint>(index, array, result));

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
        model.add_constraint(std::make_unique<IntElementConstraint>(index, array, result));

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
        model.add_constraint(std::make_unique<IntElementConstraint>(index, array, result));

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
        model.add_constraint(std::make_unique<IntElementConstraint>(index, array, result));

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
        model.add_constraint(std::make_unique<IntElementConstraint>(index, array, result));
        model.add_constraint(std::make_unique<IntLinEqConstraint>(
            std::vector<int64_t>{1, 1},
            std::vector<Variable*>{index, result},
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
    model.add_constraint(std::make_unique<IntElementConstraint>(index, array, result, true));  // zero_based = true

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
        model.add_constraint(std::make_unique<IntElementConstraint>(index, array, result));

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
        model.add_constraint(std::make_unique<IntElementConstraint>(index, array, result));

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
        model.add_constraint(std::make_unique<IntElementConstraint>(index, array, result));

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
        model.add_constraint(std::make_unique<IntElementConstraint>(index, array, result));

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
        model.add_constraint(std::make_unique<IntElementConstraint>(index, array, result));
        model.add_constraint(std::make_unique<IntLinEqConstraint>(
            std::vector<int64_t>{1, 1},
            std::vector<Variable*>{result, extra},
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
        model.add_constraint(std::make_unique<IntElementConstraint>(index, array, result, true));

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
        model.add_constraint(std::make_unique<IntElementConstraint>(index, array, result));

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
        model.add_constraint(std::make_unique<IntElementConstraint>(index, array, result));

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
        model.add_constraint(std::make_unique<IntElementConstraint>(index, array, result));

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
        model.add_constraint(std::make_unique<IntElementConstraint>(index, array, result, true));
        // result + other = 10 → result=7, other=3
        model.add_constraint(std::make_unique<IntLinEqConstraint>(
            std::vector<int64_t>{1, 1},
            std::vector<Variable*>{result, other},
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
        model.add_constraint(std::make_unique<IntElementConstraint>(index, array, result));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("index") == 1);
        REQUIRE(solution->at("result") == 100);
    }
}

// ============================================================================
// ArrayIntMaximumConstraint tests
// ============================================================================

TEST_CASE("ArrayIntMaximumConstraint name", "[constraint][array_int_maximum]") {
    Model model;
    auto* m = model.create_variable("m", Domain(1, 5));
    auto* x1 = model.create_variable("x1", Domain(1, 5));
    auto* x2 = model.create_variable("x2", Domain(1, 5));
    ArrayIntMaximumConstraint c(m, {x1, x2});

    REQUIRE(c.name() == "array_int_maximum");
}

TEST_CASE("ArrayIntMaximumConstraint is_satisfied", "[constraint][array_int_maximum]") {
    SECTION("satisfied when m equals max") {
        Model model;
        auto* m = model.create_variable("m", Domain(3, 3));
        auto* x1 = model.create_variable("x1", Domain(1, 1));
        auto* x2 = model.create_variable("x2", Domain(3, 3));
        auto* x3 = model.create_variable("x3", Domain(2, 2));
        ArrayIntMaximumConstraint c(m, {x1, x2, x3});

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == true);
    }

    SECTION("not satisfied when m differs from max") {
        Model model;
        auto* m = model.create_variable("m", Domain(2, 2));
        auto* x1 = model.create_variable("x1", Domain(1, 1));
        auto* x2 = model.create_variable("x2", Domain(3, 3));
        ArrayIntMaximumConstraint c(m, {x1, x2});

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == false);
    }

    SECTION("undetermined when variables unassigned") {
        Model model;
        auto* m = model.create_variable("m", Domain(1, 5));
        auto* x1 = model.create_variable("x1", Domain(1, 5));
        auto* x2 = model.create_variable("x2", Domain(1, 5));
        ArrayIntMaximumConstraint c(m, {x1, x2});

        REQUIRE_FALSE(c.is_satisfied(model).has_value());
    }
}

TEST_CASE("ArrayIntMaximumConstraint solver integration", "[constraint][array_int_maximum]") {
    SECTION("finds maximum value") {
        Model model;
        auto m = model.create_variable("m", 1, 10);
        auto x1 = model.create_variable("x1", 3);  // fixed
        auto x2 = model.create_variable("x2", 7);  // fixed
        auto x3 = model.create_variable("x3", 5);  // fixed

        model.add_constraint(std::make_unique<ArrayIntMaximumConstraint>(m, std::vector<Variable*>{x1, x2, x3}));

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

        model.add_constraint(std::make_unique<ArrayIntMaximumConstraint>(m, std::vector<Variable*>{x1, x2}));

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

        model.add_constraint(std::make_unique<ArrayIntMaximumConstraint>(m, std::vector<Variable*>{x1, x2}));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE_FALSE(solution.has_value());
    }
}

// ============================================================================
// ArrayIntMinimumConstraint tests
// ============================================================================

TEST_CASE("ArrayIntMinimumConstraint name", "[constraint][array_int_minimum]") {
    Model model;
    auto* m = model.create_variable("m", Domain(1, 5));
    auto* x1 = model.create_variable("x1", Domain(1, 5));
    auto* x2 = model.create_variable("x2", Domain(1, 5));
    ArrayIntMinimumConstraint c(m, {x1, x2});

    REQUIRE(c.name() == "array_int_minimum");
}

TEST_CASE("ArrayIntMinimumConstraint is_satisfied", "[constraint][array_int_minimum]") {
    SECTION("satisfied when m equals min") {
        Model model;
        auto* m = model.create_variable("m", Domain(1, 1));
        auto* x1 = model.create_variable("x1", Domain(1, 1));
        auto* x2 = model.create_variable("x2", Domain(3, 3));
        auto* x3 = model.create_variable("x3", Domain(2, 2));
        ArrayIntMinimumConstraint c(m, {x1, x2, x3});

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == true);
    }

    SECTION("not satisfied when m differs from min") {
        Model model;
        auto* m = model.create_variable("m", Domain(2, 2));
        auto* x1 = model.create_variable("x1", Domain(1, 1));
        auto* x2 = model.create_variable("x2", Domain(3, 3));
        ArrayIntMinimumConstraint c(m, {x1, x2});

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == false);
    }
}

TEST_CASE("ArrayIntMinimumConstraint solver integration", "[constraint][array_int_minimum]") {
    SECTION("finds minimum value") {
        Model model;
        auto m = model.create_variable("m", 1, 10);
        auto x1 = model.create_variable("x1", 3);  // fixed
        auto x2 = model.create_variable("x2", 7);  // fixed
        auto x3 = model.create_variable("x3", 5);  // fixed

        model.add_constraint(std::make_unique<ArrayIntMinimumConstraint>(m, std::vector<Variable*>{x1, x2, x3}));

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

        model.add_constraint(std::make_unique<ArrayIntMinimumConstraint>(m, std::vector<Variable*>{x1, x2}));

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
// TableConstraint tests
// ============================================================================

TEST_CASE("TableConstraint name", "[constraint][table]") {
    Model model;
    auto* x = model.create_variable("x", Domain(1, 3));
    auto* y = model.create_variable("y", Domain(1, 3));
    // table: (1,2), (2,3), (3,1)
    TableConstraint c({x, y}, {1,2, 2,3, 3,1});
    REQUIRE(c.name() == "table_int");
}

TEST_CASE("TableConstraint variables", "[constraint][table]") {
    Model model;
    auto* x = model.create_variable("x", Domain(1, 3));
    auto* y = model.create_variable("y", Domain(1, 3));
    TableConstraint c({x, y}, {1,2, 2,3, 3,1});
    auto& vars = c.var_ids_ref();
    REQUIRE(vars.size() == 2);
}

TEST_CASE("TableConstraint is_satisfied", "[constraint][table]") {
    SECTION("satisfied - tuple in table") {
        Model model;
        auto* x = model.create_variable("x", Domain(1, 1));
        auto* y = model.create_variable("y", Domain(2, 2));
        TableConstraint c({x, y}, {1,2, 2,3, 3,1});
        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == true);
    }

    SECTION("violated - tuple not in table") {
        Model model;
        auto* x = model.create_variable("x", Domain(1, 1));
        auto* y = model.create_variable("y", Domain(3, 3));
        TableConstraint c({x, y}, {1,2, 2,3, 3,1});
        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == false);
    }

    SECTION("unknown - not fully assigned") {
        Model model;
        auto* x = model.create_variable("x", Domain(1, 3));
        auto* y = model.create_variable("y", Domain(2, 2));
        TableConstraint c({x, y}, {1,2, 2,3, 3,1});
        REQUIRE_FALSE(c.is_satisfied(model).has_value());
    }
}

TEST_CASE("TableConstraint presolve", "[constraint][table]") {
    SECTION("removes values not in any tuple") {
        Model model;
        auto* x = model.create_variable("x", Domain(1, 5));
        auto* y = model.create_variable("y", Domain(1, 5));
        // table: (1,2), (2,3)
        TableConstraint c({x, y}, {1,2, 2,3});
        REQUIRE(c.presolve(model) != PresolveResult::Contradiction);

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
    Model model;
    auto* x = model.create_variable("x", Domain(1, 3));
    auto* y = model.create_variable("y", Domain(1, 3));
    TableConstraint c({x, y}, {});
    // Empty table = no allowed tuples = unconditional contradiction
    REQUIRE(c.prepare_propagation(model) == false);
}

TEST_CASE("TableConstraint with Solver", "[constraint][table][solver]") {
    SECTION("find all solutions for 2-var table") {
        // table: (1,2), (2,3), (3,1)
        Model model;
        auto x = model.create_variable("x", 1, 3);
        auto y = model.create_variable("y", 1, 3);
        model.add_constraint(std::make_unique<TableConstraint>(
            std::vector<Variable*>{x, y},
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
        model.add_constraint(std::make_unique<TableConstraint>(
            std::vector<Variable*>{x, y, z},
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
        model.add_constraint(std::make_unique<TableConstraint>(
            std::vector<Variable*>{x, y},
            std::vector<Domain::value_type>{1,1, 2,2}));
        model.add_constraint(std::make_unique<IntNeConstraint>(x, y));

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

        model.add_constraint(std::make_unique<TableConstraint>(
            std::vector<Variable*>{x, y}, tuples));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution&) {
            return true;
        });

        REQUIRE(count == static_cast<size_t>(expected_count));
    }

    SECTION("sparse storage path: large num_tuples + wide-domain variable") {
        // group.fzn 由来の構造を再現: arity=3, num_tuples が大きく
        // 1列が広い domain を持ち、distinct 値数が大きいケース。
        // この構成で prefer_sparse() が true を返し、sparse 経路が走る。
        // 検証: sparse パスでも全解列挙が dense と一致する。
        Model model;
        auto x = model.create_variable("x", 1, 5);
        auto y = model.create_variable("y", 1, 200);   // 広い domain
        auto z = model.create_variable("z", -2, 2);

        std::vector<Domain::value_type> tuples;
        size_t expected = 0;
        // (i, 4*i + 10*k + j, j-2) を 5 * 200 * 5 通り生成する代わりに、
        // y を疎に振って distinct 値数 ≈ num_tuples を作る。
        for (int i = 1; i <= 5; ++i) {
            for (int t = 0; t < 200; ++t) {
                int yv = (t * 7 + i * 13) % 200 + 1;
                int zv = ((i + t) % 5) - 2;
                tuples.push_back(i);
                tuples.push_back(yv);
                tuples.push_back(zv);
                ++expected;
            }
        }
        REQUIRE(expected == 1000);

        model.add_constraint(std::make_unique<TableConstraint>(
            std::vector<Variable*>{x, y, z}, tuples));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution&) {
            return true;
        });

        REQUIRE(count == expected);
    }

    SECTION("sparse storage path: backtracking consistency") {
        // sparse パスで backtracking 後の current_table_ 復元と
        // residual の挙動が壊れていないかを確認する。
        // 制約を 2 個重ねて search 中に backtracking を強制する。
        Model model;
        auto x = model.create_variable("x", 1, 4);
        auto y = model.create_variable("y", 1, 200);
        auto z = model.create_variable("z", 1, 4);

        std::vector<Domain::value_type> tuples_xy;
        // (x, y) で大量の distinct y 値
        for (int i = 1; i <= 4; ++i) {
            for (int yv = 1; yv <= 200; ++yv) {
                if ((yv * i) % 7 != 0) {
                    tuples_xy.push_back(i);
                    tuples_xy.push_back(yv);
                }
            }
        }
        std::vector<Domain::value_type> tuples_yz;
        for (int yv = 1; yv <= 200; ++yv) {
            for (int j = 1; j <= 4; ++j) {
                if ((yv + j) % 3 == 0) {
                    tuples_yz.push_back(yv);
                    tuples_yz.push_back(j);
                }
            }
        }
        model.add_constraint(std::make_unique<TableConstraint>(
            std::vector<Variable*>{x, y}, tuples_xy));
        model.add_constraint(std::make_unique<TableConstraint>(
            std::vector<Variable*>{y, z}, tuples_yz));

        // ナイーブ参照解
        std::set<std::tuple<int,int,int>> ref;
        for (int i = 1; i <= 4; ++i) {
            for (int yv = 1; yv <= 200; ++yv) {
                if ((yv * i) % 7 == 0) continue;
                for (int j = 1; j <= 4; ++j) {
                    if ((yv + j) % 3 != 0) continue;
                    ref.emplace(i, yv, j);
                }
            }
        }

        Solver solver;
        std::set<std::tuple<int,int,int>> got;
        solver.solve_all(model, [&](const Solution& sol) {
            got.emplace(static_cast<int>(sol.at("x")),
                        static_cast<int>(sol.at("y")),
                        static_cast<int>(sol.at("z")));
            return true;
        });

        REQUIRE(got == ref);
    }
}
