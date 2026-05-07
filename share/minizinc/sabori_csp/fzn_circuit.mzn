%-----------------------------------------------------------------------------%
% Constrains the elements of 'x' to define a circuit.
% Native implementation for sabori_csp.
%
% Index information is lost at the FlatZinc level, so here we restrict the
% value range to [min(index_set(x)) .. min(index_set(x)) + n - 1] before
% calling the native implementation sabori_csp_circuit.
% This allows non-1-based index sets (e.g., array[2..5]) to be handled
% correctly.
%-----------------------------------------------------------------------------%

predicate sabori_csp_circuit(array[int] of var int: x);

predicate fzn_circuit(array[int] of var int: x) =
    let {
        int: l = min(index_set(x));
        int: n = card(index_set(x));
    } in
    forall(i in index_set(x))(x[i] >= l /\ x[i] <= l + n - 1) /\
    sabori_csp_circuit(x);
