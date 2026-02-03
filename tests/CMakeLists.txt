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
