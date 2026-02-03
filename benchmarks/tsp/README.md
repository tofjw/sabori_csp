# TSP Benchmark

Traveling Salesman Problem (TSP) benchmarks for sabori_csp.

## Problem Description

Given a set of cities and distances between them, find a tour that:
1. Visits each city exactly once
2. Returns to the starting city
3. Has total distance within a given bound

## Model

The problem is modeled using:
- **circuit constraint**: Ensures the tour forms a Hamiltonian cycle
- **int_element constraint**: Looks up distances from the distance matrix
- **int_lin_eq constraint**: Computes total tour cost

### Variables
- `next_i`: The city to visit after city `i` (0-indexed)
- `edge_cost_i`: Distance from city `i` to `next_i`
- `total_cost`: Sum of all edge costs

### Constraints
```
circuit([next_0, next_1, ..., next_{n-1}])
edge_cost_i = dist[i][next_i]  (via int_element)
total_cost = sum(edge_cost_i)
total_cost <= bound
```

## Benchmark Instances

| Name | Cities | Greedy Bound | Constraint Bound | Seed |
|------|--------|--------------|------------------|------|
| small | 5 | 98 | 107 | 42 |
| medium | 8 | 286 | 314 | 43 |
| large | 10 | 300 | 330 | 44 |

## Usage

### Generate FlatZinc files
```bash
python3 generate_tsp.py [--size small|medium|large|all] [--seed N]
```

### Run benchmarks
```bash
./run_benchmark.sh [small|medium|large|all]
```

### Run single instance
```bash
./build/src/fzn/fzn_sabori benchmarks/tsp/tsp_small.fzn
```

## Sample Output

```
next_0 = 3;
next_1 = 2;
next_2 = 0;
next_3 = 4;
next_4 = 1;
total_cost = 98;
----------
==========
```

This represents the tour: 0 → 3 → 4 → 1 → 2 → 0 with total cost 98.

## Performance Results

| Instance | Status | Cost | Time |
|----------|--------|------|------|
| small (n=5) | SAT | 98 | ~0.005s |
| medium (n=8) | SAT | 281 | ~0.008s |
| large (n=10) | SAT | 316 | ~0.096s |

## Distance Matrices

### Small (n=5)
```
    0    1    2    3    4
0 [  0, 82, 15,  4, 95]
1 [ 82,  0, 36, 32, 29]
2 [ 15, 36,  0, 18, 95]
3 [  4, 32, 18,  0, 14]
4 [ 95, 29, 95, 14,  0]
```

### Medium (n=8)
```
    0    1    2    3    4    5    6    7
0 [  0,  5, 37, 90, 98, 19, 60, 48]
1 [  5,  0, 86, 90, 13, 59, 77, 64]
2 [ 37, 86,  0, 78,  3, 66, 56, 74]
3 [ 90, 90, 78,  0, 48, 80, 71, 97]
4 [ 98, 13,  3, 48,  0, 56, 51, 23]
5 [ 19, 59, 66, 80, 56,  0,  8, 14]
6 [ 60, 77, 56, 71, 51,  8,  0, 16]
7 [ 48, 64, 74, 97, 23, 14, 16,  0]
```

### Large (n=10)
```
    0    1    2    3    4    5    6    7    8    9
0 [  0, 53, 67, 70, 90, 15, 23, 49, 29, 38]
1 [ 53,  0,  4, 29, 15, 73,  2, 13, 21,100]
2 [ 67,  4,  0, 66, 89, 39, 89, 82, 49, 76]
3 [ 70, 29, 66,  0, 52, 99, 89, 86, 42, 46]
4 [ 90, 15, 89, 52,  0, 10, 44, 37, 80, 21]
5 [ 15, 73, 39, 99, 10,  0, 15, 66,  9, 76]
6 [ 23,  2, 89, 89, 44, 15,  0, 13, 35, 41]
7 [ 49, 13, 82, 86, 37, 66, 13,  0, 14, 81]
8 [ 29, 21, 49, 42, 80,  9, 35, 14,  0, 38]
9 [ 38,100, 76, 46, 21, 76, 41, 81, 38,  0]
```
