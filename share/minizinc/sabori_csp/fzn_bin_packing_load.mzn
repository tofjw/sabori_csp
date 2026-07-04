% Sabori native bin_packing_load.
% 標準ライブラリの分解（ビン×item 個の reified 制約）は flatten 爆発を招くため、
% sabori はネイティブ predicate として一括で受け取る。
predicate sabori_bin_packing_load(
  array [int] of var int: load,
  array [int] of var int: bin,
  array [int] of int: w);

predicate fzn_bin_packing_load(
  array [int] of var int: load,
  array [int] of var int: bin,
  array [int] of int: w,
) = sabori_bin_packing_load(load, bin, w);
