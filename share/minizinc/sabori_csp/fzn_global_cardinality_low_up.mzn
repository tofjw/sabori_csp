% low_up 変種も count 変数を導入して native の sabori_global_cardinality に
% 経路付けする（std 既定は cover 値ごとの count 比較に分解される）。
% 注: include 解決はファイル名単位なので fzn_global_cardinality.mzn とは
% 別ファイルにしないと std 定義と衝突する。
include "fzn_global_cardinality.mzn";

predicate fzn_global_cardinality_low_up(
  array [int] of var int: x,
  array [int] of int: cover,
  array [int] of int: lbound,
  array [int] of int: ubound,
) =
  let {
    array [int] of var int: counts = [
      let {
        var max(0, lbound[i])..min(length(x), ubound[i]): c;
      } in c
        | i in index_set(cover)
    ];
  } in sabori_global_cardinality(x, cover, counts);
