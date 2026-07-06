// Brute-force solution-set equivalence tests for subcircuit.
//
// subcircuit(x): x[i]==i means node i is out; the non-self nodes must form
// exactly one cycle (possibly empty). The model pairs SubcircuitConstraint
// with AllDifferentConstraint exactly like the fzn registry does, and the
// oracle checks permutation + single-cycle semantics.
#include <catch2/catch_test_macros.hpp>
#include "sabori_csp/constraints/global/graph.hpp"
#include "sabori_csp/constraints/global/alldifferent.hpp"
#include "sabori_csp/variable.hpp"
#include "sabori_csp/model.hpp"
#include "sabori_csp/solver.hpp"
#include <set>
#include <vector>
#include <functional>
#include <string>

using namespace sabori_csp;

namespace {

// 0-based oracle: permutation + non-self nodes form exactly one cycle.
bool subcircuit_ok(const std::vector<int>& xs) {
    const size_t n = xs.size();
    std::vector<int> seen(n, 0);
    for (int v : xs) {
        if (v < 0 || static_cast<size_t>(v) >= n || seen[v]++) return false;
    }
    size_t start = n, in_count = 0;
    for (size_t i = 0; i < n; ++i)
        if (xs[i] != static_cast<int>(i)) { start = i; ++in_count; }
    if (in_count == 0) return true;
    size_t cur = start, steps = 0;
    while (steps <= in_count) {
        int nxt = xs[cur];
        if (nxt == static_cast<int>(cur)) return false;  // fell onto a self-loop
        ++steps;
        cur = static_cast<size_t>(nxt);
        if (cur == start) break;
    }
    return cur == start && steps == in_count;
}

// dom[i] = explicit value list for x[i] (0-based node values).
void check(const std::vector<std::vector<int>>& dom) {
    const size_t n = dom.size();

    Model model;
    std::vector<Variable*> xs;
    for (size_t i = 0; i < n; ++i) {
        std::vector<Domain::value_type> vals(dom[i].begin(), dom[i].end());
        xs.push_back(model.create_variable("x" + std::to_string(i), vals));
    }
    if (n >= 2) {
        model.add_constraint(std::make_unique<AllDifferentConstraint>(xs));
    }
    model.add_constraint(std::make_unique<SubcircuitConstraint>(xs, /*index_offset=*/0));

    Solver solver;
    std::set<std::vector<int64_t>> got;
    solver.solve_all(model, [&](const Solution& sol) {
        std::vector<int64_t> row;
        for (size_t i = 0; i < n; ++i) row.push_back(sol.at("x" + std::to_string(i)));
        got.insert(std::move(row));
        return true;
    });

    std::set<std::vector<int64_t>> want;
    std::vector<int> cur(n);
    std::function<void(size_t)> rec = [&](size_t k) {
        if (k == n) {
            if (subcircuit_ok(cur))
                want.insert(std::vector<int64_t>(cur.begin(), cur.end()));
            return;
        }
        for (int v : dom[k]) { cur[k] = v; rec(k + 1); }
    };
    rec(0);

    REQUIRE(got == want);
}

std::vector<std::vector<int>> full_doms(size_t n) {
    std::vector<int> all;
    for (size_t v = 0; v < n; ++v) all.push_back(static_cast<int>(v));
    return std::vector<std::vector<int>>(n, all);
}

}  // namespace

TEST_CASE("subcircuit: name", "[constraint][subcircuit]") {
    Model model;
    std::vector<Variable*> xs{model.create_variable("x0", 0, 0)};
    SubcircuitConstraint c(xs, 0);
    REQUIRE(c.name() == "sabori_subcircuit");
}

TEST_CASE("subcircuit: full domains equivalence (brute-force)",
          "[constraint][subcircuit][brute]") {
    // n=3: all-self, three 2-cycles, two 3-cycles => 6 solutions.
    check(full_doms(3));
    // n=4: 1 + C(4,2) + C(4,3)*2 + 3!*... enumerate via oracle.
    check(full_doms(4));
    check(full_doms(5));
}

TEST_CASE("subcircuit: forced participation (brute-force)",
          "[constraint][subcircuit][brute]") {
    // x0 cannot self-loop => node 0 must be on the cycle.
    check({{1, 2}, {0, 1, 2}, {0, 1, 2}});
    // Two nodes forced in, one free.
    check({{1, 2}, {0, 2}, {0, 1, 2}});
}

TEST_CASE("subcircuit: forced edges and premature closure (brute-force)",
          "[constraint][subcircuit][brute]") {
    // Edge 0->1 fixed; node 2 must-in => closing 1->0 must be pruned.
    check({{1}, {0, 1, 2}, {0, 1}});
    // Fixed full cycle 0->1->2->0 with an extra free node 3.
    check({{1}, {2}, {0}, {0, 1, 2, 3}});
}

TEST_CASE("subcircuit: all-out and UNSAT cases (brute-force)",
          "[constraint][subcircuit][brute]") {
    // All fixed to self: valid empty subcircuit.
    check({{0}, {1}, {2}});
    // Single node that cannot self-loop: no valid cycle of length 1 => UNSAT.
    {
        Model model;
        std::vector<Variable*> xs{model.create_variable("x0", 0, 0)};
        // dom {0} but must_in via removing self is impossible here; use n=2:
        (void)xs;
    }
    // n=2 where node 0 must-in but node 1 fixed to self => UNSAT.
    check({{1}, {1}});
}

TEST_CASE("subcircuit: single node (brute-force)", "[constraint][subcircuit][brute]") {
    check({{0}});  // self only => SAT
}
