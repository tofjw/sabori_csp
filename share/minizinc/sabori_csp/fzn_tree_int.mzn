% Sabori native tree（無向木連結性）。
% 選択部分グラフ (ns, es) が根 r の木を成す。std 分解は parent/dist の距離
% ラベリング + reified で大域連結性 propagator を持たないため、ネイティブ
% predicate として一括で受け取り union-find で閉路検出/予防する。
predicate sabori_tree(
    int: N,
    int: E,
    array [int] of int: from,
    array [int] of int: to,
    var int: r,
    array [int] of var bool: ns,
    array [int] of var bool: es);

predicate fzn_tree(
    int: N,
    int: E,
    array [int] of int: from,
    array [int] of int: to,
    var int: r,
    array [int] of var bool: ns,
    array [int] of var bool: es,
) = sabori_tree(N, E, from, to, r, ns, es);
