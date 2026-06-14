% Minimal trigger for MiniZinc 1249 flatten SEGV.
% Either of these two includes alone does NOT crash; both together DO.

include "nosets.mzn";
include "fzn_diffn_nonstrict.mzn";
