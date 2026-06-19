// Brute-force solution-set equivalence tests for the disjunctive (unary
// resource) constraint.
//
// Safety net for the Phase 4 set_bits Trailer unification
// (refactoring-plan-2026-06.md): set_bits / set_bits_direct share the
// timeline bit-mask geometry and differ only in trail recording. A
// transcription error while merging them would surface as a solution-set
// mismatch against exhaustive enumeration here.
#include <catch2/catch_test_macros.hpp>
#include "sabori_csp/constraints/global.hpp"
#include "sabori_csp/variable.hpp"
#include "sabori_csp/model.hpp"
#include "sabori_csp/solver.hpp"
#include <set>
#include <vector>
#include <functional>

using namespace sabori_csp;

namespace {

struct Task {
    int slo, shi;  // start domain
    int dur;       // fixed duration (singleton)
};

// Reference: do these task starts avoid pairwise overlap on a unary resource?
bool no_overlap(const std::vector<int>& starts, const std::vector<Task>& tasks, bool strict) {
    size_t n = tasks.size();
    for (size_t i = 0; i < n; ++i) {
        if (!strict && tasks[i].dur == 0) continue;
        for (size_t j = i + 1; j < n; ++j) {
            if (!strict && tasks[j].dur == 0) continue;
            bool sep = (starts[i] + tasks[i].dur <= starts[j]) ||
                       (starts[j] + tasks[j].dur <= starts[i]);
            if (!sep) return false;
        }
    }
    return true;
}

void check(const std::vector<Task>& tasks, bool strict) {
    size_t n = tasks.size();
    Model model;
    std::vector<Variable*> sv, dv;
    for (size_t i = 0; i < n; ++i) {
        sv.push_back(model.create_variable("s" + std::to_string(i), tasks[i].slo, tasks[i].shi));
        dv.push_back(model.create_variable("d" + std::to_string(i), tasks[i].dur, tasks[i].dur));
    }
    model.add_constraint(std::make_unique<DisjunctiveConstraint>(sv, dv, strict));

    Solver solver;
    std::set<std::vector<int64_t>> got;
    solver.solve_all(model, [&](const Solution& sol) {
        std::vector<int64_t> row;
        for (size_t i = 0; i < n; ++i) row.push_back(sol.at("s" + std::to_string(i)));
        got.insert(std::move(row));
        return true;
    });

    std::set<std::vector<int64_t>> want;
    std::vector<int> starts(n);
    std::function<void(size_t)> rec = [&](size_t k) {
        if (k == n) {
            if (no_overlap(starts, tasks, strict)) {
                std::vector<int64_t> row(starts.begin(), starts.end());
                want.insert(std::move(row));
            }
            return;
        }
        for (int v = tasks[k].slo; v <= tasks[k].shi; ++v) { starts[k] = v; rec(k + 1); }
    };
    rec(0);

    REQUIRE(got == want);
}

}  // namespace

TEST_CASE("disjunctive: brute-force solution-set equivalence (strict)", "[constraint][disjunctive][brute]") {
    // Two unit tasks in a small window.
    check({{0, 3, 1}, {0, 3, 1}}, true);
    // Mixed durations, tight window forces ordering.
    check({{0, 4, 2}, {0, 4, 3}}, true);
    // Three tasks.
    check({{0, 4, 1}, {0, 4, 2}, {0, 4, 1}}, true);
    // Overlapping wide durations.
    check({{0, 5, 3}, {0, 5, 2}}, true);
}

TEST_CASE("disjunctive: brute-force solution-set equivalence (nonstrict)", "[constraint][disjunctive][brute]") {
    // Zero-duration task is exempt from non-overlap.
    check({{0, 3, 0}, {0, 3, 2}}, false);
    check({{0, 3, 0}, {0, 3, 1}, {0, 3, 1}}, false);
    // All positive durations behaves like strict.
    check({{0, 3, 1}, {0, 3, 2}}, false);
}

// Regression for a false-UNSAT soundness bug in the disjunctive propagator.
// find_first/find_last_valid_excluding advanced via find_next_zero, which skips over
// the task's OWN compulsory-part bits — so valid start positions inside/under the own
// CP were never tried, yielding a false conflict. Minimal trigger: one task with start
// window [0,1] dur 2 next to a fixed task occupying [0,1); s1=1 ([1,3)) is valid but the
// propagator reported UNSAT. This surfaced as incomplete -a enumeration (nogoods shrink a
// domain into the buggy state, then the false conflict drops valid solutions). True
// all-solution counts: 5 tasks dur 2 in [0,9] = 720. See memory
// allsol-incomplete-unsound-learning.md.
TEST_CASE("disjunctive: no false conflict when start sits under own compulsory part",
          "[constraint][disjunctive][brute]") {
    // s1 in [0,1] dur 2, s2=0 dur 1: only s1=1 is feasible (must be SAT, not UNSAT).
    Model model;
    auto* s1 = model.create_variable("s1", 0, 1);
    auto* s2 = model.create_variable("s2", 0, 0);
    auto* d1 = model.create_variable("d1", 2, 2);
    auto* d2 = model.create_variable("d2", 1, 1);
    model.add_constraint(std::make_unique<DisjunctiveConstraint>(
        std::vector<Variable*>{s1, s2}, std::vector<Variable*>{d1, d2}, true));
    Solver solver;
    auto sol = solver.solve(model);
    REQUIRE(sol.has_value());
    CHECK(sol->at("s1") == 1);
}

// All-solution enumeration must be complete (downstream symptom of the above bug).
TEST_CASE("disjunctive: all-solution enumeration completeness (strict)",
          "[constraint][disjunctive][brute]") {
    check({{0, 9, 2}, {0, 9, 2}, {0, 9, 2}, {0, 9, 2}, {0, 9, 2}}, true);
}
