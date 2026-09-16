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
#include "sabori_csp/constraints/comparison.hpp"
#include "sabori_csp/one_hot_channel_aggregator.hpp"
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
    std::vector<uint8_t> imp = {};      // imp[i]!=0: b[i] -> (x==values[i]) one-way
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
    model.add_constraint(std::make_unique<IntOneHotChannelConstraint>(
        x, inst.values, bvars, inst.imp));

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

// Exhaustive reference: reif entries need bvals[i] == (x == values[i]);
// imp entries only forbid bvals[i]==1 while x != values[i].
std::set<std::vector<int64_t>> brute_set(const Instance& inst) {
    std::set<std::vector<int64_t>> result;
    size_t n = inst.b.size();
    std::vector<int64_t> bcur(n);
    std::function<void(size_t, int64_t)> rec = [&](size_t i, int64_t xv) {
        if (i == n) {
            for (size_t k = 0; k < n; ++k) {
                bool want = (k < inst.values.size()) && (xv == inst.values[k]);
                bool is_imp = (k < inst.imp.size()) && inst.imp[k];
                if (is_imp) {
                    if (bcur[k] == 1 && !want) return;  // b=1 requires x==v
                } else if ((bcur[k] == 1) != want) {
                    return;  // violates channeling
                }
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

// ---------------------------------------------------------------------------
// imp (half-reified) エントリ: bools[i] -> (x == values[i])
// ---------------------------------------------------------------------------

TEST_CASE("IntOneHotChannel imp: x==v does NOT force b=1",
          "[constraint][one_hot_channel][imp]") {
    // x pinned to the sole imp value => b may be 0 or 1 (reif would force 1).
    check_instance({5, 5, {5}, {{0, 1}}, {1}});
}

TEST_CASE("IntOneHotChannel imp: b=0 does NOT remove the value",
          "[constraint][one_hot_channel][imp]") {
    // Exhaustive coverage but all bools pinned 0: reif => UNSAT,
    // imp => any x is fine (b=0 is vacuous).
    Instance inst{1, 3, {1, 2, 3}, {{0, 0}, {0, 0}, {0, 0}}, {1, 1, 1}};
    REQUIRE(solve_set(inst) == brute_set(inst));
    REQUIRE(!brute_set(inst).empty());  // oracle: SAT (3 solutions)
}

TEST_CASE("IntOneHotChannel imp: b=1 pins x to its value",
          "[constraint][one_hot_channel][imp]") {
    check_instance({10, 30, {10, 20, 30}, {{0, 1}, {1, 1}, {0, 1}}, {1, 1, 1}});
}

TEST_CASE("IntOneHotChannel imp: UNSAT b=1 with value outside domain",
          "[constraint][one_hot_channel][imp]") {
    // b=1 demands x==7 but x in 1..3 => contradiction even for imp.
    Instance inst{1, 3, {7}, {{1, 1}}, {1}};
    REQUIRE(solve_set(inst).empty());
    REQUIRE(brute_set(inst).empty());
}

TEST_CASE("IntOneHotChannel imp: mixed reif+imp disables exactly-one",
          "[constraint][one_hot_channel][imp]") {
    // Exhaustive values {1,2,3}: reif b0=0, reif b1=0 remove 1 and 2, so x=3.
    // b2 is imp => must NOT be forced to 1 (both 0 and 1 legal).
    check_instance({1, 3, {1, 2, 3}, {{0, 0}, {0, 0}, {0, 1}}, {0, 0, 1}});
    // All-zero with mixed flags: x can only take the imp-covered value 3.
    check_instance({1, 3, {1, 2, 3}, {{0, 0}, {0, 0}, {0, 0}}, {0, 0, 1}});
}

TEST_CASE("IntOneHotChannel imp: randomized brute-force equivalence",
          "[constraint][one_hot_channel][imp]") {
    std::mt19937 rng(20260707u);
    std::uniform_int_distribution<int> nval(1, 3);
    std::uniform_int_distribution<int> lo_dist(-3, 1);
    std::uniform_int_distribution<int> span(0, 5);
    std::uniform_int_distribution<int> bmode(0, 2);  // 0 free, 1 fixed 0, 2 fixed 1
    std::uniform_int_distribution<int> impmode(0, 1);

    for (int iter = 0; iter < 300; ++iter) {
        int64_t x_lo = lo_dist(rng);
        int64_t x_hi = x_lo + span(rng);
        int k = nval(rng);
        std::set<int64_t> vs;
        std::uniform_int_distribution<int> vpick(static_cast<int>(x_lo) - 2,
                                                 static_cast<int>(x_hi) + 2);
        while (static_cast<int>(vs.size()) < k) vs.insert(vpick(rng));
        std::vector<int64_t> values(vs.begin(), vs.end());

        std::vector<std::pair<int,int>> b;
        std::vector<uint8_t> imp;
        for (int i = 0; i < k; ++i) {
            switch (bmode(rng)) {
                case 1:  b.emplace_back(0, 0); break;
                case 2:  b.emplace_back(1, 1); break;
                default: b.emplace_back(0, 1); break;
            }
            imp.push_back(static_cast<uint8_t>(impmode(rng)));
        }
        check_instance({x_lo, x_hi, values, b, imp});
    }
}

// ---------------------------------------------------------------------------
// OneHotChannelAggregator: int_eq_imp の集約
// ---------------------------------------------------------------------------

TEST_CASE("OneHotChannelAggregator: aggregates constant int_eq_imp with imp flags",
          "[one_hot_channel][aggregator][imp]") {
    Model model;
    auto* x = model.create_variable("x", 1, 4);
    auto* c1 = model.create_variable("c1", 1, 1);
    auto* c2 = model.create_variable("c2", 2, 2);
    auto* c3 = model.create_variable("c3", 3, 3);
    auto* b1 = model.create_variable("b1", 0, 1);
    auto* b2 = model.create_variable("b2", 0, 1);
    auto* b3 = model.create_variable("b3", 0, 1);
    model.add_constraint(std::make_unique<IntEqImpConstraint>(x, c1, b1));
    model.add_constraint(std::make_unique<IntEqImpConstraint>(x, c2, b2));
    // 逆順引数 (定数が左) も拾えること
    model.add_constraint(std::make_unique<IntEqImpConstraint>(c3, x, b3));

    OneHotChannelAggregator agg;
    REQUIRE(agg.aggregate(model));

    // 3 本の imp が 1 本の IntOneHotChannel に置換されている
    const IntOneHotChannelConstraint* channel = nullptr;
    size_t channels = 0, imps_left = 0;
    for (const auto& c : model.constraints()) {
        if (!c) continue;
        if (auto* ch = dynamic_cast<const IntOneHotChannelConstraint*>(c.get())) {
            channel = ch;
            ++channels;
        } else if (dynamic_cast<const IntEqImpConstraint*>(c.get())) {
            ++imps_left;
        }
    }
    REQUIRE(channels == 1);
    REQUIRE(imps_left == 0);
    REQUIRE(channel->values() == std::vector<Domain::value_type>{1, 2, 3});
    REQUIRE(channel->imp_flags() == std::vector<uint8_t>{1, 1, 1});
}

TEST_CASE("OneHotChannelAggregator: solve-path equivalence for imp group",
          "[one_hot_channel][aggregator][imp]") {
    // Solver 経由 (aggregator 込み) の全解が、imp 意味論の brute oracle と一致。
    // x=4 は values 外 (hole)。b は自由。
    auto build = [](Model& model) {
        auto* x = model.create_variable("x", 1, 4);
        auto* c1 = model.create_variable("c1", 1, 1);
        auto* c2 = model.create_variable("c2", 2, 2);
        auto* c3 = model.create_variable("c3", 3, 3);
        auto* b1 = model.create_variable("b1", 0, 1);
        auto* b2 = model.create_variable("b2", 0, 1);
        auto* b3 = model.create_variable("b3", 0, 1);
        model.add_constraint(std::make_unique<IntEqImpConstraint>(x, c1, b1));
        model.add_constraint(std::make_unique<IntEqImpConstraint>(x, c2, b2));
        model.add_constraint(std::make_unique<IntEqImpConstraint>(x, c3, b3));
    };
    Model model;
    build(model);
    Solver solver;
    std::set<std::vector<int64_t>> got;
    solver.solve_all(model, [&](const Solution& sol) {
        got.insert({sol.at("x"), sol.at("b1"), sol.at("b2"), sol.at("b3")});
        return true;
    });

    std::set<std::vector<int64_t>> want;
    for (int64_t xv = 1; xv <= 4; ++xv)
        for (int64_t v1 = 0; v1 <= 1; ++v1)
            for (int64_t v2 = 0; v2 <= 1; ++v2)
                for (int64_t v3 = 0; v3 <= 1; ++v3) {
                    if (v1 == 1 && xv != 1) continue;
                    if (v2 == 1 && xv != 2) continue;
                    if (v3 == 1 && xv != 3) continue;
                    want.insert({xv, v1, v2, v3});
                }
    REQUIRE(got == want);
}
