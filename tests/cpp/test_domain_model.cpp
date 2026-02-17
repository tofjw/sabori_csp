#include <catch2/catch_test_macros.hpp>
#include "sabori_csp/domain.hpp"
#include "sabori_csp/variable.hpp"
#include "sabori_csp/model.hpp"

using namespace sabori_csp;

// ============================================================================
// Domain (Sparse Set) tests
// ============================================================================

TEST_CASE("Domain basic operations", "[domain]") {
    Domain d(1, 5);

    SECTION("initial state") {
        REQUIRE(d.size() == 5);
        REQUIRE(!d.empty());
        REQUIRE(d.min().value() == 1);
        REQUIRE(d.max().value() == 5);
    }

    SECTION("contains") {
        REQUIRE(d.contains(1));
        REQUIRE(d.contains(3));
        REQUIRE(d.contains(5));
        REQUIRE(!d.contains(0));
        REQUIRE(!d.contains(6));
    }

    SECTION("values") {
        auto vals = d.values();
        REQUIRE(vals.size() == 5);
        // Note: values may not be sorted after Sparse Set operations
    }
}

TEST_CASE("Domain remove", "[domain]") {
    Domain d(1, 5);

    SECTION("remove middle value") {
        REQUIRE(d.remove(3));
        REQUIRE(d.size() == 4);
        REQUIRE(!d.contains(3));
        REQUIRE(d.contains(1));
        REQUIRE(d.contains(5));
    }

    SECTION("remove min value") {
        REQUIRE(d.remove(1));
        REQUIRE(d.size() == 4);
        REQUIRE(!d.contains(1));
        REQUIRE(d.min().value() == 2);
    }

    SECTION("remove max value") {
        REQUIRE(d.remove(5));
        REQUIRE(d.size() == 4);
        REQUIRE(!d.contains(5));
        REQUIRE(d.max().value() == 4);
    }

    SECTION("remove non-existent value") {
        // 存在しない値の削除は成功（変更なし）として扱う
        REQUIRE(d.remove(10));
        REQUIRE(d.size() == 5);
    }

    SECTION("remove from singleton fails") {
        // singleton から削除しようとすると失敗
        Domain singleton(3, 3);
        REQUIRE(singleton.is_singleton());
        REQUIRE(!singleton.remove(3));  // 空になるので失敗
        REQUIRE(singleton.size() == 1);  // 削除されていない
        REQUIRE(singleton.contains(3));
    }
}

TEST_CASE("Domain assign", "[domain]") {
    Domain d(1, 5);

    SECTION("assign to existing value") {
        REQUIRE(d.assign(3));
        REQUIRE(d.size() == 1);
        REQUIRE(d.is_singleton());
        REQUIRE(d.min().value() == 3);
        REQUIRE(d.max().value() == 3);
        REQUIRE(d.contains(3));
        REQUIRE(!d.contains(1));
        REQUIRE(!d.contains(5));
    }

    SECTION("assign to non-existent value") {
        REQUIRE(!d.assign(10));
        REQUIRE(d.size() == 5);  // unchanged
    }
}

TEST_CASE("Domain from vector", "[domain]") {
    std::vector<int64_t> vals = {5, 2, 8, 2, 3};  // with duplicate
    Domain d(vals);

    REQUIRE(d.size() == 4);  // duplicates removed
    REQUIRE(d.min().value() == 2);
    REQUIRE(d.max().value() == 8);
    REQUIRE(d.contains(2));
    REQUIRE(d.contains(3));
    REQUIRE(d.contains(5));
    REQUIRE(d.contains(8));
    REQUIRE(!d.contains(4));
}

TEST_CASE("Domain Sparse Set internals", "[domain]") {
    Domain d(1, 3);

    SECTION("n and values_ref") {
        REQUIRE(d.n() == 3);
        auto& vals = d.values_ref();
        REQUIRE(vals.size() >= 3);
    }

    SECTION("set_n for backtracking") {
        d.remove(2);
        REQUIRE(d.n() == 2);

        // Simulate backtrack by restoring n
        d.set_n(3);
        REQUIRE(d.n() == 3);
        // Note: update_bounds needs to be called separately
    }
}

// ============================================================================
// Model Trail tests
// ============================================================================

TEST_CASE("Model basic operations", "[model]") {
    Model model;
    auto x = model.create_variable("x", 1, 5);
    auto y = model.create_variable("y", 1, 3);

    size_t x_idx = x->id();
    size_t y_idx = y->id();

    SECTION("variable indices") {
        REQUIRE(x_idx == 0);
        REQUIRE(y_idx == 1);
    }

    SECTION("variable data") {
        REQUIRE(model.var_min(x_idx) == 1);
        REQUIRE(model.var_max(x_idx) == 5);
        REQUIRE(model.var_size(x_idx) == 5);

        REQUIRE(model.var_min(y_idx) == 1);
        REQUIRE(model.var_max(y_idx) == 3);
        REQUIRE(model.var_size(y_idx) == 3);
    }

    SECTION("variable lookup") {
        REQUIRE(model.variable(x_idx) == x);
        REQUIRE(model.variable("y") == y);
    }
}

TEST_CASE("Model instantiate with trail", "[model][trail]") {
    Model model;
    auto x = model.create_variable("x", 1, 5);
    size_t x_idx = x->id();

    SECTION("instantiate") {
        REQUIRE(model.instantiate(1, x_idx, 3));
        REQUIRE(model.is_instantiated(x_idx));
        REQUIRE(model.value(x_idx) == 3);
        REQUIRE(model.var_size(x_idx) == 1);
        REQUIRE(model.var_trail_size() == 1);
    }

    SECTION("instantiate invalid value") {
        REQUIRE(!model.instantiate(1, x_idx, 10));
        REQUIRE(!model.is_instantiated(x_idx));
        REQUIRE(model.var_size(x_idx) == 5);
    }
}

TEST_CASE("Model rewind_to", "[model][trail]") {
    Model model;
    auto x = model.create_variable("x", 1, 5);
    auto y = model.create_variable("y", 1, 3);
    size_t x_idx = x->id();
    size_t y_idx = y->id();

    // Save initial state
    int level_0 = 0;
    int level_1 = 1;
    int level_2 = 2;

    // Level 1: instantiate x
    REQUIRE(model.instantiate(level_1, x_idx, 3));
    REQUIRE(model.is_instantiated(x_idx));
    REQUIRE(model.var_size(x_idx) == 1);

    // Level 2: instantiate y
    REQUIRE(model.instantiate(level_2, y_idx, 2));
    REQUIRE(model.is_instantiated(y_idx));
    REQUIRE(model.var_size(y_idx) == 1);

    SECTION("rewind to level 1") {
        model.rewind_to(level_1);

        // x should still be instantiated
        REQUIRE(model.is_instantiated(x_idx));
        REQUIRE(model.value(x_idx) == 3);

        // y should be restored
        REQUIRE(!model.is_instantiated(y_idx));
        REQUIRE(model.var_size(y_idx) == 3);
        REQUIRE(model.var_min(y_idx) == 1);
        REQUIRE(model.var_max(y_idx) == 3);
    }

    SECTION("rewind to level 0") {
        model.rewind_to(level_0);

        // Both should be restored
        REQUIRE(!model.is_instantiated(x_idx));
        REQUIRE(model.var_size(x_idx) == 5);
        REQUIRE(model.var_min(x_idx) == 1);
        REQUIRE(model.var_max(x_idx) == 5);

        REQUIRE(!model.is_instantiated(y_idx));
        REQUIRE(model.var_size(y_idx) == 3);
    }
}

TEST_CASE("Model set_min with trail", "[model][trail]") {
    Model model;
    auto x = model.create_variable("x", 1, 10);
    size_t x_idx = x->id();

    REQUIRE(model.set_min(1, x_idx, 5));
    REQUIRE(model.var_min(x_idx) == 5);
    REQUIRE(model.var_size(x_idx) == 10);  // sparse set は変更しない（lazy bounds）
    REQUIRE(!model.contains(x_idx, 4));
    REQUIRE(model.contains(x_idx, 5));

    // Rewind
    model.rewind_to(0);
    REQUIRE(model.var_min(x_idx) == 1);
    REQUIRE(model.var_size(x_idx) == 10);
    REQUIRE(model.contains(x_idx, 4));
}

TEST_CASE("Model set_max with trail", "[model][trail]") {
    Model model;
    auto x = model.create_variable("x", 1, 10);
    size_t x_idx = x->id();

    REQUIRE(model.set_max(1, x_idx, 5));
    REQUIRE(model.var_max(x_idx) == 5);
    REQUIRE(model.var_size(x_idx) == 10);  // sparse set は変更しない（lazy bounds）
    REQUIRE(!model.contains(x_idx, 6));
    REQUIRE(model.contains(x_idx, 5));

    // Rewind
    model.rewind_to(0);
    REQUIRE(model.var_max(x_idx) == 10);
    REQUIRE(model.var_size(x_idx) == 10);
    REQUIRE(model.contains(x_idx, 6));
}

TEST_CASE("Model remove_value with trail", "[model][trail]") {
    Model model;
    auto x = model.create_variable("x", 1, 5);
    size_t x_idx = x->id();

    REQUIRE(model.remove_value(1, x_idx, 3));
    REQUIRE(model.var_size(x_idx) == 4);
    REQUIRE(!model.contains(x_idx, 3));

    REQUIRE(model.remove_value(2, x_idx, 1));  // Remove min
    REQUIRE(model.var_size(x_idx) == 3);
    REQUIRE(model.var_min(x_idx) == 2);

    // Rewind to level 1
    model.rewind_to(1);
    REQUIRE(model.var_size(x_idx) == 4);
    REQUIRE(model.contains(x_idx, 1));
    REQUIRE(!model.contains(x_idx, 3));

    // Rewind to level 0
    model.rewind_to(0);
    REQUIRE(model.var_size(x_idx) == 5);
    REQUIRE(model.contains(x_idx, 3));
}

TEST_CASE("Model no duplicate trail save at same level", "[model][trail]") {
    Model model;
    auto x = model.create_variable("x", 1, 10);
    size_t x_idx = x->id();

    // Multiple operations at same level should only save once
    REQUIRE(model.remove_value(1, x_idx, 5));
    REQUIRE(model.var_trail_size() == 1);

    REQUIRE(model.remove_value(1, x_idx, 6));
    REQUIRE(model.var_trail_size() == 1);  // Still 1, not 2

    REQUIRE(model.remove_value(1, x_idx, 7));
    REQUIRE(model.var_trail_size() == 1);  // Still 1

    // Rewind should restore original state
    model.rewind_to(0);
    REQUIRE(model.var_size(x_idx) == 10);
    REQUIRE(model.contains(x_idx, 5));
    REQUIRE(model.contains(x_idx, 6));
    REQUIRE(model.contains(x_idx, 7));
}

// --- Regression tests for lazy bounds with holes ---
// Bug: set_min/set_max lazy path could set mins_/maxs_ to a value not in the
// domain. When bounds converged to that non-existent value, index_of() returned
// SIZE_MAX, causing swap_at(SIZE_MAX, 0) → heap-buffer-overflow.

TEST_CASE("Model set_min lazy path with hole in domain", "[model][trail]") {
    // Domain: {1, 3, 5, 7, 9} — has holes at 2, 4, 6, 8
    Model model;
    auto x = model.create_variable("x", std::vector<int64_t>{1, 3, 5, 7, 9});
    size_t x_idx = x->id();

    SECTION("set_min to hole value, then set_max converges to same hole") {
        // set_min(4) → lazy path sets mins_=4 (but 4 is not in domain)
        // set_max(4) → now mins_==maxs_==4, singleton path must not crash
        REQUIRE(model.set_min(1, x_idx, 4));
        // 4 is a hole: actual min should be 5 or lazy path defers
        REQUIRE(model.set_max(1, x_idx, 4) == false);
        // Domain should be empty (no value == 4 exists), so set_max returns false
    }

    SECTION("set_min to hole, set_max narrows further — no crash") {
        REQUIRE(model.set_min(1, x_idx, 4));  // lazy: mins_=4
        REQUIRE(model.set_max(1, x_idx, 5));  // lazy: maxs_=5
        // Bounds [4,5] — only value 5 exists, but lazy bounds don't detect singleton
        REQUIRE(model.contains(x_idx, 5));
    }

    SECTION("set_max to hole value, then set_min converges to same hole") {
        REQUIRE(model.set_max(1, x_idx, 6));  // lazy: maxs_=6
        // set_min(6) → mins_==maxs_==6, but 6 is a hole → must not crash
        REQUIRE(model.set_min(1, x_idx, 6) == false);
    }

    SECTION("set_min and set_max converge to existing value") {
        REQUIRE(model.set_min(1, x_idx, 5));
        REQUIRE(model.set_max(1, x_idx, 5));
        REQUIRE(model.is_instantiated(x_idx));
        REQUIRE(model.value(x_idx) == 5);
    }

    SECTION("set_min past support forces scan, then set_max converges") {
        // support is vals[2]=5. set_min(7) > 5 → scan path finds actual_min=7
        REQUIRE(model.set_min(1, x_idx, 7));
        REQUIRE(model.var_min(x_idx) == 7);
        // set_max(7) → converge to 7 which exists
        REQUIRE(model.set_max(1, x_idx, 7));
        REQUIRE(model.is_instantiated(x_idx));
        REQUIRE(model.value(x_idx) == 7);
    }

    SECTION("set_max below support forces scan, then set_min converges") {
        // support is 5. set_max(3) < 5 → scan path finds actual_max=3
        REQUIRE(model.set_max(1, x_idx, 3));
        REQUIRE(model.var_max(x_idx) == 3);
        // set_min(3) → converge to 3 which exists
        REQUIRE(model.set_min(1, x_idx, 3));
        REQUIRE(model.is_instantiated(x_idx));
        REQUIRE(model.value(x_idx) == 3);
    }
}

TEST_CASE("Model set_min/set_max with remove then lazy convergence", "[model][trail]") {
    // Start with contiguous domain, create hole by remove_value, then converge
    Model model;
    auto x = model.create_variable("x", 1, 5);
    size_t x_idx = x->id();

    // Remove middle value to create a hole
    REQUIRE(model.remove_value(1, x_idx, 3));

    SECTION("set_min and set_max converge to removed value") {
        // mins_=3 (lazy, but 3 was removed), maxs_=3 → must not crash
        REQUIRE(model.set_min(1, x_idx, 3));
        REQUIRE(model.set_max(1, x_idx, 3) == false);
    }

    SECTION("bounds tighten past hole correctly") {
        // set_min(3) lazy, then set_max(4) → domain should be {4}
        REQUIRE(model.set_min(1, x_idx, 3));
        REQUIRE(model.set_max(1, x_idx, 4));
        REQUIRE(model.is_instantiated(x_idx));
        REQUIRE(model.value(x_idx) == 4);
    }

    SECTION("rewind restores domain after convergence past hole") {
        REQUIRE(model.set_min(2, x_idx, 3));
        REQUIRE(model.set_max(2, x_idx, 4));
        REQUIRE(model.is_instantiated(x_idx));
        REQUIRE(model.value(x_idx) == 4);

        model.rewind_to(1);
        // After rewind: domain should be {1, 2, 4, 5} (3 still removed at level 1)
        REQUIRE(model.var_size(x_idx) == 4);
        REQUIRE(!model.contains(x_idx, 3));
        REQUIRE(model.contains(x_idx, 4));

        model.rewind_to(0);
        // Full restore: domain should be {1, 2, 3, 4, 5}
        REQUIRE(model.var_size(x_idx) == 5);
        REQUIRE(model.contains(x_idx, 3));
    }
}

// ============================================================================
// Domain bounds-only tests
// ============================================================================

TEST_CASE("Domain bounds-only basic operations", "[domain][bounds-only]") {
    Domain d(0, 100000);  // range > 10000 → bounds-only

    SECTION("initial state") {
        REQUIRE(d.is_bounds_only());
        REQUIRE(d.size() == 100001);
        REQUIRE(d.min().value() == 0);
        REQUIRE(d.max().value() == 100000);
    }

    SECTION("contains") {
        REQUIRE(d.contains(0));
        REQUIRE(d.contains(50000));
        REQUIRE(d.contains(100000));
        REQUIRE(!d.contains(-1));
        REQUIRE(!d.contains(100001));
    }

    SECTION("remove middle value") {
        REQUIRE(d.remove(50000));
        REQUIRE(d.size() == 100000);
        REQUIRE(!d.contains(50000));
        REQUIRE(d.contains(49999));
        REQUIRE(d.contains(50001));
    }

    SECTION("remove min value") {
        REQUIRE(d.remove(0));
        REQUIRE(d.min().value() == 1);
        REQUIRE(!d.contains(0));
    }

    SECTION("remove max value") {
        REQUIRE(d.remove(100000));
        REQUIRE(d.max().value() == 99999);
        REQUIRE(!d.contains(100000));
    }

    SECTION("assign") {
        REQUIRE(d.assign(42));
        REQUIRE(d.is_singleton());
        REQUIRE(d.min().value() == 42);
        REQUIRE(d.max().value() == 42);
        REQUIRE(!d.contains(41));
    }

    SECTION("remove_below") {
        REQUIRE(d.remove_below(1000));
        REQUIRE(d.min().value() == 1000);
        REQUIRE(!d.contains(999));
    }

    SECTION("remove_above") {
        REQUIRE(d.remove_above(500));
        REQUIRE(d.max().value() == 500);
        REQUIRE(!d.contains(501));
    }

    SECTION("values returns correct list") {
        Domain small_bounds(0, 15000);
        small_bounds.remove(100);
        small_bounds.remove(200);
        auto vals = small_bounds.values();
        REQUIRE(vals.size() == 14999);
        REQUIRE(std::find(vals.begin(), vals.end(), 100) == vals.end());
        REQUIRE(std::find(vals.begin(), vals.end(), 200) == vals.end());
    }
}

TEST_CASE("Domain small range is NOT bounds-only", "[domain]") {
    Domain d(1, 9999);
    REQUIRE(!d.is_bounds_only());
    REQUIRE(d.size() == 9999);
}

TEST_CASE("Domain bounds-only initial_range", "[domain][bounds-only]") {
    Domain d(100, 200000);
    REQUIRE(d.is_bounds_only());
    REQUIRE(d.initial_range() == 199901);
}

// ============================================================================
// Model bounds-only trail tests
// ============================================================================

TEST_CASE("Model bounds-only with trail", "[model][trail][bounds-only]") {
    Model model;
    auto x = model.create_variable("x", 0, 100000);
    size_t x_idx = x->id();

    SECTION("instantiate and rewind") {
        REQUIRE(model.instantiate(1, x_idx, 42));
        REQUIRE(model.is_instantiated(x_idx));
        REQUIRE(model.value(x_idx) == 42);

        model.rewind_to(0);
        REQUIRE(!model.is_instantiated(x_idx));
        REQUIRE(model.var_min(x_idx) == 0);
        REQUIRE(model.var_max(x_idx) == 100000);
    }

    SECTION("set_min and rewind") {
        REQUIRE(model.set_min(1, x_idx, 50000));
        REQUIRE(model.var_min(x_idx) == 50000);

        model.rewind_to(0);
        REQUIRE(model.var_min(x_idx) == 0);
    }

    SECTION("set_max and rewind") {
        REQUIRE(model.set_max(1, x_idx, 50000));
        REQUIRE(model.var_max(x_idx) == 50000);

        model.rewind_to(0);
        REQUIRE(model.var_max(x_idx) == 100000);
    }

    SECTION("remove_value interior and rewind") {
        REQUIRE(model.remove_value(1, x_idx, 500));
        REQUIRE(!model.contains(x_idx, 500));

        model.rewind_to(0);
        REQUIRE(model.contains(x_idx, 500));
    }

    SECTION("remove_value boundary and rewind") {
        REQUIRE(model.remove_value(1, x_idx, 0));
        REQUIRE(model.var_min(x_idx) == 1);

        model.rewind_to(0);
        REQUIRE(model.var_min(x_idx) == 0);
        REQUIRE(model.contains(x_idx, 0));
    }

    SECTION("multiple removes across levels then rewind") {
        REQUIRE(model.remove_value(1, x_idx, 100));
        REQUIRE(model.remove_value(1, x_idx, 200));
        REQUIRE(model.remove_value(2, x_idx, 300));

        model.rewind_to(1);
        REQUIRE(!model.contains(x_idx, 100));
        REQUIRE(!model.contains(x_idx, 200));
        REQUIRE(model.contains(x_idx, 300));  // level 2 の remove が復元

        model.rewind_to(0);
        REQUIRE(model.contains(x_idx, 100));
        REQUIRE(model.contains(x_idx, 200));
        REQUIRE(model.contains(x_idx, 300));
    }
}
