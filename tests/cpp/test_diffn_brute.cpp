// Brute-force solution-set equivalence tests for diffn (non-overlapping
// rectangles).
//
// Safety net for the Phase 4 DomainWriter unification (refactoring-plan-2026-06.md):
// propagate_pairwise (enqueue) and propagate_pairwise_direct (presolve direct
// removes) are merged into one templated kernel. A transcription error in the
// separation logic would surface as a solution-set mismatch against exhaustive
// enumeration here.
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

struct Rect {
    int xlo, xhi;  // x domain
    int ylo, yhi;  // y domain
    int dx, dy;    // fixed sizes (singleton)
};

// Reference: do these placed rectangles avoid pairwise overlap?
bool no_overlap(const std::vector<int>& xs, const std::vector<int>& ys,
                const std::vector<Rect>& rects, bool strict) {
    size_t n = rects.size();
    for (size_t i = 0; i < n; ++i) {
        if (!strict && (rects[i].dx == 0 || rects[i].dy == 0)) continue;
        for (size_t j = i + 1; j < n; ++j) {
            if (!strict && (rects[j].dx == 0 || rects[j].dy == 0)) continue;
            bool sep = (xs[i] + rects[i].dx <= xs[j]) ||
                       (xs[j] + rects[j].dx <= xs[i]) ||
                       (ys[i] + rects[i].dy <= ys[j]) ||
                       (ys[j] + rects[j].dy <= ys[i]);
            if (!sep) return false;
        }
    }
    return true;
}

void check(const std::vector<Rect>& rects, bool strict) {
    size_t n = rects.size();
    Model model;
    std::vector<Variable*> xv, yv, dxv, dyv;
    for (size_t i = 0; i < n; ++i) {
        xv.push_back(model.create_variable("x" + std::to_string(i), rects[i].xlo, rects[i].xhi));
        yv.push_back(model.create_variable("y" + std::to_string(i), rects[i].ylo, rects[i].yhi));
        dxv.push_back(model.create_variable("dx" + std::to_string(i), rects[i].dx, rects[i].dx));
        dyv.push_back(model.create_variable("dy" + std::to_string(i), rects[i].dy, rects[i].dy));
    }
    model.add_constraint(std::make_unique<DiffnConstraint>(xv, yv, dxv, dyv, strict));

    Solver solver;
    std::set<std::vector<int64_t>> got;
    solver.solve_all(model, [&](const Solution& sol) {
        std::vector<int64_t> row;
        for (size_t i = 0; i < n; ++i) row.push_back(sol.at("x" + std::to_string(i)));
        for (size_t i = 0; i < n; ++i) row.push_back(sol.at("y" + std::to_string(i)));
        got.insert(std::move(row));
        return true;
    });

    std::set<std::vector<int64_t>> want;
    std::vector<int> xs(n), ys(n);
    std::function<void(size_t)> rec = [&](size_t k) {
        if (k == 2 * n) {
            if (no_overlap(xs, ys, rects, strict)) {
                std::vector<int64_t> row;
                for (size_t i = 0; i < n; ++i) row.push_back(xs[i]);
                for (size_t i = 0; i < n; ++i) row.push_back(ys[i]);
                want.insert(std::move(row));
            }
            return;
        }
        if (k < n) {
            for (int v = rects[k].xlo; v <= rects[k].xhi; ++v) { xs[k] = v; rec(k + 1); }
        } else {
            size_t i = k - n;
            for (int v = rects[i].ylo; v <= rects[i].yhi; ++v) { ys[i] = v; rec(k + 1); }
        }
    };
    rec(0);

    REQUIRE(got == want);
}

}  // namespace

TEST_CASE("diffn: brute-force solution-set equivalence (strict)", "[constraint][diffn][brute]") {
    // Two unit squares in a small grid.
    check({{0, 2, 0, 2, 1, 1}, {0, 2, 0, 2, 1, 1}}, true);
    // Differing sizes.
    check({{0, 3, 0, 3, 2, 1}, {0, 3, 0, 3, 1, 2}}, true);
    // Forced separation: tall/wide rects with little room.
    check({{0, 1, 0, 1, 2, 2}, {0, 1, 0, 1, 1, 1}}, true);
    // Three rectangles.
    check({{0, 2, 0, 2, 1, 1}, {0, 2, 0, 2, 1, 1}, {0, 2, 0, 2, 1, 1}}, true);
}

TEST_CASE("diffn: brute-force solution-set equivalence (nonstrict)", "[constraint][diffn][brute]") {
    // Nonstrict with a zero-area rectangle (dx=0): exempt from separation.
    check({{0, 2, 0, 2, 0, 1}, {0, 2, 0, 2, 1, 1}}, false);
    check({{0, 2, 0, 2, 1, 0}, {0, 2, 0, 2, 1, 1}, {0, 2, 0, 2, 1, 1}}, false);
    // Nonstrict, all non-degenerate behaves like strict here.
    check({{0, 2, 0, 2, 1, 1}, {0, 2, 0, 2, 1, 1}}, false);
}
