#!/usr/bin/env python3
"""
TSP (Traveling Salesman Problem) FlatZinc generator for sabori_csp benchmarks.

Uses circuit constraint to form a Hamiltonian cycle and int_element for distance lookup.
"""

import argparse
import random
from pathlib import Path


def generate_distance_matrix(n: int, seed: int, max_dist: int = 100) -> list[list[int]]:
    """Generate a random symmetric distance matrix."""
    random.seed(seed)
    dist = [[0] * n for _ in range(n)]
    for i in range(n):
        for j in range(i + 1, n):
            d = random.randint(1, max_dist)
            dist[i][j] = d
            dist[j][i] = d
    return dist


def compute_greedy_upper_bound(n: int, dist: list[list[int]]) -> int:
    """Compute a greedy upper bound for TSP (nearest neighbor heuristic)."""
    visited = [False] * n
    current = 0
    visited[0] = True
    total = 0

    for _ in range(n - 1):
        best_next = -1
        best_dist = float('inf')
        for j in range(n):
            if not visited[j] and dist[current][j] < best_dist:
                best_dist = dist[current][j]
                best_next = j
        total += best_dist
        current = best_next
        visited[current] = True

    # Return to start
    total += dist[current][0]
    return total


def generate_fzn(n: int, dist: list[list[int]], bound: int, output_file: Path) -> None:
    """Generate FlatZinc file for TSP instance."""

    # Flatten distance matrix (1-based indexing for FlatZinc)
    # dist_flat[i*n + j + 1] = dist[i][j]
    dist_flat = []
    for i in range(n):
        for j in range(n):
            dist_flat.append(dist[i][j])

    with open(output_file, 'w') as f:
        f.write(f"% TSP instance: n={n}, bound={bound}\n")
        f.write(f"% Distance matrix (flattened, 1-based index: dist_flat[i*n + j + 1] = dist[i][j])\n\n")

        # Distance array declaration
        f.write(f"array [1..{n*n}] of int: dist_flat = [{', '.join(map(str, dist_flat))}];\n\n")

        # Variables: next[i] is the next city after city i (0-indexed for circuit)
        for i in range(n):
            f.write(f"var 0..{n-1}: next_{i} :: output_var;\n")

        f.write("\n")

        # Variables: edge_cost[i] is the distance from city i to next[i]
        # Find min and max possible edge costs
        min_edge = min(dist[i][j] for i in range(n) for j in range(n) if i != j)
        max_edge = max(dist[i][j] for i in range(n) for j in range(n))

        for i in range(n):
            f.write(f"var {min_edge}..{max_edge}: edge_cost_{i};\n")

        f.write("\n")

        # Index variables for int_element
        for i in range(n):
            base_idx = i * n + 1  # 1-based start for row i
            f.write(f"var {base_idx}..{base_idx + n - 1}: idx_{i};\n")

        f.write("\n")

        # Total cost variable
        f.write(f"var {n * min_edge}..{bound}: total_cost :: output_var;\n\n")

        # Circuit constraint (0-indexed)
        next_vars = ", ".join(f"next_{i}" for i in range(n))
        f.write(f"constraint circuit([{next_vars}]);\n\n")

        # Index computation: idx_i = next_i + i*n + 1
        # Rewrite as: 1*next_i + (-1)*idx_i = -(i*n + 1)
        f.write("% Index constraints: idx_i = next_i + i*n + 1\n")
        for i in range(n):
            base_idx = i * n + 1
            f.write(f"constraint int_lin_eq([1, -1], [next_{i}, idx_{i}], {-base_idx});\n")

        f.write("\n")

        # Edge cost constraints: edge_cost_i = dist_flat[idx_i]
        f.write("% Edge cost constraints: edge_cost_i = dist_flat[idx_i]\n")
        for i in range(n):
            f.write(f"constraint array_int_element(idx_{i}, dist_flat, edge_cost_{i});\n")

        f.write("\n")

        # Total cost constraint: sum(edge_cost_i) = total_cost
        # Rewrite as: sum(edge_cost_i) - total_cost = 0
        coeffs = ", ".join(["1"] * n + ["-1"])
        edge_vars = ", ".join(f"edge_cost_{i}" for i in range(n))
        f.write(f"% Total cost constraint\n")
        f.write(f"constraint int_lin_eq([{coeffs}], [{edge_vars}, total_cost], 0);\n\n")

        # Solve
        f.write("solve satisfy;\n")

    print(f"Generated: {output_file}")


def main():
    parser = argparse.ArgumentParser(description="Generate TSP FlatZinc benchmarks")
    parser.add_argument("--size", type=str, default="all",
                        choices=["small", "medium", "large", "all"],
                        help="Problem size to generate")
    parser.add_argument("--output-dir", type=Path, default=Path(__file__).parent,
                        help="Output directory")
    parser.add_argument("--seed", type=int, default=42,
                        help="Random seed")

    args = parser.parse_args()

    # Problem configurations
    configs = {
        "small": {"n": 5, "seed": args.seed},
        "medium": {"n": 8, "seed": args.seed + 1},
        "large": {"n": 10, "seed": args.seed + 2},
    }

    sizes = list(configs.keys()) if args.size == "all" else [args.size]

    for size in sizes:
        cfg = configs[size]
        n = cfg["n"]
        seed = cfg["seed"]

        dist = generate_distance_matrix(n, seed)
        upper_bound = compute_greedy_upper_bound(n, dist)

        # Use greedy bound + small margin as the constraint bound
        bound = int(upper_bound * 1.1)

        output_file = args.output_dir / f"tsp_{size}.fzn"
        generate_fzn(n, dist, bound, output_file)

        # Print distance matrix for reference
        print(f"\n{size.upper()} (n={n}, seed={seed}):")
        print(f"  Greedy upper bound: {upper_bound}")
        print(f"  Constraint bound: {bound}")
        print(f"  Distance matrix:")
        for i, row in enumerate(dist):
            print(f"    {i}: {row}")


if __name__ == "__main__":
    main()
