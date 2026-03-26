# Python Interface for Sabori CSP

`sabori_csp` provides two layers of Python access to the Sabori CSP constraint solver:

| Layer | Import | Description |
|-------|--------|-------------|
| **High-level API** | `from sabori_csp import ...` | Pythonic modeling with operator overloading, automatic auxiliary variables, and global constraint helpers |
| **Core API** | `from sabori_csp.core import ...` | Direct pybind11 bindings to C++ classes — full control, no magic |

## Installation

```bash
pip install .
```

For development (editable install):

```bash
pip install ninja   # first time only
pip install -e .
```

Bison/Flex are **not** required when installing the Python package alone.

### Building a wheel

```bash
pip install build
python -m build --wheel
# Output: dist/sabori_csp-0.1.0-cpXXX-cpXXX-linux_x86_64.whl
```

## Documentation

### High-level API (`sabori_csp`)

- [Quickstart](high-level/quickstart.md) — Model a problem in 5 minutes
- [API Reference](high-level/reference.md) — CpModel, CpSolver, IntVar, global constraints
- [Operator Reference](high-level/operators.md) — Arithmetic and comparison operators

### Core API (`sabori_csp.core`)

- [Quickstart](core/quickstart.md) — Low-level modeling basics
- [API Reference](core/reference.md) — Domain, Variable, Model, Solver, all constraint classes

### Examples

- [Examples](examples/) — Runnable sample programs (N-Queens, scheduling, Sudoku, etc.)

## Requirements

- Python 3.9+
- C++17 compiler (for building from source)
- CMake 3.16+
