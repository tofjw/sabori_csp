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

# int_div regressions
add_test(
    NAME int_div_regressions
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/fzn/constraints/int_div/test_int_div_regressions.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(int_div_regressions PROPERTIES
    LABELS "fzn;constraint;int_div"
    TIMEOUT 60
)

# cumulative TTEF regressions
add_test(
    NAME cumulative_ttef_regressions
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/fzn/constraints/cumulative_ttef/test_ttef_regressions.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(cumulative_ttef_regressions PROPERTIES
    LABELS "fzn;constraint;cumulative"
    TIMEOUT 60
)

# FlatZinc output format compliance (separator markers)
add_test(
    NAME fzn_output_format
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/fzn/output_format/test_output_format.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(fzn_output_format PROPERTIES
    LABELS "fzn;output"
    TIMEOUT 60
)
