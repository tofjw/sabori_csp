# Python bindings using pybind11
find_package(Python3 COMPONENTS Interpreter Development REQUIRED)

# Try system/pip-installed pybind11 first, fall back to FetchContent
find_package(pybind11 2.12 QUIET)
if(NOT pybind11_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG v2.12.0
  )
  FetchContent_MakeAvailable(pybind11)
endif()

pybind11_add_module(_sabori_csp _bindings.cpp)
target_link_libraries(_sabori_csp PRIVATE sabori_csp_core)
target_include_directories(_sabori_csp PRIVATE ${CMAKE_SOURCE_DIR}/include)

# For development: output .so into source tree
set_target_properties(_sabori_csp PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/sabori_csp"
)

# For pip install: install .so into the package directory
install(TARGETS _sabori_csp LIBRARY DESTINATION sabori_csp)
