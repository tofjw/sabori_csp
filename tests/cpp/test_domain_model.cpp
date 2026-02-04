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
    auto x = std::make_shared<Variable>("x", Domain(1, 5));
    auto y = std::make_shared<Variable>("y", Domain(1, 3));

    size_t x_idx = model.add_variable(x);
    size_t y_idx = model.add_variable(y);

    SECTION("variable indices") {
        REQUIRE(x_idx == 0);
        REQUIRE(y_idx == 1);
    }

    SECTION("SoA data") {
        REQUIRE(model.mins()[x_idx] == 1);
        REQUIRE(model.maxs()[x_idx] == 5);
        REQUIRE(model.sizes()[x_idx] == 5);

        REQUIRE(model.mins()[y_idx] == 1);
        REQUIRE(model.maxs()[y_idx] == 3);
        REQUIRE(model.sizes()[y_idx] == 3);
    }

    SECTION("variable lookup") {
        REQUIRE(model.variable(x_idx) == x);
        REQUIRE(model.variable("y") == y);
    }
}

TEST_CASE("Model instantiate with trail", "[model][trail]") {
    Model model;
    auto x = std::make_shared<Variable>("x", Domain(1, 5));
    size_t x_idx = model.add_variable(x);

    SECTION("instantiate") {
        REQUIRE(model.instantiate(1, x_idx, 3));
        REQUIRE(model.is_instantiated(x_idx));
        REQUIRE(model.value(x_idx) == 3);
        REQUIRE(model.sizes()[x_idx] == 1);
        REQUIRE(model.var_trail_size() == 1);
    }

    SECTION("instantiate invalid value") {
        REQUIRE(!model.instantiate(1, x_idx, 10));
        REQUIRE(!model.is_instantiated(x_idx));
        REQUIRE(model.sizes()[x_idx] == 5);
    }
}

TEST_CASE("Model rewind_to", "[model][trail]") {
    Model model;
    auto x = std::make_shared<Variable>("x", Domain(1, 5));
    auto y = std::make_shared<Variable>("y", Domain(1, 3));
    size_t x_idx = model.add_variable(x);
    size_t y_idx = model.add_variable(y);

    // Save initial state
    int level_0 = 0;
    int level_1 = 1;
    int level_2 = 2;

    // Level 1: instantiate x
    REQUIRE(model.instantiate(level_1, x_idx, 3));
    REQUIRE(model.is_instantiated(x_idx));
    REQUIRE(model.sizes()[x_idx] == 1);

    // Level 2: instantiate y
    REQUIRE(model.instantiate(level_2, y_idx, 2));
    REQUIRE(model.is_instantiated(y_idx));
    REQUIRE(model.sizes()[y_idx] == 1);

    SECTION("rewind to level 1") {
        model.rewind_to(level_1);

        // x should still be instantiated
        REQUIRE(model.is_instantiated(x_idx));
        REQUIRE(model.value(x_idx) == 3);

        // y should be restored
        REQUIRE(!model.is_instantiated(y_idx));
        REQUIRE(model.sizes()[y_idx] == 3);
        REQUIRE(model.mins()[y_idx] == 1);
        REQUIRE(model.maxs()[y_idx] == 3);
    }

    SECTION("rewind to level 0") {
        model.rewind_to(level_0);

        // Both should be restored
        REQUIRE(!model.is_instantiated(x_idx));
        REQUIRE(model.sizes()[x_idx] == 5);
        REQUIRE(model.mins()[x_idx] == 1);
        REQUIRE(model.maxs()[x_idx] == 5);

        REQUIRE(!model.is_instantiated(y_idx));
        REQUIRE(model.sizes()[y_idx] == 3);
    }
}

TEST_CASE("Model set_min with trail", "[model][trail]") {
    Model model;
    auto x = std::make_shared<Variable>("x", Domain(1, 10));
    size_t x_idx = model.add_variable(x);

    REQUIRE(model.set_min(1, x_idx, 5));
    REQUIRE(model.mins()[x_idx] == 5);
    REQUIRE(model.sizes()[x_idx] == 6);  // 5,6,7,8,9,10
    REQUIRE(!model.contains(x_idx, 4));
    REQUIRE(model.contains(x_idx, 5));

    // Rewind
    model.rewind_to(0);
    REQUIRE(model.mins()[x_idx] == 1);
    REQUIRE(model.sizes()[x_idx] == 10);
    REQUIRE(model.contains(x_idx, 4));
}

TEST_CASE("Model set_max with trail", "[model][trail]") {
    Model model;
    auto x = std::make_shared<Variable>("x", Domain(1, 10));
    size_t x_idx = model.add_variable(x);

    REQUIRE(model.set_max(1, x_idx, 5));
    REQUIRE(model.maxs()[x_idx] == 5);
    REQUIRE(model.sizes()[x_idx] == 5);  // 1,2,3,4,5
    REQUIRE(!model.contains(x_idx, 6));
    REQUIRE(model.contains(x_idx, 5));

    // Rewind
    model.rewind_to(0);
    REQUIRE(model.maxs()[x_idx] == 10);
    REQUIRE(model.sizes()[x_idx] == 10);
    REQUIRE(model.contains(x_idx, 6));
}

TEST_CASE("Model remove_value with trail", "[model][trail]") {
    Model model;
    auto x = std::make_shared<Variable>("x", Domain(1, 5));
    size_t x_idx = model.add_variable(x);

    REQUIRE(model.remove_value(1, x_idx, 3));
    REQUIRE(model.sizes()[x_idx] == 4);
    REQUIRE(!model.contains(x_idx, 3));

    REQUIRE(model.remove_value(2, x_idx, 1));  // Remove min
    REQUIRE(model.sizes()[x_idx] == 3);
    REQUIRE(model.mins()[x_idx] == 2);

    // Rewind to level 1
    model.rewind_to(1);
    REQUIRE(model.sizes()[x_idx] == 4);
    REQUIRE(model.contains(x_idx, 1));
    REQUIRE(!model.contains(x_idx, 3));

    // Rewind to level 0
    model.rewind_to(0);
    REQUIRE(model.sizes()[x_idx] == 5);
    REQUIRE(model.contains(x_idx, 3));
}

TEST_CASE("Model no duplicate trail save at same level", "[model][trail]") {
    Model model;
    auto x = std::make_shared<Variable>("x", Domain(1, 10));
    size_t x_idx = model.add_variable(x);

    // Multiple operations at same level should only save once
    REQUIRE(model.remove_value(1, x_idx, 5));
    REQUIRE(model.var_trail_size() == 1);

    REQUIRE(model.remove_value(1, x_idx, 6));
    REQUIRE(model.var_trail_size() == 1);  // Still 1, not 2

    REQUIRE(model.remove_value(1, x_idx, 7));
    REQUIRE(model.var_trail_size() == 1);  // Still 1

    // Rewind should restore original state
    model.rewind_to(0);
    REQUIRE(model.sizes()[x_idx] == 10);
    REQUIRE(model.contains(x_idx, 5));
    REQUIRE(model.contains(x_idx, 6));
    REQUIRE(model.contains(x_idx, 7));
}
