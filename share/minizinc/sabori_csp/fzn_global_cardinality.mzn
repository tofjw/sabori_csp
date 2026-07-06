% Sabori native global_cardinality.
% 標準ライブラリの分解（cover 値ごとの count → reified 網）は FZN を肥大させる
% (mondoku で count_eq×408)。ネイティブ predicate として一括で受け取る。
% open 意味論: x は cover 外の値も取ってよい（closed は std が membership を足す）。
predicate sabori_global_cardinality(
  array [int] of var int: x,
  array [int] of int: cover,
  array [int] of var int: counts);

predicate fzn_global_cardinality(
  array [int] of var int: x,
  array [int] of int: cover,
  array [int] of var int: counts,
) = sabori_global_cardinality(x, cover, counts);
