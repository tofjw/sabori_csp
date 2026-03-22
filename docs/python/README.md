# Python Bindings for Sabori CSP

`sabori_csp.core` provides low-level Python bindings for the Sabori CSP constraint solver via pybind11.

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

## Package Structure

```
sabori_csp/
├── __init__.py        # Top-level package (minimal)
├── _sabori_csp.so     # Compiled C++ extension
└── core/
    └── __init__.py    # Re-exports everything from _sabori_csp
```

Use `from sabori_csp.core import ...` to access all classes.

## Documentation

- [Quickstart](quickstart.md) — Get started in 5 minutes
- [API Reference](reference.md) — Complete class and method reference
- [Examples](examples/) — Runnable sample programs

## Requirements

- Python 3.9+
- C++17 compiler (for building from source)
- CMake 3.16+
