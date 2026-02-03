# sabori_csp_core library
add_library(sabori_csp_core
  domain.cpp
  variable.cpp
  constraint.cpp
  solver.cpp
  model.cpp
  # Constraints
  constraints/comparison.cpp
  constraints/global.cpp
)

target_include_directories(sabori_csp_core PUBLIC
  ${CMAKE_SOURCE_DIR}/include
)

# Alias for cleaner usage
add_library(sabori_csp::core ALIAS sabori_csp_core)
