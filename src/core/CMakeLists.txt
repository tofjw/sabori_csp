# sabori_csp_core library
add_library(sabori_csp_core
  domain.cpp
  variable.cpp
  constraint.cpp
  solver.cpp
  nogood_manager.cpp
  variable_selector.cpp
  model.cpp
  model_simplifier.cpp
  community_analysis.cpp
  # Constraints
  constraints/arithmetic.cpp
  constraints/comparison.cpp
  constraints/logical.cpp
  # Global constraints (split into separate files)
  constraints/global/all_different.cpp
  constraints/global/all_different_gac.cpp
  constraints/global/all_different_except_0.cpp
  constraints/global/int_lin_eq.cpp
  constraints/global/int_lin_le.cpp
  constraints/global/int_lin_ne.cpp
  constraints/global/circuit.cpp
  constraints/global/int_element.cpp
  constraints/global/int_element_monotonic.cpp
  constraints/global/array_int_maximum.cpp
  constraints/global/array_int_minimum.cpp
  constraints/global/int_lin_eq_reif.cpp
  constraints/global/int_lin_ne_reif.cpp
  constraints/global/int_lin_le_reif.cpp
  constraints/global/int_lin_le_imp.cpp
  constraints/global/array_var_int_element.cpp
  constraints/global/table.cpp
  constraints/global/count_eq.cpp
  constraints/global/disjunctive.cpp
  constraints/global/diffn.cpp
  constraints/global/cumulative.cpp
  constraints/global/inverse.cpp
  constraints/global/regular.cpp
  constraints/global/nvalue.cpp
)

target_include_directories(sabori_csp_core PUBLIC
  ${CMAKE_SOURCE_DIR}/include
)

# Alias for cleaner usage
add_library(sabori_csp::core ALIAS sabori_csp_core)
