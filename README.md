# Sabori CSP

A constraint solver (CSP / optimization) written from scratch in C++, FlatZinc-compatible and pluggable into the MiniZinc toolchain.

> *sabori* (サボり) is Japanese for *slacking off — cutting corners*. That's the design thesis, not an apology: the solver deliberately skips expensive machinery where correctness doesn't depend on it, and spends the saved budget on lightweight self-tuning heuristics instead.

[![MiniZinc Challenge 2026 — entrant](https://img.shields.io/badge/MiniZinc%20Challenge-2026%20entrant-blue)](https://www.minizinc.org/challenge/) ![C++17](https://img.shields.io/badge/C%2B%2B-17-blue) ![License: MIT](https://img.shields.io/badge/License-MIT-green)

## What makes it interesting

This is a hobby solver, not a Chuffed/OR-Tools competitor — but it makes a few unusual design bets worth a look if you care about CP/SAT internals:

- **"Poor man's explanation."** It runs **no lazy clause generation** and **no implication-graph analysis**. Conflict *learning* stays dumb and uniform (decision-trail NoGoods), while conflict *blame* (whose activity to bump) is heuristic-only — so it's allowed to be cheap and unsound, getting per-variable localization at dom/wdeg-like cost by just bumping the variables that moved. (I also layered per-constraint *structural* blame on top — `occupier_` for Circuit, the value pool for AllDifferent — then A/B tested it over 5 seeds: it buys nothing measurable over the generic version, so that elaboration is now future work. The honest negative result is in the write-up.) → [write-up (EN)](articles/poor-mans-explanation-en.md)
- **Bandit-tuned variable ordering.** The MRV-vs-activity mixing ratio is a 5-arm multi-armed bandit re-sampled every restart, instead of a fixed rule. It doesn't beat the best fixed heuristic — it *robustly avoids the worst* one per problem.
- **Adaptive inner/outer restarts** and a **pseudo-gradient** graft onto branch-and-bound for optimization. (There's also a Bloom-fingerprint NoGood-overlap tiebreak for variable selection — but A/B testing showed it's a no-op in 93% of cases: NoGood info already reaches variable selection through activity bumps, so the tiebreak rarely gets a turn. Needs a different use or removal. The write-up reports that honestly too.)
- **Honest negative results included.** Explicit community detection (VIG + label propagation) is shipped as *diagnostics only* — it didn't speed up search on any benchmark, because activity heuristics learn the same locality implicitly. That's documented, not hidden.

The full design tour, comparing every layer against CDCL / LCG / MiniZinc-family solvers: **[探索アルゴリズム解説 (JA)](articles/search-algorithm-explained.md)**.

The honest pitch: *no novel algorithm here* — just a standard backtracking + propagation + restart + activity + NoGood-learning skeleton, with a thin self-tuning layer on top, built to see how far cheap adaptation gets you without the heavy LCG apparatus.

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

After building, register the solver so that MiniZinc can discover it:

```bash
# Copy the solver config to MiniZinc's search path
mkdir -p ~/.minizinc/solvers
cp build/share/minizinc/solvers/sabori_csp.msc ~/.minizinc/solvers/

# Copy the solver library (redefinitions, predicate files)
cp -r build/share/minizinc/sabori_csp ~/.minizinc/
```

Verify the installation:

```bash
minizinc --solvers | grep sabori
```

Then run:

```bash
minizinc --solver sabori_csp model.mzn data.dzn
```

**Note:** The `.msc` file contains absolute paths to `fzn_sabori` and the solver library directory. If you move the build directory, regenerate by re-running `cmake --build build`.

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
