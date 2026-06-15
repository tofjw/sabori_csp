// Brute-force solution-set equivalence tests for IntOneHotChannelConstraint.
//
// Semantics: bools[i] <-> (x == values[i]), with values[] distinct.
// The constraint is created directly (the presolve OneHotChannelAggregator that
// normally synthesizes it from int_eq_reif groups is bypassed here so the
// propagator itself is exercised in isolation). A false UNSAT / false solution
// in either the exhaustive (holes==0, exactly-one) or partial (holes>0,
// at-most-one) regime surfaces as a solution-set mismatch against brute force.
#include <catch2/catch_test_macros.hpp>
#include "sabori_csp/constraints/global/misc.hpp"
#include "sabori_csp/variable.hpp"
#include "sabori_csp/model.hpp"
#include "sabori_csp/solver.hpp"
#include <random>
#include <set>
#include <vector>
#include <string>
#include <functional>

using namespace sabori_csp;

namespace {

struct Instance {
    int64_t x_lo, x_hi;
    std::vector<int64_t> values;        // distinct, the one-hot value set
    std::vector<std::pair<int,int>> b;  // per-bool domain [lo,hi] within [0,1]
};

// Collect the solver's full solution set as tuples (x, b0, b1, ...).
std::set<std::vector<int64_t>> solve_set(const Instance& inst) {
    Model model;
    auto* x = model.create_variable("x", inst.x_lo, inst.x_hi);
    std::vector<Variable*> bvars;
    for (size_t i = 0; i < inst.b.size(); ++i) {
        bvars.push_back(model.create_variable("b" + std::to_string(i),
                                              inst.b[i].first, inst.b[i].second));
    }
    model.add_constraint(std::make_unique<IntOneHotChannelConstraint>(x, inst.values, bvars));

    Solver solver;
    std::set<std::vector<int64_t>> result;
    solver.solve_all(model, [&](const Solution& sol) {
        std::vector<int64_t> row{sol.at("x")};
        for (size_t i = 0; i < inst.b.size(); ++i) row.push_back(sol.at("b" + std::to_string(i)));
        result.insert(std::move(row));
        return true;
    });
    return result;
}

// Exhaustive reference: bvals[i] must equal (x == values[i]).
std::set<std::vector<int64_t>> brute_set(const Instance& inst) {
    std::set<std::vector<int64_t>> result;
    size_t n = inst.b.size();
    std::vector<int64_t> bcur(n);
    std::function<void(size_t, int64_t)> rec = [&](size_t i, int64_t xv) {
        if (i == n) {
            for (size_t k = 0; k < n; ++k) {
                bool want = (k < inst.values.size()) && (xv == inst.values[k]);
                if ((bcur[k] == 1) != want) return;  // violates channeling
            }
            std::vector<int64_t> row{xv};
            row.insert(row.end(), bcur.begin(), bcur.end());
            result.insert(std::move(row));
            return;
        }
        for (int bv = inst.b[i].first; bv <= inst.b[i].second; ++bv) {
            bcur[i] = bv;
            rec(i + 1, xv);
        }
    };
    for (int64_t xv = inst.x_lo; xv <= inst.x_hi; ++xv) rec(0, xv);
    return result;
}

void check_instance(const Instance& inst) {
    REQUIRE(solve_set(inst) == brute_set(inst));
}

}  // namespace

TEST_CASE("IntOneHotChannel: exhaustive coverage SAT (exactly-one)",
          "[constraint][one_hot_channel]") {
    // x in {1,2,3}, values cover the whole domain => exactly one bool true.
    check_instance({1, 3, {1, 2, 3}, {{0, 1}, {0, 1}, {0, 1}}});
}

TEST_CASE("IntOneHotChannel: partial coverage SAT (at-most-one, holes allow all-false)",
          "[constraint][one_hot_channel]") {
    // x in {1,2,3,4}, values={1,2} only => x in {3,4} is a "hole" with all bools 0.
    check_instance({1, 4, {1, 2}, {{0, 1}, {0, 1}}});
}

TEST_CASE("IntOneHotChannel: single value boundary (forces its bool)",
          "[constraint][one_hot_channel]") {
    // x pinned to the sole value => its bool must be 1.
    check_instance({5, 5, {5}, {{0, 1}}});
    // x can avoid the single value (hole) => bool 0.
    check_instance({4, 6, {5}, {{0, 1}}});
}

TEST_CASE("IntOneHotChannel: UNSAT all bools forced false under exhaustive coverage",
          "[constraint][one_hot_channel]") {
    // Exhaustive coverage but every bool pinned to 0 leaves x no legal value.
    Instance inst{1, 3, {1, 2, 3}, {{0, 0}, {0, 0}, {0, 0}}};
    REQUIRE(solve_set(inst).empty());
    REQUIRE(brute_set(inst).empty());  // oracle agrees it is UNSAT
}

TEST_CASE("IntOneHotChannel: UNSAT two bools forced true",
          "[constraint][one_hot_channel]") {
    // b0=1 and b1=1 demand x==1 and x==2 simultaneously => contradiction.
    Instance inst{1, 3, {1, 2, 3}, {{1, 1}, {1, 1}, {0, 1}}};
    REQUIRE(solve_set(inst).empty());
    REQUIRE(brute_set(inst).empty());
}

TEST_CASE("IntOneHotChannel: bool forced true pins x to its value",
          "[constraint][one_hot_channel]") {
    // b1=1 forces x==values[1]==20; the other bools must then be 0.
    check_instance({10, 30, {10, 20, 30}, {{0, 1}, {1, 1}, {0, 1}}});
}

TEST_CASE("IntOneHotChannel: non-contiguous values",
          "[constraint][one_hot_channel]") {
    // values {-5, 0, 7} are non-contiguous (exercises lower_bound index path).
    check_instance({-6, 8, {-5, 0, 7}, {{0, 1}, {0, 1}, {0, 1}}});
}

TEST_CASE("IntOneHotChannel: randomized brute-force equivalence",
          "[constraint][one_hot_channel]") {
    std::mt19937 rng(20260616u);
    std::uniform_int_distribution<int> nval(1, 3);
    std::uniform_int_distribution<int> lo_dist(-3, 1);
    std::uniform_int_distribution<int> span(0, 5);
    std::uniform_int_distribution<int> bmode(0, 2);  // 0 free, 1 fixed 0, 2 fixed 1

    for (int iter = 0; iter < 300; ++iter) {
        int64_t x_lo = lo_dist(rng);
        int64_t x_hi = x_lo + span(rng);
        int k = nval(rng);
        // Pick k distinct values from a window overlapping x's domain.
        std::set<int64_t> vs;
        std::uniform_int_distribution<int> vpick(static_cast<int>(x_lo) - 2,
                                                 static_cast<int>(x_hi) + 2);
        while (static_cast<int>(vs.size()) < k) vs.insert(vpick(rng));
        std::vector<int64_t> values(vs.begin(), vs.end());

        std::vector<std::pair<int,int>> b;
        for (int i = 0; i < k; ++i) {
            switch (bmode(rng)) {
                case 1:  b.emplace_back(0, 0); break;
                case 2:  b.emplace_back(1, 1); break;
                default: b.emplace_back(0, 1); break;
            }
        }
        check_instance({x_lo, x_hi, values, b});
    }
}
