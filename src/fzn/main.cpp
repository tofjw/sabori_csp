#include "sabori_csp/fzn/model.hpp"
#include "sabori_csp/solver.hpp"
#include "fzn_parser.hpp"
#include <iostream>
#include <string>
#include <cstring>
#include <optional>
#include <limits>

void print_usage(const char* program) {
    std::cerr << "Usage: " << program << " [-a] <file.fzn>\n";
    std::cerr << "  -a    Find all solutions (or all improving solutions for optimization)\n";
}

void print_solution(const sabori_csp::Solution& sol,
                   const sabori_csp::fzn::Model& model) {
    // Print output variables
    for (const auto& name : model.output_vars()) {
        auto it = sol.find(name);
        if (it != sol.end()) {
            std::cout << name << " = " << it->second << ";\n";
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
                auto elem_it = sol.find(elem);
                if (elem_it != sol.end()) {
                    std::cout << elem_it->second;
                }
            }
            std::cout << "];\n";
        }
    }

    std::cout << "----------\n";
}

/**
 * @brief 満足可能性問題を解く
 */
void solve_satisfy(sabori_csp::fzn::Model& fzn_model, bool find_all) {
    auto model = fzn_model.to_model();
    sabori_csp::Solver solver;

    if (find_all) {
        size_t count = solver.solve_all(*model, [&fzn_model](const sabori_csp::Solution& sol) {
            print_solution(sol, fzn_model);
            return true;
        });

        if (count == 0) {
            std::cout << "=====UNSATISFIABLE=====\n";
        } else {
            std::cout << "==========\n";
        }
    } else {
        auto sol = solver.solve(*model);
        if (sol) {
            print_solution(*sol, fzn_model);
            std::cout << "==========\n";
        } else {
            std::cout << "=====UNSATISFIABLE=====\n";
        }
    }
}

/**
 * @brief 最適化問題を解く（branch-and-bound）
 */
void solve_optimize(sabori_csp::fzn::Model& fzn_model, bool find_all, bool minimize) {
    const auto& objective_var_name = fzn_model.solve_decl().objective_var;
    std::optional<sabori_csp::Domain::value_type> best_cost;
    std::optional<sabori_csp::Solution> best_solution;
    bool found_any = false;

    // Activity スコア、NoGood、ヒント解を引き継ぐ
    std::map<std::string, double> activity_map;
    std::vector<sabori_csp::NamedNoGood> nogoods;
    std::optional<sabori_csp::Solution> hint_solution;
    constexpr size_t max_nogoods_to_inherit = 1000;  // 引き継ぐ NoGood の最大数

    while (true) {
        auto model = fzn_model.to_model();
        sabori_csp::Solver solver;

        // 前回の activity を引き継ぐ
        if (!activity_map.empty()) {
            solver.set_activity(activity_map, *model);
        }

        // 前回の NoGood を引き継ぐ
        if (!nogoods.empty()) {
            solver.add_nogoods(nogoods, *model);
        }

        // 前回のベスト解をヒントとして設定
        if (hint_solution) {
            solver.set_hint_solution(*hint_solution, *model);
        }

        auto sol = solver.solve(*model);

        // 今回の activity を保存（次回に引き継ぐ）
        activity_map = solver.get_activity_map(*model);

        // 今回の NoGood を保存（次回に引き継ぐ）
        nogoods = solver.get_nogoods(*model, max_nogoods_to_inherit);

        // 今回の解をヒントとして保存（次回に引き継ぐ）
        if (sol) {
            hint_solution = *sol;
        }
        if (!sol) {
            break;  // No more solutions
        }

        found_any = true;
        auto it = sol->find(objective_var_name);
        if (it == sol->end()) {
            // Objective variable not in solution (shouldn't happen)
            if (find_all) {
                print_solution(*sol, fzn_model);
            } else {
                best_solution = *sol;
            }
            break;
        }

        auto cost = it->second;

        // Check if this is an improving solution
        if (!best_cost ||
            (minimize && cost < *best_cost) ||
            (!minimize && cost > *best_cost)) {
            best_cost = cost;

            if (find_all) {
                // Output all improving solutions
                print_solution(*sol, fzn_model);
            } else {
                // Only keep the best solution for final output
                best_solution = *sol;
            }
        }

        // Tighten bound for next iteration
        if (minimize) {
            if (!fzn_model.set_var_upper_bound(objective_var_name, cost - 1)) {
                break;  // Can't improve further
            }
        } else {
            if (!fzn_model.set_var_lower_bound(objective_var_name, cost + 1)) {
                break;  // Can't improve further
            }
        }
    }

    if (!found_any) {
        std::cout << "=====UNSATISFIABLE=====\n";
    } else {
        if (!find_all && best_solution) {
            print_solution(*best_solution, fzn_model);
        }
        std::cout << "==========\n";
    }
}

int main(int argc, char* argv[]) {
    bool find_all = false;
    const char* filename = nullptr;

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-a") == 0) {
            find_all = true;
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
