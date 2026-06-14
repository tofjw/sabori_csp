#!/usr/bin/env python3
"""Sabori vs CP-SAT ベンチマーク比較: all_different 制約を含む問題のみ
(MiniZinc Challenge 2023/2024/2025 problems)"""
import subprocess
import sys
import os
import re
import time
import signal
from pathlib import Path
from datetime import datetime
from concurrent.futures import ProcessPoolExecutor, as_completed

import lib_benchmark
from lib_benchmark import *

TIMEOUT = 40  # seconds
lib_benchmark.TIMEOUT = TIMEOUT

# alldiff は近年(2022-2025)のみを対象とする（lib の全年 PROB_DIRS を上書き）
PROB_DIRS = {
    "2022": BASE_DIR / "mznc2022_probs",
    "2023": BASE_DIR / "mznc2023_probs",
    "2024": BASE_DIR / "mznc2024_probs",
    "2025": BASE_DIR / "mznc2025_probs",
}

def has_alldifferent_in_fzn(mzn, data):
    """MiniZinc で FlatZinc にコンパイルし、all_different_int 制約が含まれるか確認する。

    MZN ソースで alldifferent を使っていても、opt 型変数の場合は MiniZinc が
    alldifferent_except_0 に展開するため、FlatZinc レベルでの確認が必要。
    """
    import tempfile
    try:
        with tempfile.NamedTemporaryFile(suffix='.fzn', delete=True) as tmp:
            cmd = [MINIZINC, "-c", "--solver", SABORI_MSC, "--fzn", tmp.name, mzn]
            if data:
                cmd.append(data)
            proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    text=True, start_new_session=True)
            try:
                proc.communicate(timeout=30)
            except subprocess.TimeoutExpired:
                kill_process_tree(proc)
                proc.wait()
                return False
            if proc.returncode != 0:
                return False
            fzn_content = Path(tmp.name).read_text()
            # all_different_int / alldifferent_int / fzn_all_different_int を検索
            # alldifferent_except_0 は除外
            cleaned = re.sub(r'(?:alldifferent_except_0|fzn_alldifferent_except_0)\s*\(', 'REMOVED(', fzn_content)
            return bool(re.search(r'(?:all_different_int|alldifferent_int|fzn_all_different_int)\s*\(', cleaned))
    except Exception:
        return False


def run_solver(problem, solver_name, solver_id, mzn, data):
    data_label = Path(data).stem if data else None
    # minizinc 自前の -t で graceful にタイムアウトさせる。communicate(timeout=…)
    # を SIGKILL の起点にすると未フラッシュ出力（obj/解）が消えるため。
    cmd = [MINIZINC, "--solver", solver_id, "-t", str(TIMEOUT * 1000), "-a",
           "--output-objective", "--output-mode", "dzn", mzn]
    if data:
        cmd.append(data)

    start = time.monotonic()
    is_to = False
    proc = None
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, start_new_session=True)
        try:
            stdout, stderr = proc.communicate(timeout=TIMEOUT + 10)
        except subprocess.TimeoutExpired:
            kill_process_tree(proc)
            stdout, _ = proc.communicate()
            is_to = True

        elapsed = time.monotonic() - start
        output = stdout

        if "==========\n" in output:
            status = "OPTIMAL"
        elif "----------\n" in output:
            status = "SOL"
        elif "=====UNSATISFIABLE=====" in output:
            status = "UNSAT"
        elif is_to or elapsed >= TIMEOUT * 0.9:
            status = "TIMEOUT"
        else:
            status = "UNKNOWN"

        obj = None
        for line in reversed(output.split('\n')):
            m = re.search(r'_objective\s*=\s*(-?\d+)', line)
            if m:
                obj = int(m.group(1))
                break

        return (problem, solver_name, status, elapsed, obj, None, data_label)

    except Exception as e:
        return (problem, solver_name, "ERROR", 0, None, str(e), data_label)
    finally:
        if proc is not None:
            kill_process_tree(proc)


def main():
    cleanup_stale_processes()

    # all_different を含む問題を全年度から収集
    # FlatZinc にコンパイルして all_different_int が含まれるか確認する
    problems = []
    prob_years = {}
    prob_dirs_map = {}  # prob_name -> prob_dir

    # Step 1: MZN ソースで alldifferent 呼び出しがある問題を事前フィルタ
    candidates = []
    for year, probs_dir in sorted(PROB_DIRS.items()):
        if not probs_dir.exists():
            print(f"WARNING: {probs_dir} does not exist, skipping")
            continue
        for p in sorted(probs_dir.iterdir(), key=lambda x: natural_sort_key(x.name)):
            if not p.is_dir():
                continue
            # MZN ソースに alldifferent/all_different の呼び出しがあるか（except_0 除外）
            has_src = False
            for f in p.glob("*.mzn"):
                try:
                    content = f.read_text()
                    cleaned = re.sub(r'(?:alldifferent_except_0|all_different_except_0)\s*\(', 'REMOVED(', content)
                    if re.search(r'(?:alldifferent|all_different)\s*\(', cleaned):
                        has_src = True
                        break
                except Exception:
                    pass
            if not has_src:
                continue
            instances = find_instances(p)
            if not instances:
                continue
            mzn, data, label = instances[0]
            candidates.append((year, p, mzn, data, label))

    # Step 2: FlatZinc にコンパイルして all_different_int が実際に含まれるか確認
    print(f"Checking {len(candidates)} candidate problems for all_different_int in FlatZinc...")
    for year, p, mzn, data, label in candidates:
        if has_alldifferent_in_fzn(mzn, data):
            prob_name = p.name
            if prob_name in prob_dirs_map:
                prob_name = f"{p.name}-{year}"
            problems.append(prob_name)
            prob_years[prob_name] = year
            prob_dirs_map[prob_name] = p
        else:
            print(f"  [{year}] {p.name}: skipped (no all_different_int in FZN)")

    print(f"\nFound {len(problems)} problems with all_different_int in FlatZinc:")
    for prob in problems:
        print(f"  [{prob_years[prob]}] {prob}")
    print()

    results = {}
    tasks = []
    prob_types = {}
    data_labels = {}

    for prob in problems:
        prob_dir = prob_dirs_map[prob]
        instances = find_instances(prob_dir)
        if not instances:
            print(f"WARNING: {prob} has no mzn files, skipping")
            continue

        mzn, data, label = instances[0]
        prob_types[prob] = detect_problem_type(mzn)
        if label:
            data_labels[prob] = label
        tasks.append((prob, "Sabori", SABORI_MSC, mzn, data))
        tasks.append((prob, "CP-SAT", "cp-sat", mzn, data))

    print(f"Running {len(tasks)} tasks with {MAX_WORKERS} workers...")
    print(f"Timeout: {TIMEOUT}s per instance")
    print("=" * 80)

    with ProcessPoolExecutor(max_workers=MAX_WORKERS) as executor:
        futures = {executor.submit(run_solver, *t): t for t in tasks}

        for future in as_completed(futures):
            prob, solver, status, elapsed, obj, err, data_label = future.result()
            key = (prob, solver)
            results[key] = (status, elapsed, obj, err)
            if data_label and prob not in data_labels:
                data_labels[prob] = data_label

            obj_str = f" obj={obj}" if obj is not None else ""
            err_str = f" ({err})" if err else ""
            print(f"{prob:30s} {solver:8s} -> {status:8s} {elapsed:6.2f}s{obj_str}{err_str}")

    # コンソール出力
    print("\n" + "=" * 80)
    print("Summary:")
    print("=" * 80)
    print(f"{'Problem':<35s} {'Year':>4s} {'Sabori':>20s} {'CP-SAT':>20s} {'Winner':>10s}")
    print("-" * 95)

    sabori_wins = 0
    cpsat_wins = 0
    ties = 0
    for prob in problems:
        s_res = results.get((prob, "Sabori"), ("?", 0, None, None))
        c_res = results.get((prob, "CP-SAT"), ("?", 0, None, None))

        ptype = prob_types.get(prob, "?")
        year = prob_years.get(prob, "")
        s_obj_str = f" obj={s_res[2]}" if s_res[2] is not None else ""
        c_obj_str = f" obj={c_res[2]}" if c_res[2] is not None else ""
        s_str = f"{s_res[0]} {s_res[1]:.1f}s{s_obj_str}"
        c_str = f"{c_res[0]} {c_res[1]:.1f}s{c_obj_str}"

        winner, _ = judge_winner(s_res[0], s_res[1], s_res[2], c_res[0], c_res[1], c_res[2], ptype)
        if winner == "Sabori":
            sabori_wins += 1
        elif winner == "CP-SAT":
            cpsat_wins += 1
        elif winner == "Tie":
            ties += 1

        if winner == "Tie":
            winner = "~"

        dlabel = data_labels.get(prob, "")
        prob_label = f"{prob} ({ptype})"
        data_str = f" [{dlabel}]" if dlabel else ""
        print(f"{prob_label:<35s} {year:>4s}{data_str:>12s} {s_str:>20s} {c_str:>20s} {winner:>10s}")

    sabori_timeouts = sum(1 for p in problems
                          if results.get((p, "Sabori"), ("?", 0, None, None))[0] == "TIMEOUT"
                          and results.get((p, "Sabori"), ("?", 0, None, None))[2] is None)
    cpsat_timeouts = sum(1 for p in problems
                         if results.get((p, "CP-SAT"), ("?", 0, None, None))[0] == "TIMEOUT"
                         and results.get((p, "CP-SAT"), ("?", 0, None, None))[2] is None)

    print("-" * 95)
    print(f"Wins:     Sabori {sabori_wins}  /  CP-SAT {cpsat_wins}  /  Tie {ties}")
    print(f"Timeouts: Sabori {sabori_timeouts}  /  CP-SAT {cpsat_timeouts}")

    generate_html(results, problems=problems, label="alldiff", prob_types=prob_types,
                  data_labels=data_labels, prob_years=prob_years)

if __name__ == "__main__":
    main()
