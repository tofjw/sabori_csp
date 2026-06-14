#!/usr/bin/env python3
"""A/B test: count_eq bump_activity override (After) vs default (Before).
Filters problems that contain count_eq constraints in their .fzn."""
import subprocess, sys, os, re, time, signal
from pathlib import Path
from datetime import datetime
from concurrent.futures import ProcessPoolExecutor, as_completed

BASE_DIR = Path(__file__).resolve().parent
MINIZINC = "/snap/bin/minizinc"
SABORI_MSC = str(Path(__file__).resolve().parent.parent.parent / "build" / "share" / "minizinc" / "solvers" / "sabori_csp.msc")
TIMEOUT = 60
MAX_WORKERS = 4

BINARY_AFTER = "/tmp/fzn_sabori_after"
BINARY_BEFORE = "/tmp/fzn_sabori_before"

PROB_DIRS = {
    "2023": BASE_DIR / "mznc2023_probs",
    "2024": BASE_DIR / "mznc2024_probs",
    "2025": BASE_DIR / "mznc2025_probs",
}

def natural_sort_key(s):
    return [int(t) if t.isdigit() else t.lower() for t in re.split('([0-9]+)', str(s))]

def get_descendant_pids(pid):
    descendants = []
    try:
        children = subprocess.run(["pgrep", "-P", str(pid)], capture_output=True, text=True).stdout.strip().split('\n')
        for child in children:
            if child:
                child_pid = int(child)
                descendants.extend(get_descendant_pids(child_pid))
                descendants.append(child_pid)
    except (ValueError, OSError):
        pass
    return descendants

def kill_process_tree(proc):
    pid = proc.pid
    for dpid in get_descendant_pids(pid):
        try: os.kill(dpid, signal.SIGKILL)
        except OSError: pass
    try: os.killpg(os.getpgid(pid), signal.SIGKILL)
    except OSError: pass
    try: os.kill(pid, signal.SIGKILL)
    except OSError: pass
    try: proc.wait(timeout=5)
    except Exception: pass

def find_instances(prob_dir):
    prob_dir = Path(prob_dir)
    mzn_files = sorted(prob_dir.glob("*.mzn"), key=natural_sort_key)
    dzn_files = sorted(prob_dir.glob("*.dzn"), key=natural_sort_key)
    json_files = sorted(prob_dir.glob("*.json"), key=natural_sort_key)
    data_files = dzn_files + json_files
    if not mzn_files:
        return []
    mzn = mzn_files[0]
    if data_files:
        return [(str(mzn), str(data_files[0]), data_files[0].stem)]
    return [(str(mzn), None, mzn.stem)]

def compile_fzn(mzn, data):
    import tempfile
    fzn_fd, fzn_path = tempfile.mkstemp(suffix=".fzn")
    os.close(fzn_fd)
    cmd = [MINIZINC, "--compile", "--solver", SABORI_MSC, "-o", fzn_path, mzn]
    if data:
        cmd.append(data)
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        if result.returncode != 0:
            os.unlink(fzn_path)
            return None
        return fzn_path
    except Exception:
        try: os.unlink(fzn_path)
        except OSError: pass
        return None

def has_count_eq(fzn_path):
    """Check if a .fzn file contains count_eq constraints."""
    try:
        with open(fzn_path) as f:
            for line in f:
                if 'count_eq' in line:
                    return True
    except Exception:
        pass
    return False

def parse_fzn_solve_type(fzn_path):
    with open(fzn_path) as f:
        for line in f:
            if line.startswith("solve"):
                if "minimize" in line: return "MIN"
                elif "maximize" in line: return "MAX"
                else: return "SAT"
    return "SAT"

def run_fzn_solver(problem, solver_name, binary, fzn_path):
    cmd = [binary, "-a", "-t", str(TIMEOUT), fzn_path]
    start = time.time()
    is_to = False
    proc = None
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, start_new_session=True)
        try:
            stdout, stderr = proc.communicate(timeout=TIMEOUT + 5)
        except subprocess.TimeoutExpired:
            kill_process_tree(proc)
            stdout, stderr = proc.communicate()
            is_to = True

        elapsed = time.time() - start
        output = stdout or ""

        if "==========\n" in output: status = "OPTIMAL"
        elif "----------\n" in output: status = "SOL"
        elif "=====UNSATISFIABLE=====" in output: status = "UNSAT"
        elif is_to or elapsed >= TIMEOUT - 0.5: status = "TIMEOUT"
        else: status = "UNKNOWN"

        obj = None
        for line in reversed(output.split('\n')):
            m = re.search(r'_objective\s*=\s*(-?\d+)', line)
            if m:
                obj = int(m.group(1))
                break
        if obj is None and stderr:
            for line in reversed(stderr.split('\n')):
                m = re.search(r'new best objective:\s*(-?\d+)', line)
                if m:
                    obj = int(m.group(1))
                    break

        return (problem, solver_name, status, elapsed, obj)
    except Exception as e:
        return (problem, solver_name, "ERROR", 0, None)
    finally:
        if proc is not None:
            kill_process_tree(proc)

def judge_winner(a_status, a_time, a_obj, b_status, b_time, b_obj, prob_type="SAT"):
    a_ok = a_status in ("OPTIMAL", "SOL", "UNSAT")
    b_ok = b_status in ("OPTIMAL", "SOL", "UNSAT")

    def obj_better(x, y):
        if prob_type == "MIN": return x < y
        elif prob_type == "MAX": return x > y
        return False

    if a_ok and b_ok:
        if a_status == "OPTIMAL" and b_status != "OPTIMAL": return "After", "a-win"
        if b_status == "OPTIMAL" and a_status != "OPTIMAL": return "Before", "b-win"
        if a_obj is not None and b_obj is not None and a_obj != b_obj:
            return ("After", "a-win") if obj_better(a_obj, b_obj) else ("Before", "b-win")
        if a_time < b_time * 0.8: return "After", "a-win"
        if b_time < a_time * 0.8: return "Before", "b-win"
        return "Tie", ""
    if a_ok and not b_ok: return "After", "a-win"
    if b_ok and not a_ok: return "Before", "b-win"
    a_has = a_obj is not None
    b_has = b_obj is not None
    if a_has and b_has:
        if a_obj != b_obj:
            return ("After", "a-win") if obj_better(a_obj, b_obj) else ("Before", "b-win")
        return "Tie", ""
    if a_has and not b_has: return "After", "a-win"
    if b_has and not a_has: return "Before", "b-win"
    return "", ""

def main():
    for label, path in [("After", BINARY_AFTER), ("Before", BINARY_BEFORE)]:
        if not os.path.isfile(path):
            print(f"ERROR: {label} binary not found: {path}")
            sys.exit(1)
    print(f"After:  {BINARY_AFTER}")
    print(f"Before: {BINARY_BEFORE}")

    subprocess.run(["pkill", "-x", "fzn_sabori"], capture_output=True)
    subprocess.run(["pkill", "-x", "minizinc"], capture_output=True)
    time.sleep(0.5)

    problems = []
    prob_years = {}
    prob_dirs_map = {}
    for year, probs_dir in sorted(PROB_DIRS.items()):
        if not probs_dir.exists():
            continue
        for p in sorted(probs_dir.iterdir(), key=lambda x: natural_sort_key(x.name)):
            if not p.is_dir():
                continue
            name = p.name
            if name in prob_dirs_map:
                name = f"{p.name}-{year}"
            prob_dirs_map[name] = p
            prob_years[name] = year

    # Compile and filter by count_eq
    print(f"\nScanning {len(prob_dirs_map)} problems for count_eq...")
    fzn_map = {}
    prob_types = {}
    data_labels = {}
    filtered = []
    for name in sorted(prob_dirs_map.keys(), key=natural_sort_key):
        instances = find_instances(prob_dirs_map[name])
        if not instances:
            continue
        mzn, data, label = instances[0]
        fzn_path = compile_fzn(mzn, data)
        if fzn_path is None:
            continue
        if not has_count_eq(fzn_path):
            os.unlink(fzn_path)
            continue
        filtered.append(name)
        fzn_map[name] = fzn_path
        prob_types[name] = parse_fzn_solve_type(fzn_path)
        if label:
            data_labels[name] = label
        print(f"  {name} ({prob_types[name]}) [year={prob_years.get(name, '?')}]")

    problems = filtered
    print(f"\nFound {len(problems)} problems with count_eq.")

    tasks = []
    for prob in problems:
        fzn = fzn_map[prob]
        tasks.append((prob, "After", BINARY_AFTER, fzn))
        tasks.append((prob, "Before", BINARY_BEFORE, fzn))

    print(f"Running {len(tasks)} tasks with {MAX_WORKERS} workers...")
    print(f"Timeout: {TIMEOUT}s per instance")
    print("=" * 90)

    results = {}
    with ProcessPoolExecutor(max_workers=MAX_WORKERS) as executor:
        futures = {executor.submit(run_fzn_solver, *t): t for t in tasks}
        for future in as_completed(futures):
            prob, solver, status, elapsed, obj = future.result()
            results[(prob, solver)] = (status, elapsed, obj)
            obj_str = f" obj={obj}" if obj is not None else ""
            print(f"{prob:35s} {solver:10s} -> {status:8s} {elapsed:6.2f}s{obj_str}")

    # Console summary
    print("\n" + "=" * 90)
    print(f"{'Problem':<35s} {'Year':>4s} {'After':>25s} {'Before':>25s} {'Winner':>10s}")
    print("-" * 105)

    a_wins = b_wins = ties = 0
    for prob in problems:
        a_res = results.get((prob, "After"), ("?", 0, None))
        b_res = results.get((prob, "Before"), ("?", 0, None))
        ptype = prob_types.get(prob, "?")
        year = prob_years.get(prob, "")
        a_obj_str = f" obj={a_res[2]}" if a_res[2] is not None else ""
        b_obj_str = f" obj={b_res[2]}" if b_res[2] is not None else ""
        a_str = f"{a_res[0]} {a_res[1]:.1f}s{a_obj_str}"
        b_str = f"{b_res[0]} {b_res[1]:.1f}s{b_obj_str}"
        winner, _ = judge_winner(a_res[0], a_res[1], a_res[2], b_res[0], b_res[1], b_res[2], ptype)
        if winner == "After": a_wins += 1
        elif winner == "Before": b_wins += 1
        elif winner == "Tie": ties += 1
        if winner == "Tie": winner = "~"
        dlabel = data_labels.get(prob, "")
        prob_label = f"{prob} ({ptype})"
        data_str = f" [{dlabel}]" if dlabel else ""
        print(f"{prob_label:<35s} {year:>4s}{data_str:>12s} {a_str:>25s} {b_str:>25s} {winner:>10s}")

    print("-" * 105)
    print(f"Wins: After {a_wins}  /  Before {b_wins}  /  Tie {ties}")

    # Cleanup
    for fzn_path in fzn_map.values():
        try: os.unlink(fzn_path)
        except OSError: pass

if __name__ == "__main__":
    main()
