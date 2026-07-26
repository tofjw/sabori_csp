# mznlib をソースからビルドディレクトリへ「クリーンコピー」する
#
# cmake -P sync_mznlib.cmake -DSRC=<source mznlib dir> -DDEST=<build mznlib dir>
#
# 出力先を丸ごと消してから配置するのが要点。差分コピーだと、ブランチ切替などで
# SRC 側から消えた .mzn が DEST に残り、未実装の sabori_* 述語を emit して
# 実行時に "Unsupported constraint" になる（flatten は通るため原因を追いにくい）。

if(NOT DEFINED SRC OR NOT DEFINED DEST)
  message(FATAL_ERROR "sync_mznlib.cmake: SRC と DEST の両方を指定すること")
endif()

if(NOT IS_DIRECTORY "${SRC}")
  message(FATAL_ERROR "sync_mznlib.cmake: SRC が存在しない: ${SRC}")
endif()

file(REMOVE_RECURSE "${DEST}")
file(MAKE_DIRECTORY "${DEST}")
file(COPY "${SRC}/" DESTINATION "${DEST}" FILES_MATCHING PATTERN "*.mzn")
