% sabori_csp redefinitions
% This file includes all solver-specific redefinitions

include "nosets.mzn";

% predicate int_eq_imp(var int: a, var int: b, var bool: r);
% predicate bool_eq_imp(var bool: a, var bool: b, var bool: r);

include "fzn_all_different_int.mzn";
include "fzn_circuit.mzn";
include "fzn_table_int.mzn";
include "fzn_count_eq.mzn";
include "fzn_disjunctive.mzn";
include "fzn_disjunctive_strict.mzn";
include "fzn_diffn.mzn";
include "fzn_diffn_nonstrict.mzn";
