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

// Relation under test. EqReif/NeReif/LeReif are biconditionals (P <-> b);
// LeImp is the half-reified implication (b=1 -> sum<=bound).
enum class Rel { EqReif, NeReif, LeReif, LeImp };

// Whether (assignment with this sum, b) satisfies the constraint.
bool sat(Rel rel, int64_t sum, int64_t rhs, int bv) {
    switch (rel) {
        case Rel::EqReif: return (sum == rhs) == (bv == 1);
        case Rel::NeReif: return (sum != rhs) == (bv == 1);
        case Rel::LeReif: return (sum <= rhs) == (bv == 1);
        case Rel::LeImp:  return (bv == 0) || (sum <= rhs);  // b=1 implies sum<=rhs
    }
    return false;
}

// Collect the solver's full solution set as sorted tuples (vars..., b).
std::set<std::vector<int64_t>> solve_set(const Instance& inst, Rel rel) {
    Model model;
    std::vector<Variable*> vars;
    for (size_t i = 0; i < inst.coeffs.size(); ++i) {
        vars.push_back(model.create_variable("x" + std::to_string(i), inst.lo[i], inst.hi[i]));
    }
    auto* b = model.create_variable("b", inst.b_lo, inst.b_hi);
    switch (rel) {
        case Rel::EqReif:
            model.add_constraint(std::make_unique<IntLinEqReifConstraint>(inst.coeffs, vars, inst.target, b));
            break;
        case Rel::NeReif:
            model.add_constraint(std::make_unique<IntLinNeReifConstraint>(inst.coeffs, vars, inst.target, b));
            break;
        case Rel::LeReif:
            model.add_constraint(std::make_unique<IntLinLeReifConstraint>(inst.coeffs, vars, inst.target, b));
            break;
        case Rel::LeImp:
            model.add_constraint(std::make_unique<IntLinLeImpConstraint>(inst.coeffs, vars, inst.target, b));
            break;
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
std::set<std::vector<int64_t>> brute_set(const Instance& inst, Rel rel) {
    std::set<std::vector<int64_t>> result;
    size_t n = inst.coeffs.size();
    std::vector<int64_t> cur(n);
    // Iterate the cross product of [lo,hi] for each var, plus b in [b_lo,b_hi].
    std::function<void(size_t)> rec = [&](size_t i) {
        if (i == n) {
            int64_t sum = 0;
            for (size_t k = 0; k < n; ++k) sum += inst.coeffs[k] * cur[k];
            for (int bv = inst.b_lo; bv <= inst.b_hi; ++bv) {
                if (sat(rel, sum, inst.target, bv)) {
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

void check_instance(const Instance& inst, Rel rel) {
    auto got = solve_set(inst, rel);
    auto want = brute_set(inst, rel);
    REQUIRE(got == want);
}

// Drive 400 randomized small instances through the solver vs brute force.
void run_random(Rel rel, uint32_t seed) {
    std::mt19937 rng(seed);
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
            if (c == 0) c = 1;  // 0-coeff handled by aggregate; keep terms non-trivial
            inst.coeffs.push_back(c);
            int lo = lo_dist(rng);
            int hi = lo + span_dist(rng);
            inst.lo.push_back(lo);
            inst.hi.push_back(hi);
        }
        // Pick rhs near the reachable range so both predicate outcomes occur.
        int64_t mid = 0;
        for (int i = 0; i < n; ++i) mid += inst.coeffs[i] * (inst.lo[i] + inst.hi[i]) / 2;
        std::uniform_int_distribution<int> tjit(-4, 4);
        inst.target = mid + tjit(rng);
        int bm = bmode(rng);
        inst.b_lo = (bm == 2) ? 1 : 0;
        inst.b_hi = (bm == 1) ? 0 : 1;

        check_instance(inst, rel);
    }
}

}  // namespace

TEST_CASE("int_lin_eq_reif: randomized solution-set equivalence", "[constraint][int_lin_eq_reif][brute]") {
    run_random(Rel::EqReif, 0xC0FFEE);
}

TEST_CASE("int_lin_ne_reif: randomized solution-set equivalence", "[constraint][int_lin_ne_reif][brute]") {
    run_random(Rel::NeReif, 0xBADF00D);
}

TEST_CASE("int_lin_le_reif: randomized solution-set equivalence", "[constraint][int_lin_le_reif][brute]") {
    run_random(Rel::LeReif, 0x1234567);
}

TEST_CASE("int_lin_le_imp: randomized solution-set equivalence", "[constraint][int_lin_le_imp][brute]") {
    run_random(Rel::LeImp, 0x7654321);
}

// Targeted edge cases: duplicate variables (exercise aggregate_terms) and tight
// domains where b inference / linear-var pruning fires.
TEST_CASE("int_lin_*_reif/imp: targeted edge instances", "[constraint][brute]") {
    // Instance fields: {coeffs, lo, hi, target, b_lo, b_hi}
    // x+y in [4,6], rhs spanning the range so b inference fires both ways.
    check_instance({{1, 1}, {2, 2}, {3, 3}, 5, 0, 1}, Rel::EqReif);
    check_instance({{1, 1}, {2, 2}, {3, 3}, 5, 0, 1}, Rel::NeReif);
    check_instance({{1, 1}, {2, 2}, {3, 3}, 5, 0, 1}, Rel::LeReif);
    check_instance({{1, 1}, {2, 2}, {3, 3}, 5, 0, 1}, Rel::LeImp);
    // negative coeffs: -2*x0 + x1, x0 in [0,3], x1 in [-2,2]
    check_instance({{-2, 1}, {0, -2}, {3, 2}, 1, 0, 1}, Rel::EqReif);
    check_instance({{-2, 1}, {0, -2}, {3, 2}, 1, 0, 1}, Rel::NeReif);
    check_instance({{-2, 1}, {0, -2}, {3, 2}, 1, 0, 1}, Rel::LeReif);
    check_instance({{-2, 1}, {0, -2}, {3, 2}, 1, 0, 1}, Rel::LeImp);
    // x==4 forced, b fixed => UNSAT for the contradicting polarity
    check_instance({{1}, {4}, {4}, 4, 0, 0}, Rel::EqReif);  // (x==4) but b fixed 0 => UNSAT
    check_instance({{1}, {4}, {4}, 4, 1, 1}, Rel::NeReif);  // (x!=4) but b fixed 1 => UNSAT
    // le_reif: b fixed 1 forces sum<=bound pruning; b fixed 0 forces sum>bound
    check_instance({{1, 1}, {0, 0}, {5, 5}, 4, 1, 1}, Rel::LeReif);
    check_instance({{1, 1}, {0, 0}, {5, 5}, 4, 0, 0}, Rel::LeReif);
    // le_imp: b fixed 1 prunes; b fixed 0 unconstrained
    check_instance({{2, -1}, {0, 0}, {5, 5}, 3, 1, 1}, Rel::LeImp);
    check_instance({{2, -1}, {0, 0}, {5, 5}, 3, 0, 0}, Rel::LeImp);
}
