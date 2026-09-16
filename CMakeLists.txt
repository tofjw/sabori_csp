cmake_minimum_required(VERSION 3.16)

# In-source ビルド禁止（生成物がソースツリーを汚染するのを防ぐ）
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR
    "In-source build は禁止です。別ディレクトリでビルドしてください:\n"
    "  cmake -B build -DCMAKE_BUILD_TYPE=Release\n"
    "  cmake --build build")
endif()

project(sabori_csp VERSION 0.1.0 LANGUAGES CXX)

# C++17
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ターゲットアーキテクチャ（SIMD 自動ベクトル化の有効化）
# 既定 x86-64-v3 = AVX2 + BMI2 + FMA（配布可能な現行 CPU の共通基盤）。
# ローカル最速を測るなら -DSABORI_MARCH=native（AVX-512 も解禁）。
# 無効化は -DSABORI_MARCH=OFF。
set(SABORI_MARCH "x86-64-v3" CACHE STRING "Target arch for SIMD (-march=...); 'OFF' to disable")
if(NOT SABORI_MARCH STREQUAL "OFF")
  include(CheckCXXCompilerFlag)
  set(_sabori_arch_flag "-march=${SABORI_MARCH}")
  check_cxx_compiler_flag("${_sabori_arch_flag}" SABORI_MARCH_SUPPORTED)
  if(SABORI_MARCH_SUPPORTED)
    add_compile_options("${_sabori_arch_flag}")
    message(STATUS "SIMD: enabling ${_sabori_arch_flag}")
  else()
    message(WARNING "SIMD: compiler rejects ${_sabori_arch_flag}; building without it")
  endif()
endif()

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
