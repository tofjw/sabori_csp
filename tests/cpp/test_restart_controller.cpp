#include <catch2/catch_test_macros.hpp>
#include "sabori_csp/restart_controller.hpp"

#include <random>

using namespace sabori_csp;

namespace {
std::mt19937& test_rng() {
    static std::mt19937 rng(42);
    return rng;
}
} // namespace

TEST_CASE("RestartController reset initializes inner/outer", "[restart]") {
    RestartController rc;
    rc.reset();
    // begin_cycle 後の conflict_limit は initial（2）
    rc.begin_cycle();
    REQUIRE(rc.conflict_limit() == 2);
    REQUIRE(rc.inner_within_outer());  // inner=2 <= outer=4
    REQUIRE(rc.outer() == 4.0);
}

TEST_CASE("RestartController advance_inner grows inner geometrically", "[restart]") {
    RestartController rc;
    rc.reset();
    rc.begin_cycle();
    // inner=2 から 1.01 倍ずつ。outer=4 を超えるまで continue できる
    int iters = 0;
    while (rc.inner_within_outer() && iters < 1000) {
        rc.advance_inner();
        iters++;
    }
    // 2 * 1.01^k > 4 となる最小 k は ln2/ln1.01 ≈ 69.66 → 70 回で抜ける
    REQUIRE(iters == 70);
}

TEST_CASE("RestartController begin_cycle resets inner but keeps outer", "[restart]") {
    RestartController rc;
    rc.reset();
    rc.begin_cycle();
    for (int i = 0; i < 10; ++i) rc.advance_inner();
    double outer_before = rc.outer();
    rc.begin_cycle();
    REQUIRE(rc.conflict_limit() == 2);     // inner はリセット
    REQUIRE(rc.outer() == outer_before);   // outer は不変
}

TEST_CASE("RestartController end_cycle grows outer when no prune/depth", "[restart]") {
    RestartController rc;
    rc.reset();
    double before = rc.outer();            // 4.0
    bool tightened = rc.end_cycle(/*prune_delta=*/0, /*depth_grew=*/false, test_rng());
    REQUIRE_FALSE(tightened);
    REQUIRE(rc.outer() > before);          // grow: 4*1.2 = 4.8
    REQUIRE(rc.outer() == 4.8);
}

TEST_CASE("RestartController end_cycle shrinks outer on prune+depth", "[restart]") {
    RestartController rc;
    rc.reset();
    // outer を一旦大きくしてから shrink を観測
    for (int i = 0; i < 20; ++i) rc.end_cycle(0, false, test_rng());
    double grown = rc.outer();
    bool tightened = rc.end_cycle(/*prune_delta=*/5, /*depth_grew=*/true, test_rng());
    REQUIRE(tightened);
    REQUIRE(rc.outer() < grown);           // shrink: *0.99
}

TEST_CASE("RestartController outer growth is capped, shrink floored", "[restart]") {
    RestartController rc;
    rc.reset();
    for (int i = 0; i < 100000; ++i) rc.end_cycle(0, false, test_rng());
    REQUIRE(rc.outer() <= 10000.0);        // outer_max
    for (int i = 0; i < 100000; ++i) rc.end_cycle(1, true, test_rng());
    REQUIRE(rc.outer() >= 3.0);            // outer_min
}

TEST_CASE("RestartController reset_outer restores ceiling", "[restart]") {
    RestartController rc;
    rc.reset();
    for (int i = 0; i < 20; ++i) rc.end_cycle(0, false, test_rng());
    REQUIRE(rc.outer() != 4.0);
    rc.reset_outer();
    REQUIRE(rc.outer() == 4.0);
}

TEST_CASE("RestartController scrambled ignores signal and follows p", "[restart]") {
    RestartController rc;
    rc.set_policy(RestartController::Policy::Scrambled, 0.3);
    rc.reset();
    // 信号は常に成立させるが、決定は p=0.3 のコイントスに従う
    for (int i = 0; i < 2000; ++i) rc.end_cycle(5, true, test_rng());
    double rate = static_cast<double>(rc.tighten_count()) /
                  static_cast<double>(rc.tighten_count() + rc.widen_count());
    REQUIRE(rate > 0.25);
    REQUIRE(rate < 0.35);
}

TEST_CASE("RestartController luby policy generates base-scaled Luby sequence", "[restart]") {
    RestartController rc;
    rc.set_policy(RestartController::Policy::Luby, 100.0);
    rc.reset();
    // Luby 列: 1,1,2,1,1,2,4,...
    const int expected[] = {100, 100, 200, 100, 100, 200, 400, 100, 100, 200,
                            100, 100, 200, 400, 800};
    for (int e : expected) {
        REQUIRE(rc.conflict_limit() == e);
        REQUIRE(rc.inner_within_outer());  // cycle 機構なし: 常に継続
        rc.advance_inner();
    }
    // begin_cycle / end_cycle / reset_outer は列に影響しない
    int cur = rc.conflict_limit();
    rc.begin_cycle();
    rc.end_cycle(5, true, test_rng());
    rc.reset_outer();
    REQUIRE(rc.conflict_limit() == cur);
}
