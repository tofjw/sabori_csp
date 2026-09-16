% Sabori native lex_lesseq.
% std 分解（位置ごとの reified 比較 + bool チェーン）を回避する。
% lex_chain_* は std が連続ペアの lex_lesseq に分解するので自動的に native 化。
predicate sabori_lex_lesseq(array [int] of var int: x, array [int] of var int: y);

predicate fzn_lex_lesseq_int(array [int] of var int: x, array [int] of var int: y) =
  sabori_lex_lesseq(x, y);
