% bool 版 lex_lesseq。bool→int 係数変換を挟まないよう専用 predicate で受ける
%（sabori 内部では bool は 0/1 整数変数なので同じ propagator に落ちる）。
predicate sabori_lex_lesseq_bool(array [int] of var bool: x, array [int] of var bool: y);

predicate fzn_lex_lesseq_bool(array [int] of var bool: x, array [int] of var bool: y) =
  sabori_lex_lesseq_bool(x, y);
