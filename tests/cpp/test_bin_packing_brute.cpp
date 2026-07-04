// Brute-force solution-set equivalence tests for bin_packing_load.
//
// bin_packing_load(load, bin, w): item i (weight w[i]) goes into bin[i] (1-indexed),
// and load[b] = sum of weights of items placed in bin b. We verify the propagator is
// both sound and complete by comparing its full solution set against exhaustive
// enumeration, including cases that exercise the load-bounds channeling and the
// include/exclude reverse propagation, plus out-of-range bin clipping.
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

// Reference check: given a bin assignment (bin value v maps to load index v-offset),
// do the resulting loads sit inside every load domain?
bool loads_ok(const std::vector<int>& bins, const std::vector<int>& w,
              const std::vector<std::pair<int, int>>& load_dom, int offset) {
    std::vector<int> load(load_dom.size(), 0);
    for (size_t i = 0; i < bins.size(); ++i) load[bins[i] - offset] += w[i];
    for (size_t b = 0; b < load_dom.size(); ++b)
        if (load[b] < load_dom[b].first || load[b] > load_dom[b].second) return false;
    return true;
}

// bin_dom[i] = (lo, hi) raw domain of item i's bin var (may extend outside the valid
// range). Valid bin values are [offset, offset + B - 1]; offset defaults to 0 (0-based,
// the C++/Python convention), pass 1 to exercise the FlatZinc 1-indexed path.
void check(const std::vector<std::pair<int, int>>& bin_dom,
           const std::vector<int>& w,
           const std::vector<std::pair<int, int>>& load_dom,
           int offset = 0) {
    const size_t M = w.size();
    const int B = static_cast<int>(load_dom.size());
    REQUIRE(bin_dom.size() == M);

    Model model;
    std::vector<Variable*> loads, bins;
    for (int b = 0; b < B; ++b)
        loads.push_back(model.create_variable("L" + std::to_string(b),
                                              load_dom[b].first, load_dom[b].second));
    for (size_t i = 0; i < M; ++i)
        bins.push_back(model.create_variable("b" + std::to_string(i),
                                             bin_dom[i].first, bin_dom[i].second));
    std::vector<int64_t> weights(w.begin(), w.end());
    model.add_constraint(std::make_unique<BinPackingLoadConstraint>(
        loads, bins, weights, offset));

    Solver solver;
    std::set<std::vector<int64_t>> got;
    solver.solve_all(model, [&](const Solution& sol) {
        std::vector<int64_t> row;
        for (size_t i = 0; i < M; ++i) row.push_back(sol.at("b" + std::to_string(i)));
        got.insert(std::move(row));
        return true;
    });

    // Brute-force: enumerate bin values clipped to the valid [offset, offset+B-1] range.
    std::set<std::vector<int64_t>> want;
    std::vector<int> bins_ref(M);
    std::function<void(size_t)> rec = [&](size_t k) {
        if (k == M) {
            if (loads_ok(bins_ref, w, load_dom, offset)) {
                std::vector<int64_t> row(bins_ref.begin(), bins_ref.end());
                want.insert(std::move(row));
            }
            return;
        }
        int lo = std::max(offset, bin_dom[k].first);
        int hi = std::min(offset + B - 1, bin_dom[k].second);
        for (int v = lo; v <= hi; ++v) { bins_ref[k] = v; rec(k + 1); }
    };
    rec(0);

    REQUIRE(got == want);
}

}  // namespace

TEST_CASE("bin_packing_load: name", "[constraint][bin_packing]") {
    Model model;
    std::vector<Variable*> loads{model.create_variable("L0", 0, 5)};
    std::vector<Variable*> bins{model.create_variable("b0", 0, 0)};
    std::vector<int64_t> w{3};
    BinPackingLoadConstraint c(loads, bins, w);
    REQUIRE(c.name() == "sabori_bin_packing_load");
}

TEST_CASE("bin_packing_load: basic equivalence, 0-based (brute-force)",
          "[constraint][bin_packing][brute]") {
    // 4 items, weights [2,3,2,5], 3 bins with generous load capacity.
    check({{0, 2}, {0, 2}, {0, 2}, {0, 2}}, {2, 3, 2, 5}, {{0, 10}, {0, 10}, {0, 10}});
    // Two items, two bins, unit weights.
    check({{0, 1}, {0, 1}}, {1, 1}, {{0, 2}, {0, 2}});
}

TEST_CASE("bin_packing_load: load bounds force placement, 0-based (brute-force)",
          "[constraint][bin_packing][brute]") {
    // Tight per-bin capacities constrain which items can co-locate.
    check({{0, 1}, {0, 1}, {0, 1}}, {2, 2, 3}, {{0, 4}, {0, 4}});
    // A high minimum load on one bin forces items into it.
    check({{0, 2}, {0, 2}, {0, 2}, {0, 2}}, {2, 3, 2, 5}, {{5, 10}, {0, 10}, {0, 10}});
    // Exact load target on bin 0.
    check({{0, 1}, {0, 1}, {0, 1}}, {1, 2, 3}, {{3, 3}, {0, 6}});
}

TEST_CASE("bin_packing_load: out-of-range bin clipping, 0-based (brute-force)",
          "[constraint][bin_packing][brute]") {
    // Bin domains extend beyond [0, B-1]; constraint must clip to valid bins.
    check({{-1, 3}, {0, 2}, {-2, 1}}, {2, 3, 1}, {{0, 6}, {0, 6}});
}

TEST_CASE("bin_packing_load: zero and larger weights, 0-based (brute-force)",
          "[constraint][bin_packing][brute]") {
    // Includes a zero-weight item (placement is free) and a heavy item.
    check({{0, 1}, {0, 1}, {0, 1}}, {0, 4, 3}, {{0, 5}, {0, 5}});
    // Single bin: all items must land there, load is the total weight.
    check({{0, 0}, {0, 0}, {0, 0}}, {2, 3, 4}, {{9, 9}});
}

TEST_CASE("bin_packing_load: FlatZinc 1-indexed offset (brute-force)",
          "[constraint][bin_packing][brute]") {
    // Same as the basic case but with bin values in [1, B] (offset = 1).
    check({{1, 3}, {1, 3}, {1, 3}, {1, 3}}, {2, 3, 2, 5},
          {{0, 10}, {0, 10}, {0, 10}}, /*offset=*/1);
    // Forcing + clipping under offset = 1.
    check({{0, 4}, {1, 2}, {2, 3}}, {2, 3, 5}, {{5, 10}, {0, 10}, {0, 10}}, /*offset=*/1);
}
