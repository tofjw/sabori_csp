#include "sabori_csp/fzn/model.hpp"
#include "sabori_csp/solver.hpp"
#include "fzn_parser.hpp"
#include <iostream>
#include <string>
#include <cstring>
#include <optional>
#include <limits>
#include <csignal>
#include <atomic>

std::atomic<bool> g_timeout_flag{false};
sabori_csp::Solver* g_current_solver = nullptr;

void timeout_handler(int) {
    g_timeout_flag = true;
    if (g_current_solver) {
        g_current_solver->stop();
    }
}

void print_usage(const char* program) {
    std::cerr << "Usage: " << program << " [-a] [-s] [-v] [-t SEC] [-b N] <file.fzn>\n";
    std::cerr << "  -a      Find all solutions (or all improving solutions for optimization)\n";
    std::cerr << "  -s      Print solver statistics to stderr\n";
    std::cerr << "  -v      Verbose mode (print presolve/restart progress)\n";
    std::cerr << "  -t SEC  Timeout in seconds\n";
    std::cerr << "  -b N    Bisection threshold (default: 8, 0=disable)\n";
}

bool g_print_stats = false;
bool g_verbose = false;

void print_stats(const sabori_csp::Solver& solver) {
    if (!g_print_stats) return;
    const auto& s = solver.stats();
    std::cerr << "% Stats: fails=" << s.fail_count
              << " restarts=" << s.restart_count
              << " max_depth=" << s.max_depth
              << " avg_depth=" << (s.depth_count > 0 ? s.depth_sum / s.depth_count : 0)
              << " nogoods=" << s.nogoods_size
              << " bisect=" << s.bisect_count
              << " enumerate=" << s.enumerate_count
              << "\n";
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
                    }
                }
            }
            std::cout << "];\n";
        }
    }

    std::cout << "----------\n";
}

/**
 * @brief 充足可能性問題を解く
 */
int g_bisection_threshold = 8;

void solve_satisfy(sabori_csp::fzn::Model& fzn_model, bool find_all) {
    auto model = fzn_model.to_model();
    sabori_csp::Solver solver;
    solver.set_verbose(g_verbose);
    solver.set_bisection_threshold(g_bisection_threshold);
    g_current_solver = &solver;

    if (find_all) {
        size_t count = solver.solve_all(*model, [&fzn_model](const sabori_csp::Solution& sol) {
            print_solution(sol, fzn_model);
            return true;
        });

        print_stats(solver);
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
        print_stats(solver);
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

    auto model = fzn_model.to_model();
    sabori_csp::Solver solver;
    solver.set_verbose(g_verbose);
    solver.set_bisection_threshold(g_bisection_threshold);
    g_current_solver = &solver;

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

    print_stats(solver);

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
    int bisection_threshold = 8;

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

    // Setup timeout
    if (timeout_sec > 0) {
        std::signal(SIGALRM, timeout_handler);
        alarm(timeout_sec);
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
