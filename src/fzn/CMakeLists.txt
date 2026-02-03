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
