% Sabori native lex_less（厳密辞書順）。
predicate sabori_lex_less(array [int] of var int: x, array [int] of var int: y);

predicate fzn_lex_less_int(array [int] of var int: x, array [int] of var int: y) =
  sabori_lex_less(x, y);
