# Magic Square Benchmark

Magic Square benchmarks for sabori_csp.

## Problem Description

A magic square of order n is an n×n grid filled with distinct integers from 1 to n²
such that:
- Each row sums to the magic constant
- Each column sums to the magic constant
- Both diagonals sum to the magic constant

**Magic constant** = n(n² + 1) / 2

## Model

The problem is modeled using:
- **all_different constraint**: All cells must have unique values
- **int_lin_eq constraint**: Row, column, and diagonal sum constraints

### Variables
- `x_i_j`: Value in cell at row i, column j (0-indexed)

### Constraints
```
all_different_int([x_0_0, x_0_1, ..., x_{n-1}_{n-1}])

% Row sums
sum(x_i_0, x_i_1, ..., x_i_{n-1}) = magic_constant  for each row i

% Column sums
sum(x_0_j, x_1_j, ..., x_{n-1}_j) = magic_constant  for each column j

% Main diagonal
sum(x_0_0, x_1_1, ..., x_{n-1}_{n-1}) = magic_constant

% Anti-diagonal
sum(x_0_{n-1}, x_1_{n-2}, ..., x_{n-1}_0) = magic_constant
```

## Benchmark Instances

| Name | Order | Grid Size | Values | Magic Constant | Known Solutions |
|------|-------|-----------|--------|----------------|-----------------|
| small | 3 | 3×3 | 1-9 | 15 | 8 |
| medium | 4 | 4×4 | 1-16 | 34 | 7,040 |
| large | 5 | 5×5 | 1-25 | 65 | 275,305,224 |

Note: Solution counts include rotations and reflections.

## Usage

### Generate FlatZinc files
```bash
python3 generate_magic_square.py [--size small|medium|large|all]
```

### Run benchmarks
```bash
# Find first solution
./run_benchmark.sh [small|medium|large|all]

# Count all solutions
./run_benchmark.sh small --count-all
```

### Run single instance
```bash
# Find first solution
./build/src/fzn/fzn_sabori benchmarks/magic_square/magic_square_small.fzn

# Find all solutions
./build/src/fzn/fzn_sabori -a benchmarks/magic_square/magic_square_small.fzn
```

## Sample Output

```
x_0_0 = 6;
x_0_1 = 1;
x_0_2 = 8;
x_1_0 = 7;
x_1_1 = 5;
x_1_2 = 3;
x_2_0 = 2;
x_2_1 = 9;
x_2_2 = 4;
----------
==========
```

This represents the classic 3×3 magic square:
```
  6  1  8     (sum = 15)
  7  5  3     (sum = 15)
  2  9  4     (sum = 15)
```

## Performance Results

| Instance | Order | Status | Time (first) | Time (all) | Solutions |
|----------|-------|--------|--------------|------------|-----------|
| small | 3 | SAT | ~0.003s | ~0.005s | 8 |
| medium | 4 | SAT | ~0.28s | - | - |
| large | 5 | - | >60s | - | - |

Note: 5×5 magic squares require more sophisticated search strategies or symmetry breaking.

## Mathematical Background

### Magic Constants by Order
| n | Magic Constant |
|---|----------------|
| 3 | 15 |
| 4 | 34 |
| 5 | 65 |
| 6 | 111 |
| 7 | 175 |

### Number of Solutions (including rotations/reflections)
| n | Solutions |
|---|-----------|
| 3 | 8 |
| 4 | 7,040 |
| 5 | 275,305,224 |

### Symmetry Breaking

To reduce search space, one can add symmetry-breaking constraints:
- Fix the smallest corner value
- Order adjacent corners
- Fix the center value (for odd n)

These are not implemented in the current benchmark to test raw solver performance.
