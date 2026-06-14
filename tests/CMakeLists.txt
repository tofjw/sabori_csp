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

# disjunctive_strict (zero-duration) regressions
add_test(
    NAME disjunctive_strict_regressions
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/fzn/constraints/disjunctive_strict/test_disjunctive_strict_regressions.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(disjunctive_strict_regressions PROPERTIES
    LABELS "fzn;constraint;disjunctive"
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

# Python binding tests (pybind11 ビルド時のみ。ABI 一致のため Python3_EXECUTABLE を使用)
if(BUILD_PYTHON_BINDINGS)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -m pytest --version
        RESULT_VARIABLE _pytest_rc OUTPUT_QUIET ERROR_QUIET
    )
    if(_pytest_rc EQUAL 0)
        add_test(
            NAME python_bindings
            COMMAND ${Python3_EXECUTABLE} -m pytest ${CMAKE_CURRENT_SOURCE_DIR}/python -q
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        )
        # 拡張は python/sabori_csp/ に出力されるため PYTHONPATH を通す
        set_tests_properties(python_bindings PROPERTIES
            LABELS "python"
            ENVIRONMENT "PYTHONPATH=${CMAKE_SOURCE_DIR}/python"
            TIMEOUT 120
        )
    else()
        message(STATUS "pytest が ${Python3_EXECUTABLE} で見つからないため python_bindings テストをスキップ")
    endif()
endif()

# ゴールデンマスター: 代表 fzn 約180本の -a -s 出力を照合しリファクタ不変性を検証。
# 実際にビルドした fzn_sabori を FZN_SABORI 経由で渡す（build ディレクトリ非依存）。
if(BUILD_FZN)
    add_test(
        NAME golden_master
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/golden/run_golden.sh check
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(golden_master PROPERTIES
        LABELS "golden;fzn"
        ENVIRONMENT "FZN_SABORI=$<TARGET_FILE:fzn_sabori>"
        TIMEOUT 120
    )
endif()
