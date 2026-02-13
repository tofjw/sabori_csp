%-----------------------------------------------------------------------------%
% Constrains 'c' to be the number of elements in 'x' equal to 'y'.
% Native implementation for sabori_csp - do not decompose.
%-----------------------------------------------------------------------------%

predicate fzn_count_eq(array[int] of var int: x, var int: y, var int: c);
