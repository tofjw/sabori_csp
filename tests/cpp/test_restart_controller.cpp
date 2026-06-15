#include <catch2/catch_test_macros.hpp>
#include "sabori_csp/restart_controller.hpp"

using namespace sabori_csp;

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
    rc.end_cycle(/*prune_delta=*/0, /*depth_grew=*/false);
    REQUIRE(rc.outer() > before);          // grow: 4*1.2 = 4.8
    REQUIRE(rc.outer() == 4.8);
}

TEST_CASE("RestartController end_cycle shrinks outer on prune+depth", "[restart]") {
    RestartController rc;
    rc.reset();
    // outer を一旦大きくしてから shrink を観測
    for (int i = 0; i < 20; ++i) rc.end_cycle(0, false);
    double grown = rc.outer();
    rc.end_cycle(/*prune_delta=*/5, /*depth_grew=*/true);
    REQUIRE(rc.outer() < grown);           // shrink: *0.99
}

TEST_CASE("RestartController outer growth is capped, shrink floored", "[restart]") {
    RestartController rc;
    rc.reset();
    for (int i = 0; i < 100000; ++i) rc.end_cycle(0, false);
    REQUIRE(rc.outer() <= 10000.0);        // outer_max
    for (int i = 0; i < 100000; ++i) rc.end_cycle(1, true);
    REQUIRE(rc.outer() >= 3.0);            // outer_min
}

TEST_CASE("RestartController reset_outer restores ceiling", "[restart]") {
    RestartController rc;
    rc.reset();
    for (int i = 0; i < 20; ++i) rc.end_cycle(0, false);
    REQUIRE(rc.outer() != 4.0);
    rc.reset_outer();
    REQUIRE(rc.outer() == 4.0);
}
