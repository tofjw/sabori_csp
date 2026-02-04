# sabori_csp_core library
add_library(sabori_csp_core
  domain.cpp
  variable.cpp
  constraint.cpp
  solver.cpp
  model.cpp
  # Constraints
  constraints/arithmetic.cpp
  constraints/comparison.cpp
  constraints/logical.cpp
  # Global constraints (split into separate files)
  constraints/global/all_different.cpp
  constraints/global/int_lin_eq.cpp
  constraints/global/int_lin_le.cpp
  constraints/global/int_lin_ne.cpp
  constraints/global/circuit.cpp
  constraints/global/int_element.cpp
  constraints/global/array_int_maximum.cpp
  constraints/global/array_int_minimum.cpp
  constraints/global/int_lin_eq_reif.cpp
  constraints/global/int_lin_ne_reif.cpp
  constraints/global/int_lin_le_reif.cpp
  constraints/global/int_lin_le_imp.cpp
)

target_include_directories(sabori_csp_core PUBLIC
  ${CMAKE_SOURCE_DIR}/include
)

# Alias for cleaner usage
add_library(sabori_csp::core ALIAS sabori_csp_core)
