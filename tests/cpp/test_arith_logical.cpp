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
// ArrayBoolAndConstraint tests
// ============================================================================

TEST_CASE("ArrayBoolAndConstraint name", "[constraint][array_bool_and]") {
    Model model;
    auto* b1 = model.create_variable("b1", Domain(0, 1));
    auto* b2 = model.create_variable("b2", Domain(0, 1));
    auto* r = model.create_variable("r", Domain(0, 1));
    ArrayBoolAndConstraint c({b1, b2}, r);

    REQUIRE(c.name() == "array_bool_and");
}

TEST_CASE("ArrayBoolAndConstraint variables", "[constraint][array_bool_and]") {
    Model model;
    auto* b1 = model.create_variable("b1", Domain(0, 1));
    auto* b2 = model.create_variable("b2", Domain(0, 1));
    auto* r = model.create_variable("r", Domain(0, 1));
    ArrayBoolAndConstraint c({b1, b2}, r);

    auto& vars = c.var_ids_ref();
    REQUIRE(vars.size() == 3);  // b1, b2, r
}

TEST_CASE("ArrayBoolAndConstraint is_satisfied", "[constraint][array_bool_and]") {
    SECTION("all true - r=1 satisfied") {
        Model model;
        auto* b1 = model.create_variable("b1", Domain(1, 1));
        auto* b2 = model.create_variable("b2", Domain(1, 1));
        auto* r = model.create_variable("r", Domain(1, 1));
        ArrayBoolAndConstraint c({b1, b2}, r);

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == true);
    }

    SECTION("one false - r=0 satisfied") {
        Model model;
        auto* b1 = model.create_variable("b1", Domain(1, 1));
        auto* b2 = model.create_variable("b2", Domain(0, 0));
        auto* r = model.create_variable("r", Domain(0, 0));
        ArrayBoolAndConstraint c({b1, b2}, r);

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == true);
    }

    SECTION("one false - r=1 not satisfied") {
        Model model;
        auto* b1 = model.create_variable("b1", Domain(1, 1));
        auto* b2 = model.create_variable("b2", Domain(0, 0));
        auto* r = model.create_variable("r", Domain(1, 1));
        ArrayBoolAndConstraint c({b1, b2}, r);

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == false);
    }

    SECTION("not all assigned - unknown") {
        Model model;
        auto* b1 = model.create_variable("b1", Domain(0, 1));
        auto* b2 = model.create_variable("b2", Domain(1, 1));
        auto* r = model.create_variable("r", Domain(0, 1));
        ArrayBoolAndConstraint c({b1, b2}, r);

        REQUIRE_FALSE(c.is_satisfied(model).has_value());
    }
}

TEST_CASE("ArrayBoolAndConstraint on_final_instantiate", "[constraint][array_bool_and]") {
    SECTION("all true - r=1") {
        Model model;
        auto b1 = model.create_variable("b1", 1, 1);
        auto b2 = model.create_variable("b2", 1, 1);
        auto r = model.create_variable("r", 1, 1);
        ArrayBoolAndConstraint c({b1, b2}, r);

        REQUIRE(c.on_final_instantiate(model) == true);
    }

    SECTION("one false - r=0") {
        Model model;
        auto b1 = model.create_variable("b1", 0, 0);
        auto b2 = model.create_variable("b2", 1, 1);
        auto r = model.create_variable("r", 0, 0);
        ArrayBoolAndConstraint c({b1, b2}, r);

        REQUIRE(c.on_final_instantiate(model) == true);
    }
}

TEST_CASE("ArrayBoolAndConstraint solver integration", "[constraint][array_bool_and]") {
    SECTION("r=1 forces all bi=1") {
        Model model;
        auto b1 = model.create_variable("b1", 0, 1);
        auto b2 = model.create_variable("b2", 0, 1);
        auto b3 = model.create_variable("b3", 0, 1);
        auto r = model.create_variable("r", 1);  // r = 1 fixed

        model.add_constraint(std::make_unique<ArrayBoolAndConstraint>(
            std::vector<Variable*>{b1, b2, b3}, r));

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

        model.add_constraint(std::make_unique<ArrayBoolAndConstraint>(
            std::vector<Variable*>{b1, b2, b3}, r));

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

        model.add_constraint(std::make_unique<ArrayBoolAndConstraint>(
            std::vector<Variable*>{b1, b2}, r));

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

        model.add_constraint(std::make_unique<ArrayBoolAndConstraint>(
            std::vector<Variable*>{b1, b2, b3}, r));

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

        model.add_constraint(std::make_unique<ArrayBoolAndConstraint>(
            std::vector<Variable*>{b1, b2, b3}, r));

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
    Model model;
    auto* b1 = model.create_variable("b1", Domain(0, 1));
    auto* b2 = model.create_variable("b2", Domain(0, 1));
    auto* r = model.create_variable("r", Domain(0, 1));
    ArrayBoolOrConstraint c({b1, b2}, r);

    REQUIRE(c.name() == "array_bool_or");
}

TEST_CASE("ArrayBoolOrConstraint is_satisfied", "[constraint][array_bool_or]") {
    SECTION("one true - r=1 satisfied") {
        Model model;
        auto* b1 = model.create_variable("b1", Domain(0, 0));
        auto* b2 = model.create_variable("b2", Domain(1, 1));
        auto* r = model.create_variable("r", Domain(1, 1));
        ArrayBoolOrConstraint c({b1, b2}, r);

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == true);
    }

    SECTION("all false - r=0 satisfied") {
        Model model;
        auto* b1 = model.create_variable("b1", Domain(0, 0));
        auto* b2 = model.create_variable("b2", Domain(0, 0));
        auto* r = model.create_variable("r", Domain(0, 0));
        ArrayBoolOrConstraint c({b1, b2}, r);

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == true);
    }

    SECTION("all false - r=1 not satisfied") {
        Model model;
        auto* b1 = model.create_variable("b1", Domain(0, 0));
        auto* b2 = model.create_variable("b2", Domain(0, 0));
        auto* r = model.create_variable("r", Domain(1, 1));
        ArrayBoolOrConstraint c({b1, b2}, r);

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == false);
    }
}

TEST_CASE("ArrayBoolOrConstraint solver integration", "[constraint][array_bool_or]") {
    SECTION("r=0 forces all bi=0") {
        Model model;
        auto b1 = model.create_variable("b1", 0, 1);
        auto b2 = model.create_variable("b2", 0, 1);
        auto r = model.create_variable("r", 0);  // r = 0 fixed

        model.add_constraint(std::make_unique<ArrayBoolOrConstraint>(
            std::vector<Variable*>{b1, b2}, r));

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

        model.add_constraint(std::make_unique<ArrayBoolOrConstraint>(
            std::vector<Variable*>{b1, b2}, r));

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

        model.add_constraint(std::make_unique<ArrayBoolOrConstraint>(
            std::vector<Variable*>{b1, b2}, r));

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

        model.add_constraint(std::make_unique<ArrayBoolOrConstraint>(
            std::vector<Variable*>{b1, b2, b3}, r));

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

        model.add_constraint(std::make_unique<ArrayBoolOrConstraint>(
            std::vector<Variable*>{b1, b2, b3}, r));

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
    Model model;
    auto* p1 = model.create_variable("p1", Domain(0, 1));
    auto* n1 = model.create_variable("n1", Domain(0, 1));
    BoolClauseConstraint c({p1}, {n1});

    REQUIRE(c.name() == "bool_clause");
}

TEST_CASE("BoolClauseConstraint is_satisfied", "[constraint][bool_clause]") {
    SECTION("positive literal true - satisfied") {
        // p1 ∨ ¬n1, p1=1 → satisfied
        Model model;
        auto* p1 = model.create_variable("p1", Domain(1, 1));
        auto* n1 = model.create_variable("n1", Domain(1, 1));
        BoolClauseConstraint c({p1}, {n1});

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == true);
    }

    SECTION("negative literal false - satisfied") {
        // p1 ∨ ¬n1, p1=0, n1=0 → satisfied (because ¬n1 = true)
        Model model;
        auto* p1 = model.create_variable("p1", Domain(0, 0));
        auto* n1 = model.create_variable("n1", Domain(0, 0));
        BoolClauseConstraint c({p1}, {n1});

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == true);
    }

    SECTION("all falsified - not satisfied") {
        // p1 ∨ ¬n1, p1=0, n1=1 → not satisfied
        Model model;
        auto* p1 = model.create_variable("p1", Domain(0, 0));
        auto* n1 = model.create_variable("n1", Domain(1, 1));
        BoolClauseConstraint c({p1}, {n1});

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == false);
    }

    SECTION("undetermined") {
        Model model;
        auto* p1 = model.create_variable("p1", Domain(0, 1));
        auto* n1 = model.create_variable("n1", Domain(0, 1));
        BoolClauseConstraint c({p1}, {n1});

        REQUIRE_FALSE(c.is_satisfied(model).has_value());
    }
}

TEST_CASE("BoolClauseConstraint unit propagation", "[constraint][bool_clause]") {
    SECTION("single positive literal - forces true") {
        // p1 (clause with only positive), p1 must be 1
        Model model;
        auto p1 = model.create_variable("p1", 0, 1);

        model.add_constraint(std::make_unique<BoolClauseConstraint>(
            std::vector<Variable*>{p1}, std::vector<Variable*>{}));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("p1") == 1);
    }

    SECTION("single negative literal - forces false") {
        // ¬n1 (clause with only negative), n1 must be 0
        Model model;
        auto n1 = model.create_variable("n1", 0, 1);

        model.add_constraint(std::make_unique<BoolClauseConstraint>(
            std::vector<Variable*>{}, std::vector<Variable*>{n1}));

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

        model.add_constraint(std::make_unique<BoolClauseConstraint>(
            std::vector<Variable*>{p1, p2, p3}, std::vector<Variable*>{}));

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

        model.add_constraint(std::make_unique<BoolClauseConstraint>(
            std::vector<Variable*>{p1}, std::vector<Variable*>{n1, n2}));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("n2") == 0);
    }
}

TEST_CASE("BoolClauseConstraint solver integration", "[constraint][bool_clause]") {
    SECTION("empty clause is unsatisfiable") {
        Model model;
        model.add_constraint(std::make_unique<BoolClauseConstraint>(
            std::vector<Variable*>{}, std::vector<Variable*>{}));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE_FALSE(solution.has_value());
    }

    SECTION("conflicting clauses") {
        // p1 AND ¬p1 is unsatisfiable
        Model model;
        auto p1 = model.create_variable("p1", 0, 1);

        // clause 1: p1 (p1 must be true)
        model.add_constraint(std::make_unique<BoolClauseConstraint>(
            std::vector<Variable*>{p1}, std::vector<Variable*>{}));
        // clause 2: ¬p1 (p1 must be false)
        model.add_constraint(std::make_unique<BoolClauseConstraint>(
            std::vector<Variable*>{}, std::vector<Variable*>{p1}));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE_FALSE(solution.has_value());
    }

    SECTION("all solutions for simple clause") {
        // p1 ∨ p2 has 3 solutions: (0,1), (1,0), (1,1)
        Model model;
        auto p1 = model.create_variable("p1", 0, 1);
        auto p2 = model.create_variable("p2", 0, 1);

        model.add_constraint(std::make_unique<BoolClauseConstraint>(
            std::vector<Variable*>{p1, p2}, std::vector<Variable*>{}));

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
        model.add_constraint(std::make_unique<BoolClauseConstraint>(
            std::vector<Variable*>{p1, p2}, std::vector<Variable*>{}));
        // ¬p1 ∨ p3 (pos={p3}, neg={p1})
        model.add_constraint(std::make_unique<BoolClauseConstraint>(
            std::vector<Variable*>{p3}, std::vector<Variable*>{p1}));
        // ¬p2 ∨ ¬p3 (pos={}, neg={p2, p3})
        model.add_constraint(std::make_unique<BoolClauseConstraint>(
            std::vector<Variable*>{}, std::vector<Variable*>{p2, p3}));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution&) { return true; });

        REQUIRE(count == 2);
    }
}

TEST_CASE("BoolClauseConstraint tautology (x in both pos and neg)",
          "[constraint][bool_clause][tautology]") {
    SECTION("x ∨ ¬x alone - always satisfied, x free") {
        Model model;
        auto x = model.create_variable("x", 0, 1);
        model.add_constraint(std::make_unique<BoolClauseConstraint>(
            std::vector<Variable*>{x}, std::vector<Variable*>{x}));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution&) { return true; });
        REQUIRE(count == 2);  // x=0 も x=1 も両方許容される
    }

    SECTION("x ∨ ¬x ∨ p - tautology dominates, p free") {
        Model model;
        auto x = model.create_variable("x", 0, 1);
        auto p = model.create_variable("p", 0, 1);
        model.add_constraint(std::make_unique<BoolClauseConstraint>(
            std::vector<Variable*>{x, p}, std::vector<Variable*>{x}));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution&) { return true; });
        REQUIRE(count == 4);  // 全組み合わせ許容
    }

    SECTION("x ∨ ¬x with x fixed to 0 - still satisfied") {
        Model model;
        auto x = model.create_variable("x", 0);  // fixed
        model.add_constraint(std::make_unique<BoolClauseConstraint>(
            std::vector<Variable*>{x}, std::vector<Variable*>{x}));

        Solver solver;
        auto solution = solver.solve(model);
        REQUIRE(solution.has_value());
        REQUIRE(solution->at("x") == 0);
    }

    SECTION("x ∨ ¬x with x fixed to 1 - still satisfied") {
        Model model;
        auto x = model.create_variable("x", 1);  // fixed
        model.add_constraint(std::make_unique<BoolClauseConstraint>(
            std::vector<Variable*>{x}, std::vector<Variable*>{x}));

        Solver solver;
        auto solution = solver.solve(model);
        REQUIRE(solution.has_value());
        REQUIRE(solution->at("x") == 1);
    }

    SECTION("tautology does not over-propagate unrelated vars") {
        // (x ∨ ¬x) ∧ (p ∨ q): tautology は何もしない、(p ∨ q) は通常通り
        Model model;
        auto x = model.create_variable("x", 0, 1);
        auto p = model.create_variable("p", 0);  // fixed 0
        auto q = model.create_variable("q", 0, 1);
        model.add_constraint(std::make_unique<BoolClauseConstraint>(
            std::vector<Variable*>{x}, std::vector<Variable*>{x}));
        model.add_constraint(std::make_unique<BoolClauseConstraint>(
            std::vector<Variable*>{p, q}, std::vector<Variable*>{}));

        Solver solver;
        auto solution = solver.solve(model);
        REQUIRE(solution.has_value());
        REQUIRE(solution->at("q") == 1);  // p=0 なので q=1 が強制される
    }
}

// ============================================================================
// IntAbsConstraint tests
// ============================================================================

TEST_CASE("IntAbsConstraint solve_all: positive x", "[constraint][int_abs]") {
    // x ∈ [1,5], y ∈ [0,10], |x| = y
    // 期待: (1,1),(2,2),(3,3),(4,4),(5,5) の 5 解
    Model model;
    auto x = model.create_variable("x", 1, 5);
    auto y = model.create_variable("y", 0, 10);
    model.add_constraint(std::make_unique<IntAbsConstraint>(x, y));

    Solver solver;
    size_t count = solver.solve_all(model, [](const Solution& sol) {
        auto x_val = sol.at("x");
        auto y_val = sol.at("y");
        REQUIRE(x_val >= 0);
        REQUIRE(y_val == x_val);
        return true;
    });
    REQUIRE(count == 5);
}

TEST_CASE("IntAbsConstraint solve_all: negative x", "[constraint][int_abs]") {
    // x ∈ [-5,-1], y ∈ [0,10], |x| = y
    // 期待: (-5,5),(-4,4),(-3,3),(-2,2),(-1,1) の 5 解
    Model model;
    auto x = model.create_variable("x", -5, -1);
    auto y = model.create_variable("y", 0, 10);
    model.add_constraint(std::make_unique<IntAbsConstraint>(x, y));

    Solver solver;
    size_t count = solver.solve_all(model, [](const Solution& sol) {
        auto x_val = sol.at("x");
        auto y_val = sol.at("y");
        REQUIRE(x_val <= 0);
        REQUIRE(y_val == -x_val);
        return true;
    });
    REQUIRE(count == 5);
}

TEST_CASE("IntAbsConstraint solve_all: x spans zero", "[constraint][int_abs]") {
    // x ∈ [-3,3], y ∈ [0,5], |x| = y
    // 期待: (-3,3),(-2,2),(-1,1),(0,0),(1,1),(2,2),(3,3) の 7 解
    Model model;
    auto x = model.create_variable("x", -3, 3);
    auto y = model.create_variable("y", 0, 5);
    model.add_constraint(std::make_unique<IntAbsConstraint>(x, y));

    Solver solver;
    std::set<std::pair<int,int>> solutions;
    size_t count = solver.solve_all(model, [&](const Solution& sol) {
        auto x_val = sol.at("x");
        auto y_val = sol.at("y");
        auto abs_x = (x_val >= 0) ? x_val : -x_val;
        REQUIRE(y_val == abs_x);
        solutions.insert({x_val, y_val});
        return true;
    });
    REQUIRE(count == 7);
    // 各解が含まれていることを確認
    REQUIRE(solutions.count({-3, 3}) == 1);
    REQUIRE(solutions.count({0, 0}) == 1);
    REQUIRE(solutions.count({3, 3}) == 1);
}

TEST_CASE("IntAbsConstraint solve_all: x spans zero, y constrained", "[constraint][int_abs]") {
    // x ∈ [-4,4], y ∈ [2,3], |x| = y
    // y_min=2 により |x|>=2 → x ∈ {-4,-3,-2,2,3,4}
    // y_max=3 により |x|<=3 → x ∈ {-3,-2,2,3}
    // 期待: (-3,3),(-2,2),(2,2),(3,3) の 4 解
    Model model;
    auto x = model.create_variable("x", -4, 4);
    auto y = model.create_variable("y", 2, 3);
    model.add_constraint(std::make_unique<IntAbsConstraint>(x, y));

    Solver solver;
    std::set<std::pair<int,int>> solutions;
    size_t count = solver.solve_all(model, [&](const Solution& sol) {
        auto x_val = sol.at("x");
        auto y_val = sol.at("y");
        auto abs_x = (x_val >= 0) ? x_val : -x_val;
        REQUIRE(y_val == abs_x);
        solutions.insert({x_val, y_val});
        return true;
    });
    REQUIRE(count == 4);
    REQUIRE(solutions.count({-3, 3}) == 1);
    REQUIRE(solutions.count({-2, 2}) == 1);
    REQUIRE(solutions.count({2, 2}) == 1);
    REQUIRE(solutions.count({3, 3}) == 1);
}

TEST_CASE("IntAbsConstraint bounds propagation", "[constraint][int_abs]") {
    SECTION("y fixed propagates x bounds") {
        // x ∈ [-10,10], y = 5, |x| = y → x ∈ {-5, 5}
        Model model;
        auto x = model.create_variable("x", -10, 10);
        auto y = model.create_variable("y", 5);
        model.add_constraint(std::make_unique<IntAbsConstraint>(x, y));

        Solver solver;
        std::set<int> x_vals;
        solver.solve_all(model, [&](const Solution& sol) {
            x_vals.insert(sol.at("x"));
            return true;
        });
        REQUIRE(x_vals.size() == 2);
        REQUIRE(x_vals.count(-5) == 1);
        REQUIRE(x_vals.count(5) == 1);
    }

    SECTION("x positive fixed propagates y") {
        // x = 7, y ∈ [0,10], |x| = y → y = 7
        Model model;
        auto x = model.create_variable("x", 7);
        auto y = model.create_variable("y", 0, 10);
        model.add_constraint(std::make_unique<IntAbsConstraint>(x, y));

        Solver solver;
        auto solution = solver.solve(model);
        REQUIRE(solution.has_value());
        REQUIRE(solution->at("y") == 7);
    }

    SECTION("x negative fixed propagates y") {
        // x = -4, y ∈ [0,10], |x| = y → y = 4
        Model model;
        auto x = model.create_variable("x", -4);
        auto y = model.create_variable("y", 0, 10);
        model.add_constraint(std::make_unique<IntAbsConstraint>(x, y));

        Solver solver;
        auto solution = solver.solve(model);
        REQUIRE(solution.has_value());
        REQUIRE(solution->at("y") == 4);
    }

    SECTION("unsatisfiable: y range too small") {
        // x ∈ [-5,-3], y ∈ [0,1], |x| = y → |x| ∈ [3,5] but y ∈ [0,1]: 矛盾
        Model model;
        auto x = model.create_variable("x", -5, -3);
        auto y = model.create_variable("y", 0, 1);
        model.add_constraint(std::make_unique<IntAbsConstraint>(x, y));

        Solver solver;
        auto solution = solver.solve(model);
        REQUIRE_FALSE(solution.has_value());
    }
}

// ============================================================================
// IntTimesConstraint tests
// ============================================================================

TEST_CASE("IntTimesConstraint name", "[constraint][int_times]") {
    Model model;
    auto* x = model.create_variable("x", Domain(1, 5));
    auto* y = model.create_variable("y", Domain(1, 5));
    auto* z = model.create_variable("z", Domain(1, 25));
    IntTimesConstraint c(x, y, z);

    REQUIRE(c.name() == "int_times");
}

TEST_CASE("IntTimesConstraint is_satisfied", "[constraint][int_times]") {
    SECTION("satisfied: 2 * 3 = 6") {
        Model model;
        auto* x = model.create_variable("x", Domain(2, 2));
        auto* y = model.create_variable("y", Domain(3, 3));
        auto* z = model.create_variable("z", Domain(6, 6));
        IntTimesConstraint c(x, y, z);

        auto satisfied = c.is_satisfied(model);
        REQUIRE(satisfied.has_value());
        REQUIRE(satisfied.value() == true);
    }

    SECTION("not satisfied: 2 * 3 != 5") {
        Model model;
        auto* x = model.create_variable("x", Domain(2, 2));
        auto* y = model.create_variable("y", Domain(3, 3));
        auto* z = model.create_variable("z", Domain(5, 5));
        IntTimesConstraint c(x, y, z);

        auto satisfied = c.is_satisfied(model);
        REQUIRE(satisfied.has_value());
        REQUIRE(satisfied.value() == false);
    }

    SECTION("unassigned") {
        Model model;
        auto* x = model.create_variable("x", Domain(1, 5));
        auto* y = model.create_variable("y", Domain(1, 5));
        auto* z = model.create_variable("z", Domain(1, 25));
        IntTimesConstraint c(x, y, z);

        REQUIRE_FALSE(c.is_satisfied(model).has_value());
    }
}

TEST_CASE("IntTimesConstraint solver integration", "[constraint][int_times]") {
    SECTION("basic multiplication") {
        Model model;
        auto x = model.create_variable("x", 3);  // x = 3
        auto y = model.create_variable("y", 4);  // y = 4
        auto z = model.create_variable("z", 1, 20);  // z to be determined

        model.add_constraint(std::make_unique<IntTimesConstraint>(x, y, z));

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

        model.add_constraint(std::make_unique<IntTimesConstraint>(x, y, z));

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

        model.add_constraint(std::make_unique<IntTimesConstraint>(x, y, z));

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

        model.add_constraint(std::make_unique<IntTimesConstraint>(x, y, z));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE_FALSE(solution.has_value());
    }

    SECTION("negative numbers") {
        Model model;
        auto x = model.create_variable("x", -3);  // x = -3
        auto y = model.create_variable("y", 4);   // y = 4
        auto z = model.create_variable("z", -20, 20);

        model.add_constraint(std::make_unique<IntTimesConstraint>(x, y, z));

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
        model.add_constraint(std::make_unique<IntTimesConstraint>(x, y, z));

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
        model.add_constraint(std::make_unique<IntTimesConstraint>(x, y, z));

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
        model.add_constraint(std::make_unique<IntTimesConstraint>(x, y, z));

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
        model.add_constraint(std::make_unique<IntTimesConstraint>(x, y, z));

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
        model.add_constraint(std::make_unique<IntTimesConstraint>(x, y, z));

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
        model.add_constraint(std::make_unique<IntTimesConstraint>(x, y, z));

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
        model.add_constraint(std::make_unique<IntTimesConstraint>(x, y, z));

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

TEST_CASE("IntTimesConstraint mixed sign propagation", "[constraint][int_times]") {
    SECTION("x positive fixed, y negative range") {
        Model model;
        auto x = model.create_variable("x", 3);
        auto y = model.create_variable("y", -4, -1);
        auto z = model.create_variable("z", -20, 20);
        model.add_constraint(std::make_unique<IntTimesConstraint>(x, y, z));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution& sol) {
            REQUIRE(sol.at("x") * sol.at("y") == sol.at("z"));
            return true;
        });
        // y in {-4,-3,-2,-1}, z = 3*y: {-12,-9,-6,-3}
        REQUIRE(count == 4);
    }

    SECTION("x negative fixed, y mixed range") {
        Model model;
        auto x = model.create_variable("x", -2);
        auto y = model.create_variable("y", -3, 3);
        auto z = model.create_variable("z", -10, 10);
        model.add_constraint(std::make_unique<IntTimesConstraint>(x, y, z));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution& sol) {
            REQUIRE(sol.at("x") * sol.at("y") == sol.at("z"));
            return true;
        });
        // y in {-3..3}, z = -2*y: {6,4,2,0,-2,-4,-6}
        REQUIRE(count == 7);
    }

    SECTION("y positive fixed, x negative range") {
        Model model;
        auto x = model.create_variable("x", -4, -1);
        auto y = model.create_variable("y", 3);
        auto z = model.create_variable("z", -20, 20);
        model.add_constraint(std::make_unique<IntTimesConstraint>(x, y, z));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution& sol) {
            REQUIRE(sol.at("x") * sol.at("y") == sol.at("z"));
            return true;
        });
        // x in {-4,-3,-2,-1}, z = x*3: {-12,-9,-6,-3}
        REQUIRE(count == 4);
    }

    SECTION("y negative fixed, x mixed range") {
        Model model;
        auto x = model.create_variable("x", -3, 3);
        auto y = model.create_variable("y", -2);
        auto z = model.create_variable("z", -10, 10);
        model.add_constraint(std::make_unique<IntTimesConstraint>(x, y, z));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution& sol) {
            REQUIRE(sol.at("x") * sol.at("y") == sol.at("z"));
            return true;
        });
        // x in {-3..3}, z = x*(-2): {6,4,2,0,-2,-4,-6}
        REQUIRE(count == 7);
    }

    SECTION("x zero, y mixed range") {
        Model model;
        auto x = model.create_variable("x", 0);
        auto y = model.create_variable("y", -3, 3);
        auto z = model.create_variable("z", -5, 5);
        model.add_constraint(std::make_unique<IntTimesConstraint>(x, y, z));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution& sol) {
            REQUIRE(sol.at("x") * sol.at("y") == sol.at("z"));
            REQUIRE(sol.at("z") == 0);
            return true;
        });
        // y in {-3..3}, z = 0 for all
        REQUIRE(count == 7);
    }

    SECTION("y zero, x mixed range") {
        Model model;
        auto x = model.create_variable("x", -3, 3);
        auto y = model.create_variable("y", 0);
        auto z = model.create_variable("z", -5, 5);
        model.add_constraint(std::make_unique<IntTimesConstraint>(x, y, z));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution& sol) {
            REQUIRE(sol.at("x") * sol.at("y") == sol.at("z"));
            REQUIRE(sol.at("z") == 0);
            return true;
        });
        // x in {-3..3}, z = 0 for all
        REQUIRE(count == 7);
    }

    SECTION("z range constrains valid products") {
        Model model;
        auto x = model.create_variable("x", -3);
        auto y = model.create_variable("y", -3, 3);
        auto z = model.create_variable("z", -5, 5);
        model.add_constraint(std::make_unique<IntTimesConstraint>(x, y, z));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution& sol) {
            REQUIRE(sol.at("x") * sol.at("y") == sol.at("z"));
            return true;
        });
        // x=-3, z=-3*y: y=-3→9, y=-2→6, y=-1→3, y=0→0, y=1→-3, y=2→-6, y=3→-9
        // z in [-5,5]: y in {-1,0,1} → z in {3,0,-3}
        REQUIRE(count == 3);
    }

    SECTION("all variables mixed range") {
        Model model;
        auto x = model.create_variable("x", -2, 2);
        auto y = model.create_variable("y", -2, 2);
        auto z = model.create_variable("z", -4, 4);
        model.add_constraint(std::make_unique<IntTimesConstraint>(x, y, z));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution& sol) {
            REQUIRE(sol.at("x") * sol.at("y") == sol.at("z"));
            return true;
        });
        // All 25 combinations should be valid (x*y in [-4,4])
        REQUIRE(count == 25);
    }
}

// ============================================================================
// CountEqConstraint tests
// ============================================================================

TEST_CASE("CountEqConstraint name", "[constraint][count_eq]") {
    Model model;
    auto* x1 = model.create_variable("x1", Domain(1, 3));
    auto* x2 = model.create_variable("x2", Domain(1, 3));
    auto* c = model.create_variable("c", Domain(0, 2));
    CountEqConstraint cst({x1, x2}, 2, c);

    REQUIRE(cst.name() == "count_eq");
}

TEST_CASE("CountEqConstraint variables", "[constraint][count_eq]") {
    Model model;
    auto* x1 = model.create_variable("x1", Domain(1, 3));
    auto* x2 = model.create_variable("x2", Domain(1, 3));
    auto* c = model.create_variable("c", Domain(0, 2));
    CountEqConstraint cst({x1, x2}, 2, c);

    auto& vars = cst.var_ids_ref();
    REQUIRE(vars.size() == 3);  // x1, x2, c
}

TEST_CASE("CountEqConstraint is_satisfied", "[constraint][count_eq]") {
    SECTION("satisfied - count matches") {
        Model model;
        auto* x1 = model.create_variable("x1", Domain(2, 2));
        auto* x2 = model.create_variable("x2", Domain(1, 1));
        auto* x3 = model.create_variable("x3", Domain(2, 2));
        auto* c = model.create_variable("c", Domain(2, 2));
        CountEqConstraint cst({x1, x2, x3}, 2, c);

        REQUIRE(cst.is_satisfied(model).has_value());
        REQUIRE(cst.is_satisfied(model).value() == true);
    }

    SECTION("violated - count does not match") {
        Model model;
        auto* x1 = model.create_variable("x1", Domain(2, 2));
        auto* x2 = model.create_variable("x2", Domain(1, 1));
        auto* x3 = model.create_variable("x3", Domain(2, 2));
        auto* c = model.create_variable("c", Domain(1, 1));
        CountEqConstraint cst({x1, x2, x3}, 2, c);

        REQUIRE(cst.is_satisfied(model).has_value());
        REQUIRE(cst.is_satisfied(model).value() == false);
    }

    SECTION("not fully assigned") {
        Model model;
        auto* x1 = model.create_variable("x1", Domain(1, 3));
        auto* x2 = model.create_variable("x2", Domain(2, 2));
        auto* c = model.create_variable("c", Domain(0, 2));
        CountEqConstraint cst({x1, x2}, 2, c);

        REQUIRE_FALSE(cst.is_satisfied(model).has_value());
    }
}

TEST_CASE("CountEqConstraint presolve", "[constraint][count_eq]") {
    SECTION("c bounds tightened") {
        Model model;
        auto* x1 = model.create_variable("x1", Domain(2, 2));  // definite: target=2
        auto* x2 = model.create_variable("x2", Domain(1, 3));  // possible
        auto* x3 = model.create_variable("x3", Domain(3, 3));  // not possible (no 2)
        auto* c = model.create_variable("c", Domain(0, 3));
        CountEqConstraint cst({x1, x2, x3}, 2, c);

        REQUIRE(cst.presolve(model) != PresolveResult::Contradiction);
        // definite=1, possible=1 → c ∈ [1, 2]
        REQUIRE(c->min() == 1);
        REQUIRE(c->max() == 2);
    }

    SECTION("all must be target when c forces it") {
        Model model;
        auto* x1 = model.create_variable("x1", Domain(1, 3));
        auto* x2 = model.create_variable("x2", Domain(1, 3));
        auto* x3 = model.create_variable("x3", Domain(1, 3));
        auto* c = model.create_variable("c", Domain(3, 3));
        CountEqConstraint cst({x1, x2, x3}, 2, c);

        REQUIRE(cst.presolve(model) != PresolveResult::Contradiction);
        // c=3, definite=0, possible=3 → c.min(3) == def+poss(3) → all forced to 2
        REQUIRE(x1->min() == 2);
        REQUIRE(x1->max() == 2);
        REQUIRE(x2->min() == 2);
        REQUIRE(x2->max() == 2);
        REQUIRE(x3->min() == 2);
        REQUIRE(x3->max() == 2);
    }

    SECTION("none can be target when c=0") {
        Model model;
        auto* x1 = model.create_variable("x1", Domain(1, 3));
        auto* x2 = model.create_variable("x2", Domain(1, 3));
        auto* c = model.create_variable("c", Domain(0, 0));
        CountEqConstraint cst({x1, x2}, 2, c);

        REQUIRE(cst.presolve(model) != PresolveResult::Contradiction);
        // c=0, definite=0 → c.max(0)==definite(0) → remove target from all
        REQUIRE_FALSE(x1->domain().contains(2));
        REQUIRE_FALSE(x2->domain().contains(2));
    }

    SECTION("infeasible - not enough possible") {
        Model model;
        auto* x1 = model.create_variable("x1", Domain(3, 3));
        auto* x2 = model.create_variable("x2", Domain(3, 3));
        auto* c = model.create_variable("c", Domain(2, 2));
        CountEqConstraint cst({x1, x2}, 2, c);

        // definite=0, possible=0, c=2 → impossible
        REQUIRE(cst.presolve(model) == PresolveResult::Contradiction);
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
        model.add_constraint(std::make_unique<CountEqConstraint>(
            std::vector<Variable*>{x1, x2}, 2, c));

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
        model.add_constraint(std::make_unique<CountEqConstraint>(
            std::vector<Variable*>{x1, x2, x3}, 1, c));

        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution&) {
            return true;
        });

        REQUIRE(count == 6);
    }
}

// ============================================================================
// IncreasingConstraint tests
// ============================================================================

TEST_CASE("IncreasingConstraint name", "[constraint][increasing]") {
    Model model;
    auto* x = model.create_variable("x", Domain(1, 3));
    auto* y = model.create_variable("y", Domain(1, 3));
    IncreasingConstraint inc({x, y}, /*strict=*/false);
    IncreasingConstraint sinc({x, y}, /*strict=*/true);
    REQUIRE(inc.name() == "increasing");
    REQUIRE(sinc.name() == "strictly_increasing");
}

TEST_CASE("IncreasingConstraint non-strict presolve", "[constraint][increasing]") {
    Model model;
    auto* x = model.create_variable("x", Domain(2, 5));
    auto* y = model.create_variable("y", Domain(1, 4));
    auto* z = model.create_variable("z", Domain(0, 6));
    IncreasingConstraint c({x, y, z}, /*strict=*/false);
    auto r = c.presolve(model);
    REQUIRE(r != PresolveResult::Contradiction);
    // y.min >= x.min = 2; z.min >= y.min = 2
    REQUIRE(y->min() >= 2);
    REQUIRE(z->min() >= 2);
    // x.max <= y.max <= z.max = 6 → also bounded by y.max <= z.max
    REQUIRE(x->max() <= y->max());
    REQUIRE(y->max() <= z->max());
}

TEST_CASE("IncreasingConstraint strict presolve", "[constraint][increasing]") {
    Model model;
    auto* x = model.create_variable("x", Domain(1, 5));
    auto* y = model.create_variable("y", Domain(1, 5));
    auto* z = model.create_variable("z", Domain(1, 5));
    IncreasingConstraint c({x, y, z}, /*strict=*/true);
    REQUIRE(c.presolve(model) != PresolveResult::Contradiction);
    REQUIRE(x->max() <= 3);  // x < y < z, z <= 5 → x <= 3
    REQUIRE(z->min() >= 3);  // x >= 1, y > x, z > y → z >= 3
}

TEST_CASE("IncreasingConstraint detects infeasibility", "[constraint][increasing]") {
    SECTION("non-strict over too narrow domain") {
        Model model;
        auto* x = model.create_variable("x", Domain(3, 5));
        auto* y = model.create_variable("y", Domain(1, 2));  // y < x always
        IncreasingConstraint c({x, y}, /*strict=*/false);
        REQUIRE(c.presolve(model) == PresolveResult::Contradiction);
    }
    SECTION("strict over equal singletons") {
        Model model;
        auto* x = model.create_variable("x", 2, 2);
        auto* y = model.create_variable("y", 2, 2);
        IncreasingConstraint c({x, y}, /*strict=*/true);
        REQUIRE(c.presolve(model) == PresolveResult::Contradiction);
    }
}

TEST_CASE("IncreasingConstraint solve_all enumerates correct count", "[constraint][increasing]") {
    SECTION("strictly_increasing: C(5,3) = 10 monotonic chains") {
        Model model;
        auto* x = model.create_variable("x", Domain(1, 5));
        auto* y = model.create_variable("y", Domain(1, 5));
        auto* z = model.create_variable("z", Domain(1, 5));
        model.add_constraint(std::make_unique<IncreasingConstraint>(
            std::vector<Variable*>{x, y, z}, /*strict=*/true));
        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution&) { return true; });
        REQUIRE(count == 10);
    }
    SECTION("increasing (non-strict): C(5+3-1, 3) = 35 weak chains") {
        Model model;
        auto* x = model.create_variable("x", Domain(1, 5));
        auto* y = model.create_variable("y", Domain(1, 5));
        auto* z = model.create_variable("z", Domain(1, 5));
        model.add_constraint(std::make_unique<IncreasingConstraint>(
            std::vector<Variable*>{x, y, z}, /*strict=*/false));
        Solver solver;
        size_t count = solver.solve_all(model, [](const Solution&) { return true; });
        REQUIRE(count == 35);
    }
}
