#include "sabori_csp/fzn/model.hpp"
#include "sabori_csp/solver.hpp"
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

std::atomic<bool> g_timeout_flag{false};
sabori_csp::Solver* g_current_solver = nullptr;
int g_timeout_sec = 0;

void timeout_handler(int) {
    g_timeout_flag = true;
    if (g_current_solver) {
        g_current_solver->stop();
    }
}

void print_usage(const char* program) {
    std::cerr << "Usage: " << program << " [-a] [-s] [-v] [-c] [-t SEC] [-b N] [-p N] <file.fzn>\n";
    std::cerr << "  -a      Find all solutions (or all improving solutions for optimization)\n";
    std::cerr << "  -s      Print solver statistics to stderr\n";
    std::cerr << "  -v      Verbose mode (print presolve/restart progress)\n";
    std::cerr << "  -c      Community analysis (print VIG/community/locality stats)\n";
    std::cerr << "  -t SEC  Timeout in seconds\n";
    std::cerr << "  -b N    Bisection threshold (default: 8, 0=disable)\n";
    std::cerr << "  -p N    Probe fail limit for improvement probe (default: 10, 0=disable)\n";
    std::cerr << "  -G      Use GAC (Régin's algorithm) for all_different\n";
    std::cerr << "  -N      Disable nogood learning\n";
    std::cerr << "  -E      Disable variable elimination\n";
}

bool g_print_stats = false;
bool g_verbose = false;
bool g_no_nogood = false;
bool g_no_elimination = false;
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
int g_bisection_threshold = 8;
int g_probe_fail_limit = 5;

void solve_satisfy(sabori_csp::fzn::Model& fzn_model, bool find_all) {
    auto model = fzn_model.to_model(g_verbose, g_use_gac);
    if (!g_no_elimination) simplify_model(*model, fzn_model);
    sabori_csp::Solver solver;
    solver.set_verbose(g_verbose);
    solver.set_bisection_threshold(g_bisection_threshold);
    if (g_no_nogood) solver.set_nogood_learning(false);
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
            print_solution(*sol, fzn_model);
            std::cout << "==========\n";
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

    auto model = fzn_model.to_model(g_verbose, g_use_gac);
    if (!g_no_elimination) simplify_model(*model, fzn_model);
    sabori_csp::Solver solver;
    solver.set_verbose(g_verbose);
    solver.set_bisection_threshold(g_bisection_threshold);
    solver.set_probe_fail_limit(g_probe_fail_limit);
    if (g_no_nogood) solver.set_nogood_learning(false);
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

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-a") == 0) {
            find_all = true;
        } else if (std::strcmp(argv[i], "-s") == 0) {
            g_print_stats = true;
        } else if (std::strcmp(argv[i], "-v") == 0) {
            g_verbose = true;
        } else if (std::strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            timeout_sec = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            bisection_threshold = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            g_probe_fail_limit = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "-c") == 0) {
            g_community_analysis = true;
        } else if (std::strcmp(argv[i], "-G") == 0) {
            g_use_gac = true;
        } else if (std::strcmp(argv[i], "-N") == 0) {
            g_no_nogood = true;
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
