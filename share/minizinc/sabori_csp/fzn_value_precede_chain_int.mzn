% value_precede_chain を native value_precede の連続ペア連言に経路付けする。
% std 既定はマッピング変数 + seq_precede_chain (max チェーン) に分解され
% int_max/element/reif が値数×変数数で積まれる (community-detection 等)。
include "fzn_value_precede_int.mzn";

predicate fzn_value_precede_chain_int(
  array [int] of int: T,
  array [int] of var int: X,
) =
  forall (i in index_set(T) where i > min(index_set(T))) (
    sabori_value_precede(T[i - 1], T[i], X)
  );
