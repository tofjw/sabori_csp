#include "sabori_csp/fzn/model.hpp"
#include "sabori_csp/solver.hpp"
#include "fzn_parser.hpp"
#include <iostream>
#include <string>
#include <cstring>

void print_usage(const char* program) {
    std::cerr << "Usage: " << program << " [-a] <file.fzn>\n";
    std::cerr << "  -a    Find all solutions\n";
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

        // Convert to core model
        auto model = fzn_model->to_model();

        // Create solver
        sabori_csp::Solver solver;

        if (find_all) {
            // Find all solutions
            size_t count = solver.solve_all(*model, [&fzn_model](const sabori_csp::Solution& sol) {
                print_solution(sol, *fzn_model);
                return true;  // Continue searching
            });

            if (count == 0) {
                std::cout << "=====UNSATISFIABLE=====\n";
            } else {
                std::cout << "==========\n";
            }
        } else {
            // Find first solution
            auto sol = solver.solve(*model);
            if (sol) {
                print_solution(*sol, *fzn_model);
                std::cout << "==========\n";
            } else {
                std::cout << "=====UNSATISFIABLE=====\n";
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
