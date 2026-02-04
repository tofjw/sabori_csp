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
file(GLOB MZNLIB_FILES "${CMAKE_SOURCE_DIR}/share/minizinc/sabori_csp/*.mzn")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/share/minizinc/sabori_csp")
foreach(MZNLIB_FILE ${MZNLIB_FILES})
  get_filename_component(MZNLIB_FILENAME ${MZNLIB_FILE} NAME)
  configure_file(
    ${MZNLIB_FILE}
    "${CMAKE_BINARY_DIR}/share/minizinc/sabori_csp/${MZNLIB_FILENAME}"
    COPYONLY
  )
endforeach()
