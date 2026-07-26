# Generate parser and lexer
BISON_TARGET(FznParser parser.yy ${CMAKE_CURRENT_BINARY_DIR}/parser.cpp
  DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/parser.hpp
)
FLEX_TARGET(FznLexer lexer.ll ${CMAKE_CURRENT_BINARY_DIR}/lexer.cpp)
ADD_FLEX_BISON_DEPENDENCY(FznLexer FznParser)

# fzn_sabori executable
add_executable(fzn_sabori
  main.cpp
  model.cpp
  fzn_build_context.cpp
  constraint_registry.cpp
  fzn_parser.cpp
  ${BISON_FznParser_OUTPUTS}
  ${FLEX_FznLexer_OUTPUTS}
)

target_include_directories(fzn_sabori PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
  ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(fzn_sabori PRIVATE
  sabori_csp_core
)

# MiniZinc solver configuration
set(FZN_SABORI_EXECUTABLE "$<TARGET_FILE:fzn_sabori>")
set(SABORI_CSP_MZNLIB_DIR "${CMAKE_BINARY_DIR}/share/minizinc/sabori_csp")

# Generate msc file (use file(GENERATE) to support generator expressions)
file(GENERATE
  OUTPUT "${CMAKE_BINARY_DIR}/share/minizinc/solvers/sabori_csp.msc"
  CONTENT "{
  \"name\": \"sabori_csp\",
  \"version\": \"1.0.0\",
  \"id\": \"io.github.tofjw.sabori_csp\",
  \"executable\": \"$<TARGET_FILE:fzn_sabori>\",
  \"mznlib\": \"${CMAKE_BINARY_DIR}/share/minizinc/sabori_csp\",
  \"tags\": [\"cp\"],
  \"stdFlags\": [\"-a\"],
  \"supportsMzn\": false,
  \"supportsFzn\": true,
  \"needsSolns2Out\": true,
  \"needsMznExecutable\": false,
  \"needsStdlibDir\": false,
  \"isGUIApplication\": false
}
"
)

# Copy mznlib files to build directory
#
# 毎ビルド、出力先を丸ごと消してから配置し直す（差分コピーにしない）。
# 差分コピーだと、ブランチ切替などで share/ 側から消えた .mzn が build 側に残り、
# 未実装の sabori_* 述語を emit して実行時に "Unsupported constraint" になる。
# flatten は通ってしまうため原因を追いにくく、ベンチが黙って ERR になる。
#
# configure 時ではなくビルド時に走らせるのが要点。configure 時のコピーだけだと
# `cmake --build` しか実行しない場合に同期されない（ブランチ切替の直後がまさにこれ）。
set(SABORI_CSP_MZNLIB_SRC_DIR "${CMAKE_SOURCE_DIR}/share/minizinc/sabori_csp")

add_custom_target(sabori_csp_mznlib ALL
  COMMAND "${CMAKE_COMMAND}"
          "-DSRC=${SABORI_CSP_MZNLIB_SRC_DIR}"
          "-DDEST=${SABORI_CSP_MZNLIB_DIR}"
          -P "${CMAKE_SOURCE_DIR}/cmake/sync_mznlib.cmake"
  COMMENT "Syncing mznlib to ${SABORI_CSP_MZNLIB_DIR} (clean copy)"
  VERBATIM
)
add_dependencies(fzn_sabori sabori_csp_mznlib)
