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
