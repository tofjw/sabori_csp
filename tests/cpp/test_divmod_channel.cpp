// Solution-set equivalence tests for DivModChannelAggregator.
//
// The pass rewrites the pair (int_div(x, c, q), int_mod(x, c, r)) — same
// dividend, same positive constant divisor — into the Euclidean channel
// x = c*q + r with 0 <= r < c. Two modes are covered:
//   augment (default): the linear is added, div/mod stay in the model
//   replace:           div/mod are dropped, the linear carries the semantics
// Both must preserve the solution set exactly. A wrong guard (e.g. firing on a
// dividend that can go negative, where truncated division makes r negative)
// shows up here as a solution-set mismatch against brute force.
#include <catch2/catch_test_macros.hpp>
#include "sabori_csp/constraints/arithmetic.hpp"
#include "sabori_csp/divmod_channel_aggregator.hpp"
#include "sabori_csp/variable.hpp"
#include "sabori_csp/model.hpp"
#include "sabori_csp/solver.hpp"
#include <set>
#include <vector>

using namespace sabori_csp;

namespace {

struct Instance {
    int64_t x_lo, x_hi;
    int64_t c;
    int64_t q_lo, q_hi;
    int64_t r_lo, r_hi;
};

enum class Mode { Default, PreReplace, Disabled };

// Collect the solver's full solution set as tuples (x, q, r).
std::set<std::vector<int64_t>> solve_set(const Instance& inst, Mode mode) {
    Model model;
    auto* x = model.create_variable("x", inst.x_lo, inst.x_hi);
    auto* c = model.create_variable("c", inst.c, inst.c);
    auto* q = model.create_variable("q", inst.q_lo, inst.q_hi);
    auto* r = model.create_variable("r", inst.r_lo, inst.r_hi);
    model.add_constraint(std::make_unique<IntDivConstraint>(x, c, q));
    model.add_constraint(std::make_unique<IntModConstraint>(x, c, r));

    if (mode == Mode::PreReplace) {
        // Rewrite up front so the solver's own (augment) pass finds no pair left.
        DivModChannelAggregator agg;
        agg.set_replace(true);
        REQUIRE(agg.aggregate(model, false));
    }

    Solver solver;
    if (mode == Mode::Disabled) solver.set_divmod_channel(false);
    std::set<std::vector<int64_t>> result;
    solver.solve_all(model, [&](const Solution& sol) {
        result.insert({sol.at("x"), sol.at("q"), sol.at("r")});
        return true;
    });
    return result;
}

// Exhaustive reference over the declared domains, using C++ truncated div/mod
// (the same semantics IntDiv/IntMod implement).
std::set<std::vector<int64_t>> brute_set(const Instance& inst) {
    std::set<std::vector<int64_t>> result;
    for (int64_t xv = inst.x_lo; xv <= inst.x_hi; ++xv) {
        int64_t qv = xv / inst.c;
        int64_t rv = xv % inst.c;
        if (qv < inst.q_lo || qv > inst.q_hi) continue;
        if (rv < inst.r_lo || rv > inst.r_hi) continue;
        result.insert({xv, qv, rv});
    }
    return result;
}

void check_instance(const Instance& inst) {
    auto expected = brute_set(inst);
    REQUIRE(solve_set(inst, Mode::Disabled) == expected);
    REQUIRE(solve_set(inst, Mode::Default) == expected);
    REQUIRE(solve_set(inst, Mode::PreReplace) == expected);
}

}  // namespace

TEST_CASE("DivModChannel: non-negative dividend, exact fit", "[constraint][divmod_channel]") {
    check_instance({0, 30, 7, 0, 10, 0, 9});
}

TEST_CASE("DivModChannel: remainder domain wider than the divisor", "[constraint][divmod_channel]") {
    // r declared 0..20 but must land in 0..4; the pass tightens it to 0..c-1.
    check_instance({0, 23, 5, -5, 20, 0, 20});
}

TEST_CASE("DivModChannel: dividend not starting at zero", "[constraint][divmod_channel]") {
    check_instance({13, 41, 6, 0, 20, 0, 10});
}

TEST_CASE("DivModChannel: divisor 1 (remainder pinned to 0)", "[constraint][divmod_channel]") {
    check_instance({0, 9, 1, 0, 9, -3, 3});
}

TEST_CASE("DivModChannel: negative dividend must not be rewritten", "[constraint][divmod_channel]") {
    // Truncated division gives r < 0 for x < 0, so the Euclidean channel does
    // not hold; the guard must skip these and leave div/mod semantics intact.
    check_instance({-10, 10, 7, -10, 10, -9, 9});
    check_instance({-8, -1, 3, -10, 10, -9, 9});
}

TEST_CASE("DivModChannel: quotient bound makes it UNSAT", "[constraint][divmod_channel]") {
    // x >= 20 forces q >= 4, but q is capped at 2.
    Instance inst{20, 25, 5, 0, 2, 0, 4};
    REQUIRE(brute_set(inst).empty());
    REQUIRE(solve_set(inst, Mode::Default).empty());
    REQUIRE(solve_set(inst, Mode::PreReplace).empty());
}
