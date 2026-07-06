% Sabori native value_precede.
% std 分解（bool チェーン b[i] + 3n 個の reified 制約）を回避する。
predicate sabori_value_precede(int: s, int: t, array [int] of var int: x);

predicate fzn_value_precede_int(int: s, int: t, array [int] of var int: x) =
  sabori_value_precede(s, t, x);
