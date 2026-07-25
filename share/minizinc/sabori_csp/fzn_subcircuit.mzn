% Sabori native subcircuit.
% x[i] = j は「ノード i の次は j」、x[i] = i は「ノード i は閉路に含まれない」。
% 非自己ループのノードが単一の部分閉路を成す。std の分解（order 変数 + reified）は
% 大域伝播を持たないため、ネイティブ predicate として一括で受け取る。
predicate sabori_csp_subcircuit(array[int] of var int: x);

predicate fzn_subcircuit(array[int] of var int: x) =
    let {
        int: l = min(index_set(x));
        int: n = card(index_set(x));
    } in
    forall(i in index_set(x))(x[i] >= l /\ x[i] <= l + n - 1) /\
    sabori_csp_subcircuit(x);
