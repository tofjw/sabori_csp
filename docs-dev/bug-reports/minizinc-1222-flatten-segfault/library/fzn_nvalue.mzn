%-----------------------------------------------------------------------------%
% Constrains 'n' to be the number of distinct values in 'x'.
% Native implementation for sabori_csp - do not decompose.
%-----------------------------------------------------------------------------%

predicate fzn_nvalue(var int: n, array[int] of var int: x);
