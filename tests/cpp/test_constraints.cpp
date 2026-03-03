#include <catch2/catch_test_macros.hpp>
#include "sabori_csp/constraint.hpp"
#include "sabori_csp/variable.hpp"
#include "sabori_csp/domain.hpp"
#include "sabori_csp/model.hpp"
#include "sabori_csp/solver.hpp"

using namespace sabori_csp;

// Shared model for tests (variables are registered here for SoA access)
static Model dummy_model;

// Helper to create a variable (registered in dummy_model)
VariablePtr make_var(const std::string& name, Domain::value_type min, Domain::value_type max) {
    return dummy_model.create_variable(name, min, max);
}

VariablePtr make_var(const std::string& name, Domain::value_type value) {
    return dummy_model.create_variable(name, value);
}

// ============================================================================
// IntEqConstraint tests
// ============================================================================

TEST_CASE("IntEqConstraint name", "[constraint][int_eq]") {
    auto x = make_var("x", 1, 3);
    auto y = make_var("y", 1, 3);
    IntEqConstraint c(x, y);

    REQUIRE(c.name() == "int_eq");
}

TEST_CASE("IntEqConstraint variables", "[constraint][int_eq]") {
    auto x = make_var("x", 1, 3);
    auto y = make_var("y", 1, 3);
    IntEqConstraint c(x, y);

    auto& vars = c.var_ids_ref();
    REQUIRE(vars.size() == 2);
    REQUIRE(vars[0] == x->id());
    REQUIRE(vars[1] == y->id());
}

TEST_CASE("IntEqConstraint is_satisfied", "[constraint][int_eq]") {
    SECTION("both assigned and equal") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        IntEqConstraint c(x, y);

        REQUIRE(c.is_satisfied(dummy_model).has_value());
        REQUIRE(c.is_satisfied(dummy_model).value() == true);
    }

    SECTION("both assigned and not equal") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        IntEqConstraint c(x, y);

        REQUIRE(c.is_satisfied(dummy_model).has_value());
        REQUIRE(c.is_satisfied(dummy_model).value() == false);
    }

    SECTION("not fully assigned") {
        auto x = make_var("x", 1, 3);
        auto y = make_var("y", 5);
        IntEqConstraint c(x, y);

        REQUIRE_FALSE(c.is_satisfied(dummy_model).has_value());
    }
}

TEST_CASE("IntEqConstraint propagate", "[constraint][int_eq]") {
    SECTION("intersect domains") {
        auto x = make_var("x", 1, 5);
        auto y = make_var("y", 3, 7);
        IntEqConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) != PresolveResult::Contradiction);
        REQUIRE(x->min() == 3);
        REQUIRE(x->max() == 5);
        REQUIRE(y->min() == 3);
        REQUIRE(y->max() == 5);
    }

    SECTION("no intersection - failure") {
        auto x = make_var("x", 1, 2);
        auto y = make_var("y", 5, 6);
        IntEqConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) == PresolveResult::Contradiction);
    }

    SECTION("one value in common") {
        auto x = make_var("x", 1, 3);
        auto y = make_var("y", 3, 5);
        IntEqConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) != PresolveResult::Contradiction);
        REQUIRE(x->domain().size() == 1);
        REQUIRE(y->domain().size() == 1);
        REQUIRE(x->assigned_value() == 3);
        REQUIRE(y->assigned_value() == 3);
    }
}

// ============================================================================
// IntNeConstraint tests
// ============================================================================

TEST_CASE("IntNeConstraint name", "[constraint][int_ne]") {
    auto x = make_var("x", 1, 3);
    auto y = make_var("y", 1, 3);
    IntNeConstraint c(x, y);

    REQUIRE(c.name() == "int_ne");
}

TEST_CASE("IntNeConstraint is_satisfied", "[constraint][int_ne]") {
    SECTION("both assigned and different") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        IntNeConstraint c(x, y);

        REQUIRE(c.is_satisfied(dummy_model).has_value());
        REQUIRE(c.is_satisfied(dummy_model).value() == true);
    }

    SECTION("both assigned and equal") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        IntNeConstraint c(x, y);

        REQUIRE(c.is_satisfied(dummy_model).has_value());
        REQUIRE(c.is_satisfied(dummy_model).value() == false);
    }

    SECTION("not fully assigned") {
        auto x = make_var("x", 1, 3);
        auto y = make_var("y", 5);
        IntNeConstraint c(x, y);

        REQUIRE_FALSE(c.is_satisfied(dummy_model).has_value());
    }
}

TEST_CASE("IntNeConstraint propagate", "[constraint][int_ne]") {
    SECTION("x assigned - remove from y") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 1, 5);
        IntNeConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) != PresolveResult::Contradiction);
        REQUIRE_FALSE(y->domain().contains(3));
        REQUIRE(y->domain().size() == 4);
    }

    SECTION("y assigned - remove from x") {
        auto x = make_var("x", 1, 5);
        auto y = make_var("y", 3);
        IntNeConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) != PresolveResult::Contradiction);
        REQUIRE_FALSE(x->domain().contains(3));
        REQUIRE(x->domain().size() == 4);
    }

    SECTION("both singletons with same value - failure") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 3);
        IntNeConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) == PresolveResult::Contradiction);
    }

    SECTION("both singletons with different values - success") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        IntNeConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) != PresolveResult::Contradiction);
    }

    SECTION("neither assigned - no change") {
        auto x = make_var("x", 1, 3);
        auto y = make_var("y", 1, 3);
        IntNeConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) != PresolveResult::Contradiction);
        REQUIRE(x->domain().size() == 3);
        REQUIRE(y->domain().size() == 3);
    }
}

// ============================================================================
// IntLtConstraint tests
// ============================================================================

TEST_CASE("IntLtConstraint name", "[constraint][int_lt]") {
    auto x = make_var("x", 1, 3);
    auto y = make_var("y", 1, 3);
    IntLtConstraint c(x, y);

    REQUIRE(c.name() == "int_lt");
}

TEST_CASE("IntLtConstraint is_satisfied", "[constraint][int_lt]") {
    SECTION("x < y") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        IntLtConstraint c(x, y);

        REQUIRE(c.is_satisfied(dummy_model).has_value());
        REQUIRE(c.is_satisfied(dummy_model).value() == true);
    }

    SECTION("x == y") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        IntLtConstraint c(x, y);

        REQUIRE(c.is_satisfied(dummy_model).has_value());
        REQUIRE(c.is_satisfied(dummy_model).value() == false);
    }

    SECTION("x > y") {
        auto x = make_var("x", 7);
        auto y = make_var("y", 5);
        IntLtConstraint c(x, y);

        REQUIRE(c.is_satisfied(dummy_model).has_value());
        REQUIRE(c.is_satisfied(dummy_model).value() == false);
    }

    SECTION("not fully assigned") {
        auto x = make_var("x", 1, 3);
        auto y = make_var("y", 5);
        IntLtConstraint c(x, y);

        REQUIRE_FALSE(c.is_satisfied(dummy_model).has_value());
    }
}

TEST_CASE("IntLtConstraint propagate", "[constraint][int_lt]") {
    SECTION("basic bound propagation") {
        auto x = make_var("x", 1, 5);
        auto y = make_var("y", 1, 5);
        IntLtConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) != PresolveResult::Contradiction);
        // x < y means x.max < y.max, y.min > x.min
        REQUIRE(x->max() == 4);  // x < 5
        REQUIRE(y->min() == 2);  // y > 1
    }

    SECTION("infeasible - x.min >= y.max") {
        auto x = make_var("x", 5, 7);
        auto y = make_var("y", 1, 3);
        IntLtConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) == PresolveResult::Contradiction);
    }

    SECTION("tight domains") {
        auto x = make_var("x", 1, 2);
        auto y = make_var("y", 2, 3);
        IntLtConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) != PresolveResult::Contradiction);
        // x must be < y.max (3), so x can be 1 or 2
        // y must be > x.min (1), so y can be 2 or 3
        REQUIRE(x->domain().contains(1));
        REQUIRE(x->domain().contains(2));
        REQUIRE(y->domain().contains(2));
        REQUIRE(y->domain().contains(3));
    }
}

// ============================================================================
// IntLeConstraint tests
// ============================================================================

TEST_CASE("IntLeConstraint name", "[constraint][int_le]") {
    auto x = make_var("x", 1, 3);
    auto y = make_var("y", 1, 3);
    IntLeConstraint c(x, y);

    REQUIRE(c.name() == "int_le");
}

TEST_CASE("IntLeConstraint is_satisfied", "[constraint][int_le]") {
    SECTION("x < y") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        IntLeConstraint c(x, y);

        REQUIRE(c.is_satisfied(dummy_model).has_value());
        REQUIRE(c.is_satisfied(dummy_model).value() == true);
    }

    SECTION("x == y") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        IntLeConstraint c(x, y);

        REQUIRE(c.is_satisfied(dummy_model).has_value());
        REQUIRE(c.is_satisfied(dummy_model).value() == true);
    }

    SECTION("x > y") {
        auto x = make_var("x", 7);
        auto y = make_var("y", 5);
        IntLeConstraint c(x, y);

        REQUIRE(c.is_satisfied(dummy_model).has_value());
        REQUIRE(c.is_satisfied(dummy_model).value() == false);
    }

    SECTION("not fully assigned") {
        auto x = make_var("x", 1, 3);
        auto y = make_var("y", 5);
        IntLeConstraint c(x, y);

        REQUIRE_FALSE(c.is_satisfied(dummy_model).has_value());
    }
}

TEST_CASE("IntLeConstraint propagate", "[constraint][int_le]") {
    SECTION("basic bound propagation") {
        auto x = make_var("x", 1, 5);
        auto y = make_var("y", 1, 5);
        IntLeConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) != PresolveResult::Contradiction);
        // x <= y means x.max <= y.max, y.min >= x.min
        REQUIRE(x->max() == 5);
        REQUIRE(y->min() == 1);
    }

    SECTION("propagate upper bound of x") {
        auto x = make_var("x", 1, 10);
        auto y = make_var("y", 1, 5);
        IntLeConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) != PresolveResult::Contradiction);
        REQUIRE(x->max() == 5);  // x <= y.max
    }

    SECTION("propagate lower bound of y") {
        auto x = make_var("x", 3, 5);
        auto y = make_var("y", 1, 10);
        IntLeConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) != PresolveResult::Contradiction);
        REQUIRE(y->min() == 3);  // y >= x.min
    }

    SECTION("infeasible - x.min > y.max") {
        auto x = make_var("x", 6, 8);
        auto y = make_var("y", 1, 3);
        IntLeConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) == PresolveResult::Contradiction);
    }

    SECTION("equal singleton values - success") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        IntLeConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) != PresolveResult::Contradiction);
    }
}

// ============================================================================
// IntEqReifConstraint tests
// ============================================================================

TEST_CASE("IntEqReifConstraint name", "[constraint][int_eq_reif]") {
    auto x = make_var("x", 1, 3);
    auto y = make_var("y", 1, 3);
    auto b = make_var("b", 0, 1);
    IntEqReifConstraint c(x, y, b);

    REQUIRE(c.name() == "int_eq_reif");
}

TEST_CASE("IntEqReifConstraint variables", "[constraint][int_eq_reif]") {
    auto x = make_var("x", 1, 3);
    auto y = make_var("y", 1, 3);
    auto b = make_var("b", 0, 1);
    IntEqReifConstraint c(x, y, b);

    auto& vars = c.var_ids_ref();
    REQUIRE(vars.size() == 3);
    REQUIRE(vars[0] == x->id());
    REQUIRE(vars[1] == y->id());
    REQUIRE(vars[2] == b->id());
}

TEST_CASE("IntEqReifConstraint is_satisfied", "[constraint][int_eq_reif]") {
    SECTION("x == y and b == 1") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        auto b = make_var("b", 1);
        IntEqReifConstraint c(x, y, b);

        REQUIRE(c.is_satisfied(dummy_model).has_value());
        REQUIRE(c.is_satisfied(dummy_model).value() == true);
    }

    SECTION("x != y and b == 0") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        auto b = make_var("b", 0);
        IntEqReifConstraint c(x, y, b);

        REQUIRE(c.is_satisfied(dummy_model).has_value());
        REQUIRE(c.is_satisfied(dummy_model).value() == true);
    }

    SECTION("x == y and b == 0 - violated") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        auto b = make_var("b", 0);
        IntEqReifConstraint c(x, y, b);

        REQUIRE(c.is_satisfied(dummy_model).has_value());
        REQUIRE(c.is_satisfied(dummy_model).value() == false);
    }

    SECTION("x != y and b == 1 - violated") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        auto b = make_var("b", 1);
        IntEqReifConstraint c(x, y, b);

        REQUIRE(c.is_satisfied(dummy_model).has_value());
        REQUIRE(c.is_satisfied(dummy_model).value() == false);
    }

    SECTION("not fully assigned") {
        auto x = make_var("x", 1, 3);
        auto y = make_var("y", 5);
        auto b = make_var("b", 0, 1);
        IntEqReifConstraint c(x, y, b);

        REQUIRE_FALSE(c.is_satisfied(dummy_model).has_value());
    }
}

TEST_CASE("IntEqReifConstraint propagate", "[constraint][int_eq_reif]") {
    SECTION("b=1 enforces x==y intersection") {
        auto x = make_var("x", 1, 5);
        auto y = make_var("y", 3, 7);
        auto b = make_var("b", 1);
        IntEqReifConstraint c(x, y, b);

        REQUIRE(c.presolve(dummy_model) != PresolveResult::Contradiction);
        REQUIRE(x->min() == 3);
        REQUIRE(x->max() == 5);
        REQUIRE(y->min() == 3);
        REQUIRE(y->max() == 5);
    }

    SECTION("b=0 and x singleton removes from y") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 1, 5);
        auto b = make_var("b", 0);
        IntEqReifConstraint c(x, y, b);

        REQUIRE(c.presolve(dummy_model) != PresolveResult::Contradiction);
        REQUIRE_FALSE(y->domain().contains(3));
    }

    SECTION("x==y singletons set b=1") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        auto b = make_var("b", 0, 1);
        IntEqReifConstraint c(x, y, b);

        REQUIRE(c.presolve(dummy_model) != PresolveResult::Contradiction);
        REQUIRE(b->is_assigned());
        REQUIRE(b->assigned_value() == 1);
    }

    SECTION("x!=y singletons set b=0") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        auto b = make_var("b", 0, 1);
        IntEqReifConstraint c(x, y, b);

        REQUIRE(c.presolve(dummy_model) != PresolveResult::Contradiction);
        REQUIRE(b->is_assigned());
        REQUIRE(b->assigned_value() == 0);
    }

    SECTION("b=1 with no intersection - failure") {
        auto x = make_var("x", 1, 2);
        auto y = make_var("y", 5, 6);
        auto b = make_var("b", 1);
        IntEqReifConstraint c(x, y, b);

        REQUIRE(c.presolve(dummy_model) == PresolveResult::Contradiction);
    }
}

// ============================================================================
// 2-Watched Literal (2WL) tests
// ============================================================================

TEST_CASE("Constraint ID assignment", "[constraint][2wl]") {
    auto x1 = make_var("x1", 1, 3);
    auto y1 = make_var("y1", 1, 3);
    auto x2 = make_var("x2", 1, 3);
    auto y2 = make_var("y2", 1, 3);

    IntEqConstraint c1(x1, y1);
    IntEqConstraint c2(x2, y2);

    // 各制約にはユニークなIDが付与される
    REQUIRE(c1.id() != c2.id());
}

TEST_CASE("Constraint watches initialization", "[constraint][2wl]") {
    SECTION("two unassigned variables") {
        auto x = make_var("x", 1, 3);
        auto y = make_var("y", 1, 3);
        IntEqConstraint c(x, y);

        // 両方未確定なら w1=0, w2=1 を監視
        REQUIRE(c.watch1() == 0);
        REQUIRE(c.watch2() == 1);
    }

    SECTION("one assigned variable") {
        auto x = make_var("x", 5);  // assigned
        auto y = make_var("y", 1, 3);
        IntEqConstraint c(x, y);

        // init_watches はデフォルト w1=0, w2=1 を設定
        // refine_watches(Model&) が後で適切に再配置する
        REQUIRE(c.watch1() == 0);
        REQUIRE(c.watch2() == 1);
    }

    SECTION("both assigned variables") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        IntEqConstraint c(x, y);
    }
}

TEST_CASE("Constraint on_final_instantiate", "[constraint][2wl]") {
    SECTION("IntEqConstraint - satisfied") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        IntEqConstraint c(x, y);

        REQUIRE(c.on_final_instantiate(dummy_model) == true);
    }

    SECTION("IntEqConstraint - violated") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        IntEqConstraint c(x, y);

        REQUIRE(c.on_final_instantiate(dummy_model) == false);
    }

    SECTION("IntNeConstraint - satisfied") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        IntNeConstraint c(x, y);

        REQUIRE(c.on_final_instantiate(dummy_model) == true);
    }

    SECTION("IntNeConstraint - violated") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        IntNeConstraint c(x, y);

        REQUIRE(c.on_final_instantiate(dummy_model) == false);
    }

    SECTION("IntLtConstraint - satisfied") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        IntLtConstraint c(x, y);

        REQUIRE(c.on_final_instantiate(dummy_model) == true);
    }

    SECTION("IntLtConstraint - violated (equal)") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        IntLtConstraint c(x, y);

        REQUIRE(c.on_final_instantiate(dummy_model) == false);
    }

    SECTION("IntLeConstraint - satisfied (equal)") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        IntLeConstraint c(x, y);

        REQUIRE(c.on_final_instantiate(dummy_model) == true);
    }

    SECTION("IntLeConstraint - violated") {
        auto x = make_var("x", 7);
        auto y = make_var("y", 5);
        IntLeConstraint c(x, y);

        REQUIRE(c.on_final_instantiate(dummy_model) == false);
    }
}

// ============================================================================
// IntLeReifConstraint tests
// ============================================================================

TEST_CASE("IntLeReifConstraint name", "[constraint][int_le_reif]") {
    auto x = make_var("x", 1, 3);
    auto y = make_var("y", 1, 3);
    auto b = make_var("b", 0, 1);
    IntLeReifConstraint c(x, y, b);

    REQUIRE(c.name() == "int_le_reif");
}

TEST_CASE("IntLeReifConstraint variables", "[constraint][int_le_reif]") {
    auto x = make_var("x", 1, 3);
    auto y = make_var("y", 1, 3);
    auto b = make_var("b", 0, 1);
    IntLeReifConstraint c(x, y, b);

    auto& vars = c.var_ids_ref();
    REQUIRE(vars.size() == 3);
    REQUIRE(vars[0] == x->id());
    REQUIRE(vars[1] == y->id());
    REQUIRE(vars[2] == b->id());
}

TEST_CASE("IntLeReifConstraint is_satisfied", "[constraint][int_le_reif]") {
    SECTION("x <= y and b == 1") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        auto b = make_var("b", 1);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.is_satisfied(dummy_model).has_value());
        REQUIRE(c.is_satisfied(dummy_model).value() == true);
    }

    SECTION("x == y and b == 1") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        auto b = make_var("b", 1);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.is_satisfied(dummy_model).has_value());
        REQUIRE(c.is_satisfied(dummy_model).value() == true);
    }

    SECTION("x > y and b == 0") {
        auto x = make_var("x", 7);
        auto y = make_var("y", 5);
        auto b = make_var("b", 0);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.is_satisfied(dummy_model).has_value());
        REQUIRE(c.is_satisfied(dummy_model).value() == true);
    }

    SECTION("x <= y but b == 0") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        auto b = make_var("b", 0);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.is_satisfied(dummy_model).has_value());
        REQUIRE(c.is_satisfied(dummy_model).value() == false);
    }

    SECTION("x > y but b == 1") {
        auto x = make_var("x", 7);
        auto y = make_var("y", 5);
        auto b = make_var("b", 1);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.is_satisfied(dummy_model).has_value());
        REQUIRE(c.is_satisfied(dummy_model).value() == false);
    }

    SECTION("not fully assigned") {
        auto x = make_var("x", 1, 3);
        auto y = make_var("y", 5);
        auto b = make_var("b", 0, 1);
        IntLeReifConstraint c(x, y, b);

        REQUIRE_FALSE(c.is_satisfied(dummy_model).has_value());
    }
}

TEST_CASE("IntLeReifConstraint propagate", "[constraint][int_le_reif]") {
    SECTION("b=1 enforces x <= y") {
        auto x = make_var("x", 1, 10);
        auto y = make_var("y", 1, 5);
        auto b = make_var("b", 1);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.presolve(dummy_model) != PresolveResult::Contradiction);
        REQUIRE(x->max() == 5);  // x <= y.max
    }

    SECTION("b=0 enforces x > y") {
        auto x = make_var("x", 1, 10);
        auto y = make_var("y", 3, 8);
        auto b = make_var("b", 0);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.presolve(dummy_model) != PresolveResult::Contradiction);
        REQUIRE(x->min() == 4);  // x > y.min (3)
        REQUIRE(y->max() == 8);  // y < x.max would be 9, but y.max is 8
    }

    SECTION("x.max <= y.min implies b=1") {
        auto x = make_var("x", 1, 3);
        auto y = make_var("y", 5, 10);
        auto b = make_var("b", 0, 1);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.presolve(dummy_model) != PresolveResult::Contradiction);
        REQUIRE(b->is_assigned());
        REQUIRE(b->assigned_value().value() == 1);
    }

    SECTION("x.min > y.max implies b=0") {
        auto x = make_var("x", 8, 10);
        auto y = make_var("y", 1, 5);
        auto b = make_var("b", 0, 1);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.presolve(dummy_model) != PresolveResult::Contradiction);
        REQUIRE(b->is_assigned());
        REQUIRE(b->assigned_value().value() == 0);
    }

    SECTION("b=1 with x.min > y.max is infeasible") {
        auto x = make_var("x", 8, 10);
        auto y = make_var("y", 1, 5);
        auto b = make_var("b", 1);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.presolve(dummy_model) == PresolveResult::Contradiction);
    }

    SECTION("b=0 with x.max <= y.min is infeasible") {
        auto x = make_var("x", 1, 3);
        auto y = make_var("y", 5, 10);
        auto b = make_var("b", 0);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.presolve(dummy_model) == PresolveResult::Contradiction);
    }
}

TEST_CASE("IntLeReifConstraint on_final_instantiate", "[constraint][int_le_reif]") {
    SECTION("satisfied - x <= y and b=1") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        auto b = make_var("b", 1);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.on_final_instantiate(dummy_model) == true);
    }

    SECTION("satisfied - x > y and b=0") {
        auto x = make_var("x", 7);
        auto y = make_var("y", 5);
        auto b = make_var("b", 0);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.on_final_instantiate(dummy_model) == true);
    }

    SECTION("violated - x <= y but b=0") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        auto b = make_var("b", 0);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.on_final_instantiate(dummy_model) == false);
    }

    SECTION("violated - x > y but b=1") {
        auto x = make_var("x", 7);
        auto y = make_var("y", 5);
        auto b = make_var("b", 1);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.on_final_instantiate(dummy_model) == false);
    }
}

// ============================================================================
// on_set_min / on_set_max unit tests
// ============================================================================

TEST_CASE("IntLtConstraint on_set_min/on_set_max", "[constraint][int_lt][bounds]") {
    SECTION("on_set_min(x) enqueues set_min(y, new_min+1)") {
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 1, 10);
        auto c_uptr = std::make_unique<IntLtConstraint>(x, y);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        // x.min が 3 に上がった → y.min >= 4
        REQUIRE(c->on_set_min(model, 0, x->id(), 0, 3, 1) == true);

        bool found = false;
        while (model.has_pending_updates()) {
            auto upd = model.pop_pending_update();
            if (upd.type == PendingUpdate::Type::SetMin &&
                upd.var_idx == y->id() && upd.value == 4) {
                found = true;
            }
        }
        REQUIRE(found);
    }

    SECTION("on_set_max(y) enqueues set_max(x, new_max-1)") {
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 1, 10);
        auto c_uptr = std::make_unique<IntLtConstraint>(x, y);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        // y.max が 7 に下がった → x.max <= 6
        REQUIRE(c->on_set_max(model, 0, y->id(), 0, 7, 10) == true);

        bool found = false;
        while (model.has_pending_updates()) {
            auto upd = model.pop_pending_update();
            if (upd.type == PendingUpdate::Type::SetMax &&
                upd.var_idx == x->id() && upd.value == 6) {
                found = true;
            }
        }
        REQUIRE(found);
    }

    SECTION("on_set_min(y) does nothing") {
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 1, 10);
        auto c_uptr = std::make_unique<IntLtConstraint>(x, y);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        REQUIRE(c->on_set_min(model, 0, y->id(), 0, 3, 1) == true);
        REQUIRE_FALSE(model.has_pending_updates());
    }

    SECTION("on_set_max(x) does nothing") {
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 1, 10);
        auto c_uptr = std::make_unique<IntLtConstraint>(x, y);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        REQUIRE(c->on_set_max(model, 0, x->id(), 0, 8, 10) == true);
        REQUIRE_FALSE(model.has_pending_updates());
    }
}

TEST_CASE("IntLeConstraint on_set_min/on_set_max", "[constraint][int_le][bounds]") {
    SECTION("on_set_min(x) enqueues set_min(y, new_min)") {
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 1, 10);
        auto c_uptr = std::make_unique<IntLeConstraint>(x, y);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        REQUIRE(c->on_set_min(model, 0, x->id(), 0, 5, 1) == true);

        bool found = false;
        while (model.has_pending_updates()) {
            auto upd = model.pop_pending_update();
            if (upd.type == PendingUpdate::Type::SetMin &&
                upd.var_idx == y->id() && upd.value == 5) {
                found = true;
            }
        }
        REQUIRE(found);
    }

    SECTION("on_set_max(y) enqueues set_max(x, new_max)") {
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 1, 10);
        auto c_uptr = std::make_unique<IntLeConstraint>(x, y);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        REQUIRE(c->on_set_max(model, 0, y->id(), 0, 7, 10) == true);

        bool found = false;
        while (model.has_pending_updates()) {
            auto upd = model.pop_pending_update();
            if (upd.type == PendingUpdate::Type::SetMax &&
                upd.var_idx == x->id() && upd.value == 7) {
                found = true;
            }
        }
        REQUIRE(found);
    }
}

TEST_CASE("IntEqConstraint on_set_min/on_set_max", "[constraint][int_eq][bounds]") {
    SECTION("on_set_min(x) enqueues set_min(y)") {
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 1, 10);
        auto c_uptr = std::make_unique<IntEqConstraint>(x, y);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        REQUIRE(c->on_set_min(model, 0, x->id(), 0, 4, 1) == true);

        bool found = false;
        while (model.has_pending_updates()) {
            auto upd = model.pop_pending_update();
            if (upd.type == PendingUpdate::Type::SetMin &&
                upd.var_idx == y->id() && upd.value == 4) {
                found = true;
            }
        }
        REQUIRE(found);
    }

    SECTION("on_set_max(y) enqueues set_max(x)") {
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 1, 10);
        auto c_uptr = std::make_unique<IntEqConstraint>(x, y);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        REQUIRE(c->on_set_max(model, 0, y->id(), 0, 6, 10) == true);

        bool found = false;
        while (model.has_pending_updates()) {
            auto upd = model.pop_pending_update();
            if (upd.type == PendingUpdate::Type::SetMax &&
                upd.var_idx == x->id() && upd.value == 6) {
                found = true;
            }
        }
        REQUIRE(found);
    }

    SECTION("on_set_min(y) enqueues set_min(x)") {
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 1, 10);
        auto c_uptr = std::make_unique<IntEqConstraint>(x, y);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        REQUIRE(c->on_set_min(model, 0, y->id(), 0, 3, 1) == true);

        bool found = false;
        while (model.has_pending_updates()) {
            auto upd = model.pop_pending_update();
            if (upd.type == PendingUpdate::Type::SetMin &&
                upd.var_idx == x->id() && upd.value == 3) {
                found = true;
            }
        }
        REQUIRE(found);
    }
}

TEST_CASE("IntMaxConstraint on_set_min/on_set_max", "[constraint][int_max][bounds]") {
    SECTION("on_set_min(x) enqueues set_min(m, max(x.min, y.min))") {
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 3, 10);
        auto m = model.create_variable("m", 1, 10);
        auto c_uptr = std::make_unique<IntMaxConstraint>(x, y, m);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        // x.min 5 に上がった → m.min >= max(5, 3) = 5
        // model.var_min(x) はまだ 1 だが、on_set_min に渡された new_min は 5
        // ただし実装では model.var_min を使っているので、実際に set_min を適用する必要がある
        model.set_min(0, x->id(), 5);
        model.clear_pending_updates();

        REQUIRE(c->on_set_min(model, 0, x->id(), 0, 5, 1) == true);

        bool found = false;
        while (model.has_pending_updates()) {
            auto upd = model.pop_pending_update();
            if (upd.type == PendingUpdate::Type::SetMin &&
                upd.var_idx == m->id() && upd.value == 5) {
                found = true;
            }
        }
        REQUIRE(found);
    }

    SECTION("on_set_max(m) enqueues set_max(x) and set_max(y)") {
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 1, 10);
        auto m = model.create_variable("m", 1, 10);
        auto c_uptr = std::make_unique<IntMaxConstraint>(x, y, m);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        REQUIRE(c->on_set_max(model, 0, m->id(), 0, 7, 10) == true);

        bool found_x = false, found_y = false;
        while (model.has_pending_updates()) {
            auto upd = model.pop_pending_update();
            if (upd.type == PendingUpdate::Type::SetMax && upd.var_idx == x->id() && upd.value == 7) {
                found_x = true;
            }
            if (upd.type == PendingUpdate::Type::SetMax && upd.var_idx == y->id() && upd.value == 7) {
                found_y = true;
            }
        }
        REQUIRE(found_x);
        REQUIRE(found_y);
    }

    SECTION("on_set_max(x) enqueues set_max(m, max(x.max, y.max))") {
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 1, 8);
        auto m = model.create_variable("m", 1, 10);
        auto c_uptr = std::make_unique<IntMaxConstraint>(x, y, m);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        // x.max が 6 に下がった → m.max <= max(6, 8) = 8
        model.set_max(0, x->id(), 6);
        model.clear_pending_updates();

        REQUIRE(c->on_set_max(model, 0, x->id(), 0, 6, 10) == true);

        bool found = false;
        while (model.has_pending_updates()) {
            auto upd = model.pop_pending_update();
            if (upd.type == PendingUpdate::Type::SetMax &&
                upd.var_idx == m->id() && upd.value == 8) {
                found = true;
            }
        }
        REQUIRE(found);
    }

    SECTION("on_set_min(m) does nothing") {
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 1, 10);
        auto m = model.create_variable("m", 1, 10);
        auto c_uptr = std::make_unique<IntMaxConstraint>(x, y, m);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        REQUIRE(c->on_set_min(model, 0, m->id(), 0, 3, 1) == true);
        REQUIRE_FALSE(model.has_pending_updates());
    }
}

TEST_CASE("IntMinConstraint on_set_min/on_set_max", "[constraint][int_min][bounds]") {
    SECTION("on_set_min(m) enqueues set_min(x) and set_min(y)") {
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 1, 10);
        auto m = model.create_variable("m", 1, 10);
        auto c_uptr = std::make_unique<IntMinConstraint>(x, y, m);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        REQUIRE(c->on_set_min(model, 0, m->id(), 0, 4, 1) == true);

        bool found_x = false, found_y = false;
        while (model.has_pending_updates()) {
            auto upd = model.pop_pending_update();
            if (upd.type == PendingUpdate::Type::SetMin && upd.var_idx == x->id() && upd.value == 4) {
                found_x = true;
            }
            if (upd.type == PendingUpdate::Type::SetMin && upd.var_idx == y->id() && upd.value == 4) {
                found_y = true;
            }
        }
        REQUIRE(found_x);
        REQUIRE(found_y);
    }

    SECTION("on_set_max(x) enqueues set_max(m, min(x.max, y.max))") {
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 1, 8);
        auto m = model.create_variable("m", 1, 10);
        auto c_uptr = std::make_unique<IntMinConstraint>(x, y, m);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        // x.max が 6 に下がった → m.max <= min(6, 8) = 6
        model.set_max(0, x->id(), 6);
        model.clear_pending_updates();

        REQUIRE(c->on_set_max(model, 0, x->id(), 0, 6, 10) == true);

        bool found = false;
        while (model.has_pending_updates()) {
            auto upd = model.pop_pending_update();
            if (upd.type == PendingUpdate::Type::SetMax &&
                upd.var_idx == m->id() && upd.value == 6) {
                found = true;
            }
        }
        REQUIRE(found);
    }

    SECTION("on_set_max(m) does nothing") {
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 1, 10);
        auto m = model.create_variable("m", 1, 10);
        auto c_uptr = std::make_unique<IntMinConstraint>(x, y, m);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        REQUIRE(c->on_set_max(model, 0, m->id(), 0, 7, 10) == true);
        REQUIRE_FALSE(model.has_pending_updates());
    }
}

TEST_CASE("IntEqReifConstraint on_set_min/on_set_max", "[constraint][int_eq_reif][bounds]") {
    SECTION("b undecided, disjoint bounds → enqueue b=0") {
        Model model;
        auto x = model.create_variable("x", 1, 3);
        auto y = model.create_variable("y", 5, 8);
        auto b = model.create_variable("b", 0, 1);
        auto c_uptr = std::make_unique<IntEqReifConstraint>(x, y, b);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        // x.max=3 < y.min=5 なので b=0
        REQUIRE(c->on_set_max(model, 0, x->id(), 0, 3, 5) == true);

        bool found = false;
        while (model.has_pending_updates()) {
            auto upd = model.pop_pending_update();
            if (upd.type == PendingUpdate::Type::Instantiate &&
                upd.var_idx == b->id() && upd.value == 0) {
                found = true;
            }
        }
        REQUIRE(found);
    }

    SECTION("b=1, on_set_min(x) propagates to y") {
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 1, 10);
        auto b = model.create_variable("b", 1);
        auto c_uptr = std::make_unique<IntEqReifConstraint>(x, y, b);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        model.set_min(0, x->id(), 4);
        model.clear_pending_updates();

        REQUIRE(c->on_set_min(model, 0, x->id(), 0, 4, 1) == true);

        bool found = false;
        while (model.has_pending_updates()) {
            auto upd = model.pop_pending_update();
            if (upd.type == PendingUpdate::Type::SetMin &&
                upd.var_idx == y->id() && upd.value == 4) {
                found = true;
            }
        }
        REQUIRE(found);
    }
}

TEST_CASE("IntNeReifConstraint on_set_min/on_set_max", "[constraint][int_ne_reif][bounds]") {
    SECTION("b undecided, disjoint bounds → enqueue b=1") {
        Model model;
        auto x = model.create_variable("x", 1, 3);
        auto y = model.create_variable("y", 5, 8);
        auto b = model.create_variable("b", 0, 1);
        auto c_uptr = std::make_unique<IntNeReifConstraint>(x, y, b);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        REQUIRE(c->on_set_max(model, 0, x->id(), 0, 3, 5) == true);

        bool found = false;
        while (model.has_pending_updates()) {
            auto upd = model.pop_pending_update();
            if (upd.type == PendingUpdate::Type::Instantiate &&
                upd.var_idx == b->id() && upd.value == 1) {
                found = true;
            }
        }
        REQUIRE(found);
    }

    SECTION("b=0, on_set_min(x) propagates to y") {
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 1, 10);
        auto b = model.create_variable("b", 0);
        auto c_uptr = std::make_unique<IntNeReifConstraint>(x, y, b);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        model.set_min(0, x->id(), 4);
        model.clear_pending_updates();

        REQUIRE(c->on_set_min(model, 0, x->id(), 0, 4, 1) == true);

        bool found = false;
        while (model.has_pending_updates()) {
            auto upd = model.pop_pending_update();
            if (upd.type == PendingUpdate::Type::SetMin &&
                upd.var_idx == y->id() && upd.value == 4) {
                found = true;
            }
        }
        REQUIRE(found);
    }
}

TEST_CASE("IntLeReifConstraint on_set_min/on_set_max", "[constraint][int_le_reif][bounds]") {
    SECTION("b undecided, x.max <= y.min → enqueue b=1") {
        Model model;
        auto x = model.create_variable("x", 1, 3);
        auto y = model.create_variable("y", 5, 8);
        auto b = model.create_variable("b", 0, 1);
        auto c_uptr = std::make_unique<IntLeReifConstraint>(x, y, b);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        REQUIRE(c->on_set_min(model, 0, y->id(), 0, 5, 3) == true);

        bool found = false;
        while (model.has_pending_updates()) {
            auto upd = model.pop_pending_update();
            if (upd.type == PendingUpdate::Type::Instantiate &&
                upd.var_idx == b->id() && upd.value == 1) {
                found = true;
            }
        }
        REQUIRE(found);
    }

    SECTION("b undecided, x.min > y.max → enqueue b=0") {
        Model model;
        auto x = model.create_variable("x", 6, 10);
        auto y = model.create_variable("y", 1, 5);
        auto b = model.create_variable("b", 0, 1);
        auto c_uptr = std::make_unique<IntLeReifConstraint>(x, y, b);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        REQUIRE(c->on_set_min(model, 0, x->id(), 0, 6, 4) == true);

        bool found = false;
        while (model.has_pending_updates()) {
            auto upd = model.pop_pending_update();
            if (upd.type == PendingUpdate::Type::Instantiate &&
                upd.var_idx == b->id() && upd.value == 0) {
                found = true;
            }
        }
        REQUIRE(found);
    }

    SECTION("b=1, on_set_min(x) propagates to y") {
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 1, 10);
        auto b = model.create_variable("b", 1);
        auto c_uptr = std::make_unique<IntLeReifConstraint>(x, y, b);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        model.set_min(0, x->id(), 5);
        model.clear_pending_updates();

        REQUIRE(c->on_set_min(model, 0, x->id(), 0, 5, 1) == true);

        bool found = false;
        while (model.has_pending_updates()) {
            auto upd = model.pop_pending_update();
            if (upd.type == PendingUpdate::Type::SetMin &&
                upd.var_idx == y->id() && upd.value == 5) {
                found = true;
            }
        }
        REQUIRE(found);
    }

    SECTION("b=1, on_set_max(y) propagates to x") {
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 1, 10);
        auto b = model.create_variable("b", 1);
        auto c_uptr = std::make_unique<IntLeReifConstraint>(x, y, b);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        model.set_max(0, y->id(), 7);
        model.clear_pending_updates();

        REQUIRE(c->on_set_max(model, 0, y->id(), 0, 7, 10) == true);

        bool found = false;
        while (model.has_pending_updates()) {
            auto upd = model.pop_pending_update();
            if (upd.type == PendingUpdate::Type::SetMax &&
                upd.var_idx == x->id() && upd.value == 7) {
                found = true;
            }
        }
        REQUIRE(found);
    }

    SECTION("b=0, on_set_min(y) propagates x >= y.min+1") {
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 1, 10);
        auto b = model.create_variable("b", 0);
        auto c_uptr = std::make_unique<IntLeReifConstraint>(x, y, b);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        model.set_min(0, y->id(), 4);
        model.clear_pending_updates();

        REQUIRE(c->on_set_min(model, 0, y->id(), 0, 4, 1) == true);

        bool found = false;
        while (model.has_pending_updates()) {
            auto upd = model.pop_pending_update();
            if (upd.type == PendingUpdate::Type::SetMin &&
                upd.var_idx == x->id() && upd.value == 5) {
                found = true;
            }
        }
        REQUIRE(found);
    }

    SECTION("b=0, on_set_max(x) propagates y <= x.max-1") {
        Model model;
        auto x = model.create_variable("x", 1, 10);
        auto y = model.create_variable("y", 1, 10);
        auto b = model.create_variable("b", 0);
        auto c_uptr = std::make_unique<IntLeReifConstraint>(x, y, b);        auto* c = c_uptr.get();
        model.add_constraint(std::move(c_uptr));
        model.clear_pending_updates();

        model.set_max(0, x->id(), 8);
        model.clear_pending_updates();

        REQUIRE(c->on_set_max(model, 0, x->id(), 0, 8, 10) == true);

        bool found = false;
        while (model.has_pending_updates()) {
            auto upd = model.pop_pending_update();
            if (upd.type == PendingUpdate::Type::SetMax &&
                upd.var_idx == y->id() && upd.value == 7) {
                found = true;
            }
        }
        REQUIRE(found);
    }
}

// ============================================================================
// Solver integration tests for bounds propagation
// ============================================================================

TEST_CASE("IntLtConstraint solver with bounds propagation", "[constraint][int_lt][solver]") {
    SECTION("x < y with x,y in [1,4] - all solutions correct") {
        Model model;
        auto x = model.create_variable("x", 1, 4);
        auto y = model.create_variable("y", 1, 4);
        model.add_constraint(std::make_unique<IntLtConstraint>(x, y));

        Solver solver;
        std::vector<Solution> solutions;
        solver.solve_all(model, [&](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        // (1,2),(1,3),(1,4),(2,3),(2,4),(3,4) = 6
        REQUIRE(solutions.size() == 6);
        for (const auto& sol : solutions) {
            REQUIRE(sol.at("x") < sol.at("y"));
        }
    }
}

TEST_CASE("IntLeConstraint solver with bounds propagation", "[constraint][int_le][solver]") {
    SECTION("x <= y with x,y in [1,4] - all solutions correct") {
        Model model;
        auto x = model.create_variable("x", 1, 4);
        auto y = model.create_variable("y", 1, 4);
        model.add_constraint(std::make_unique<IntLeConstraint>(x, y));

        Solver solver;
        std::vector<Solution> solutions;
        solver.solve_all(model, [&](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        // 6 (x<y) + 4 (x==y) = 10
        REQUIRE(solutions.size() == 10);
        for (const auto& sol : solutions) {
            REQUIRE(sol.at("x") <= sol.at("y"));
        }
    }
}

TEST_CASE("IntEqConstraint solver with bounds propagation", "[constraint][int_eq][solver]") {
    SECTION("x == y with x in [1,5], y in [3,7]") {
        Model model;
        auto x = model.create_variable("x", 1, 5);
        auto y = model.create_variable("y", 3, 7);
        model.add_constraint(std::make_unique<IntEqConstraint>(x, y));

        Solver solver;
        std::vector<Solution> solutions;
        solver.solve_all(model, [&](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        // x==y: {3,4,5} = 3 solutions
        REQUIRE(solutions.size() == 3);
        for (const auto& sol : solutions) {
            REQUIRE(sol.at("x") == sol.at("y"));
        }
    }
}

TEST_CASE("IntMaxConstraint solver with bounds propagation", "[constraint][int_max][solver]") {
    SECTION("m = max(x, y) with x,y in [1,3], m in [1,3]") {
        Model model;
        auto x = model.create_variable("x", 1, 3);
        auto y = model.create_variable("y", 1, 3);
        auto m = model.create_variable("m", 1, 3);
        model.add_constraint(std::make_unique<IntMaxConstraint>(x, y, m));

        Solver solver;
        std::vector<Solution> solutions;
        solver.solve_all(model, [&](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        for (const auto& sol : solutions) {
            REQUIRE(sol.at("m") == std::max(sol.at("x"), sol.at("y")));
        }
        // (1,1,1),(1,2,2),(1,3,3),(2,1,2),(2,2,2),(2,3,3),(3,1,3),(3,2,3),(3,3,3) = 9
        REQUIRE(solutions.size() == 9);
    }
}

TEST_CASE("IntMinConstraint solver with bounds propagation", "[constraint][int_min][solver]") {
    SECTION("m = min(x, y) with x,y in [1,3], m in [1,3]") {
        Model model;
        auto x = model.create_variable("x", 1, 3);
        auto y = model.create_variable("y", 1, 3);
        auto m = model.create_variable("m", 1, 3);
        model.add_constraint(std::make_unique<IntMinConstraint>(x, y, m));

        Solver solver;
        std::vector<Solution> solutions;
        solver.solve_all(model, [&](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        for (const auto& sol : solutions) {
            REQUIRE(sol.at("m") == std::min(sol.at("x"), sol.at("y")));
        }
        REQUIRE(solutions.size() == 9);
    }
}

TEST_CASE("IntLeReifConstraint solver with bounds propagation", "[constraint][int_le_reif][solver]") {
    SECTION("(x <= y) <-> b with x,y in [1,3]") {
        Model model;
        auto x = model.create_variable("x", 1, 3);
        auto y = model.create_variable("y", 1, 3);
        auto b = model.create_variable("b", 0, 1);
        model.add_constraint(std::make_unique<IntLeReifConstraint>(x, y, b));

        Solver solver;
        std::vector<Solution> solutions;
        solver.solve_all(model, [&](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        // 9 combinations * 1 b value each = 9
        REQUIRE(solutions.size() == 9);
        for (const auto& sol : solutions) {
            bool le = (sol.at("x") <= sol.at("y"));
            REQUIRE(le == (sol.at("b") == 1));
        }
    }
}

TEST_CASE("IntEqReifConstraint solver with bounds propagation", "[constraint][int_eq_reif][solver]") {
    SECTION("(x == y) <-> b with x,y in [1,3]") {
        Model model;
        auto x = model.create_variable("x", 1, 3);
        auto y = model.create_variable("y", 1, 3);
        auto b = model.create_variable("b", 0, 1);
        model.add_constraint(std::make_unique<IntEqReifConstraint>(x, y, b));

        Solver solver;
        std::vector<Solution> solutions;
        solver.solve_all(model, [&](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        REQUIRE(solutions.size() == 9);
        for (const auto& sol : solutions) {
            bool eq = (sol.at("x") == sol.at("y"));
            REQUIRE(eq == (sol.at("b") == 1));
        }
    }
}

TEST_CASE("IntNeReifConstraint solver with bounds propagation", "[constraint][int_ne_reif][solver]") {
    SECTION("(x != y) <-> b with x,y in [1,3]") {
        Model model;
        auto x = model.create_variable("x", 1, 3);
        auto y = model.create_variable("y", 1, 3);
        auto b = model.create_variable("b", 0, 1);
        model.add_constraint(std::make_unique<IntNeReifConstraint>(x, y, b));

        Solver solver;
        std::vector<Solution> solutions;
        solver.solve_all(model, [&](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        REQUIRE(solutions.size() == 9);
        for (const auto& sol : solutions) {
            bool ne = (sol.at("x") != sol.at("y"));
            REQUIRE(ne == (sol.at("b") == 1));
        }
    }
}

// ============================================================================
// IntModConstraint solver integration tests
// ============================================================================

TEST_CASE("IntModConstraint solve_all basic", "[constraint][int_mod][solver]") {
    SECTION("x mod 3 = z, x in [0,8]") {
        // x in [0,8], y = 3, z in [0,2]
        // Solutions: (0,0),(1,1),(2,2),(3,0),(4,1),(5,2),(6,0),(7,1),(8,2) = 9
        Model model;
        auto x = model.create_variable("x", 0, 8);
        auto y = model.create_variable("y", 3, 3);
        auto z = model.create_variable("z", 0, 2);
        model.add_constraint(std::make_unique<IntModConstraint>(x, y, z));

        Solver solver;
        std::vector<Solution> solutions;
        size_t count = solver.solve_all(model, [&](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        REQUIRE(count == 9);
        for (const auto& sol : solutions) {
            REQUIRE(sol.at("x") % sol.at("y") == sol.at("z"));
        }
    }

    SECTION("variable y: x mod y = z") {
        // x in [1,12], y in [3,4], z in [0,3]
        // y=3: z∈{0,1,2}, x∈{3,6,9,12,1,4,7,10,2,5,8,11} → 12
        // y=4: z∈{0,1,2,3}, x∈{4,8,12,1,5,9,2,6,10,3,7,11} → 12
        // Total = 24
        Model model;
        auto x = model.create_variable("x", 1, 12);
        auto y = model.create_variable("y", 3, 4);
        auto z = model.create_variable("z", 0, 3);
        model.add_constraint(std::make_unique<IntModConstraint>(x, y, z));

        Solver solver;
        std::vector<Solution> solutions;
        size_t count = solver.solve_all(model, [&](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        REQUIRE(count == 24);
        for (const auto& sol : solutions) {
            auto y_val = sol.at("y");
            REQUIRE(y_val != 0);
            REQUIRE(sol.at("x") % y_val == sol.at("z"));
        }
    }
}

TEST_CASE("IntModConstraint with int_lin_eq propagation chain", "[constraint][int_mod][solver]") {
    // x in [5,9], y in [2,5], z in [0,4]
    // Constraints: x % y = z AND x - z = 5 (i.e., x = z + 5)
    // This forces a propagation chain: assigning z → int_lin_eq propagates x → int_mod propagates
    // Solutions: (z+5) % y = z for z∈[0,4], y∈[2,5]:
    //   z=0,x=5: 5%y=0 → y=5 → (5,5,0)
    //   z=1,x=6: 6%y=1 → y=5 → (6,5,1)
    //   z=2,x=7: 7%y=2 → y=5 → (7,5,2)
    //   z=3,x=8: 8%y=3 → y=5 → (8,5,3)
    //   z=4,x=9: 9%y=4 → y=5 → (9,5,4)
    // Total: 5 solutions
    Model model;
    auto x = model.create_variable("x", 5, 9);
    auto y = model.create_variable("y", 2, 5);
    auto z = model.create_variable("z", 0, 4);
    model.add_constraint(std::make_unique<IntModConstraint>(x, y, z));
    // x - z = 5 → coeffs=[1,-1], vars=[x,z], rhs=5
    model.add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{1, -1}, std::vector<VariablePtr>{x, z}, 5));

    Solver solver;
    std::vector<Solution> solutions;
    size_t count = solver.solve_all(model, [&](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    REQUIRE(count == 5);
    for (const auto& sol : solutions) {
        REQUIRE(sol.at("x") % sol.at("y") == sol.at("z"));
        REQUIRE(sol.at("x") - sol.at("z") == 5);
    }
}

TEST_CASE("IntModConstraint multiple int_mod sharing y", "[constraint][int_mod][solver]") {
    // Two int_mod constraints sharing y:
    //   x1 % y = z1  AND  x2 % y = z2
    // x1=10 (fixed), x2=6 (fixed), y in [3,5], z1 in [0,4], z2 in [0,4]
    //   y=3: 10%3=1, 6%3=0 → (z1=1, z2=0)
    //   y=4: 10%4=2, 6%4=2 → (z1=2, z2=2)
    //   y=5: 10%5=0, 6%5=1 → (z1=0, z2=1)
    // Total: 3 solutions
    // This tests the case where one int_mod's propagation affects y's domain,
    // and the second int_mod must see the updated y state.
    Model model;
    auto x1 = model.create_variable("x1", 10, 10);
    auto x2 = model.create_variable("x2", 6, 6);
    auto y = model.create_variable("y", 3, 5);
    auto z1 = model.create_variable("z1", 0, 4);
    auto z2 = model.create_variable("z2", 0, 4);
    model.add_constraint(std::make_unique<IntModConstraint>(x1, y, z1));
    model.add_constraint(std::make_unique<IntModConstraint>(x2, y, z2));

    Solver solver;
    std::vector<Solution> solutions;
    size_t count = solver.solve_all(model, [&](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    REQUIRE(count == 3);
    for (const auto& sol : solutions) {
        auto y_val = sol.at("y");
        REQUIRE(sol.at("x1") % y_val == sol.at("z1"));
        REQUIRE(sol.at("x2") % y_val == sol.at("z2"));
    }
}

TEST_CASE("IntModConstraint with int_eq on y", "[constraint][int_mod][solver]") {
    // int_mod(x, y, z) AND int_eq(y, w)
    // x=7 (fixed), y in [2,6], z in [0,5], w in [2,6]
    // int_mod: 7%y=z → y=2:z=1, y=3:z=1, y=4:z=3, y=5:z=2, y=6:z=1
    // int_eq: y=w
    // 5 solutions, w must equal y
    // Tests that domain changes to y (via int_mod filtering) are properly
    // propagated to constraints on y (int_eq).
    Model model;
    auto x = model.create_variable("x", 7, 7);
    auto y = model.create_variable("y", 2, 6);
    auto z = model.create_variable("z", 0, 5);
    auto w = model.create_variable("w", 2, 6);
    model.add_constraint(std::make_unique<IntModConstraint>(x, y, z));
    model.add_constraint(std::make_unique<IntEqConstraint>(y, w));

    Solver solver;
    std::vector<Solution> solutions;
    size_t count = solver.solve_all(model, [&](const Solution& sol) {
        solutions.push_back(sol);
        return true;
    });

    REQUIRE(count == 5);
    for (const auto& sol : solutions) {
        REQUIRE(sol.at("x") % sol.at("y") == sol.at("z"));
        REQUIRE(sol.at("y") == sol.at("w"));
    }
}
