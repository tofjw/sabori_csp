// Brute-force solution-set equivalence tests for the cumulative constraint.
//
// Motivated by the disjunctive false-UNSAT bug (a task's start window sitting under its
// own compulsory part was wrongly pruned). Cumulative uses a profile-based propagator
// that subtracts each task's own mandatory contribution, but we verify soundness +
// completeness directly against exhaustive enumeration, including capacity==1 cases
// (where cumulative must behave exactly like disjunctive) and the specific small-window
// trigger geometry.
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

struct CTask {
    int slo, shi;  // start domain
    int dur;       // fixed duration
    int req;       // fixed requirement
};

// Reference: does this assignment respect the resource capacity at every time point?
bool within_capacity(const std::vector<int>& starts, const std::vector<CTask>& tasks, int cap) {
    // Find the time horizon.
    int tmax = 0;
    for (size_t i = 0; i < tasks.size(); ++i)
        tmax = std::max(tmax, starts[i] + tasks[i].dur);
    for (int t = 0; t <= tmax; ++t) {
        int used = 0;
        for (size_t i = 0; i < tasks.size(); ++i)
            if (tasks[i].dur > 0 && starts[i] <= t && t < starts[i] + tasks[i].dur)
                used += tasks[i].req;
        if (used > cap) return false;
    }
    return true;
}

void check(const std::vector<CTask>& tasks, int cap) {
    size_t n = tasks.size();
    Model model;
    std::vector<Variable*> sv, dv, rv;
    for (size_t i = 0; i < n; ++i) {
        sv.push_back(model.create_variable("s" + std::to_string(i), tasks[i].slo, tasks[i].shi));
        dv.push_back(model.create_variable("d" + std::to_string(i), tasks[i].dur, tasks[i].dur));
        rv.push_back(model.create_variable("r" + std::to_string(i), tasks[i].req, tasks[i].req));
    }
    auto* capv = model.create_variable("cap", cap, cap);
    model.add_constraint(std::make_unique<CumulativeConstraint>(sv, dv, rv, capv));

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
            if (within_capacity(starts, tasks, cap)) {
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

TEST_CASE("cumulative: capacity=1 behaves like disjunctive (brute-force)",
          "[constraint][cumulative][brute]") {
    // The disjunctive false-UNSAT trigger, expressed as cumulative with cap=1:
    // task0 start [0,1] dur 2, task1 fixed at 0 dur 1. Only s0=1 is feasible.
    check({{0, 1, 2, 1}, {0, 0, 1, 1}}, 1);
    // Two unit-duration-2 tasks in a small window, cap=1.
    check({{0, 3, 2, 1}, {0, 3, 2, 1}}, 1);
    // Mixed durations, cap=1.
    check({{0, 4, 2, 1}, {0, 4, 3, 1}}, 1);
    // Three tasks, cap=1.
    check({{0, 4, 1, 1}, {0, 4, 2, 1}, {0, 4, 1, 1}}, 1);
}

TEST_CASE("cumulative: capacity>1 (brute-force)",
          "[constraint][cumulative][brute]") {
    // Two tasks that can overlap under cap=2 but not exceed it.
    check({{0, 3, 2, 1}, {0, 3, 2, 1}}, 2);
    // Requirements force partial separation under cap=2.
    check({{0, 4, 2, 2}, {0, 4, 2, 1}}, 2);
    // Three tasks, cap=2, varied requirements (small-window own-CP geometry).
    check({{0, 3, 2, 1}, {0, 3, 2, 2}, {0, 3, 1, 1}}, 2);
    // cap=3, tasks of req 2 must pack carefully.
    check({{0, 4, 2, 2}, {0, 4, 2, 2}, {0, 4, 1, 2}}, 3);
}
