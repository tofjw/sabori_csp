# MiniZinc 1249 internal segfault during flatten — peaceable_queens regression

## Summary

MiniZinc snap revision **1249** (version 2.9.6) crashes with a segmentation
fault during the *flatten* phase when compiling
`mznc2022_probs/generalized-peacable-queens/peaceable_queens.mzn + n8_q3.json`
against a library that contains both of the following includes:

```mzn
include "nosets.mzn";
include "fzn_diffn_nonstrict.mzn";
```

The crash happens **before any solver executable is invoked** — it is purely an
internal MiniZinc bug triggered by the combination of the loaded library
redefinitions and the input model. Removing either include alone makes the
crash disappear.

**This is a new regression in 1249.** Revision 1222 (version 2.9.5) flattens
this model successfully. Conversely, the unrelated bug documented in
`../minizinc-1222-flatten-segfault/` (gfd-schedule on 1222) is **fixed** in
1249. The two reports describe different underlying bugs.

## Affected versions

- MiniZinc snap **1249** (`MiniZinc to FlatZinc converter, version 2.9.6, build 2476448810`)
- Snap auto-updated from 1222 → 1249 on 2026-04-24 around 20:45 JST
- Triggering model: `peaceable_queens.mzn` + `n8_q3.json`
  (MiniZinc Challenge 2022, `generalized-peacable-queens`)

## Reproduction

```sh
./repro.sh
```

The script invokes:

```sh
/snap/minizinc/1249/bin/minizinc --solver ./sabori_min.msc -c \
    --fzn /tmp/repro.fzn \
    peaceable_queens.mzn n8_q3.json
```

`sabori_min.msc` points at `/bin/true` as the solver executable, so the crash
cannot come from any downstream solver binary. Expected output:

```
minizinc version:
MiniZinc to FlatZinc converter, version 2.9.6, build 2476448810

Running -c (compile only, no solver)...
MiniZinc error: Memory violation detected (segmentation fault).
This is a bug. Please file a bug report using the MiniZinc bug tracker.

exit code: 134
REPRODUCED: minizinc crashed during flatten.
```

On revision 1222, the same invocation exits 0 (flatten succeeds).

## Bisect results

The minimal library that still triggers the crash on 1249 is:

```mzn
% library/redefinitions.mzn
include "nosets.mzn";
include "fzn_diffn_nonstrict.mzn";
```

Individual checks (`library/redefinitions.mzn` variants, with the same
`peaceable_queens.mzn` / `n8_q3.json` as input):

| Library content | 1249 result |
|---|---|
| `nosets` + `fzn_diffn_nonstrict` | **crash** |
| `nosets` only | no crash |
| `fzn_diffn_nonstrict` only | no crash |
| `fzn_diffn` + `fzn_diffn_nonstrict` (without `nosets`) | no crash |
| `nosets` + `fzn_diffn` (without `fzn_diffn_nonstrict`) | no crash |
| `nosets` + `fzn_diffn` + `fzn_diffn_nonstrict` (all three) | crash |

So the crash requires **both** `nosets.mzn` and `fzn_diffn_nonstrict.mzn` to be
loaded. Their joint presence is sufficient; `fzn_diffn.mzn` is not needed.

Notably:

- `peaceable_queens.mzn` does **not** itself use `diffn` or any set-typed
  variables. It uses `regular`, `all_equal`, `lex_lesseq`,
  `value_precede_chain`, `global_cardinality`. The crash is triggered purely
  by the presence of the two redefinition declarations in the loaded library.
- On revision 1222, these same two includes do not cause a crash on this
  model (confirmed by running `/snap/minizinc/1222/bin/minizinc` with the
  identical invocation).
- On revision 1249, the earlier
  `gfd-schedule + nosets + fzn_diffn_nonstrict + fzn_inverse + fzn_regular`
  trigger (see `../minizinc-1222-flatten-segfault/`) no longer crashes —
  that bug is fixed, and this one regressed.

## Files in this report

| File | Purpose |
|---|---|
| `README.md` | this document |
| `repro.sh` | one-shot reproducer |
| `sabori_min.msc` | minimal solver config pointing at `/bin/true` |
| `library/redefinitions.mzn` | the 2-line library that triggers the bug |
| `library/fzn_diffn_nonstrict.mzn` | predicate declaration referenced by the redefinition |
| `peaceable_queens.mzn` | model from MiniZinc Challenge 2022 |
| `n8_q3.json` | data file from MiniZinc Challenge 2022 |

## Workarounds

| Option | Effect | Cost |
|---|---|---|
| Drop `fzn_diffn_nonstrict.mzn` from `redefinitions.mzn` | Crash gone | `diffn_nonstrict` constraints fall back to MiniZinc's decomposition |
| Drop `nosets.mzn` | Crash gone | Models with set variables lose the solver-forced decomposition to integer vars |
| Pin minizinc to 1222 (`snap revert minizinc --revision=1222`) | Crash gone; gfd-schedule 1222 bug returns | Need to pick between two distinct regressions depending on the workload mix |

For the sabori_csp use case, dropping `fzn_diffn_nonstrict.mzn` is the
lightest workaround if the benchmark mix does not involve non-strict `diffn`.
Holding snap updates (`snap refresh --hold=forever minizinc`) while staying on
1222 keeps the non-regressing state for peaceable_queens.

## Upstream

Should be reported at <https://github.com/MiniZinc/libminizinc/issues>.
The reproducer in this directory is self-contained and does not require any
sabori binaries.
