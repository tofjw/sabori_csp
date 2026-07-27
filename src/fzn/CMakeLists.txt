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
#
# .msc は 2 種類生成する（CMake の build-tree export / install-tree export と同じ考え方）。
#   - build ツリー用: build ディレクトリを絶対パスで指す。ベンチマークと手動の
#     MiniZinc 確認がこれを使う。開発ループに install を挟まないための存在。
#   - インストール用: インストール先を「相対パス」で指す（下の Installation 参照）。
#
# JSON の本体は share/minizinc/solvers/sabori_csp.msc.in ただ 1 箇所。
# 埋め込む 2 つのパスだけを SABORI_MSC_EXECUTABLE / SABORI_MSC_MZNLIB で差し替える。
set(SABORI_CSP_MSC_TEMPLATE "${CMAKE_SOURCE_DIR}/share/minizinc/solvers/sabori_csp.msc.in")
set(SABORI_CSP_MZNLIB_DIR "${CMAKE_BINARY_DIR}/share/minizinc/sabori_csp")

# build ツリー用の .msc
#
# 実行ファイルのパスはジェネレータ式 $<TARGET_FILE:> でしか正確に取れないが、
# configure_file はジェネレータ式を展開できない。そこで
#   configure_file で @VAR@ を「ジェネレータ式のまま」埋める → file(GENERATE) で展開
# の 2 段構えにする。中間ファイルは直接使わない。
set(SABORI_MSC_EXECUTABLE "$<TARGET_FILE:fzn_sabori>")
set(SABORI_MSC_MZNLIB "${SABORI_CSP_MZNLIB_DIR}")
configure_file("${SABORI_CSP_MSC_TEMPLATE}"
               "${CMAKE_CURRENT_BINARY_DIR}/sabori_csp.msc.genexp" @ONLY)
file(GENERATE
  OUTPUT "${CMAKE_BINARY_DIR}/share/minizinc/solvers/sabori_csp.msc"
  INPUT "${CMAKE_CURRENT_BINARY_DIR}/sabori_csp.msc.genexp"
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

# ---------------------------------------------------------------------------
# Installation
#
#   cmake --install build --prefix ~/.local
#
# を実行すると、以下が配置される:
#   <prefix>/bin/fzn_sabori
#   <prefix>/share/minizinc/sabori_csp/*.mzn
#   <prefix>/share/minizinc/solvers/sabori_csp.msc
#
# インストール先の .msc は絶対パスを埋めない。MiniZinc は .msc 内の相対パスを
# 「.msc ファイル自身の位置」を基準に解決するので、相対パスで書いておけば
# ツリーごと移動しても、prefix を変えても、そのまま動く。
# （逆に configure 時の CMAKE_INSTALL_PREFIX を焼き込むと、あとから
#   `cmake --install --prefix ...` で prefix を上書きされたときに嘘になる。）
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

set(SABORI_CSP_INSTALL_MZNDIR "${CMAKE_INSTALL_DATADIR}/minizinc")

# .msc の置き場所から見た、実行ファイルと mznlib への相対パスを求める。
# 両者とも prefix 配下なので、この相対関係は prefix に依存しない。
if(IS_ABSOLUTE "${CMAKE_INSTALL_BINDIR}" OR IS_ABSOLUTE "${CMAKE_INSTALL_DATADIR}")
  # BINDIR/DATADIR に絶対パスを指定された場合は相対化できないので絶対パスで書く
  message(STATUS "sabori_csp: absolute CMAKE_INSTALL_BINDIR/DATADIR -> .msc uses absolute paths")
  set(SABORI_MSC_EXECUTABLE "${CMAKE_INSTALL_FULL_BINDIR}/fzn_sabori")
  set(SABORI_MSC_MZNLIB "${CMAKE_INSTALL_PREFIX}/${SABORI_CSP_INSTALL_MZNDIR}/sabori_csp")
else()
  set(_msc_dir "/${SABORI_CSP_INSTALL_MZNDIR}/solvers")
  file(RELATIVE_PATH SABORI_MSC_EXECUTABLE "${_msc_dir}" "/${CMAKE_INSTALL_BINDIR}/fzn_sabori")
  file(RELATIVE_PATH SABORI_MSC_MZNLIB "${_msc_dir}" "/${SABORI_CSP_INSTALL_MZNDIR}/sabori_csp")
  unset(_msc_dir)
endif()

configure_file("${SABORI_CSP_MSC_TEMPLATE}"
               "${CMAKE_CURRENT_BINARY_DIR}/install/sabori_csp.msc" @ONLY)

install(TARGETS fzn_sabori RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

# mznlib はビルドツリー側のコピーではなくソースの正本から入れる
install(DIRECTORY "${SABORI_CSP_MZNLIB_SRC_DIR}"
        DESTINATION "${SABORI_CSP_INSTALL_MZNDIR}"
        FILES_MATCHING PATTERN "*.mzn")

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/install/sabori_csp.msc"
        DESTINATION "${SABORI_CSP_INSTALL_MZNDIR}/solvers")

# BUILD_SHARED_LIBS=ON のときは core も入れないと実行ファイルが動かない。
# 既定（static）では何もしない。
if(BUILD_SHARED_LIBS)
  set_target_properties(fzn_sabori PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
  install(TARGETS sabori_csp_core LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endif()
