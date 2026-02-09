%-----------------------------------------------------------------------------%
% Constrains 'x' to take a combination of values from the rows of 'table'.
% Native implementation for sabori_csp - do not decompose.
%-----------------------------------------------------------------------------%

predicate sabori_table_int(array[int] of var int: x, array[int] of int: t);

predicate fzn_table_int(array[int] of var int: x, array[int, int] of int: t) =
    sabori_table_int(x, array1d(t));
