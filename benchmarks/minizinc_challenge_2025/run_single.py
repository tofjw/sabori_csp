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
import os
import re
import argparse

MINIZINC = "./squashfs-root/usr/bin/minizinc"
DEFAULT_TIMEOUT = 30

def natural_sort_key(s):
    return [int(t) if t.isdigit() else t.lower() for t in re.split('([0-9]+)', s)]

def find_files(prob_dir):
    mzn = [f for f in os.listdir(prob_dir) if f.endswith('.mzn')]
    dzn = sorted([f for f in os.listdir(prob_dir) if f.endswith('.dzn')], key=natural_sort_key)
    json = sorted([f for f in os.listdir(prob_dir) if f.endswith('.json')], key=natural_sort_key)
    return mzn[0] if mzn else None, dzn, json

def run_solver(solver_name, solver_id, mzn, dzn, timeout):
    cmd = [MINIZINC, "--solver", solver_id, "-a", mzn]
    if dzn:
        cmd.append(dzn)

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

    os.chdir("/home/tofjw/develop/cp/sabori_csp/benchmarks/minizinc_challenge_2025")

    prob_dir = f"mznc2025_probs/{args.problem}"
    if not os.path.isdir(prob_dir):
        print(f"Error: Problem '{args.problem}' not found")
        print(f"Available: {sorted(os.listdir('mznc2025_probs'))}")
        sys.exit(1)

    mzn_file, dzn_files, json_files = find_files(prob_dir)
    if not mzn_file:
        print(f"Error: No .mzn file in {prob_dir}")
        sys.exit(1)

    mzn = os.path.join(prob_dir, mzn_file)

    # Find instance
    all_instances = dzn_files + json_files
    if args.instance:
        matches = [f for f in all_instances if args.instance in f]
        if not matches:
            print(f"Error: Instance '{args.instance}' not found")
            print(f"Available: {all_instances}")
            sys.exit(1)
        dzn = os.path.join(prob_dir, matches[0])
    elif all_instances:
        dzn = os.path.join(prob_dir, all_instances[0])
        print(f"Using smallest instance: {all_instances[0]}")
    else:
        dzn = None

    print(f"Problem: {args.problem}")
    print(f"MZN: {mzn}")
    print(f"Data: {dzn or '(none)'}")

    solvers = []
    if args.solver in ('sabori', 'both'):
        solvers.append(('Sabori', 'Sabori CSP'))
    if args.solver in ('cpsat', 'both'):
        solvers.append(('CP-SAT', 'cp-sat'))

    for name, sid in solvers:
        run_solver(name, sid, mzn, dzn, args.timeout)

if __name__ == "__main__":
    main()
