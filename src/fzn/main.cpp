#include "sabori_csp/fzn/model.hpp"
#include "sabori_csp/solver.hpp"
#include "sabori_csp/parallel_solver.hpp"
#include "sabori_csp/model_simplifier.hpp"
#include "sabori_csp/constraints/global.hpp"
#include "fzn_parser.hpp"
#include <iostream>
#include <string>
#include <cstring>
#include <optional>
#include <limits>
#include <csignal>
#include <atomic>
#include <algorithm>
#include <iomanip>
#include <numeric>
#include <vector>

std::atomic<bool> g_timeout_flag{false};
sabori_csp::Solver* g_current_solver = nullptr;
sabori_csp::ParallelSolver* g_parallel_solver = nullptr;
int g_timeout_sec = 0;

void timeout_handler(int) {
    g_timeout_flag = true;
    // マルチスレッド時は ParallelSolver 経由で全ワーカーを停止。
    // ハンドラ内は atomic store のみ（async-signal-safe）。
    if (g_parallel_solver) {
        g_parallel_solver->stop();
    } else if (g_current_solver) {
        g_current_solver->stop();
    }
}

void print_usage(const char* program) {
    std::cerr << "Usage: " << program << " [-a] [-s] [-v] [-c] [-t SEC] [-b N] [-p N] [-j N] <file.fzn>\n";
    std::cerr << "  -a      Find all solutions (or all improving solutions for optimization)\n";
    std::cerr << "  -j N    Number of portfolio threads (default: 1). N>1 runs parallel search.\n";
    std::cerr << "          (also settable via SABORI_THREADS env; -j overrides env)\n";
    std::cerr << "  -s      Print solver statistics to stderr\n";
    std::cerr << "  -v      Verbose mode (print presolve/restart progress; worker 0 when -j>1)\n";
    std::cerr << "  -V N    Verbose mode for worker N (implies -v; default 0)\n";
    std::cerr << "  -c      Community analysis [diagnostic only — does not speed up search]\n";
    std::cerr << "  -t SEC  Timeout in seconds\n";
    std::cerr << "  -b N    Bisection threshold (default: 8, 0=disable)\n";
    std::cerr << "  -p N    Probe fail limit for improvement probe (default: 10, 0=disable)\n";
    std::cerr << "  -G      Use GAC (Régin's algorithm) for all_different\n";
    std::cerr << "  -N      Disable nogood learning\n";
    std::cerr << "  -C      Enable conflict learning (violated-constraint scope nogoods)\n";
    std::cerr << "  -E      Disable variable elimination\n";
}

bool g_print_stats = false;
bool g_verbose = false;
int g_verbose_worker = 0;  ///< 並列時に verbose を出すワーカー番号（既定 0）
bool g_no_nogood = false;
bool g_conflict_learning = false;
bool g_no_elimination = false;
int g_bisection_threshold = 8;
int g_probe_fail_limit = 5;
// 診断専用フラグ。ベンチマーク結果上、探索性能は改善しないため
// デフォルト off。`-c` で明示的に有効化したときだけ VIG/コミュニティ/
// 局所性統計を出力する。
bool g_community_analysis = false;
bool g_use_gac = false;

void print_stats(const sabori_csp::Solver& solver, const sabori_csp::Model* model = nullptr) {
    if (!g_print_stats) return;
    const auto& s = solver.stats();
    std::cerr << "% Stats: fails=" << s.fail_count
              << " restarts=" << s.restart_count
              << " max_depth=" << s.max_depth
              << " avg_depth=" << (s.depth_count > 0 ? s.depth_sum / s.depth_count : 0)
              << " nogoods=" << s.nogoods_size
              << " unit_nogoods=" << s.unit_nogoods_size
              << " ng_check=" << s.nogood_check_count
              << " ng_domain=" << s.nogood_domain_count
              << " ng_prune=" << s.nogood_prune_count
              << " ng_noop=" << (s.nogood_check_count - s.nogood_domain_count - s.nogood_prune_count)
              << " bisect=" << s.bisect_count
              << " enumerate=" << s.enumerate_count
              << "\n";
    auto dist = solver.nogood_length_distribution();
    if (!dist.empty()) {
        std::cerr << "% NG length distribution:";
        for (const auto& [len, count] : dist) {
            std::cerr << " " << len << ":" << count;
        }
        std::cerr << "\n";
    }

    // 制約タイプ別統計（verbose モード時のみ）
    const auto& cstats = solver.constraint_stats();
    if (!cstats.empty()) {
        std::vector<std::pair<std::string, sabori_csp::ConstraintStats>> sorted_stats(cstats.begin(), cstats.end());
        std::sort(sorted_stats.begin(), sorted_stats.end(),
                  [](const auto& a, const auto& b) { return a.second.call_count > b.second.call_count; });

        std::cerr << "% Constraint statistics:\n";
        std::cerr << "%   " << std::left << std::setw(24) << "Name"
                  << std::right << std::setw(10) << "Calls"
                  << std::setw(12) << "Reductions"
                  << std::setw(10) << "Fails"
                  << std::setw(14) << "AvgFailDepth" << "\n";
        for (const auto& [name, cs] : sorted_stats) {
            std::cerr << "%   " << std::left << std::setw(24) << name
                      << std::right << std::setw(10) << cs.call_count
                      << std::setw(12) << cs.reduction_count
                      << std::setw(10) << cs.fail_count;
            if (cs.fail_count > 0) {
                std::cerr << std::setw(14) << std::fixed << std::setprecision(1)
                          << (double)cs.fail_depth_sum / cs.fail_count;
            } else {
                std::cerr << std::setw(14) << "-";
            }
            std::cerr << "\n";
        }
    }

    // Per-instance constraint stats (Top 20 by call_count)
    if (model) {
        const auto& istats = solver.instance_stats();
        const auto& constraints = model->constraints();
        if (!istats.empty()) {
            // Build index sorted by call_count descending
            std::vector<size_t> indices(istats.size());
            std::iota(indices.begin(), indices.end(), 0);
            std::sort(indices.begin(), indices.end(),
                      [&](size_t a, size_t b) { return istats[a].call_count > istats[b].call_count; });

            size_t top_n = std::min<size_t>(20, indices.size());
            // Skip if top entry has 0 calls
            if (top_n > 0 && istats[indices[0]].call_count > 0) {
                std::cerr << "% Top constraint instances:\n";
                std::cerr << "%   " << std::left << std::setw(30) << "Label"
                          << std::right << std::setw(10) << "Calls"
                          << std::setw(12) << "Reductions"
                          << std::setw(10) << "Fails"
                          << std::setw(14) << "AvgFailDepth" << "\n";
                for (size_t i = 0; i < top_n; ++i) {
                    size_t idx = indices[i];
                    const auto& is = istats[idx];
                    if (is.call_count == 0) break;
                    const auto& lbl = constraints[idx]->label();
                    const std::string& display = lbl.empty() ? constraints[idx]->name() : lbl;
                    std::cerr << "%   " << std::left << std::setw(30) << display
                              << std::right << std::setw(10) << is.call_count
                              << std::setw(12) << is.reduction_count
                              << std::setw(10) << is.fail_count;
                    if (is.fail_count > 0) {
                        std::cerr << std::setw(14) << std::fixed << std::setprecision(1)
                                  << (double)is.fail_depth_sum / is.fail_count;
                    } else {
                        std::cerr << std::setw(14) << "-";
                    }
                    std::cerr << "\n";
                }
            }
        }
    }

    // Per-engine cumulative stats
    if (model) {
        for (const auto& c : model->constraints()) {
            auto* cum = dynamic_cast<sabori_csp::CumulativeConstraint*>(c.get());
            if (!cum) continue;
            auto names = cum->engine_names();
            const auto& estats = cum->engine_stats();
            for (size_t i = 0; i < names.size(); ++i) {
                std::cerr << "% Cumulative engine: " << names[i]
                          << " calls=" << estats[i].call_count
                          << " reductions=" << estats[i].reduction_count
                          << " contradictions=" << estats[i].contradiction_count
                          << "\n";
            }
        }
    }
}

// マルチスレッド時の簡易統計（勝者ワーカーの SolverStats のみ。制約別詳細は省略）。
void print_stats_brief(const sabori_csp::SolverStats& s) {
    if (!g_print_stats) return;
    std::cerr << "% Stats: fails=" << s.fail_count
              << " restarts=" << s.restart_count
              << " max_depth=" << s.max_depth
              << " avg_depth=" << (s.depth_count > 0 ? s.depth_sum / s.depth_count : 0)
              << " nogoods=" << s.nogoods_size
              << " unit_nogoods=" << s.unit_nogoods_size
              << " ng_prune=" << s.nogood_prune_count
              << " bisect=" << s.bisect_count
              << " enumerate=" << s.enumerate_count
              << "\n";
}

int g_num_threads = 1;

// ポートフォリオの多様化テーブルを構築する。
// worker0 = デフォルト構成（seed 12345678, adaptive mix_p, 全機能ON）で
//           「単一スレッドより悪くならない」軸を確保。
// worker1.. = シードをずらしつつ、多様化軸を「VBS 限界インパクトの大きい順」に適用する。
//             -j を 2,3,4... と増やすほど、影響の小さい軸が後から足される。
// 影響順・採否の根拠（決定論ベンチ, MZC, 2026-06-29〜30）:
//   benchmarks/minizinc_challenge/bench_portfolio_diversity.py（最適化中心）と
//   benchmarks/minizinc_challenge/bench_restart_sat.py（SAT）の VBS 貪欲分析。
//   - activity優先固定は不採用（最弱・他が解ける問題を解けず・adaptive mix_p に劣る）。
//   - 影響順は問題タイプで異なるため is_optimize で分岐する（下の switch 参照）。
std::vector<sabori_csp::WorkerConfig> build_worker_configs(size_t n, bool is_optimize) {
    std::vector<sabori_csp::WorkerConfig> cfgs;
    cfgs.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        sabori_csp::WorkerConfig c;
        c.bisection_threshold = static_cast<size_t>(g_bisection_threshold);
        c.probe_fail_limit = g_probe_fail_limit;
        if (g_no_nogood) c.nogood_learning = false;
        c.conflict_learning = g_conflict_learning;  // -C を全ワーカーで尊重（既定 off）
        if (i == 0) {
            cfgs.push_back(c);  // worker0 = 既定
            continue;
        }
        c.seed = static_cast<uint32_t>(12345678u + i * 2654435761u);
        // 多様化軸を VBS 限界インパクトの大きい順に採用する（worker1=k0 が最大）。
        // スレッドを 2,3,4... と増やすほど影響の小さい軸が足される。
        // 影響順は問題タイプで異なる（bench_portfolio_diversity / bench_restart_sat, 2026-06-30）:
        //   最適化: conflict(Δ+0.062,elitser単独勝ち) > mrv(+0.041,tppv) > no_nogood(+0.011,celar16)
        //           > gradient/probe/temporal(~0) > restart(no-op シード変種)
        //   SAT:    restart緩和sc8(最良,costas/we) > conf_sc8(併用,VBS2位,costas17 10.2→4.9s)
        //           > mrv > no_nogood > gradient/probe/temporal
        size_t k = (i - 1) % 7;  // 0-based 多様化インデックス（-j>8 はシード変えて循環）
        if (is_optimize) {
            switch (k) {
                case 0: c.conflict_learning = !g_conflict_learning; break;
                case 1: c.fixed_mixp = 0; break;
                case 2: c.nogood_learning = false; break;
                case 3: c.gradient_enabled = false; break;
                case 4: c.probe_enabled = false; break;
                case 5: c.temporal_enabled = false; break;
                case 6: c.restart_enabled = false; break;  // no-op/シード変種（最小）
            }
        } else {  // SAT
            switch (k) {
                case 0: c.restart_scale = 8.0; break;
                case 1: c.conflict_learning = !g_conflict_learning;
                        c.restart_scale = 8.0; break;       // conf_sc8（併用）
                case 2: c.fixed_mixp = 0; break;
                case 3: c.nogood_learning = false; break;
                case 4: c.gradient_enabled = false; break;
                case 5: c.probe_enabled = false; break;
                case 6: c.temporal_enabled = false; break;
            }
        }
        cfgs.push_back(c);
    }
    return cfgs;
}

void print_value(int64_t value, bool is_bool) {
    if (is_bool) {
        std::cout << (value ? "true" : "false");
    } else {
        std::cout << value;
    }
}

void print_solution(const sabori_csp::Solution& sol,
                   const sabori_csp::fzn::Model& model) {
    // Print output variables
    for (const auto& name : model.output_vars()) {
        auto it = sol.find(name);
        if (it != sol.end()) {
            auto var_it = model.var_decls().find(name);
            bool is_bool = (var_it != model.var_decls().end() && var_it->second.is_bool);
            std::cout << name << " = ";
            print_value(it->second, is_bool);
            std::cout << ";\n";
        }
    }

    // Print output arrays
    for (const auto& array_name : model.output_arrays()) {
        auto it = model.array_decls().find(array_name);
        if (it != model.array_decls().end()) {
            const auto& decl = it->second;
            std::cout << array_name << " = [";
            bool first = true;
            for (const auto& elem : decl.elements) {
                if (!first) std::cout << ", ";
                first = false;

                // Check if it's an inline literal (__inline_N)
                if (elem.rfind("__inline_", 0) == 0) {
                    // Extract the value from the name
                    std::string val_str = elem.substr(9);  // strlen("__inline_") = 9
                    int64_t val = std::stoll(val_str);
                    print_value(val, decl.is_bool);
                } else {
                    auto elem_it = sol.find(elem);
                    if (elem_it != sol.end()) {
                        // Check if element is bool (use array's is_bool or individual var's is_bool)
                        bool is_bool = decl.is_bool;
                        if (!is_bool) {
                            auto var_it = model.var_decls().find(elem);
                            is_bool = (var_it != model.var_decls().end() && var_it->second.is_bool);
                        }
                        print_value(elem_it->second, is_bool);
                    } else {
                        // Variable not in solution (should not happen); print 0 as fallback
                        std::cout << 0;
                    }
                }
            }
            std::cout << "];\n";
        }
    }

    std::cout << "----------" << std::endl;
}

/**
 * @brief protected 変数ID集合を構築し、ModelSimplifier を実行
 */
sabori_csp::ModelSimplifier simplify_model(
    sabori_csp::Model& model,
    const sabori_csp::fzn::Model& fzn_model) {
    // protected variable IDs を構築（目的変数のみ）
    // output 変数は定義制約が残るため伝播で値が決まる
    std::unordered_set<size_t> protected_ids;
    if (fzn_model.solve_decl().kind != sabori_csp::fzn::SolveKind::Satisfy &&
        !fzn_model.solve_decl().objective_var.empty()) {
        size_t idx = model.find_variable_index(fzn_model.solve_decl().objective_var);
        if (idx != SIZE_MAX) protected_ids.insert(idx);
    }

    sabori_csp::ModelSimplifier simplifier;
    simplifier.simplify(model, protected_ids, g_verbose);
    return simplifier;
}

/**
 * @brief 充足可能性問題を解く
 */
void solve_satisfy(sabori_csp::fzn::Model& fzn_model, bool find_all) {
    // マルチスレッド・ポートフォリオ（-j N, N>1）。find_all は portfolio で重複列挙に
    // なるため単一スレッド経路にフォールバックする。
    if (g_num_threads > 1 && !find_all) {
        auto model = fzn_model.to_model(g_verbose, g_use_gac);
        if (!g_no_elimination) simplify_model(*model, fzn_model);
        sabori_csp::ParallelSolver ps(static_cast<size_t>(g_num_threads),
                                      build_worker_configs(g_num_threads, /*is_optimize=*/false));
        ps.set_verbose(g_verbose, static_cast<size_t>(g_verbose_worker));
        g_parallel_solver = &ps;
        if (g_timeout_sec > 0) alarm(g_timeout_sec);
        auto r = ps.solve(*model);
        g_parallel_solver = nullptr;
        print_stats_brief(r.winner_stats);
        if (r.status == sabori_csp::SearchResult::SAT && r.solution) {
            print_solution(*r.solution, fzn_model);
        } else if (r.status == sabori_csp::SearchResult::UNSAT) {
            std::cout << "=====UNSATISFIABLE=====\n";
        } else {
            std::cout << "=====UNKNOWN=====\n";
        }
        return;
    }

    auto model = fzn_model.to_model(g_verbose, g_use_gac);
    if (!g_no_elimination) simplify_model(*model, fzn_model);
    sabori_csp::Solver solver;
    solver.set_verbose(g_verbose);
    solver.set_bisection_threshold(g_bisection_threshold);
    if (g_no_nogood) solver.set_nogood_learning(false);
    if (g_conflict_learning) solver.set_conflict_learning(true);
    if (g_community_analysis) solver.set_community_analysis(true);
    g_current_solver = &solver;
    if (g_timeout_sec > 0) alarm(g_timeout_sec);

    if (find_all) {
        size_t count = solver.solve_all(*model, [&fzn_model](const sabori_csp::Solution& sol) {
            print_solution(sol, fzn_model);
            return true;
        });

        print_stats(solver, model.get());
        if (count == 0) {
            if (solver.is_stopped()) {
                std::cout << "=====UNKNOWN=====\n";
            } else {
                std::cout << "=====UNSATISFIABLE=====\n";
            }
        } else {
            std::cout << "==========\n";
        }
    } else {
        auto sol = solver.solve(*model);
        print_stats(solver, model.get());
        if (sol) {
            // SAT で -a なし: 1解見つけて終了したので探索完了は証明できていない。
            // FlatZinc 仕様では `----------` (print_solution 内) のみで終わる。
            print_solution(*sol, fzn_model);
        } else if (solver.is_stopped()) {
            std::cout << "=====UNKNOWN=====\n";
        } else {
            std::cout << "=====UNSATISFIABLE=====\n";
        }
    }
}

/**
 * @brief 最適化問題を解く（探索内 branch-and-bound）
 */
void solve_optimize(sabori_csp::fzn::Model& fzn_model, bool find_all, bool minimize) {
    const auto& objective_var_name = fzn_model.solve_decl().objective_var;

    // マルチスレッド・ポートフォリオ（-j N, N>1）+ bound 共有。
    if (g_num_threads > 1) {
        auto model = fzn_model.to_model(g_verbose, g_use_gac);
        if (!g_no_elimination) simplify_model(*model, fzn_model);
        size_t obj_var_idx = model->find_variable_index(objective_var_name);
        if (obj_var_idx == SIZE_MAX) {
            std::cerr << "Error: objective variable '" << objective_var_name << "' not found\n";
            std::cout << "=====UNKNOWN=====\n";
            return;
        }
        sabori_csp::ParallelSolver ps(static_cast<size_t>(g_num_threads),
                                      build_worker_configs(g_num_threads, /*is_optimize=*/true));
        ps.set_verbose(g_verbose, static_cast<size_t>(g_verbose_worker));
        g_parallel_solver = &ps;
        if (g_timeout_sec > 0) alarm(g_timeout_sec);

        // 改善 incumbent は publish 時に（result_mtx 下で同期して）呼ばれる。
        // -a なら都度出力（出力は result_mtx 下で直列化され単調）。
        auto r = ps.solve_optimize(*model, obj_var_idx, minimize,
            [&](const sabori_csp::Solution& sol, sabori_csp::Domain::value_type) {
                if (find_all) print_solution(sol, fzn_model);
            });
        g_parallel_solver = nullptr;
        print_stats_brief(r.winner_stats);
        // ベンチ用: SABORI_PRINT_OBJ 設定時のみ目的値を stderr に出す
        // （通常出力・golden には影響しない）。
        if (std::getenv("SABORI_PRINT_OBJ") && r.objective) {
            std::cerr << "% objective = " << *r.objective << "\n";
        }

        if (r.status == sabori_csp::SearchResult::SAT && r.solution) {
            if (!find_all) print_solution(*r.solution, fzn_model);
            std::cout << (r.proved_optimal ? "==========\n" : "=====TIMEOUT=====\n");
        } else if (r.status == sabori_csp::SearchResult::UNSAT) {
            std::cout << "=====UNSATISFIABLE=====\n";
        } else {
            std::cout << "=====UNKNOWN=====\n";
        }
        return;
    }

    auto model = fzn_model.to_model(g_verbose, g_use_gac);
    if (!g_no_elimination) simplify_model(*model, fzn_model);
    sabori_csp::Solver solver;
    solver.set_verbose(g_verbose);
    solver.set_bisection_threshold(g_bisection_threshold);
    solver.set_probe_fail_limit(g_probe_fail_limit);
    if (g_no_nogood) solver.set_nogood_learning(false);
    if (g_conflict_learning) solver.set_conflict_learning(true);
    if (g_community_analysis) solver.set_community_analysis(true);
    g_current_solver = &solver;
    if (g_timeout_sec > 0) alarm(g_timeout_sec);

    // 目的変数のインデックスを検索
    size_t obj_var_idx = model->find_variable_index(objective_var_name);
    if (obj_var_idx == SIZE_MAX) {
        std::cerr << "Error: objective variable '" << objective_var_name << "' not found\n";
        std::cout << "=====UNKNOWN=====\n";
        return;
    }

    bool found_any = false;
    std::optional<sabori_csp::Solution> last_solution;

    auto result = solver.solve_optimize(*model, obj_var_idx, minimize,
        [&](const sabori_csp::Solution& sol) {
            found_any = true;
            if (find_all) {
                print_solution(sol, fzn_model);
            } else {
                last_solution = sol;
            }
            return true;
        });

    print_stats(solver, model.get());
    // ベンチ用: SABORI_PRINT_OBJ 設定時のみ目的値を stderr に出す（golden 不変）。
    if (std::getenv("SABORI_PRINT_OBJ") && result) {
        auto it = result->find(objective_var_name);
        if (it != result->end()) std::cerr << "% objective = " << it->second << "\n";
    }

    if (!found_any) {
        if (solver.is_stopped()) {
            std::cout << "=====UNKNOWN=====\n";
        } else {
            std::cout << "=====UNSATISFIABLE=====\n";
        }
    } else {
        if (!find_all && last_solution) {
            print_solution(*last_solution, fzn_model);
        }
        if (solver.is_stopped()) {
            std::cout << "=====TIMEOUT=====\n";
        } else {
            std::cout << "==========\n";
        }
    }
}

int main(int argc, char* argv[]) {
    bool find_all = false;
    const char* filename = nullptr;
    int timeout_sec = 0;
    int bisection_threshold = g_bisection_threshold;

    // スレッド数は SABORI_THREADS env でも指定できる（テスト用）。
    // 優先順位: CLI -j > 環境変数 > default(1)。env を先に読み、-j があれば上書きする。
    if (const char* e = std::getenv("SABORI_THREADS")) {
        g_num_threads = std::atoi(e);
        if (g_num_threads < 1) g_num_threads = 1;
    }

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-a") == 0) {
            find_all = true;
        } else if (std::strcmp(argv[i], "-s") == 0) {
            g_print_stats = true;
        } else if (std::strcmp(argv[i], "-v") == 0) {
            g_verbose = true;
        } else if (std::strcmp(argv[i], "-V") == 0 && i + 1 < argc) {
            g_verbose = true;
            g_verbose_worker = std::atoi(argv[++i]);
            if (g_verbose_worker < 0) g_verbose_worker = 0;
        } else if (std::strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            timeout_sec = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            bisection_threshold = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            g_probe_fail_limit = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-j") == 0 && i + 1 < argc) {
            g_num_threads = std::atoi(argv[++i]);
            if (g_num_threads < 1) g_num_threads = 1;
        } else if (std::strcmp(argv[i], "-c") == 0) {
            g_community_analysis = true;
        } else if (std::strcmp(argv[i], "-G") == 0) {
            g_use_gac = true;
        } else if (std::strcmp(argv[i], "-N") == 0) {
            g_no_nogood = true;
        } else if (std::strcmp(argv[i], "-C") == 0) {
            g_conflict_learning = true;
        } else if (std::strcmp(argv[i], "-E") == 0) {
            g_no_elimination = true;
        } else if (std::strcmp(argv[i], "-h") == 0 ||
                   std::strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            filename = argv[i];
        } else {
            std::cerr << "Unknown option: " << argv[i] << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    g_bisection_threshold = bisection_threshold;

    // Setup timeout (alarm is deferred until solver is created)
    g_timeout_sec = timeout_sec;
    if (timeout_sec > 0) {
        std::signal(SIGALRM, timeout_handler);
    }

    if (!filename) {
        print_usage(argv[0]);
        return 1;
    }

    try {
        // Parse FlatZinc file
        auto fzn_model = sabori_csp::fzn::parse_file(filename);

        // Dispatch based on solve kind
        switch (fzn_model->solve_decl().kind) {
            case sabori_csp::fzn::SolveKind::Satisfy:
                solve_satisfy(*fzn_model, find_all);
                break;
            case sabori_csp::fzn::SolveKind::Minimize:
                solve_optimize(*fzn_model, find_all, true);
                break;
            case sabori_csp::fzn::SolveKind::Maximize:
                solve_optimize(*fzn_model, find_all, false);
                break;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
