cmake_minimum_required(VERSION 3.16)
project(sabori_csp VERSION 0.1.0 LANGUAGES CXX)

# C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find Bison and Flex (only needed for FlatZinc solver)
option(BUILD_FZN "Build FlatZinc solver (requires Bison/Flex)" ON)
if(BUILD_FZN)
  find_package(BISON REQUIRED)
  find_package(FLEX REQUIRED)
endif()

# FetchContent for dependencies
include(FetchContent)

# Catch2 for testing
option(BUILD_TESTS "Build tests" ON)
if(BUILD_TESTS)
  FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.2
  )
  FetchContent_MakeAvailable(Catch2)
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)

# Add subdirectories
add_subdirectory(src)

# Python bindings (optional)
option(BUILD_PYTHON_BINDINGS "Build Python bindings via pybind11" OFF)
if(BUILD_PYTHON_BINDINGS)
  add_subdirectory(python)
endif()

# Enable testing
if(BUILD_TESTS)
  enable_testing()
  if(EXISTS ${CMAKE_SOURCE_DIR}/tests/CMakeLists.txt)
    add_subdirectory(tests)
  endif()
endif()
