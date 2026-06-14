// Brute-force solution-set equivalence tests for the scalar comparison
// constraints (int_eq/ne/lt/le and their reif/imp variants).
//
// Safety net for the Phase 4 comparison.cpp helper extraction
// (refactoring-plan-2026-06.md): the intersect_eq / enforce_le presolve
// helpers are shared across plain, reified, and half-reified forms, so a
// false UNSAT introduced while refactoring would surface as a solution-set
// mismatch against exhaustive enumeration.
#include <catch2/catch_test_macros.hpp>
#include "sabori_csp/constraints/comparison.hpp"
#include "sabori_csp/variable.hpp"
#include "sabori_csp/model.hpp"
#include "sabori_csp/solver.hpp"
#include <set>
#include <vector>

using namespace sabori_csp;

namespace {

enum class Cmp { Eq, EqReif, EqImp, Ne, NeReif, Lt, Le, LeReif };

bool is_reified(Cmp c) {
    return c == Cmp::EqReif || c == Cmp::EqImp || c == Cmp::NeReif || c == Cmp::LeReif;
}

bool sat(Cmp c, int64_t x, int64_t y, int bv) {
    switch (c) {
        case Cmp::Eq:     return x == y;
        case Cmp::Ne:     return x != y;
        case Cmp::Lt:     return x < y;
        case Cmp::Le:     return x <= y;
        case Cmp::EqReif: return (x == y) == (bv == 1);
        case Cmp::NeReif: return (x != y) == (bv == 1);
        case Cmp::LeReif: return (x <= y) == (bv == 1);
        case Cmp::EqImp:  return (bv == 0) || (x == y);  // b=1 implies x==y
    }
    return false;
}

void add_constraint(Model& model, Cmp c, Variable* x, Variable* y, Variable* b) {
    switch (c) {
        case Cmp::Eq:     model.add_constraint(std::make_unique<IntEqConstraint>(x, y)); break;
        case Cmp::Ne:     model.add_constraint(std::make_unique<IntNeConstraint>(x, y)); break;
        case Cmp::Lt:     model.add_constraint(std::make_unique<IntLtConstraint>(x, y)); break;
        case Cmp::Le:     model.add_constraint(std::make_unique<IntLeConstraint>(x, y)); break;
        case Cmp::EqReif: model.add_constraint(std::make_unique<IntEqReifConstraint>(x, y, b)); break;
        case Cmp::NeReif: model.add_constraint(std::make_unique<IntNeReifConstraint>(x, y, b)); break;
        case Cmp::LeReif: model.add_constraint(std::make_unique<IntLeReifConstraint>(x, y, b)); break;
        case Cmp::EqImp:  model.add_constraint(std::make_unique<IntEqImpConstraint>(x, y, b)); break;
    }
}

// Compare the solver's full solution set against exhaustive enumeration for a
// single (x-range, y-range, b-range) instance.
void check(Cmp c, int xlo, int xhi, int ylo, int yhi, int blo, int bhi) {
    Model model;
    auto* x = model.create_variable("x", xlo, xhi);
    auto* y = model.create_variable("y", ylo, yhi);
    Variable* b = is_reified(c) ? model.create_variable("b", blo, bhi) : nullptr;
    add_constraint(model, c, x, y, b);

    Solver solver;
    std::set<std::vector<int64_t>> got;
    solver.solve_all(model, [&](const Solution& sol) {
        std::vector<int64_t> row{sol.at("x"), sol.at("y")};
        if (b) row.push_back(sol.at("b"));
        got.insert(std::move(row));
        return true;
    });

    std::set<std::vector<int64_t>> want;
    int blo_eff = b ? blo : 0;
    int bhi_eff = b ? bhi : 0;  // dummy single b for non-reified
    for (int xi = xlo; xi <= xhi; ++xi) {
        for (int yi = ylo; yi <= yhi; ++yi) {
            for (int bv = blo_eff; bv <= bhi_eff; ++bv) {
                bool ok = b ? sat(c, xi, yi, bv) : sat(c, xi, yi, /*unused*/0);
                if (!ok) continue;
                std::vector<int64_t> row{xi, yi};
                if (b) row.push_back(bv);
                want.insert(std::move(row));
            }
        }
    }
    REQUIRE(got == want);
}

void exhaustive(Cmp c) {
    // Sweep overlapping / disjoint / nested ranges and every b polarity.
    const int lo_choices[] = {-2, 0, 2};
    const int span_choices[] = {0, 1, 3};
    for (int xlo : lo_choices)
        for (int xs : span_choices)
            for (int ylo : lo_choices)
                for (int ys : span_choices) {
                    if (is_reified(c)) {
                        check(c, xlo, xlo + xs, ylo, ylo + ys, 0, 1);  // b free
                        check(c, xlo, xlo + xs, ylo, ylo + ys, 0, 0);  // b fixed 0
                        check(c, xlo, xlo + xs, ylo, ylo + ys, 1, 1);  // b fixed 1
                    } else {
                        check(c, xlo, xlo + xs, ylo, ylo + ys, 0, 0);
                    }
                }
}

}  // namespace

TEST_CASE("int_eq: solution-set equivalence", "[constraint][int_eq][brute]")          { exhaustive(Cmp::Eq); }
TEST_CASE("int_ne: solution-set equivalence", "[constraint][int_ne][brute]")          { exhaustive(Cmp::Ne); }
TEST_CASE("int_lt: solution-set equivalence", "[constraint][int_lt][brute]")          { exhaustive(Cmp::Lt); }
TEST_CASE("int_le: solution-set equivalence", "[constraint][int_le][brute]")          { exhaustive(Cmp::Le); }
TEST_CASE("int_eq_reif: solution-set equivalence", "[constraint][int_eq_reif][brute]") { exhaustive(Cmp::EqReif); }
TEST_CASE("int_eq_imp: solution-set equivalence", "[constraint][int_eq_imp][brute]")   { exhaustive(Cmp::EqImp); }
TEST_CASE("int_ne_reif: solution-set equivalence", "[constraint][int_ne_reif][brute]") { exhaustive(Cmp::NeReif); }
TEST_CASE("int_le_reif: solution-set equivalence", "[constraint][int_le_reif][brute]") { exhaustive(Cmp::LeReif); }
