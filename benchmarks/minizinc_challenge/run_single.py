#!/usr/bin/env python3
"""単一問題の実行スクリプト

Usage:
    ./run_single.py <problem> [instance] [--solver sabori|cpsat|both] [--timeout N]

Examples:
    ./run_single.py atsp                      # 最小インスタンスを両方で実行
    ./run_single.py atsp instance1_0p05       # 指定インスタンスを両方で実行
    ./run_single.py hitori h5-1 --solver sabori
    ./run_single.py fbd1 --timeout 60
"""
import subprocess
import sys
import re
import argparse
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent
MINIZINC = "/snap/bin/minizinc"
SABORI_MSC = str(Path(__file__).resolve().parent.parent.parent / "build" / "share" / "minizinc" / "solvers" / "sabori_csp.msc")
DEFAULT_TIMEOUT = 30

def natural_sort_key(s):
    return [int(t) if t.isdigit() else t.lower() for t in re.split('([0-9]+)', str(s))]

def find_files(prob_dir):
    mzn_files = sorted(prob_dir.glob("*.mzn"), key=natural_sort_key)
    dzn_files = sorted(prob_dir.glob("*.dzn"), key=natural_sort_key)
    json_files = sorted(prob_dir.glob("*.json"), key=natural_sort_key)
    return mzn_files[0] if mzn_files else None, dzn_files, json_files

def run_solver(solver_name, solver_id, mzn, dzn, timeout):
    cmd = [MINIZINC, "--solver", solver_id, "-a", str(mzn)]
    if dzn:
        cmd.append(str(dzn))

    print(f"\n{'='*60}")
    print(f"Solver: {solver_name}")
    print(f"Command: {' '.join(cmd)}")
    print(f"Timeout: {timeout}s")
    print('='*60)

    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        print(result.stdout)
        if result.stderr:
            print(f"[stderr]\n{result.stderr}")
        return result.returncode
    except subprocess.TimeoutExpired:
        print(f"[TIMEOUT after {timeout}s]")
        return -1

def main():
    parser = argparse.ArgumentParser(description='Run single MiniZinc problem')
    parser.add_argument('problem', help='Problem name (directory in mznc2025_probs/)')
    parser.add_argument('instance', nargs='?', help='Instance name (without .dzn/.json)')
    parser.add_argument('--solver', choices=['sabori', 'cpsat', 'both'], default='both')
    parser.add_argument('--timeout', type=int, default=DEFAULT_TIMEOUT)
    args = parser.parse_args()

    probs_dir = BASE_DIR / "mznc2025_probs"
    prob_dir = probs_dir / args.problem
    if not prob_dir.is_dir():
        print(f"Error: Problem '{args.problem}' not found")
        available = sorted(p.name for p in probs_dir.iterdir() if p.is_dir())
        print(f"Available: {available}")
        sys.exit(1)

    mzn_file, dzn_files, json_files = find_files(prob_dir)
    if not mzn_file:
        print(f"Error: No .mzn file in {prob_dir}")
        sys.exit(1)

    mzn = mzn_file

    # Find instance
    all_instances = dzn_files + json_files
    if args.instance:
        matches = [f for f in all_instances if args.instance in f.name]
        if not matches:
            print(f"Error: Instance '{args.instance}' not found")
            print(f"Available: {[f.name for f in all_instances]}")
            sys.exit(1)
        dzn = matches[0]
    elif all_instances:
        dzn = all_instances[0]
        print(f"Using smallest instance: {all_instances[0].name}")
    else:
        dzn = None

    print(f"Problem: {args.problem}")
    print(f"MZN: {mzn}")
    print(f"Data: {dzn if dzn else '(none)'}")

    solvers = []
    if args.solver in ('sabori', 'both'):
        solvers.append(('Sabori', SABORI_MSC))
    if args.solver in ('cpsat', 'both'):
        solvers.append(('CP-SAT', 'cp-sat'))

    for name, sid in solvers:
        run_solver(name, sid, mzn, dzn, args.timeout)

if __name__ == "__main__":
    main()
