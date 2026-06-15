# C++ unit tests with Catch2
add_executable(test_sabori_csp
  test_constraints.cpp
  test_domain_model.cpp
  test_alldifferent.cpp
  test_linear_constraints.cpp
  test_graph_constraints.cpp
  test_element_table.cpp
  test_arith_logical.cpp
  test_solver_misc.cpp
  test_nogood.cpp
  test_sparse_set_pool.cpp
  test_mode_reward_policy.cpp
  test_restart_controller.cpp
  test_lin_reif_brute.cpp
  test_cmp_reif_brute.cpp
  test_diffn_brute.cpp
  test_disjunctive_brute.cpp
)

target_link_libraries(test_sabori_csp
  sabori_csp_core
  Catch2::Catch2WithMain
)

# Register tests with CTest
include(CTest)
include(Catch)
catch_discover_tests(test_sabori_csp)
