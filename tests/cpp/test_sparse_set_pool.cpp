#include <catch2/catch_test_macros.hpp>
#include "sabori_csp/sparse_set_pool.hpp"

#include <algorithm>
#include <random>
#include <set>
#include <vector>

using namespace sabori_csp;

namespace {

// 参照モデル: アクティブ集合を std::set で素朴に追跡する
struct RefModel {
    std::set<SparseSetPool::value_type> active;
    std::set<SparseSetPool::value_type> known;  // 登録された全値
};

// pool の状態が参照モデルと一致するか検証する
void check_consistency(const SparseSetPool& pool, const RefModel& ref) {
    REQUIRE(pool.size() == ref.active.size());
    // contains / consumed が全登録値で一致
    for (auto v : ref.known) {
        bool active = ref.active.count(v) != 0;
        REQUIRE(pool.contains(v) == active);
        REQUIRE(pool.consumed(v) == !active);
    }
    // value_at(0..size-1) はアクティブ集合と集合として一致
    std::set<SparseSetPool::value_type> from_pool;
    for (size_t i = 0; i < pool.size(); ++i) {
        from_pool.insert(pool.value_at(i));
    }
    REQUIRE(from_pool == ref.active);
}

}  // namespace

TEST_CASE("SparseSetPool basic operations", "[sparse_set_pool]") {
    SparseSetPool pool;
    pool.assign({3, 1, 4, 1, 5});  // 注: 呼び出し側は通常 unique を渡すが、ここでは挙動確認用

    SECTION("contains / remove / size") {
        SparseSetPool p2;
        p2.assign({10, 20, 30});
        REQUIRE(p2.size() == 3);
        REQUIRE(p2.contains(20));
        REQUIRE_FALSE(p2.contains(99));

        REQUIRE(p2.remove(20));
        REQUIRE_FALSE(p2.contains(20));
        REQUIRE(p2.consumed(20));
        REQUIRE(p2.size() == 2);

        // 二重削除は false
        REQUIRE_FALSE(p2.remove(20));
        // 未知の値の削除は false
        REQUIRE_FALSE(p2.remove(99));
    }

    SECTION("remove last active element") {
        SparseSetPool p2;
        p2.assign({1, 2});
        REQUIRE(p2.remove(p2.value_at(1)));  // 末尾を削除
        REQUIRE(p2.size() == 1);
        REQUIRE(p2.contains(p2.value_at(0)));
    }

    SECTION("restore_active_count is reversible") {
        SparseSetPool p2;
        p2.assign({1, 2, 3, 4, 5});
        size_t snap = p2.active_count();
        p2.remove(2);
        p2.remove(4);
        REQUIRE(p2.size() == 3);
        p2.restore_active_count(snap);
        REQUIRE(p2.size() == 5);
        for (int v : {1, 2, 3, 4, 5}) REQUIRE(p2.contains(v));
    }
}

TEST_CASE("SparseSetPool empty pool", "[sparse_set_pool]") {
    SparseSetPool pool;
    pool.assign({});
    REQUIRE(pool.empty());
    REQUIRE(pool.size() == 0);
    REQUIRE_FALSE(pool.contains(0));
    REQUIRE_FALSE(pool.remove(0));
}

TEST_CASE("SparseSetPool flat vs map representations", "[sparse_set_pool]") {
    // 密な値域 → フラット配列パス、疎で広い値域 → map フォールバックパス。
    // どちらも同一セマンティクスを保つことをランダム照合で確認する。
    std::mt19937 rng(20260615);

    auto run_trial = [&](std::vector<SparseSetPool::value_type> values) {
        std::sort(values.begin(), values.end());
        values.erase(std::unique(values.begin(), values.end()), values.end());

        SparseSetPool pool;
        pool.assign(values);
        RefModel ref;
        ref.active.insert(values.begin(), values.end());
        ref.known = ref.active;

        check_consistency(pool, ref);

        // ランダムに remove と restore を繰り返す
        std::vector<size_t> snaps;
        std::vector<SparseSetPool::value_type> active_vec(values.begin(), values.end());
        std::uniform_int_distribution<int> op_dist(0, 9);

        for (int step = 0; step < 200; ++step) {
            int op = op_dist(rng);
            if (op < 6 && !ref.active.empty()) {
                // remove a currently-active value
                std::vector<SparseSetPool::value_type> act(ref.active.begin(), ref.active.end());
                auto v = act[rng() % act.size()];
                REQUIRE(pool.remove(v));
                ref.active.erase(v);
            } else if (op < 7) {
                // snapshot
                snaps.push_back(pool.active_count());
            } else if (op < 9 && !snaps.empty()) {
                // restore to a snapshot (only restore to a >= count snapshot is valid;
                // snapshots are monotonically decreasing active counts so any earlier snap is >=)
                size_t target = snaps.back();
                snaps.pop_back();
                if (target >= pool.active_count()) {
                    pool.restore_active_count(target);
                    // rebuild ref.active = first `target` known values minus nothing:
                    // restore brings back exactly the elements removed since snapshot.
                    // Easiest: reconstruct from pool.
                    ref.active.clear();
                    for (size_t i = 0; i < pool.size(); ++i) ref.active.insert(pool.value_at(i));
                }
            }
            check_consistency(pool, ref);
        }
    };

    SECTION("dense range (flat path)") {
        std::vector<SparseSetPool::value_type> v;
        for (int i = -10; i <= 30; ++i) v.push_back(i);
        run_trial(v);
    }

    SECTION("sparse wide range (map fallback path)") {
        std::vector<SparseSetPool::value_type> v;
        std::uniform_int_distribution<int> d(-1000000, 1000000);
        for (int i = 0; i < 40; ++i) v.push_back(d(rng));
        run_trial(v);
    }
}
