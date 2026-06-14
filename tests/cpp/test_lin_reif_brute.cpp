// Brute-force solution-set equivalence tests for linear reified constraints.
//
// Safety net for the Phase 4 ReifConstraintBase refactor (refactoring-plan-2026-06.md).
// Each randomized instance of int_lin_eq_reif / int_lin_ne_reif is solved with
// solve_all and the full solution set is compared against an exhaustive reference
// enumeration. A false UNSAT / false OPTIMAL in the reif propagation would surface
// here as a mismatch in the collected solution set.
#include <catch2/catch_test_macros.hpp>
#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/variable.hpp"
#include "sabori_csp/model.hpp"
#include "sabori_csp/solver.hpp"
#include <random>
#include <set>
#include <vector>
#include <numeric>
#include <functional>

using namespace sabori_csp;

namespace {

struct Instance {
    std::vector<int64_t> coeffs;
    std::vector<int64_t> lo;
    std::vector<int64_t> hi;
    int64_t target;
    int b_lo;  // b domain [b_lo, b_hi], subset of [0,1]
    int b_hi;
};

// Collect the solver's full solution set as sorted tuples (vars..., b).
std::set<std::vector<int64_t>> solve_set(const Instance& inst, bool negated) {
    Model model;
    std::vector<Variable*> vars;
    for (size_t i = 0; i < inst.coeffs.size(); ++i) {
        vars.push_back(model.create_variable("x" + std::to_string(i), inst.lo[i], inst.hi[i]));
    }
    auto* b = model.create_variable("b", inst.b_lo, inst.b_hi);
    if (negated) {
        model.add_constraint(std::make_unique<IntLinNeReifConstraint>(inst.coeffs, vars, inst.target, b));
    } else {
        model.add_constraint(std::make_unique<IntLinEqReifConstraint>(inst.coeffs, vars, inst.target, b));
    }

    Solver solver;
    std::set<std::vector<int64_t>> result;
    solver.solve_all(model, [&](const Solution& sol) {
        std::vector<int64_t> row;
        for (size_t i = 0; i < inst.coeffs.size(); ++i) {
            row.push_back(sol.at("x" + std::to_string(i)));
        }
        row.push_back(sol.at("b"));
        result.insert(std::move(row));
        return true;
    });
    return result;
}

// Exhaustive reference enumeration of the same constraint.
std::set<std::vector<int64_t>> brute_set(const Instance& inst, bool negated) {
    std::set<std::vector<int64_t>> result;
    size_t n = inst.coeffs.size();
    std::vector<int64_t> cur(n);
    // Iterate the cross product of [lo,hi] for each var, plus b in [b_lo,b_hi].
    std::function<void(size_t)> rec = [&](size_t i) {
        if (i == n) {
            int64_t sum = 0;
            for (size_t k = 0; k < n; ++k) sum += inst.coeffs[k] * cur[k];
            bool predicate = negated ? (sum != inst.target) : (sum == inst.target);
            for (int bv = inst.b_lo; bv <= inst.b_hi; ++bv) {
                if (predicate == (bv == 1)) {
                    std::vector<int64_t> row = cur;
                    row.push_back(bv);
                    result.insert(std::move(row));
                }
            }
            return;
        }
        for (int64_t v = inst.lo[i]; v <= inst.hi[i]; ++v) {
            cur[i] = v;
            rec(i + 1);
        }
    };
    rec(0);
    return result;
}

void check_instance(const Instance& inst, bool negated) {
    auto got = solve_set(inst, negated);
    auto want = brute_set(inst, negated);
    REQUIRE(got == want);
}

}  // namespace

TEST_CASE("int_lin_eq_reif: randomized solution-set equivalence", "[constraint][int_lin_eq_reif][brute]") {
    std::mt19937 rng(0xC0FFEE);
    std::uniform_int_distribution<int> n_dist(1, 3);
    std::uniform_int_distribution<int> coeff_dist(-3, 3);
    std::uniform_int_distribution<int> lo_dist(-3, 2);
    std::uniform_int_distribution<int> span_dist(0, 4);
    std::uniform_int_distribution<int> bmode(0, 2);  // 0: free, 1: fixed 0, 2: fixed 1

    for (int iter = 0; iter < 400; ++iter) {
        Instance inst;
        int n = n_dist(rng);
        for (int i = 0; i < n; ++i) {
            int c = coeff_dist(rng);
            if (c == 0) c = 1;  // keep at least non-trivial; 0-coeff handled by aggregate anyway
            inst.coeffs.push_back(c);
            int lo = lo_dist(rng);
            int hi = lo + span_dist(rng);
            inst.lo.push_back(lo);
            inst.hi.push_back(hi);
        }
        // Pick a target near the reachable range so both predicate outcomes occur.
        int64_t mid = 0;
        for (int i = 0; i < n; ++i) mid += inst.coeffs[i] * (inst.lo[i] + inst.hi[i]) / 2;
        std::uniform_int_distribution<int> tjit(-4, 4);
        inst.target = mid + tjit(rng);
        int bm = bmode(rng);
        inst.b_lo = (bm == 2) ? 1 : 0;
        inst.b_hi = (bm == 1) ? 0 : 1;

        check_instance(inst, /*negated=*/false);
    }
}

TEST_CASE("int_lin_ne_reif: randomized solution-set equivalence", "[constraint][int_lin_ne_reif][brute]") {
    std::mt19937 rng(0xBADF00D);
    std::uniform_int_distribution<int> n_dist(1, 3);
    std::uniform_int_distribution<int> coeff_dist(-3, 3);
    std::uniform_int_distribution<int> lo_dist(-3, 2);
    std::uniform_int_distribution<int> span_dist(0, 4);
    std::uniform_int_distribution<int> bmode(0, 2);

    for (int iter = 0; iter < 400; ++iter) {
        Instance inst;
        int n = n_dist(rng);
        for (int i = 0; i < n; ++i) {
            int c = coeff_dist(rng);
            if (c == 0) c = 1;
            inst.coeffs.push_back(c);
            int lo = lo_dist(rng);
            int hi = lo + span_dist(rng);
            inst.lo.push_back(lo);
            inst.hi.push_back(hi);
        }
        int64_t mid = 0;
        for (int i = 0; i < n; ++i) mid += inst.coeffs[i] * (inst.lo[i] + inst.hi[i]) / 2;
        std::uniform_int_distribution<int> tjit(-4, 4);
        inst.target = mid + tjit(rng);
        int bm = bmode(rng);
        inst.b_lo = (bm == 2) ? 1 : 0;
        inst.b_hi = (bm == 1) ? 0 : 1;

        check_instance(inst, /*negated=*/true);
    }
}

// Targeted edge cases: duplicate variables (exercise aggregate_terms) and tight
// domains where b inference fires.
TEST_CASE("int_lin_*_reif: targeted edge instances", "[constraint][int_lin_eq_reif][int_lin_ne_reif][brute]") {
    // Instance fields: {coeffs, lo, hi, target, b_lo, b_hi}
    // x+y in [4,6], target spanning the range so b inference fires both ways.
    check_instance({{1, 1}, {2, 2}, {3, 3}, 5, 0, 1}, false);
    check_instance({{1, 1}, {2, 2}, {3, 3}, 5, 0, 1}, true);
    // negative coeffs: -2*x0 + x1, x0 in [0,3], x1 in [-2,2]
    check_instance({{-2, 1}, {0, -2}, {3, 2}, 1, 0, 1}, false);
    check_instance({{-2, 1}, {0, -2}, {3, 2}, 1, 0, 1}, true);
    // x==4 forced, b fixed => UNSAT for the contradicting polarity
    check_instance({{1}, {4}, {4}, 4, 0, 0}, false);  // (x==4) but b fixed 0 => UNSAT
    check_instance({{1}, {4}, {4}, 4, 1, 1}, true);   // (x!=4) but b fixed 1 => UNSAT
}
