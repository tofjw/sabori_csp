# Sabori CSP

A constraint satisfaction problem (CSP) solver written in C++ with FlatZinc support and MiniZinc integration.

## Features

- **FlatZinc solver** (`fzn_sabori`) compatible with the MiniZinc toolchain
- **Backtracking search** with constraint propagation
- **Supported constraints:**
  - Comparison: `int_eq`, `int_ne`, `int_lt`, `int_le`, `int_max` (with reification variants)
  - Arithmetic: `int_times`, `int_abs`
  - Linear: `int_lin_eq`, `int_lin_le`, `int_lin_ne` (with reification/implication variants)
  - Logical: `array_bool_and`, `array_bool_or`, `bool_clause`, `bool_not`
  - Global: `all_different`, `circuit`, `table_int`, `int_element`, `array_var_int_element`, `array_int_maximum`, `array_int_minimum`, `count_eq`, `disjunctive`
- **Python bindings** via pybind11

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

### Use with MiniZinc

Install the solver configuration so that MiniZinc can discover it, then run:

```bash
minizinc --solver "Sabori CSP" model.mzn data.dzn
```

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
