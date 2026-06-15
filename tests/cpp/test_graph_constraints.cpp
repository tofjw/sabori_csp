#include <catch2/catch_test_macros.hpp>
#include "sabori_csp/constraint.hpp"
#include "sabori_csp/constraints/logical.hpp"
#include "sabori_csp/constraints/all_different_gac.hpp"
#include "sabori_csp/variable.hpp"
#include "sabori_csp/domain.hpp"
#include "sabori_csp/model.hpp"
#include "sabori_csp/solver.hpp"
#include <set>
#include <tuple>
#include <algorithm>
#include <iostream>

using namespace sabori_csp;


// ============================================================================
// CircuitConstraint tests
// ============================================================================

TEST_CASE("CircuitConstraint name", "[constraint][circuit]") {
    Model model;
    auto* x0 = model.create_variable("x0", Domain(0, 2));
    auto* x1 = model.create_variable("x1", Domain(0, 2));
    auto* x2 = model.create_variable("x2", Domain(0, 2));
    CircuitConstraint c({x0, x1, x2});

    REQUIRE(c.name() == "circuit");
}

TEST_CASE("CircuitConstraint variables", "[constraint][circuit]") {
    Model model;
    auto* x0 = model.create_variable("x0", Domain(0, 2));
    auto* x1 = model.create_variable("x1", Domain(0, 2));
    auto* x2 = model.create_variable("x2", Domain(0, 2));
    CircuitConstraint c({x0, x1, x2});

    auto& vars = c.var_ids_ref();
    REQUIRE(vars.size() == 3);
}

TEST_CASE("CircuitConstraint is_satisfied", "[constraint][circuit]") {
    SECTION("valid circuit - satisfied") {
        // Circuit: 0 -> 1 -> 2 -> 0
        Model model;
        auto* x0 = model.create_variable("x0", Domain(1, 1));  // x[0] = 1
        auto* x1 = model.create_variable("x1", Domain(2, 2));  // x[1] = 2
        auto* x2 = model.create_variable("x2", Domain(0, 0));  // x[2] = 0
        CircuitConstraint c({x0, x1, x2});

        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == true);
    }

    SECTION("subcircuit - violated") {
        // Subcircuit: 0 -> 1 -> 0 (x[2] is not part of circuit)
        Model model;
        auto* x0 = model.create_variable("x0", Domain(1, 1));  // x[0] = 1
        auto* x1 = model.create_variable("x1", Domain(0, 0));  // x[1] = 0
        auto* x2 = model.create_variable("x2", Domain(2, 2));  // x[2] = 2 (self-loop)
        CircuitConstraint c({x0, x1, x2});

        // This should be marked as initially inconsistent or is_satisfied returns false
        REQUIRE(c.is_satisfied(model).has_value());
        REQUIRE(c.is_satisfied(model).value() == false);
    }

    SECTION("duplicate values - violated") {
        Model model;
        auto* x0 = model.create_variable("x0", Domain(1, 1));  // x[0] = 1
        auto* x1 = model.create_variable("x1", Domain(1, 1));  // x[1] = 1 (duplicate)
        auto* x2 = model.create_variable("x2", Domain(0, 0));  // x[2] = 0
        CircuitConstraint c({x0, x1, x2});

        // Duplicate value - prepare_propagation() should detect contradiction
        REQUIRE(c.prepare_propagation(model) == false);
    }

    SECTION("not fully assigned") {
        Model model;
        auto* x0 = model.create_variable("x0", Domain(1, 1));
        auto* x1 = model.create_variable("x1", Domain(0, 2));
        auto* x2 = model.create_variable("x2", Domain(0, 0));
        CircuitConstraint c({x0, x1, x2});

        REQUIRE_FALSE(c.is_satisfied(model).has_value());
    }
}

TEST_CASE("CircuitConstraint pool management", "[constraint][circuit]") {
    Model model;
    auto* x0 = model.create_variable("x0", Domain(0, 2));
    auto* x1 = model.create_variable("x1", Domain(0, 2));
    auto* x2 = model.create_variable("x2", Domain(0, 2));
    CircuitConstraint c({x0, x1, x2});

    SECTION("initial pool size") {
        REQUIRE(c.pool_size() == 3);  // {0, 1, 2}
    }
}

TEST_CASE("CircuitConstraint on_final_instantiate", "[constraint][circuit]") {
    SECTION("valid circuit") {
        // Circuit: 0 -> 1 -> 2 -> 0
        Model model;
        auto* x0 = model.create_variable("x0", Domain(1, 1));
        auto* x1 = model.create_variable("x1", Domain(2, 2));
        auto* x2 = model.create_variable("x2", Domain(0, 0));
        CircuitConstraint c({x0, x1, x2});

        REQUIRE(c.on_final_instantiate(model) == true);
    }

    SECTION("valid circuit reversed") {
        // Circuit: 0 -> 2 -> 1 -> 0
        Model model;
        auto* x0 = model.create_variable("x0", Domain(2, 2));
        auto* x1 = model.create_variable("x1", Domain(0, 0));
        auto* x2 = model.create_variable("x2", Domain(1, 1));
        CircuitConstraint c({x0, x1, x2});

        REQUIRE(c.on_final_instantiate(model) == true);
    }
}

TEST_CASE("CircuitConstraint rewind_to", "[constraint][circuit][trail]") {
    Model model;
    auto x0 = model.create_variable("x0", 0, 2);
    auto x1 = model.create_variable("x1", 0, 2);
    auto x2 = model.create_variable("x2", 0, 2);
    CircuitConstraint c({x0, x1, x2});

    REQUIRE(c.pool_size() == 3);

    // Simulate instantiation at level 1: x[0] = 1
    model.instantiate(1, x0->id(), 1);
    REQUIRE(c.on_instantiate(model, 1, 0, 1, 0, 2));
    REQUIRE(c.pool_size() == 2);

    // Simulate instantiation at level 2: x[1] = 2
    model.instantiate(2, x1->id(), 2);
    REQUIRE(c.on_instantiate(model, 2, 1, 2, 0, 2));
    REQUIRE(c.pool_size() == 1);

    // Rewind to level 1
    c.rewind_to(1);
    REQUIRE(c.pool_size() == 2);

    // Rewind to level 0
    c.rewind_to(0);
    REQUIRE(c.pool_size() == 3);
}

TEST_CASE("CircuitConstraint subcircuit detection", "[constraint][circuit]") {
    SECTION("subcircuit detected during instantiation") {
        Model model;
        auto x0 = model.create_variable("x0", 0, 2);
        auto x1 = model.create_variable("x1", 0, 2);
        auto x2 = model.create_variable("x2", 0, 2);
        CircuitConstraint c({x0, x1, x2});

        // x[0] = 1: 0 -> 1
        model.instantiate(1, x0->id(), 1);
        REQUIRE(c.on_instantiate(model, 1, 0, 1, 0, 2));

        // x[1] = 0: 1 -> 0, forms subcircuit (0 -> 1 -> 0)
        model.instantiate(2, x1->id(), 0);
        REQUIRE_FALSE(c.on_instantiate(model, 2, 1, 0, 0, 2));
    }

    SECTION("self-loop is subcircuit") {
        Model model;
        auto x0 = model.create_variable("x0", 0, 2);
        auto x1 = model.create_variable("x1", 0, 2);
        auto x2 = model.create_variable("x2", 0, 2);
        CircuitConstraint c({x0, x1, x2});

        // x[0] = 0: self-loop, forms subcircuit of size 1
        model.instantiate(1, x0->id(), 0);
        REQUIRE_FALSE(c.on_instantiate(model, 1, 0, 0, 0, 2));
    }
}

TEST_CASE("CircuitConstraint SCC disconnected graph presolve", "[constraint][circuit][scc]") {
    // 2つの三角形に分断されたグラフ: 各ノードの出入次数は2あるが強連結でない。
    // サブサーキット規則やペアワイズでは検出できず、SCC 分析でのみ presolve 矛盾になる。
    Model model;
    std::vector<Variable*> vars;
    vars.push_back(model.create_variable("x0", Domain(std::vector<int64_t>{1, 2})));
    vars.push_back(model.create_variable("x1", Domain(std::vector<int64_t>{0, 2})));
    vars.push_back(model.create_variable("x2", Domain(std::vector<int64_t>{0, 1})));
    vars.push_back(model.create_variable("x3", Domain(std::vector<int64_t>{4, 5})));
    vars.push_back(model.create_variable("x4", Domain(std::vector<int64_t>{3, 5})));
    vars.push_back(model.create_variable("x5", Domain(std::vector<int64_t>{3, 4})));
    CircuitConstraint c(vars);

    REQUIRE(c.presolve(model) == PresolveResult::Contradiction);
}

TEST_CASE("CircuitConstraint SCC in-degree forcing", "[constraint][circuit][scc]") {
    // ノード 0 への入エッジ候補は x3 のみ → presolve で x3 = 0 が確定する
    Model model;
    std::vector<Variable*> vars;
    vars.push_back(model.create_variable("x0", Domain(std::vector<int64_t>{1, 2, 3})));
    vars.push_back(model.create_variable("x1", Domain(std::vector<int64_t>{2, 3})));
    vars.push_back(model.create_variable("x2", Domain(std::vector<int64_t>{1, 3})));
    vars.push_back(model.create_variable("x3", Domain(std::vector<int64_t>{0, 1, 2})));
    CircuitConstraint c(vars);

    REQUIRE(c.presolve(model) != PresolveResult::Contradiction);
    REQUIRE(vars[3]->is_assigned());
    REQUIRE(vars[3]->assigned_value().value() == 0);
}

TEST_CASE("CircuitConstraint SCC randomized solution count", "[constraint][circuit][scc]") {
    // ランダムな部分ドメインで全解数をブルートフォースと比較（false UNSAT 検出）
    std::mt19937 rng(424242);
    constexpr size_t kN = 6;
    std::uniform_int_distribution<int> coin(0, 99);

    for (int trial = 0; trial < 30; ++trial) {
        std::array<std::vector<int64_t>, kN> doms;
        for (size_t i = 0; i < kN; ++i) {
            for (int64_t v = 0; v < static_cast<int64_t>(kN); ++v) {
                if (static_cast<size_t>(v) == i) continue;  // 自己ループ除外
                if (coin(rng) < 60) doms[i].push_back(v);
            }
            if (doms[i].empty()) doms[i].push_back(static_cast<int64_t>((i + 1) % kN));
        }
        // base_offset 検出を 0 に固定するため、値 0 を必ずどこかに含める
        if (std::find(doms[kN - 1].begin(), doms[kN - 1].end(), 0) == doms[kN - 1].end()) {
            doms[kN - 1].insert(doms[kN - 1].begin(), 0);
        }

        // ブルートフォースで単一ハミルトン閉路の数を数える
        size_t expected = 0;
        std::array<int64_t, kN> assign{};
        std::array<bool, kN> used{};
        std::function<void(size_t)> rec = [&](size_t depth) {
            if (depth == kN) {
                std::array<bool, kN> vis{};
                size_t cur = 0;
                for (size_t s = 0; s < kN; ++s) {
                    if (vis[cur]) return;
                    vis[cur] = true;
                    cur = static_cast<size_t>(assign[cur]);
                }
                if (cur == 0) ++expected;
                return;
            }
            for (auto v : doms[depth]) {
                if (used[static_cast<size_t>(v)]) continue;
                used[static_cast<size_t>(v)] = true;
                assign[depth] = v;
                rec(depth + 1);
                used[static_cast<size_t>(v)] = false;
            }
        };
        rec(0);

        Model model;
        std::vector<Variable*> vars;
        for (size_t i = 0; i < kN; ++i) {
            vars.push_back(model.create_variable("x" + std::to_string(i), Domain(doms[i])));
        }
        model.add_constraint(std::make_unique<CircuitConstraint>(vars));

        Solver solver;
        size_t actual = 0;
        solver.solve_all(model, [&](const Solution&) {
            ++actual;
            return true;
        });

        INFO("trial " << trial);
        REQUIRE(actual == expected);
    }
}

TEST_CASE("CircuitConstraint solver integration", "[solver][circuit]") {
    SECTION("n=3 find one solution") {
        Model model;
        std::vector<Variable*> vars;
        for (int i = 0; i < 3; ++i) {
            auto var = model.create_variable("x" + std::to_string(i), 0, 2);
            vars.push_back(var);
        }

        model.add_constraint(std::make_unique<CircuitConstraint>(vars));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());

        // Verify it's a valid circuit
        std::vector<int64_t> values;
        for (int i = 0; i < 3; ++i) {
            values.push_back(solution->at("x" + std::to_string(i)));
        }

        // Check all different
        std::set<int64_t> unique_values(values.begin(), values.end());
        REQUIRE(unique_values.size() == 3);

        // Check forms a cycle starting from 0
        std::set<int64_t> visited;
        int64_t current = 0;
        for (int i = 0; i < 3; ++i) {
            REQUIRE(visited.find(current) == visited.end());
            visited.insert(current);
            current = values[static_cast<size_t>(current)];
        }
        REQUIRE(current == 0);  // Returns to start
    }

    SECTION("n=3 find all solutions") {
        Model model;
        std::vector<Variable*> vars;
        for (int i = 0; i < 3; ++i) {
            auto var = model.create_variable("x" + std::to_string(i), 0, 2);
            vars.push_back(var);
        }

        model.add_constraint(std::make_unique<CircuitConstraint>(vars));

        Solver solver;
        std::vector<Solution> solutions;
        size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        // For n=3, there are exactly 2 Hamiltonian cycles:
        // 0 -> 1 -> 2 -> 0
        // 0 -> 2 -> 1 -> 0
        REQUIRE(count == 2);
        REQUIRE(solutions.size() == 2);

        // Verify all solutions are valid and different
        std::set<std::vector<int64_t>> unique_solutions;
        for (const auto& sol : solutions) {
            std::vector<int64_t> vals;
            for (int i = 0; i < 3; ++i) {
                vals.push_back(sol.at("x" + std::to_string(i)));
            }

            // Verify forms a valid circuit
            std::set<int64_t> visited;
            int64_t current = 0;
            for (int i = 0; i < 3; ++i) {
                REQUIRE(visited.find(current) == visited.end());
                visited.insert(current);
                current = vals[static_cast<size_t>(current)];
            }
            REQUIRE(current == 0);

            unique_solutions.insert(vals);
        }

        REQUIRE(unique_solutions.size() == 2);
    }

    SECTION("n=4 find all solutions") {
        Model model;
        std::vector<Variable*> vars;
        for (int i = 0; i < 4; ++i) {
            auto var = model.create_variable("x" + std::to_string(i), 0, 3);
            vars.push_back(var);
        }

        model.add_constraint(std::make_unique<CircuitConstraint>(vars));

        Solver solver;
        std::vector<Solution> solutions;
        size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        // For n=4, there are exactly 6 Hamiltonian cycles
        // (n-1)!/2 = 3!/2 = 3 for undirected, but directed has (n-1)! = 6
        REQUIRE(count == 6);

        // Verify all solutions are valid
        for (const auto& sol : solutions) {
            std::vector<int64_t> vals;
            for (int i = 0; i < 4; ++i) {
                vals.push_back(sol.at("x" + std::to_string(i)));
            }

            std::set<int64_t> visited;
            int64_t current = 0;
            for (int i = 0; i < 4; ++i) {
                REQUIRE(visited.find(current) == visited.end());
                visited.insert(current);
                current = vals[static_cast<size_t>(current)];
            }
            REQUIRE(current == 0);
        }
    }
}

TEST_CASE("CircuitConstraint with partial assignment", "[solver][circuit]") {
    SECTION("one variable fixed - consistent") {
        Model model;
        std::vector<Variable*> vars;
        for (int i = 0; i < 3; ++i) {
            auto var = model.create_variable("x" + std::to_string(i), 0, 2);
            vars.push_back(var);
        }

        // Fix x[0] = 1
        model.instantiate(0, vars[0]->id(), 1);

        model.add_constraint(std::make_unique<CircuitConstraint>(vars));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("x0") == 1);

        // Verify valid circuit
        std::vector<int64_t> values = {
            solution->at("x0"),
            solution->at("x1"),
            solution->at("x2")
        };

        std::set<int64_t> visited;
        int64_t current = 0;
        for (int i = 0; i < 3; ++i) {
            REQUIRE(visited.find(current) == visited.end());
            visited.insert(current);
            current = values[static_cast<size_t>(current)];
        }
        REQUIRE(current == 0);
    }

    SECTION("two variables fixed - consistent") {
        Model model;
        std::vector<Variable*> vars;
        for (int i = 0; i < 3; ++i) {
            auto var = model.create_variable("x" + std::to_string(i), 0, 2);
            vars.push_back(var);
        }

        // Fix x[0] = 1, x[1] = 2 (forces x[2] = 0 for valid circuit)
        model.instantiate(0, vars[0]->id(), 1);
        model.instantiate(0, vars[1]->id(), 2);

        model.add_constraint(std::make_unique<CircuitConstraint>(vars));

        Solver solver;
        auto solution = solver.solve(model);

        REQUIRE(solution.has_value());
        REQUIRE(solution->at("x0") == 1);
        REQUIRE(solution->at("x1") == 2);
        REQUIRE(solution->at("x2") == 0);
    }

    SECTION("partial assignment creates subcircuit - inconsistent") {
        Model model;
        std::vector<Variable*> vars;
        for (int i = 0; i < 3; ++i) {
            auto var = model.create_variable("x" + std::to_string(i), 0, 2);
            vars.push_back(var);
        }

        // Fix x[0] = 1, x[1] = 0 (creates subcircuit 0 -> 1 -> 0)
        model.instantiate(0, vars[0]->id(), 1);
        model.instantiate(0, vars[1]->id(), 0);

        auto constraint_uptr = std::make_unique<CircuitConstraint>(vars);
        auto* constraint = constraint_uptr.get();
        model.add_constraint(std::move(constraint_uptr));

        // prepare_propagation() should detect contradiction
        REQUIRE(constraint->prepare_propagation(model) == false);
    }
}

// ============================================================================
// InverseConstraint tests
// ============================================================================

TEST_CASE("InverseConstraint name", "[constraint][inverse]") {
    Model model;
    auto* f1 = model.create_variable("f1", Domain(0, 1));
    auto* f2 = model.create_variable("f2", Domain(0, 1));
    auto* g1 = model.create_variable("g1", Domain(0, 1));
    auto* g2 = model.create_variable("g2", Domain(0, 1));
    InverseConstraint c({f1, f2}, {g1, g2});
    REQUIRE(c.name() == "inverse");
}

TEST_CASE("InverseConstraint on_final_instantiate", "[constraint][inverse]") {
    SECTION("valid inverse pair (0-based)") {
        Model model;
        auto* f1 = model.create_variable("f1", Domain(1, 1));  // f[0]=1
        auto* f2 = model.create_variable("f2", Domain(0, 0));  // f[1]=0
        auto* g1 = model.create_variable("g1", Domain(1, 1));  // g[0]=1
        auto* g2 = model.create_variable("g2", Domain(0, 0));  // g[1]=0
        InverseConstraint c({f1, f2}, {g1, g2});
        REQUIRE(c.on_final_instantiate(model) == true);
    }

    SECTION("invalid inverse pair (0-based)") {
        Model model;
        auto* f1 = model.create_variable("f1", Domain(1, 1));  // f[0]=1
        auto* f2 = model.create_variable("f2", Domain(0, 0));  // f[1]=0
        auto* g1 = model.create_variable("g1", Domain(0, 0));  // g[0]=0 — wrong
        auto* g2 = model.create_variable("g2", Domain(1, 1));  // g[1]=1 — wrong
        InverseConstraint c({f1, f2}, {g1, g2});
        REQUIRE(c.on_final_instantiate(model) == false);
    }
}

TEST_CASE("InverseConstraint solver integration", "[constraint][inverse]") {
    SECTION("3 variables - all solutions (0-based)") {
        Model model;
        auto* f1 = model.create_variable("f1", Domain(0, 2));
        auto* f2 = model.create_variable("f2", Domain(0, 2));
        auto* f3 = model.create_variable("f3", Domain(0, 2));
        auto* g1 = model.create_variable("g1", Domain(0, 2));
        auto* g2 = model.create_variable("g2", Domain(0, 2));
        auto* g3 = model.create_variable("g3", Domain(0, 2));
        model.add_constraint(std::make_unique<InverseConstraint>(
            std::vector<Variable*>{f1, f2, f3},
            std::vector<Variable*>{g1, g2, g3}));

        Solver solver;
        std::vector<Solution> solutions;
        size_t count = solver.solve_all(model, [&solutions](const Solution& sol) {
            solutions.push_back(sol);
            return true;
        });

        // 3! = 6 permutations, each is a valid inverse
        REQUIRE(count == 6);

        // Verify each solution satisfies f[i]=j <-> g[j]=i (0-based)
        for (const auto& sol : solutions) {
            auto fv1 = sol.at("f1"), fv2 = sol.at("f2"), fv3 = sol.at("f3");
            auto gv1 = sol.at("g1"), gv2 = sol.at("g2"), gv3 = sol.at("g3");
            std::vector<int64_t> f = {fv1, fv2, fv3};
            std::vector<int64_t> g = {gv1, gv2, gv3};
            for (int i = 0; i < 3; ++i) {
                REQUIRE(g[static_cast<size_t>(f[i])] == i);
                REQUIRE(f[static_cast<size_t>(g[i])] == i);
            }
        }
    }

    SECTION("2 variables with partial assignment (0-based)") {
        Model model;
        auto* f1 = model.create_variable("f1", Domain(1, 1));  // f[0] = 1 fixed
        auto* f2 = model.create_variable("f2", Domain(0, 1));
        auto* g1 = model.create_variable("g1", Domain(0, 1));
        auto* g2 = model.create_variable("g2", Domain(0, 1));
        model.add_constraint(std::make_unique<InverseConstraint>(
            std::vector<Variable*>{f1, f2},
            std::vector<Variable*>{g1, g2}));

        Solver solver;
        auto result = solver.solve(model);

        REQUIRE(result.has_value());
        REQUIRE(result->at("f1") == 1);
        REQUIRE(result->at("f2") == 0);
        REQUIRE(result->at("g1") == 1);
        REQUIRE(result->at("g2") == 0);
    }

    SECTION("2 variables with partial assignment - solve_all (0-based)") {
        Model model;
        auto* f1 = model.create_variable("f1", Domain(1, 1));
        auto* f2 = model.create_variable("f2", Domain(0, 1));
        auto* g1 = model.create_variable("g1", Domain(0, 1));
        auto* g2 = model.create_variable("g2", Domain(0, 1));
        model.add_constraint(std::make_unique<InverseConstraint>(
            std::vector<Variable*>{f1, f2},
            std::vector<Variable*>{g1, g2}));

        Solver solver;
        // restarts enabled by default — this was the infinite loop case
        size_t count = solver.solve_all(model, [](const Solution&) { return true; });
        REQUIRE(count == 1);
    }
}
