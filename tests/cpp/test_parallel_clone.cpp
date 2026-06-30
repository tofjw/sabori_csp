#include <catch2/catch_test_macros.hpp>
#include "sabori_csp/constraint.hpp"
#include "sabori_csp/constraints/global/alldifferent.hpp"
#include "sabori_csp/constraints/global/linear.hpp"
#include "sabori_csp/constraints/comparison.hpp"
#include "sabori_csp/variable.hpp"
#include "sabori_csp/model.hpp"
#include "sabori_csp/solver.hpp"
#include "sabori_csp/parallel_solver.hpp"
#include <memory>
#include <vector>
#include <set>

using namespace sabori_csp;

// ============================================================================
// Phase 1 ゲート: Model::clone() + solve_prepared が、presolve を 1 度だけ実行し
// clone した後でも、原本の solve()/solve_optimize() と同一結果・同一統計を再現する
// ことを検証する。マルチスレッド・ポートフォリオの土台の正当性を保証する。
//
// presolve は rng_ を消費しないため、presolve をスキップした worker と原本の
// solve() は RNG 消費列が一致し、探索軌道まで同一になるはず。
// ============================================================================

namespace {

// 比較に使う統計フィールド（決定論的な探索量を表す）。
bool same_stats(const SolverStats& a, const SolverStats& b) {
    return a.max_depth == b.max_depth
        && a.restart_count == b.restart_count
        && a.fail_count == b.fail_count
        && a.nogood_count == b.nogood_count
        && a.nogood_prune_count == b.nogood_prune_count
        && a.nogoods_size == b.nogoods_size
        && a.bisect_count == b.bisect_count
        && a.enumerate_count == b.enumerate_count;
}

// 3x3 Magic Square モデルを構築する（8 解、AllDifferent + 行/列/対角の線形和）。
std::unique_ptr<Model> build_magic_square() {
    auto model = std::make_unique<Model>();
    std::vector<Variable*> c;
    for (int i = 0; i < 9; ++i) {
        c.push_back(model->create_variable("x" + std::to_string(i), 1, 9));
    }
    model->add_constraint(std::make_unique<AllDifferentConstraint>(c));
    auto lineq = [&](std::vector<Variable*> vs) {
        model->add_constraint(std::make_unique<IntLinEqConstraint>(
            std::vector<int64_t>(vs.size(), 1), vs, 15));
    };
    lineq({c[0], c[1], c[2]});
    lineq({c[3], c[4], c[5]});
    lineq({c[6], c[7], c[8]});
    lineq({c[0], c[3], c[6]});
    lineq({c[1], c[4], c[7]});
    lineq({c[2], c[5], c[8]});
    lineq({c[0], c[4], c[8]});
    lineq({c[2], c[4], c[6]});
    return model;
}

// 最適化モデル: x in 0..20, y in 0..20, x + y <= 25, maximize 3x + 2y。
std::unique_ptr<Model> build_optimize_model(size_t& obj_idx) {
    auto model = std::make_unique<Model>();
    auto x = model->create_variable("x", 0, 20);
    auto y = model->create_variable("y", 0, 20);
    auto obj = model->create_variable("obj", 0, 100);
    obj_idx = obj->id();
    // x + y <= 25
    model->add_constraint(std::make_unique<IntLinLeConstraint>(
        std::vector<int64_t>{1, 1}, std::vector<Variable*>{x, y}, 25));
    // 3x + 2y - obj = 0
    model->add_constraint(std::make_unique<IntLinEqConstraint>(
        std::vector<int64_t>{3, 2, -1},
        std::vector<Variable*>{x, y, obj}, 0));
    return model;
}

} // namespace

TEST_CASE("clone: SAT first-solution は原本と同一", "[parallel][clone]") {
    // 原本
    auto m_orig = build_magic_square();
    Solver a;
    auto sol_a = a.solve(*m_orig);
    REQUIRE(sol_a.has_value());

    // master prepare → clone → worker solve_prepared（同一シード）
    auto m_master = build_magic_square();
    Solver master;
    REQUIRE(master.prepare(*m_master));
    auto m_clone = m_master->clone();
    Solver worker;
    worker.apply_worker_config(WorkerConfig{});  // デフォルト = seed 12345678, 全機能ON
    auto sol_w = worker.solve_prepared(*m_clone);
    REQUIRE(sol_w.has_value());

    CHECK(*sol_a == *sol_w);
    CHECK(same_stats(a.stats(), worker.stats()));
}

TEST_CASE("clone: worker-vs-worker は完全決定論的", "[parallel][clone]") {
    auto m_master = build_magic_square();
    Solver master;
    REQUIRE(master.prepare(*m_master));

    auto m1 = m_master->clone();
    auto m2 = m_master->clone();

    // 位置指定初期化は WorkerConfig のフィールド変更に脆いので明示代入で seed のみ変える。
    WorkerConfig cfg; cfg.seed = 42;
    Solver w1; w1.apply_worker_config(cfg);
    Solver w2; w2.apply_worker_config(cfg);

    auto s1 = w1.solve_prepared(*m1);
    auto s2 = w2.solve_prepared(*m2);
    REQUIRE(s1.has_value());
    REQUIRE(s2.has_value());
    CHECK(*s1 == *s2);
    CHECK(same_stats(w1.stats(), w2.stats()));
}

TEST_CASE("clone: 最適化は原本と同一の最適値・統計", "[parallel][clone]") {
    size_t obj_idx = 0;
    auto m_orig = build_optimize_model(obj_idx);
    Solver a;
    auto opt_a = a.solve_optimize(*m_orig, obj_idx, /*minimize=*/false);
    REQUIRE(opt_a.has_value());
    // 最適値: x=20,y=5 -> obj=70 (x+y=25), あるいは x=20,y=5。3*20+2*5=70。
    REQUIRE(opt_a->at("obj") == 70);

    size_t obj_idx2 = 0;
    auto m_master = build_optimize_model(obj_idx2);
    Solver master;
    REQUIRE(master.prepare(*m_master));
    auto m_clone = m_master->clone();
    Solver worker;
    worker.apply_worker_config(WorkerConfig{});
    auto opt_w = worker.solve_optimize_prepared(*m_clone, obj_idx2, /*minimize=*/false);
    REQUIRE(opt_w.has_value());

    CHECK(opt_w->at("obj") == 70);
    CHECK(*opt_a == *opt_w);
    CHECK(same_stats(a.stats(), worker.stats()));
}

TEST_CASE("clone: master と clone は独立（clone 探索が master を汚さない）", "[parallel][clone]") {
    auto m_master = build_magic_square();
    Solver master;
    REQUIRE(master.prepare(*m_master));

    // 2 つの clone を別々に解いても、互いに影響しない（独立性）。
    auto m1 = m_master->clone();
    auto m2 = m_master->clone();
    Solver w1; w1.apply_worker_config(WorkerConfig{});
    auto s1 = w1.solve_prepared(*m1);
    // m1 を解いた後に m2 を解く: m2 は master の clone なので未探索状態のはず。
    Solver w2; w2.apply_worker_config(WorkerConfig{});
    auto s2 = w2.solve_prepared(*m2);

    REQUIRE(s1.has_value());
    REQUIRE(s2.has_value());
    CHECK(*s1 == *s2);
    CHECK(same_stats(w1.stats(), w2.stats()));
}

TEST_CASE("clone: 異なるシードは探索軌道を変える（多様化が効く）", "[parallel][clone]") {
    // ポートフォリオの前提: シードが違えば探索が変わりうる（必ずではないが、
    // 少なくとも片方のシードで原本と異なる軌道になることを期待）。
    auto m_master = build_magic_square();
    Solver master;
    REQUIRE(master.prepare(*m_master));

    auto base = m_master->clone();
    Solver wb; wb.apply_worker_config(WorkerConfig{});  // seed 12345678
    auto sb = wb.solve_prepared(*base);
    REQUIRE(sb.has_value());

    bool any_different_stats = false;
    for (uint32_t seed : {1u, 7u, 99u, 12345u, 999983u}) {
        auto mc = m_master->clone();
        Solver w;
        WorkerConfig cfg{}; cfg.seed = seed;
        w.apply_worker_config(cfg);
        auto s = w.solve_prepared(*mc);
        REQUIRE(s.has_value());  // どのシードでも解は見つかる（健全性）
        if (!same_stats(wb.stats(), w.stats())) any_different_stats = true;
    }
    CHECK(any_different_stats);
}

// ============================================================================
// ParallelSolver: ポートフォリオ並列の正しさ（SAT first-wins / 最適化 bound 共有）
// ============================================================================

namespace {

std::vector<WorkerConfig> diversified_configs(size_t n) {
    std::vector<WorkerConfig> cfgs;
    for (size_t i = 0; i < n; ++i) {
        WorkerConfig c;
        c.seed = static_cast<uint32_t>(12345678u + i * 2654435761u);
        // ローテーションで探索時 ablation を散らす
        switch (i % 4) {
            case 1: c.restart_enabled = false; break;
            case 2: c.activity_first_pin = true; break;
            case 3: c.nogood_learning = false; break;
            default: break;
        }
        cfgs.push_back(c);
    }
    return cfgs;
}

// 3 つの変数を {1,2} に入れて全相異 → 鳩ノ巣で UNSAT（presolve では落ちにくい）。
std::unique_ptr<Model> build_pigeonhole_unsat() {
    auto model = std::make_unique<Model>();
    std::vector<Variable*> v;
    for (int i = 0; i < 3; ++i) v.push_back(model->create_variable("p" + std::to_string(i), 1, 2));
    model->add_constraint(std::make_unique<AllDifferentConstraint>(v));
    return model;
}

} // namespace

TEST_CASE("ParallelSolver: SAT は有効な解を返す（-j4）", "[parallel][manager]") {
    auto m = build_magic_square();
    ParallelSolver ps(4, diversified_configs(4));
    auto r = ps.solve(*m);
    REQUIRE(r.status == SearchResult::SAT);
    REQUIRE(r.solution.has_value());

    // 解の妥当性: 9 セルが全相異、各行が 15。
    const auto& sol = *r.solution;
    std::set<Domain::value_type> vals;
    for (int i = 0; i < 9; ++i) {
        auto it = sol.find("x" + std::to_string(i));
        REQUIRE(it != sol.end());
        vals.insert(it->second);
    }
    CHECK(vals.size() == 9);
    CHECK(sol.at("x0") + sol.at("x1") + sol.at("x2") == 15);
    CHECK(sol.at("x3") + sol.at("x4") + sol.at("x5") == 15);
    CHECK(sol.at("x6") + sol.at("x7") + sol.at("x8") == 15);
}

TEST_CASE("ParallelSolver: UNSAT を正しく判定する（-j4）", "[parallel][manager]") {
    auto m = build_pigeonhole_unsat();
    ParallelSolver ps(4, diversified_configs(4));
    auto r = ps.solve(*m);
    CHECK(r.status == SearchResult::UNSAT);
    CHECK_FALSE(r.solution.has_value());
}

TEST_CASE("ParallelSolver: 最適化は最適値と最適性証明を返す（-j4）", "[parallel][manager]") {
    size_t obj_idx = 0;
    auto m = build_optimize_model(obj_idx);
    ParallelSolver ps(4, diversified_configs(4));
    auto r = ps.solve_optimize(*m, obj_idx, /*minimize=*/false);
    REQUIRE(r.status == SearchResult::SAT);
    REQUIRE(r.objective.has_value());
    CHECK(*r.objective == 70);
    CHECK(r.proved_optimal);
    REQUIRE(r.solution.has_value());
    CHECK(r.solution->at("obj") == 70);
}

TEST_CASE("ParallelSolver: 最適化 -j8 を反復しても健全（矛盾なし・最適超えなし）", "[parallel][manager][soundness]") {
    for (int rep = 0; rep < 8; ++rep) {
        size_t obj_idx = 0;
        auto m = build_optimize_model(obj_idx);
        ParallelSolver ps(8, diversified_configs(8));
        auto r = ps.solve_optimize(*m, obj_idx, /*minimize=*/false);
        REQUIRE(r.status == SearchResult::SAT);
        REQUIRE(r.objective.has_value());
        // 証明済み最適は常に 70。bound 共有が不健全なら 70 を超える（偽の改善）はず。
        CHECK(*r.objective == 70);
        CHECK(r.proved_optimal);
    }
}
