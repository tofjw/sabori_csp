% seq_precede_chain(X): 正の値 j+1 の初出は j の初出より後。
% j >= 1 の連続ペアへの value_precede 連言と解集合が一致する
% (0 以下の値は無制約 = std の H = max(X, 0) と同じ扱い)。
% std 既定の H[i] = max(X[i], H[i-1]) チェーン分解 (int_max×n + reif) を回避する。
include "fzn_value_precede_int.mzn";

predicate fzn_seq_precede_chain_int(array [int] of var int: X) =
  forall (j in 1..ub_array(X) - 1) (
    sabori_value_precede(j, j + 1, X)
  );
