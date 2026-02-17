# C++ unit tests with Catch2
add_executable(test_sabori_csp
  test_constraints.cpp
  test_domain_model.cpp
  test_global_constraints.cpp
  test_nogood.cpp
)

target_link_libraries(test_sabori_csp
  sabori_csp_core
  Catch2::Catch2WithMain
)

# Register tests with CTest
include(CTest)
include(Catch)
catch_discover_tests(test_sabori_csp)
