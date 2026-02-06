#include <catch2/catch_test_macros.hpp>
#include "sabori_csp/constraint.hpp"
#include "sabori_csp/variable.hpp"
#include "sabori_csp/domain.hpp"
#include "sabori_csp/model.hpp"

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

    auto vars = c.variables();
    REQUIRE(vars.size() == 2);
    REQUIRE(vars[0] == x);
    REQUIRE(vars[1] == y);
}

TEST_CASE("IntEqConstraint is_satisfied", "[constraint][int_eq]") {
    SECTION("both assigned and equal") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        IntEqConstraint c(x, y);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("both assigned and not equal") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        IntEqConstraint c(x, y);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("not fully assigned") {
        auto x = make_var("x", 1, 3);
        auto y = make_var("y", 5);
        IntEqConstraint c(x, y);

        REQUIRE_FALSE(c.is_satisfied().has_value());
    }
}

TEST_CASE("IntEqConstraint propagate", "[constraint][int_eq]") {
    SECTION("intersect domains") {
        auto x = make_var("x", 1, 5);
        auto y = make_var("y", 3, 7);
        IntEqConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) == true);
        REQUIRE(x->min() == 3);
        REQUIRE(x->max() == 5);
        REQUIRE(y->min() == 3);
        REQUIRE(y->max() == 5);
    }

    SECTION("no intersection - failure") {
        auto x = make_var("x", 1, 2);
        auto y = make_var("y", 5, 6);
        IntEqConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) == false);
    }

    SECTION("one value in common") {
        auto x = make_var("x", 1, 3);
        auto y = make_var("y", 3, 5);
        IntEqConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) == true);
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

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("both assigned and equal") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        IntNeConstraint c(x, y);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("not fully assigned") {
        auto x = make_var("x", 1, 3);
        auto y = make_var("y", 5);
        IntNeConstraint c(x, y);

        REQUIRE_FALSE(c.is_satisfied().has_value());
    }
}

TEST_CASE("IntNeConstraint propagate", "[constraint][int_ne]") {
    SECTION("x assigned - remove from y") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 1, 5);
        IntNeConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) == true);
        REQUIRE_FALSE(y->domain().contains(3));
        REQUIRE(y->domain().size() == 4);
    }

    SECTION("y assigned - remove from x") {
        auto x = make_var("x", 1, 5);
        auto y = make_var("y", 3);
        IntNeConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) == true);
        REQUIRE_FALSE(x->domain().contains(3));
        REQUIRE(x->domain().size() == 4);
    }

    SECTION("both singletons with same value - failure") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 3);
        IntNeConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) == false);
    }

    SECTION("both singletons with different values - success") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        IntNeConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) == true);
    }

    SECTION("neither assigned - no change") {
        auto x = make_var("x", 1, 3);
        auto y = make_var("y", 1, 3);
        IntNeConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) == true);
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

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("x == y") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        IntLtConstraint c(x, y);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("x > y") {
        auto x = make_var("x", 7);
        auto y = make_var("y", 5);
        IntLtConstraint c(x, y);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("not fully assigned") {
        auto x = make_var("x", 1, 3);
        auto y = make_var("y", 5);
        IntLtConstraint c(x, y);

        REQUIRE_FALSE(c.is_satisfied().has_value());
    }
}

TEST_CASE("IntLtConstraint propagate", "[constraint][int_lt]") {
    SECTION("basic bound propagation") {
        auto x = make_var("x", 1, 5);
        auto y = make_var("y", 1, 5);
        IntLtConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) == true);
        // x < y means x.max < y.max, y.min > x.min
        REQUIRE(x->max() == 4);  // x < 5
        REQUIRE(y->min() == 2);  // y > 1
    }

    SECTION("infeasible - x.min >= y.max") {
        auto x = make_var("x", 5, 7);
        auto y = make_var("y", 1, 3);
        IntLtConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) == false);
    }

    SECTION("tight domains") {
        auto x = make_var("x", 1, 2);
        auto y = make_var("y", 2, 3);
        IntLtConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) == true);
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

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("x == y") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        IntLeConstraint c(x, y);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("x > y") {
        auto x = make_var("x", 7);
        auto y = make_var("y", 5);
        IntLeConstraint c(x, y);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("not fully assigned") {
        auto x = make_var("x", 1, 3);
        auto y = make_var("y", 5);
        IntLeConstraint c(x, y);

        REQUIRE_FALSE(c.is_satisfied().has_value());
    }
}

TEST_CASE("IntLeConstraint propagate", "[constraint][int_le]") {
    SECTION("basic bound propagation") {
        auto x = make_var("x", 1, 5);
        auto y = make_var("y", 1, 5);
        IntLeConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) == true);
        // x <= y means x.max <= y.max, y.min >= x.min
        REQUIRE(x->max() == 5);
        REQUIRE(y->min() == 1);
    }

    SECTION("propagate upper bound of x") {
        auto x = make_var("x", 1, 10);
        auto y = make_var("y", 1, 5);
        IntLeConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) == true);
        REQUIRE(x->max() == 5);  // x <= y.max
    }

    SECTION("propagate lower bound of y") {
        auto x = make_var("x", 3, 5);
        auto y = make_var("y", 1, 10);
        IntLeConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) == true);
        REQUIRE(y->min() == 3);  // y >= x.min
    }

    SECTION("infeasible - x.min > y.max") {
        auto x = make_var("x", 6, 8);
        auto y = make_var("y", 1, 3);
        IntLeConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) == false);
    }

    SECTION("equal singleton values - success") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        IntLeConstraint c(x, y);

        REQUIRE(c.presolve(dummy_model) == true);
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

    auto vars = c.variables();
    REQUIRE(vars.size() == 3);
    REQUIRE(vars[0] == x);
    REQUIRE(vars[1] == y);
    REQUIRE(vars[2] == b);
}

TEST_CASE("IntEqReifConstraint is_satisfied", "[constraint][int_eq_reif]") {
    SECTION("x == y and b == 1") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        auto b = make_var("b", 1);
        IntEqReifConstraint c(x, y, b);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("x != y and b == 0") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        auto b = make_var("b", 0);
        IntEqReifConstraint c(x, y, b);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("x == y and b == 0 - violated") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        auto b = make_var("b", 0);
        IntEqReifConstraint c(x, y, b);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("x != y and b == 1 - violated") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        auto b = make_var("b", 1);
        IntEqReifConstraint c(x, y, b);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("not fully assigned") {
        auto x = make_var("x", 1, 3);
        auto y = make_var("y", 5);
        auto b = make_var("b", 0, 1);
        IntEqReifConstraint c(x, y, b);

        REQUIRE_FALSE(c.is_satisfied().has_value());
    }
}

TEST_CASE("IntEqReifConstraint propagate", "[constraint][int_eq_reif]") {
    SECTION("b=1 enforces x==y intersection") {
        auto x = make_var("x", 1, 5);
        auto y = make_var("y", 3, 7);
        auto b = make_var("b", 1);
        IntEqReifConstraint c(x, y, b);

        REQUIRE(c.presolve(dummy_model) == true);
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

        REQUIRE(c.presolve(dummy_model) == true);
        REQUIRE_FALSE(y->domain().contains(3));
    }

    SECTION("x==y singletons set b=1") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        auto b = make_var("b", 0, 1);
        IntEqReifConstraint c(x, y, b);

        REQUIRE(c.presolve(dummy_model) == true);
        REQUIRE(b->is_assigned());
        REQUIRE(b->assigned_value() == 1);
    }

    SECTION("x!=y singletons set b=0") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        auto b = make_var("b", 0, 1);
        IntEqReifConstraint c(x, y, b);

        REQUIRE(c.presolve(dummy_model) == true);
        REQUIRE(b->is_assigned());
        REQUIRE(b->assigned_value() == 0);
    }

    SECTION("b=1 with no intersection - failure") {
        auto x = make_var("x", 1, 2);
        auto y = make_var("y", 5, 6);
        auto b = make_var("b", 1);
        IntEqReifConstraint c(x, y, b);

        REQUIRE(c.presolve(dummy_model) == false);
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
        REQUIRE(!c.can_be_finalized());
    }

    SECTION("one assigned variable") {
        auto x = make_var("x", 5);  // assigned
        auto y = make_var("y", 1, 3);
        IntEqConstraint c(x, y);

        // x は確定済みなので y を監視
        REQUIRE(c.watch1() == 1);
        REQUIRE(!c.can_be_finalized());
    }

    SECTION("both assigned variables") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        IntEqConstraint c(x, y);

        // 両方確定済み
        REQUIRE(c.can_be_finalized());
    }
}

TEST_CASE("Constraint on_final_instantiate", "[constraint][2wl]") {
    SECTION("IntEqConstraint - satisfied") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        IntEqConstraint c(x, y);

        REQUIRE(c.on_final_instantiate() == true);
    }

    SECTION("IntEqConstraint - violated") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        IntEqConstraint c(x, y);

        REQUIRE(c.on_final_instantiate() == false);
    }

    SECTION("IntNeConstraint - satisfied") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        IntNeConstraint c(x, y);

        REQUIRE(c.on_final_instantiate() == true);
    }

    SECTION("IntNeConstraint - violated") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        IntNeConstraint c(x, y);

        REQUIRE(c.on_final_instantiate() == false);
    }

    SECTION("IntLtConstraint - satisfied") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        IntLtConstraint c(x, y);

        REQUIRE(c.on_final_instantiate() == true);
    }

    SECTION("IntLtConstraint - violated (equal)") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        IntLtConstraint c(x, y);

        REQUIRE(c.on_final_instantiate() == false);
    }

    SECTION("IntLeConstraint - satisfied (equal)") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        IntLeConstraint c(x, y);

        REQUIRE(c.on_final_instantiate() == true);
    }

    SECTION("IntLeConstraint - violated") {
        auto x = make_var("x", 7);
        auto y = make_var("y", 5);
        IntLeConstraint c(x, y);

        REQUIRE(c.on_final_instantiate() == false);
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

    auto vars = c.variables();
    REQUIRE(vars.size() == 3);
    REQUIRE(vars[0] == x);
    REQUIRE(vars[1] == y);
    REQUIRE(vars[2] == b);
}

TEST_CASE("IntLeReifConstraint is_satisfied", "[constraint][int_le_reif]") {
    SECTION("x <= y and b == 1") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        auto b = make_var("b", 1);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("x == y and b == 1") {
        auto x = make_var("x", 5);
        auto y = make_var("y", 5);
        auto b = make_var("b", 1);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("x > y and b == 0") {
        auto x = make_var("x", 7);
        auto y = make_var("y", 5);
        auto b = make_var("b", 0);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == true);
    }

    SECTION("x <= y but b == 0") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        auto b = make_var("b", 0);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("x > y but b == 1") {
        auto x = make_var("x", 7);
        auto y = make_var("y", 5);
        auto b = make_var("b", 1);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.is_satisfied().has_value());
        REQUIRE(c.is_satisfied().value() == false);
    }

    SECTION("not fully assigned") {
        auto x = make_var("x", 1, 3);
        auto y = make_var("y", 5);
        auto b = make_var("b", 0, 1);
        IntLeReifConstraint c(x, y, b);

        REQUIRE_FALSE(c.is_satisfied().has_value());
    }
}

TEST_CASE("IntLeReifConstraint propagate", "[constraint][int_le_reif]") {
    SECTION("b=1 enforces x <= y") {
        auto x = make_var("x", 1, 10);
        auto y = make_var("y", 1, 5);
        auto b = make_var("b", 1);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.presolve(dummy_model) == true);
        REQUIRE(x->max() == 5);  // x <= y.max
    }

    SECTION("b=0 enforces x > y") {
        auto x = make_var("x", 1, 10);
        auto y = make_var("y", 3, 8);
        auto b = make_var("b", 0);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.presolve(dummy_model) == true);
        REQUIRE(x->min() == 4);  // x > y.min (3)
        REQUIRE(y->max() == 8);  // y < x.max would be 9, but y.max is 8
    }

    SECTION("x.max <= y.min implies b=1") {
        auto x = make_var("x", 1, 3);
        auto y = make_var("y", 5, 10);
        auto b = make_var("b", 0, 1);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.presolve(dummy_model) == true);
        REQUIRE(b->is_assigned());
        REQUIRE(b->assigned_value().value() == 1);
    }

    SECTION("x.min > y.max implies b=0") {
        auto x = make_var("x", 8, 10);
        auto y = make_var("y", 1, 5);
        auto b = make_var("b", 0, 1);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.presolve(dummy_model) == true);
        REQUIRE(b->is_assigned());
        REQUIRE(b->assigned_value().value() == 0);
    }

    SECTION("b=1 with x.min > y.max is infeasible") {
        auto x = make_var("x", 8, 10);
        auto y = make_var("y", 1, 5);
        auto b = make_var("b", 1);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.is_initially_inconsistent() == true);
    }

    SECTION("b=0 with x.max <= y.min is infeasible") {
        auto x = make_var("x", 1, 3);
        auto y = make_var("y", 5, 10);
        auto b = make_var("b", 0);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.is_initially_inconsistent() == true);
    }
}

TEST_CASE("IntLeReifConstraint on_final_instantiate", "[constraint][int_le_reif]") {
    SECTION("satisfied - x <= y and b=1") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        auto b = make_var("b", 1);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.on_final_instantiate() == true);
    }

    SECTION("satisfied - x > y and b=0") {
        auto x = make_var("x", 7);
        auto y = make_var("y", 5);
        auto b = make_var("b", 0);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.on_final_instantiate() == true);
    }

    SECTION("violated - x <= y but b=0") {
        auto x = make_var("x", 3);
        auto y = make_var("y", 5);
        auto b = make_var("b", 0);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.on_final_instantiate() == false);
    }

    SECTION("violated - x > y but b=1") {
        auto x = make_var("x", 7);
        auto y = make_var("y", 5);
        auto b = make_var("b", 1);
        IntLeReifConstraint c(x, y, b);

        REQUIRE(c.on_final_instantiate() == false);
    }
}
