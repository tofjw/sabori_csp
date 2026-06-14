#!/usr/bin/env python3
"""Sabori-only benchmark for all_different problems (quick A/B comparison)"""
import subprocess, sys, os, re, time, signal, json
from pathlib import Path
from concurrent.futures import ProcessPoolExecutor, as_completed

BASE_DIR = Path(__file__).resolve().parent
MINIZINC = "/snap/bin/minizinc"
SABORI_MSC = str(Path(__file__).resolve().parent.parent.parent / "build" / "share" / "minizinc" / "solvers" / "sabori_csp.msc")
TIMEOUT = 30
MAX_WORKERS = 4

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

def has_alldifferent(prob_dir):
    for f in Path(prob_dir).glob("*.mzn"):
        try:
            if re.search(r'alldifferent|all_different', f.read_text()):
                return True
        except Exception:
            pass
    return False

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
        return [(str(mzn), str(data_files[0]))]
    return [(str(mzn), None)]

def detect_problem_type(mzn_path):
    content = Path(mzn_path).read_text()
    lines = [l for l in content.splitlines() if not l.strip().startswith('%')]
    text = '\n'.join(lines)
    if re.search(r'\bminimize\b', text): return "MIN"
    elif re.search(r'\bmaximize\b', text): return "MAX"
    return "SAT"

def run_solver(prob_name, mzn, data):
    # minizinc 自前の -t で graceful にタイムアウトさせる。communicate(timeout=…)
    # を SIGKILL の起点にすると未フラッシュ出力（obj/解）が消えるため。
    cmd = [MINIZINC, "--solver", SABORI_MSC, "-t", str(TIMEOUT * 1000), "-a",
           "--output-objective", "--output-mode", "dzn", mzn]
    if data:
        cmd.append(data)
    start = time.time()
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

        elapsed = time.time() - start
        if "==========\n" in stdout: status = "OPTIMAL"
        elif "----------\n" in stdout: status = "SOL"
        elif "=====UNSATISFIABLE=====" in stdout: status = "UNSAT"
        elif is_to or elapsed >= TIMEOUT * 0.9: status = "TIMEOUT"
        else: status = "UNKNOWN"

        obj = None
        for line in reversed(stdout.split('\n')):
            m = re.search(r'_objective\s*=\s*(-?\d+)', line)
            if m:
                obj = int(m.group(1))
                break
        return (prob_name, status, elapsed, obj)
    except Exception as e:
        return (prob_name, "ERROR", 0, None)
    finally:
        if proc is not None:
            kill_process_tree(proc)

def main():
    subprocess.run(["pkill", "-x", "fzn_sabori"], capture_output=True)
    subprocess.run(["pkill", "-x", "minizinc"], capture_output=True)
    time.sleep(0.5)

    problems = []
    prob_dirs_map = {}
    for year, probs_dir in sorted(PROB_DIRS.items()):
        if not probs_dir.exists():
            continue
        for p in sorted(probs_dir.iterdir(), key=lambda x: natural_sort_key(x.name)):
            if not p.is_dir():
                continue
            if has_alldifferent(p):
                name = p.name
                if name in prob_dirs_map:
                    name = f"{p.name}-{year}"
                problems.append(name)
                prob_dirs_map[name] = p

    print(f"Found {len(problems)} all_different problems")

    tasks = []
    for prob in problems:
        instances = find_instances(prob_dirs_map[prob])
        if instances:
            mzn, data = instances[0]
            tasks.append((prob, mzn, data))

    print(f"Running {len(tasks)} tasks (Sabori only, {MAX_WORKERS} workers, {TIMEOUT}s timeout)")
    print("=" * 70)

    results = {}
    with ProcessPoolExecutor(max_workers=MAX_WORKERS) as executor:
        futures = {executor.submit(run_solver, *t): t for t in tasks}
        for future in as_completed(futures):
            prob, status, elapsed, obj = future.result()
            results[prob] = {"status": status, "time": round(elapsed, 2), "obj": obj}
            obj_str = f" obj={obj}" if obj is not None else ""
            print(f"  {prob:35s} {status:8s} {elapsed:6.2f}s{obj_str}")

    # Output JSON for comparison
    output_file = sys.argv[1] if len(sys.argv) > 1 else "bench_result.json"
    with open(output_file, 'w') as f:
        json.dump(results, f, indent=2)
    print(f"\nResults saved to {output_file}")

    # Summary
    solved = sum(1 for r in results.values() if r["status"] in ("OPTIMAL", "SOL", "UNSAT"))
    timeouts = sum(1 for r in results.values() if r["status"] == "TIMEOUT" and r["obj"] is None)
    print(f"Solved: {solved}/{len(results)}, Timeouts (no sol): {timeouts}")

if __name__ == "__main__":
    main()
