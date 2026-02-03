#!/usr/bin/env python3
"""
Magic Square FlatZinc generator for sabori_csp benchmarks.

A magic square of order n is an n×n grid filled with distinct integers 1 to n²
such that all rows, columns, and both diagonals sum to the same magic constant.

Magic constant = n(n² + 1) / 2
"""

import argparse
from pathlib import Path


def magic_constant(n: int) -> int:
    """Calculate the magic constant for an n×n magic square."""
    return n * (n * n + 1) // 2


def generate_fzn(n: int, output_file: Path) -> None:
    """Generate FlatZinc file for magic square of order n."""
    mc = magic_constant(n)
    n2 = n * n

    with open(output_file, 'w') as f:
        f.write(f"% Magic Square of order {n}\n")
        f.write(f"% Magic constant: {mc}\n")
        f.write(f"% Variables: x_i_j for i,j in 0..{n-1}\n\n")

        # Variable declarations
        # x[i][j] = cell at row i, column j (0-indexed)
        for i in range(n):
            for j in range(n):
                f.write(f"var 1..{n2}: x_{i}_{j} :: output_var;\n")

        f.write("\n")

        # All different constraint
        all_vars = ", ".join(f"x_{i}_{j}" for i in range(n) for j in range(n))
        f.write(f"% All cells must have different values\n")
        f.write(f"constraint all_different_int([{all_vars}]);\n\n")

        # Row constraints
        f.write(f"% Row sums = {mc}\n")
        for i in range(n):
            row_vars = ", ".join(f"x_{i}_{j}" for j in range(n))
            coeffs = ", ".join(["1"] * n)
            f.write(f"constraint int_lin_eq([{coeffs}], [{row_vars}], {mc});\n")

        f.write("\n")

        # Column constraints
        f.write(f"% Column sums = {mc}\n")
        for j in range(n):
            col_vars = ", ".join(f"x_{i}_{j}" for i in range(n))
            coeffs = ", ".join(["1"] * n)
            f.write(f"constraint int_lin_eq([{coeffs}], [{col_vars}], {mc});\n")

        f.write("\n")

        # Diagonal constraints
        f.write(f"% Main diagonal sum = {mc}\n")
        main_diag_vars = ", ".join(f"x_{i}_{i}" for i in range(n))
        coeffs = ", ".join(["1"] * n)
        f.write(f"constraint int_lin_eq([{coeffs}], [{main_diag_vars}], {mc});\n")

        f.write(f"\n% Anti-diagonal sum = {mc}\n")
        anti_diag_vars = ", ".join(f"x_{i}_{n-1-i}" for i in range(n))
        f.write(f"constraint int_lin_eq([{coeffs}], [{anti_diag_vars}], {mc});\n")

        f.write("\n")

        # Solve
        f.write("solve satisfy;\n")

    print(f"Generated: {output_file} (n={n}, magic_constant={mc})")


def main():
    parser = argparse.ArgumentParser(description="Generate Magic Square FlatZinc benchmarks")
    parser.add_argument("--size", type=str, default="all",
                        choices=["small", "medium", "large", "all"],
                        help="Problem size to generate")
    parser.add_argument("--output-dir", type=Path, default=Path(__file__).parent,
                        help="Output directory")

    args = parser.parse_args()

    # Problem configurations
    configs = {
        "small": {"n": 3},   # 8 solutions
        "medium": {"n": 4},  # 880 solutions (excluding rotations/reflections: 7040)
        "large": {"n": 5},   # Many solutions
    }

    sizes = list(configs.keys()) if args.size == "all" else [args.size]

    for size in sizes:
        cfg = configs[size]
        n = cfg["n"]

        output_file = args.output_dir / f"magic_square_{size}.fzn"
        generate_fzn(n, output_file)

        mc = magic_constant(n)
        print(f"  {size.upper()}: {n}x{n} grid, values 1..{n*n}, magic constant = {mc}")


if __name__ == "__main__":
    main()
