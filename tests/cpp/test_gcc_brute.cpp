// Brute-force solution-set equivalence tests for global_cardinality.
//
// global_cardinality(x, cover, counts): counts[j] = |{i : x[i] == cover[j]}|,
// open semantics (x may take values outside cover). We verify the propagator is
// both sound and complete by comparing its full solution set against exhaustive
// enumeration, including out-of-cover values, count-bound forcing (include /
// exclude), duplicate cover entries, and gap domains.
#include <catch2/catch_test_macros.hpp>
#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/variable.hpp"
#include "sabori_csp/model.hpp"
#include "sabori_csp/solver.hpp"
#include <set>
#include <vector>
#include <functional>
#include <string>

using namespace sabori_csp;

namespace {

// x_dom[i] = explicit value list for x[i] (allows gap domains).
// count_dom[j] = (lo, hi) domain of counts[j].
void check(const std::vector<std::vector<int>>& x_dom,
           const std::vector<int>& cover,
           const std::vector<std::pair<int, int>>& count_dom) {
    const size_t M = x_dom.size();
    const size_t V = cover.size();
    REQUIRE(count_dom.size() == V);

    Model model;
    std::vector<Variable*> xs, counts;
    for (size_t i = 0; i < M; ++i) {
        std::vector<Domain::value_type> vals(x_dom[i].begin(), x_dom[i].end());
        xs.push_back(model.create_variable("x" + std::to_string(i), vals));
    }
    for (size_t j = 0; j < V; ++j)
        counts.push_back(model.create_variable("c" + std::to_string(j),
                                               count_dom[j].first, count_dom[j].second));
    std::vector<int64_t> cover64(cover.begin(), cover.end());
    model.add_constraint(std::make_unique<GlobalCardinalityConstraint>(
        xs, cover64, counts));

    Solver solver;
    std::set<std::vector<int64_t>> got;
    solver.solve_all(model, [&](const Solution& sol) {
        std::vector<int64_t> row;
        for (size_t i = 0; i < M; ++i) row.push_back(sol.at("x" + std::to_string(i)));
        for (size_t j = 0; j < V; ++j) row.push_back(sol.at("c" + std::to_string(j)));
        got.insert(std::move(row));
        return true;
    });

    // Brute-force: enumerate x, derive occurrence counts, keep rows whose counts
    // fit every count domain (counts are functionally determined by x).
    std::set<std::vector<int64_t>> want;
    std::vector<int> x_ref(M);
    std::function<void(size_t)> rec = [&](size_t k) {
        if (k == M) {
            std::vector<int64_t> row(x_ref.begin(), x_ref.end());
            for (size_t j = 0; j < V; ++j) {
                int occ = 0;
                for (size_t i = 0; i < M; ++i)
                    if (x_ref[i] == cover[j]) ++occ;
                if (occ < count_dom[j].first || occ > count_dom[j].second) return;
                row.push_back(occ);
            }
            want.insert(std::move(row));
            return;
        }
        for (int v : x_dom[k]) { x_ref[k] = v; rec(k + 1); }
    };
    rec(0);

    REQUIRE(got == want);
}

}  // namespace

TEST_CASE("global_cardinality: name", "[constraint][gcc]") {
    Model model;
    std::vector<Variable*> xs{model.create_variable("x0", 1, 2)};
    std::vector<Variable*> counts{model.create_variable("c0", 0, 1)};
    std::vector<int64_t> cover{1};
    GlobalCardinalityConstraint c(xs, cover, counts);
    REQUIRE(c.name() == "sabori_global_cardinality");
}

TEST_CASE("global_cardinality: basic equivalence (brute-force)",
          "[constraint][gcc][brute]") {
    // 3 vars over 1..3, full cover, free counts.
    check({{1, 2, 3}, {1, 2, 3}, {1, 2, 3}}, {1, 2, 3},
          {{0, 3}, {0, 3}, {0, 3}});
    // Partial cover: value 3 is unconstrained (open semantics).
    check({{1, 2, 3}, {1, 2, 3}, {1, 2, 3}}, {1, 2},
          {{0, 3}, {0, 3}});
}

TEST_CASE("global_cardinality: count bounds force placement (brute-force)",
          "[constraint][gcc][brute]") {
    // Lower bound forces all possible supporters to take the value (include).
    check({{1, 2}, {1, 2}, {2, 3}}, {1, 2}, {{2, 2}, {0, 3}});
    // Upper bound zero excludes the value everywhere (exclude).
    check({{1, 2}, {1, 2}, {1, 3}}, {1, 2}, {{0, 0}, {0, 3}});
    // Exact counts over the full cover (tight distribution).
    check({{1, 2}, {1, 2}, {1, 2}}, {1, 2}, {{1, 1}, {2, 2}});
    // Infeasible: 3 vars all in {1}, but count of 1 capped at 2.
    check({{1}, {1}, {1}}, {1}, {{0, 2}});
}

TEST_CASE("global_cardinality: duplicate cover entries (brute-force)",
          "[constraint][gcc][brute]") {
    // Value 1 appears twice in cover; both counts must equal its occurrences,
    // and their domains intersect to [1, 2].
    check({{1, 2}, {1, 2}, {1, 2}}, {1, 1, 2},
          {{0, 1}, {1, 2}, {0, 3}});
}

TEST_CASE("global_cardinality: gap domains and negative values (brute-force)",
          "[constraint][gcc][brute]") {
    // Gap domains exercise the domain-size-based must-in-cover accounting.
    check({{-1, 1, 3}, {1, 3}, {-1, 3}}, {-1, 1, 3},
          {{0, 3}, {0, 3}, {0, 3}});
    // Cover value absent from every domain: its count must be 0.
    check({{1, 2}, {1, 2}}, {5, 1}, {{0, 2}, {0, 2}});
}

TEST_CASE("global_cardinality: empty edge cases", "[constraint][gcc]") {
    // No x vars: every count must be 0.
    {
        Model model;
        std::vector<Variable*> xs;
        std::vector<Variable*> counts{model.create_variable("c0", 0, 2)};
        std::vector<int64_t> cover{1};
        model.add_constraint(std::make_unique<GlobalCardinalityConstraint>(
            xs, cover, counts));
        Solver solver;
        auto sol = solver.solve(model);
        REQUIRE(sol.has_value());
        REQUIRE(sol->at("c0") == 0);
    }
    // Empty cover: constraint is trivially satisfied.
    {
        Model model;
        std::vector<Variable*> xs{model.create_variable("x0", 1, 2)};
        std::vector<Variable*> counts;
        std::vector<int64_t> cover;
        model.add_constraint(std::make_unique<GlobalCardinalityConstraint>(
            xs, cover, counts));
        Solver solver;
        auto sol = solver.solve(model);
        REQUIRE(sol.has_value());
    }
}
