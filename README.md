# Sabori CSP

A constraint solver (CSP / optimization) written from scratch in C++, FlatZinc-compatible and pluggable into the MiniZinc toolchain.

> *sabori* (サボり) is Japanese for *slacking off — cutting corners*. That's the design thesis, not an apology: the solver deliberately skips expensive machinery where correctness doesn't depend on it, and spends the saved budget on lightweight self-tuning heuristics instead.

[![MiniZinc Challenge 2026 — entrant](https://img.shields.io/badge/MiniZinc%20Challenge-2026%20entrant-blue)](https://www.minizinc.org/challenge/) ![C++17](https://img.shields.io/badge/C%2B%2B-17-blue) ![License: MIT](https://img.shields.io/badge/License-MIT-green)

## What makes it interesting

This is a hobby solver, not a Chuffed/OR-Tools competitor. What's distinctive is *how* it cuts wasted search, easiest to state by contrast with the mainstream LCG/CDCL line (lazy clause generation, à la Chuffed) — which turns conflicts into sound logical constraints and prunes deductively:

> **LCG stops wasted search with *logic* — sound learned clauses prune deductively. sabori_csp stops it with *tendency* — it feeds the same conflict info into the variable-selection heuristic, a conflict-directed lean toward recently-conflicted variables.**

It's deliberately lightweight — **no LCG, no implication-graph analysis, no per-propagator explanations** — leaning on cheap, self-tuning heuristics instead. Every design bet below was A/B-tested rather than asserted, and the wins cluster on the *foundation*, not the cleverness layered on top:

- **What does the work:** variable selection itself — a conflict-directed primary criterion (a **Last-Conflict**-style mechanism) picks the most-recently-conflicted variable after a backtrack (the single most effective component), while **activity drives the descent** (its weight is masked by the former, but large once you turn that off). The weak decision-trail NoGood (no LCG, no 1-UIP — yet turning it off consistently hurts) feeds that activity; a 5-arm bandit tunes the activity-vs-MRV mix (robustly avoids the *worst* fixed choice); a one-hot channeling presolve cuts search effort dramatically on some problems.
- **No measurable gain (reported, not hidden):** a Bloom-fingerprint NoGood-overlap tiebreak (almost always a no-op — activity already carries the signal); per-constraint *structural* conflict-blame, a "poor man's explanation" I was proud of (no gain over the dumb generic version → future work).
- **Problem-dependent:** a pseudo-gradient value hint (negative on average, but helps design/assignment and backfires on resource-coupled scheduling → portfolio-only).
- **Honest negatives kept in:** community detection (VIG + label propagation) ships as *diagnostics only* — it didn't speed up search, because activity learns the same locality implicitly.

The consistent lesson: the conflict-directed selection foundation works; the cleverness piled on top mostly doesn't, because the foundation already decides search. (Exact tables and methodology are in the write-ups.)

Full write-up, every layer measured against CDCL / LCG / MiniZinc-family solvers:
**[the short version (EN, start here)](articles/mznc2026/search-algorithm-en-short.md)** · **[full write-up (EN)](articles/mznc2026/search-algorithm-explained-en.md)** · **[日本語 短縮](articles/mznc2026/search-algorithm-short.md)** · **[日本語 全文](articles/mznc2026/search-algorithm-explained.md)**

The honest pitch: *no novel algorithm here* — a standard backtracking + propagation + restart + activity + NoGood-learning skeleton with a thin self-tuning layer on top, built (and measured) to see how far cheap tendency-control gets you without the heavy LCG apparatus. (Not a claim that deduction is redundant: sabori carries only the weakest learning by design, so on classes where strong learning is decisive, tendency alone won't reach — that's outside what's measured here.)

## Features

- **FlatZinc solver** (`fzn_sabori`) compatible with the MiniZinc toolchain
- **Backtracking search** with constraint propagation
- **Supported constraints:**
  - Comparison: `int_eq`, `int_ne`, `int_lt`, `int_le`, `int_max` (with reification variants)
  - Arithmetic: `int_times`, `int_abs`
  - Linear: `int_lin_eq`, `int_lin_le`, `int_lin_ne` (with reification/implication variants)
  - Logical: `array_bool_and`, `array_bool_or`, `bool_clause`, `bool_not`
  - Global: `all_different`, `all_different_except_0`, `circuit`, `table_int`, `int_element`, `array_var_int_element`, `array_int_maximum`, `array_int_minimum`, `count_eq`, `disjunctive`, `diffn`, `cumulative`, `inverse`, `regular`, `nvalue`
- **Python bindings** via pybind11 (`sabori_csp.core`)

## Requirements

- C++17 compiler
- CMake 3.16+
- Bison & Flex
- (Optional) Python 3.9+ for Python bindings
- (Optional) [MiniZinc](https://www.minizinc.org/) for `.mzn` model support

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## Usage

### Solve a FlatZinc file

```bash
./build/src/fzn/fzn_sabori problem.fzn
```

### Find all solutions

```bash
./build/src/fzn/fzn_sabori -a problem.fzn
```

### Install as a MiniZinc solver

```bash
cmake --install build --prefix ~/.local
```

This installs:

```
~/.local/
├── bin/fzn_sabori
└── share/minizinc/
    ├── sabori_csp/*.mzn            # solver library (redefinitions, predicate files)
    └── solvers/sabori_csp.msc      # relative paths to both of the above
```

Register it with MiniZinc — either put the `solvers/` directory on the search path, or symlink the single `.msc` into MiniZinc's user config directory:

```bash
export MZN_SOLVER_PATH="$HOME/.local/share/minizinc/solvers"
# or:
mkdir -p ~/.minizinc/solvers
ln -sf ~/.local/share/minizinc/solvers/sabori_csp.msc ~/.minizinc/solvers/

minizinc --solvers | grep sabori          # verify discovery
minizinc --solver sabori_csp model.mzn data.dzn
```

The installed `.msc` uses paths relative to itself, so the installed tree can be moved or installed under any prefix and keeps working. **Do not copy the `.msc` alone to a different directory** — it resolves `../sabori_csp` relative to its own location, so it only works from inside the installed tree (symlinking is fine; MiniZinc resolves through the link).

### Use directly from the build tree

For development, benchmarking, and debugging there is no need to install anything. The build also emits a `.msc` that points into the build directory with absolute paths, kept in sync on every build:

```bash
minizinc --solver "$PWD/build/share/minizinc/solvers/sabori_csp.msc" model.mzn data.dzn
# or
export MZN_SOLVER_PATH="$PWD/build/share/minizinc/solvers"
```

Since those paths are absolute, moving or deleting the build directory breaks this one; re-run `cmake -B build && cmake --build build` to regenerate it.

**Don't hand-copy the solver library into `~/.minizinc/sabori_csp`.** Both `.msc` files already point at a library directory of their own, so a hand-made copy is never read — it just goes stale and can shadow the real solver by name in `minizinc --solvers`. If you have such leftovers from an older setup, remove them:

```bash
rm -rf ~/.minizinc/solvers/sabori_csp.msc ~/.minizinc/sabori_csp
```

## Python

### Install

```bash
pip install .
```

For development (editable install):

```bash
pip install ninja   # first time only
pip install -e .
```

Bison/Flex are not required when installing the Python package alone.

### Usage

```python
from sabori_csp.core import Model, Solver, AllDifferentConstraint

m = Model()
xs = [m.create_variable(f"x{i}", 1, 5) for i in range(5)]
m.add_constraint(AllDifferentConstraint(xs))

s = Solver()
solution = s.solve(m)
print(solution)  # {'x0': 2, 'x1': 5, 'x2': 4, 'x3': 1, 'x4': 3}
```

`sabori_csp.core` provides the following classes:

- **Core**: `Model`, `Solver`, `SolverStats`, `Domain`, `Variable`, `Constraint`
- **Comparison**: `IntEqConstraint`, `IntNeConstraint`, `IntLtConstraint`, `IntLeConstraint`, `IntMaxConstraint`, `IntMinConstraint` + Reif/Imp variants
- **Arithmetic**: `IntTimesConstraint`, `IntAbsConstraint`, `IntModConstraint`, `IntDivConstraint`
- **Linear**: `IntLinEqConstraint`, `IntLinLeConstraint`, `IntLinNeConstraint` + Reif/Imp variants
- **Logical**: `ArrayBoolAndConstraint`, `ArrayBoolOrConstraint`, `BoolClauseConstraint`, `BoolNotConstraint`, `ArrayBoolXorConstraint`, `BoolXorConstraint`
- **Global**: `AllDifferentConstraint`, `AllDifferentGACConstraint`, `AllDifferentExcept0Constraint`, `CircuitConstraint`, `IntElementConstraint`, `ArrayVarIntElementConstraint`, `ArrayIntMaximumConstraint`, `ArrayIntMinimumConstraint`, `TableConstraint`, `CountEqConstraint`, `DisjunctiveConstraint`, `DiffnConstraint`, `CumulativeConstraint`, `InverseConstraint`, `RegularConstraint`, `NValueConstraint`

## Testing

```bash
# All tests
ctest --test-dir build

# C++ unit tests (Catch2)
./build/tests/cpp/test_sabori_csp "[constraint]"

# FlatZinc integration tests
pytest tests/fzn/run_tests.py -v
```

## Project Structure

```
├── include/sabori_csp/     # Public C++ headers
├── src/
│   ├── core/               # Core solver library
│   └── fzn/                # FlatZinc frontend (Bison/Flex parser)
├── python/                 # pybind11 bindings
├── share/minizinc/         # MiniZinc solver configuration
├── tests/
│   ├── cpp/                # Catch2 unit tests
│   ├── python/             # pytest
│   └── fzn/                # FlatZinc integration tests
└── docs/                   # Documentation
```

## License

MIT License. See [LICENSE](LICENSE) for details.
