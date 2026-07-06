// Brute-force solution-set equivalence tests for lex_less / lex_lesseq.
//
// lex_lesseq(x, y): x is lexicographically <= y (strict variant: <).
// Arrays may differ in length (an equal prefix makes the shorter array
// lexicographically smaller). We verify the propagator is both sound and
// complete by comparing its full solution set against exhaustive enumeration.
#include <catch2/catch_test_macros.hpp>
#include "sabori_csp/constraints/global/misc.hpp"
#include "sabori_csp/variable.hpp"
#include "sabori_csp/model.hpp"
#include "sabori_csp/solver.hpp"
#include <set>
#include <vector>
#include <functional>
#include <string>

using namespace sabori_csp;

namespace {

bool lex_ok(const std::vector<int>& xs, const std::vector<int>& ys, bool strict) {
    size_t nmin = std::min(xs.size(), ys.size());
    for (size_t i = 0; i < nmin; ++i) {
        if (xs[i] < ys[i]) return true;
        if (xs[i] > ys[i]) return false;
    }
    if (xs.size() < ys.size()) return true;
    if (xs.size() > ys.size()) return false;
    return !strict;
}

// x_dom / y_dom: per-variable (lo, hi) ranges.
void check(const std::vector<std::pair<int, int>>& x_dom,
           const std::vector<std::pair<int, int>>& y_dom,
           bool strict) {
    const size_t nx = x_dom.size(), ny = y_dom.size();

    Model model;
    std::vector<Variable*> xs, ys;
    for (size_t i = 0; i < nx; ++i)
        xs.push_back(model.create_variable("x" + std::to_string(i),
                                           x_dom[i].first, x_dom[i].second));
    for (size_t i = 0; i < ny; ++i)
        ys.push_back(model.create_variable("y" + std::to_string(i),
                                           y_dom[i].first, y_dom[i].second));
    model.add_constraint(std::make_unique<LexLessEqConstraint>(xs, ys, strict));

    Solver solver;
    std::set<std::vector<int64_t>> got;
    solver.solve_all(model, [&](const Solution& sol) {
        std::vector<int64_t> row;
        for (size_t i = 0; i < nx; ++i) row.push_back(sol.at("x" + std::to_string(i)));
        for (size_t i = 0; i < ny; ++i) row.push_back(sol.at("y" + std::to_string(i)));
        got.insert(std::move(row));
        return true;
    });

    std::set<std::vector<int64_t>> want;
    std::vector<int> xv(nx), yv(ny);
    std::function<void(size_t)> rec_y = [&](size_t k) {
        if (k == ny) {
            if (lex_ok(xv, yv, strict)) {
                std::vector<int64_t> row(xv.begin(), xv.end());
                row.insert(row.end(), yv.begin(), yv.end());
                want.insert(std::move(row));
            }
            return;
        }
        for (int v = y_dom[k].first; v <= y_dom[k].second; ++v) { yv[k] = v; rec_y(k + 1); }
    };
    std::function<void(size_t)> rec_x = [&](size_t k) {
        if (k == nx) { rec_y(0); return; }
        for (int v = x_dom[k].first; v <= x_dom[k].second; ++v) { xv[k] = v; rec_x(k + 1); }
    };
    rec_x(0);

    REQUIRE(got == want);
}

}  // namespace

TEST_CASE("lex: name", "[constraint][lex]") {
    Model model;
    std::vector<Variable*> xs{model.create_variable("x0", 1, 2)};
    std::vector<Variable*> ys{model.create_variable("y0", 1, 2)};
    REQUIRE(LexLessEqConstraint(xs, ys, false).name() == "sabori_lex_lesseq");
    REQUIRE(LexLessEqConstraint(xs, ys, true).name() == "sabori_lex_less");
}

TEST_CASE("lex: basic equivalence (brute-force)", "[constraint][lex][brute]") {
    for (bool strict : {false, true}) {
        check({{1, 2}, {1, 2}, {1, 2}}, {{1, 2}, {1, 2}, {1, 2}}, strict);
        check({{1, 3}, {1, 3}}, {{1, 3}, {1, 3}}, strict);
    }
}

TEST_CASE("lex: forced prefixes (brute-force)", "[constraint][lex][brute]") {
    for (bool strict : {false, true}) {
        // Equal forced prefix, decision at the tail.
        check({{2, 2}, {1, 3}}, {{2, 2}, {1, 3}}, strict);
        // First position already strictly ordered => suffix free.
        check({{1, 1}, {1, 3}}, {{2, 2}, {1, 3}}, strict);
        // First position violated.
        check({{3, 3}, {1, 2}}, {{1, 1}, {1, 2}}, strict);
    }
}

TEST_CASE("lex: different lengths (brute-force)", "[constraint][lex][brute]") {
    for (bool strict : {false, true}) {
        // x shorter: equal prefix makes x smaller.
        check({{1, 2}}, {{1, 2}, {1, 2}}, strict);
        // x longer: equal prefix makes x greater.
        check({{1, 2}, {1, 2}}, {{1, 2}}, strict);
        // Empty x.
        check({}, {{1, 2}}, strict);
    }
}

TEST_CASE("lex: single position boundary (brute-force)", "[constraint][lex][brute]") {
    // n=1: reduces to x <= y (or x < y).
    check({{1, 3}}, {{1, 3}}, false);
    check({{1, 3}}, {{1, 3}}, true);
    // Empty both: lesseq SAT, less UNSAT.
    {
        Model model;
        std::vector<Variable*> none;
        auto* z = model.create_variable("z", 0, 0);  // dummy so the model has a var
        (void)z;
        model.add_constraint(std::make_unique<LexLessEqConstraint>(none, none, false));
        Solver solver;
        REQUIRE(solver.solve(model).has_value());
    }
    {
        Model model;
        std::vector<Variable*> none;
        auto* z = model.create_variable("z", 0, 0);
        (void)z;
        model.add_constraint(std::make_unique<LexLessEqConstraint>(none, none, true));
        Solver solver;
        REQUIRE(!solver.solve(model).has_value());
    }
}

TEST_CASE("lex: negative values (brute-force)", "[constraint][lex][brute]") {
    for (bool strict : {false, true}) {
        check({{-2, 0}, {-1, 1}}, {{-2, 0}, {-1, 1}}, strict);
    }
}
