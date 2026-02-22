# Tests
add_subdirectory(cpp)

# FlatZinc integration tests
add_test(
    NAME TSP_optimal_cost
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/fzn/problems/tsp/test_tsp_optimal.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(TSP_optimal_cost PROPERTIES
    LABELS "fzn;optimization"
    TIMEOUT 300
)

# int_lin_eq substitution tests
add_test(
    NAME int_lin_eq_subst
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/fzn/constraints/int_lin_eq_subst/test_subst.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(int_lin_eq_subst PROPERTIES
    LABELS "fzn;constraint;substitution"
    TIMEOUT 60
)
