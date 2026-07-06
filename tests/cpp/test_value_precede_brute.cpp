// Brute-force solution-set equivalence tests for value_precede.
//
// value_precede(s, t, x): x[j]==t requires some i<j with x[i]==s.
// We verify the propagator is both sound and complete by comparing its full
// solution set against exhaustive enumeration, including the degenerate s==t
// case, chains (consecutive pairs), gap domains, and values absent from
// domains.
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

bool precede_ok(const std::vector<int>& xs, int s, int t) {
    if (s == t) {
        for (int v : xs) if (v == s) return false;
        return true;
    }
    for (int v : xs) {
        if (v == s) return true;
        if (v == t) return false;
    }
    return true;
}

// x_dom[i] = explicit value list (allows gap domains).
// pairs = list of (s, t) — one ValuePrecedeConstraint per pair (chain testing).
void check(const std::vector<std::vector<int>>& x_dom,
           const std::vector<std::pair<int, int>>& pairs) {
    const size_t M = x_dom.size();

    Model model;
    std::vector<Variable*> xs;
    for (size_t i = 0; i < M; ++i) {
        std::vector<Domain::value_type> vals(x_dom[i].begin(), x_dom[i].end());
        xs.push_back(model.create_variable("x" + std::to_string(i), vals));
    }
    for (const auto& [s, t] : pairs) {
        model.add_constraint(std::make_unique<ValuePrecedeConstraint>(s, t, xs));
    }

    Solver solver;
    std::set<std::vector<int64_t>> got;
    solver.solve_all(model, [&](const Solution& sol) {
        std::vector<int64_t> row;
        for (size_t i = 0; i < M; ++i) row.push_back(sol.at("x" + std::to_string(i)));
        got.insert(std::move(row));
        return true;
    });

    std::set<std::vector<int64_t>> want;
    std::vector<int> cur(M);
    std::function<void(size_t)> rec = [&](size_t k) {
        if (k == M) {
            for (const auto& [s, t] : pairs)
                if (!precede_ok(cur, s, t)) return;
            want.insert(std::vector<int64_t>(cur.begin(), cur.end()));
            return;
        }
        for (int v : x_dom[k]) { cur[k] = v; rec(k + 1); }
    };
    rec(0);

    REQUIRE(got == want);
}

}  // namespace

TEST_CASE("value_precede: name", "[constraint][value_precede]") {
    Model model;
    std::vector<Variable*> xs{model.create_variable("x0", 1, 2)};
    ValuePrecedeConstraint c(1, 2, xs);
    REQUIRE(c.name() == "sabori_value_precede");
}

TEST_CASE("value_precede: basic equivalence (brute-force)",
          "[constraint][value_precede][brute]") {
    // 3 vars over 1..3, precede(1, 2).
    check({{1, 2, 3}, {1, 2, 3}, {1, 2, 3}}, {{1, 2}});
    // 4 vars over 1..2.
    check({{1, 2}, {1, 2}, {1, 2}, {1, 2}}, {{1, 2}});
}

TEST_CASE("value_precede: chain via consecutive pairs (brute-force)",
          "[constraint][value_precede][brute]") {
    // value_precede_chain([1,2,3], x) == precede(1,2) /\ precede(2,3).
    check({{1, 2, 3}, {1, 2, 3}, {1, 2, 3}, {1, 2, 3}}, {{1, 2}, {2, 3}});
}

TEST_CASE("value_precede: values absent from domains (brute-force)",
          "[constraint][value_precede][brute]") {
    // s absent everywhere => t must not appear.
    check({{2, 3}, {2, 3}, {2, 3}}, {{1, 2}});
    // t absent everywhere => trivially satisfied.
    check({{1, 3}, {1, 3}}, {{1, 2}});
    // Both absent.
    check({{3, 4}, {3, 4}}, {{1, 2}});
}

TEST_CASE("value_precede: forced t requires earlier s (brute-force)",
          "[constraint][value_precede][brute]") {
    // x1 pinned to t=2: x0 must be s=1.
    check({{1, 3}, {2, 2}}, {{1, 2}});
    // t pinned at index 0: UNSAT.
    check({{2, 2}, {1, 2}}, {{1, 2}});
}

TEST_CASE("value_precede: degenerate s==t (brute-force)",
          "[constraint][value_precede][brute]") {
    // s==t: the value may not appear at all.
    check({{1, 2}, {1, 2}, {1, 2}}, {{1, 1}});
}

TEST_CASE("value_precede: gap domains and negatives (brute-force)",
          "[constraint][value_precede][brute]") {
    check({{-1, 1, 3}, {-1, 3}, {-1, 1, 3}}, {{-1, 3}});
    // Mixed: some vars cannot take s.
    check({{2, 3}, {1, 2, 3}, {2, 3}}, {{1, 2}});
}
