% Sabori native subcircuit.
% std 分解（order 変数 n 個 + ノードごとの var element/reif 網 + 分岐対象の増加）
% を回避する。alldifferent の併設は registry 側で行う（circuit と同流儀）。
predicate sabori_subcircuit(array [int] of var int: x);

predicate fzn_subcircuit(array [int] of var int: x) = sabori_subcircuit(x);
