# MiniZinc 1222 internal segfault during flatten with sabori_csp library

## Summary

`/snap/bin/minizinc` (snap version 1222) crashes with a segmentation fault during
the *flatten* phase when compiling certain models against the `sabori_csp` MiniZinc
library. The crash happens **before any solver executable is invoked** — it is purely
an internal MiniZinc bug triggered by the combination of predicate redefinitions
loaded from `sabori_csp/redefinitions.mzn`, plus the input model.

`fzn_sabori` is **not** involved; running with `-c` (compile only) or pointing the
`executable` field at `/bin/true` reproduces it identically.

## Affected versions

- MiniZinc snap **1222**
- sabori_csp `mznlib/sabori_csp/redefinitions.mzn` at HEAD `eee750f` (2026-04-15)
- Triggering model: `mznc2016_probs/gfd-schedule/gfd-schedule2.mzn` with
  `n25f5d20m10k3.dzn` (MiniZinc Challenge 2016)

## Reproduction

```sh
./repro.sh
```

The script invokes:

```sh
/snap/bin/minizinc --solver ./sabori_min.msc -c \
    --fzn /tmp/repro.fzn \
    gfd-schedule2.mzn n25f5d20m10k3.dzn
```

`sabori_min.msc` points at `/bin/true` as the solver executable, so the crash
cannot come from sabori's own binary. Expected output:

```
Flattening ...
        CompilePass: Flatten with './library' library ...
MiniZinc error: Memory violation detected (segmentation fault).
This is a bug. Please file a bug report using the MiniZinc bug tracker.
exit code: 134
REPRODUCED: minizinc crashed during flatten.
```

## Files in this report

| File | Purpose |
|---|---|
| `README.md` | this document |
| `repro.sh` | one-shot reproducer |
| `sabori_min.msc` | minimal solver config pointing at `/bin/true` |
| `library/redefinitions.mzn` | the sabori_csp redefinitions library that triggers the bug |
| `library/fzn_*.mzn` | sabori predicate declarations referenced by the redefinitions |
| `gfd-schedule2.mzn` | model from MiniZinc Challenge 2016 |
| `n25f5d20m10k3.dzn` | data file from MiniZinc Challenge 2016 |

## Bisect results

The crash requires the simultaneous presence of **`nosets.mzn` + at least three
specific sabori redefinition includes**. Removing any one of the following four
includes from `library/redefinitions.mzn` makes the crash disappear:

```mzn
include "nosets.mzn";
include "fzn_diffn_nonstrict.mzn";
include "fzn_inverse.mzn";
include "fzn_regular.mzn";
```

- Disabling `nosets.mzn` → no crash
- Disabling `fzn_diffn_nonstrict.mzn` → no crash
- Disabling `fzn_inverse.mzn` → no crash
- Disabling `fzn_regular.mzn` → no crash
- Each of the four enabled in isolation → no crash
- Each pair from the three sabori predicates enabled (with `nosets`) → no crash
- All four enabled → **crash**

Notably:

- `gfd-schedule2.mzn` does **not** itself use `regular`, `inverse`, or `diffn`.
  It only uses `cumulative`, `nvalue`, and `at_most`. The crash is triggered
  purely by the *presence* of the three redefinition declarations in the loaded
  library.
- The crash is independent of whether sabori's own `fzn_cumulative.mzn`,
  `fzn_nvalue.mzn`, etc. are included — disabling them does not help.
- Reordering the four includes in `redefinitions.mzn` does not help.
- Replacing the `fzn_regular` wrapper definition with a flat declaration does
  not help.
- A trivial model (`var 1..10: x; solve satisfy;`) does **not** reproduce, even
  with the same library — the bug requires both the library state and something
  about `gfd-schedule2.mzn`'s flatten path.

## Verbose minizinc output (last lines before crash)

```
processing file '/snap/minizinc/1222/share/minizinc/std/fzn_nvalue_reif.mzn'
processing data file 'n25f5d20m10k3.dzn'
 done parsing (0.03 s)
Flattening ...
        CompilePass: Flatten with './library' library ...
MiniZinc error: Memory violation detected (segmentation fault).
```

## Workarounds (in sabori_csp)

| Option | Effect | Cost |
|---|---|---|
| Drop `nosets.mzn` from `redefinitions.mzn` | Crash gone | Models with set vars will fail to flatten (sabori has no native set support) |
| Drop `fzn_regular.mzn` include | Crash gone | `regular` constraints fall back to MiniZinc's bool-array decomposition (slower) |
| Drop `fzn_inverse.mzn` or `fzn_diffn_nonstrict.mzn` include | Crash gone | Corresponding constraints fall back to decomposition |

The "least worst" workaround is dropping `fzn_regular.mzn` since `regular` has
the lowest occurrence rate in the MiniZinc Challenge benchmarks and the
decomposition is a complete fallback.

## Upstream

Should be reported at <https://github.com/MiniZinc/libminizinc/issues>.
The reproducer in this directory is self-contained and does not require any
sabori binaries.
