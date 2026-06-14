#!/usr/bin/env python3
"""Cumulative bump_activity A/B test: with vs without custom bump_activity.
Runs fzn_sabori directly (not via minizinc) on pre-compiled .fzn files."""
import subprocess
import sys
import os
import re
import time
import signal
from pathlib import Path
from datetime import datetime
from concurrent.futures import ProcessPoolExecutor, as_completed

BASE_DIR = Path(__file__).resolve().parent
MINIZINC = "/snap/bin/minizinc"
SABORI_MSC = str(Path(__file__).resolve().parent.parent.parent / "build" / "share" / "minizinc" / "solvers" / "sabori_csp.msc")
TIMEOUT = 60  # seconds
MAX_WORKERS = 4

BINARY_WITH_BUMP = str(BASE_DIR / "../../build/src/fzn/fzn_sabori")
BINARY_NO_BUMP = str(BASE_DIR / "../../.claude/worktrees/agent-a217d80d/build/src/fzn/fzn_sabori")

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
        children = subprocess.run(
            ["pgrep", "-P", str(pid)],
            capture_output=True, text=True
        ).stdout.strip().split('\n')
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
    descendants = get_descendant_pids(pid)
    for dpid in descendants:
        try:
            os.kill(dpid, signal.SIGKILL)
        except OSError:
            pass
    try:
        os.killpg(os.getpgid(pid), signal.SIGKILL)
    except OSError:
        pass
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass
    try:
        proc.wait(timeout=5)
    except Exception:
        pass

def cleanup_stale_processes():
    for name in ["fzn_sabori", "fzn-cp-sat"]:
        subprocess.run(["pkill", "-x", name], capture_output=True)
    subprocess.run(["pkill", "-x", "minizinc"], capture_output=True)
    time.sleep(0.5)

def detect_problem_type(mzn_path):
    content = Path(mzn_path).read_text()
    lines = [l for l in content.splitlines() if not l.strip().startswith('%')]
    text = '\n'.join(lines)
    if re.search(r'\bminimize\b', text):
        return "MIN"
    elif re.search(r'\bmaximize\b', text):
        return "MAX"
    else:
        return "SAT"

def has_cumulative(prob_dir):
    prob_dir = Path(prob_dir)
    for f in prob_dir.glob("*.mzn"):
        try:
            content = f.read_text()
            for line in content.splitlines():
                stripped = line.strip()
                if stripped.startswith('%'):
                    continue
                if re.search(r'\bcumulative\s*\(', stripped):
                    return True
                if re.search(r'include\s+"cumulative\.mzn"', stripped):
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

    if len(mzn_files) > 1 and not data_files:
        mzn = mzn_files[0]
        return [(str(mzn), None, mzn.stem)]

    mzn = mzn_files[0]
    if data_files:
        inst = data_files[0]
        return [(str(mzn), str(inst), inst.stem)]
    else:
        return [(str(mzn), None, mzn.stem)]

def compile_fzn(mzn, data):
    """Compile .mzn (+.dzn) to .fzn via minizinc, return path to .fzn"""
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
        try:
            os.unlink(fzn_path)
        except OSError:
            pass
        return None

def parse_fzn_solve_type(fzn_path):
    """Parse .fzn to detect minimize/maximize/satisfy"""
    with open(fzn_path) as f:
        for line in f:
            if line.startswith("solve"):
                if "minimize" in line:
                    return "MIN"
                elif "maximize" in line:
                    return "MAX"
                else:
                    return "SAT"
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

        if "==========\n" in output:
            status = "OPTIMAL"
        elif "----------\n" in output:
            status = "SOL"
        elif "=====UNSATISFIABLE=====" in output:
            status = "UNSAT"
        elif is_to or elapsed >= TIMEOUT - 0.5:
            status = "TIMEOUT"
        else:
            status = "UNKNOWN"

        # Parse objective from output
        obj = None
        for line in reversed(output.split('\n')):
            # FlatZinc direct output: look for objective variable assignment
            m = re.search(r'_objective\s*=\s*(-?\d+)', line)
            if m:
                obj = int(m.group(1))
                break

        # Also check stderr for "new best objective" (verbose output)
        if obj is None and stderr:
            for line in reversed(stderr.split('\n')):
                m = re.search(r'new best objective:\s*(-?\d+)', line)
                if m:
                    obj = int(m.group(1))
                    break

        # Parse stats
        stats = {}
        if stderr:
            for line in stderr.split('\n'):
                sm = re.search(r'Stats:\s*(.*)', line)
                if sm:
                    for kv in sm.group(1).split():
                        parts = kv.split('=')
                        if len(parts) == 2:
                            stats[parts[0]] = parts[1]

        return (problem, solver_name, status, elapsed, obj, stats)

    except Exception as e:
        return (problem, solver_name, "ERROR", 0, None, {"error": str(e)})
    finally:
        if proc is not None:
            kill_process_tree(proc)

def judge_winner(a_status, a_time, a_obj, b_status, b_time, b_obj, prob_type="SAT"):
    a_ok = a_status in ("OPTIMAL", "SOL", "UNSAT")
    b_ok = b_status in ("OPTIMAL", "SOL", "UNSAT")

    def obj_better(x, y):
        if prob_type == "MIN":
            return x < y
        elif prob_type == "MAX":
            return x > y
        return False

    if a_ok and b_ok:
        if a_status == "OPTIMAL" and b_status != "OPTIMAL":
            return "WithBump", "a-win"
        if b_status == "OPTIMAL" and a_status != "OPTIMAL":
            return "NoBump", "b-win"
        if a_obj is not None and b_obj is not None and a_obj != b_obj:
            return ("WithBump", "a-win") if obj_better(a_obj, b_obj) else ("NoBump", "b-win")
        if a_time < b_time * 0.8:
            return "WithBump", "a-win"
        if b_time < a_time * 0.8:
            return "NoBump", "b-win"
        return "Tie", ""
    if a_ok and not b_ok:
        return "WithBump", "a-win"
    if b_ok and not a_ok:
        return "NoBump", "b-win"
    # Both not solved — compare best obj if available
    a_has = a_obj is not None
    b_has = b_obj is not None
    if a_has and b_has:
        if a_obj != b_obj:
            return ("WithBump", "a-win") if obj_better(a_obj, b_obj) else ("NoBump", "b-win")
        return "Tie", ""
    if a_has and not b_has:
        return "WithBump", "a-win"
    if b_has and not a_has:
        return "NoBump", "b-win"
    return "", ""

def generate_html(results, problems, output_path, prob_types, data_labels, prob_years):
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    a_wins = b_wins = ties = 0
    rows = []
    for prob in problems:
        a_res = results.get((prob, "WithBump"), ("?", 0, None, {}))
        b_res = results.get((prob, "NoBump"), ("?", 0, None, {}))

        a_status, a_time, a_obj, a_stats = a_res
        b_status, b_time, b_obj, b_stats = b_res

        ptype = prob_types.get(prob, "?")
        year = prob_years.get(prob, "")
        winner, winner_class = judge_winner(a_status, a_time, a_obj, b_status, b_time, b_obj, ptype)
        if winner == "WithBump":
            a_wins += 1
        elif winner == "NoBump":
            b_wins += 1
        elif winner == "Tie":
            ties += 1

        a_obj_str = f" (obj={a_obj})" if a_obj is not None else ""
        b_obj_str = f" (obj={b_obj})" if b_obj is not None else ""

        a_solved = a_status in ("OPTIMAL", "SOL", "UNSAT")
        b_solved = b_status in ("OPTIMAL", "SOL", "UNSAT")
        a_class = "success" if a_solved else ("timeout" if a_status == "TIMEOUT" else "error")
        b_class = "success" if b_solved else ("timeout" if b_status == "TIMEOUT" else "error")

        dlabel = (data_labels or {}).get(prob, "")
        data_str = f" [{dlabel}]" if dlabel else ""

        rows.append(f"""
        <tr>
            <td>{prob} ({ptype}){data_str}</td>
            <td>{year}</td>
            <td class="{a_class}">{a_status} {a_time:.2f}s{a_obj_str}</td>
            <td class="{b_class}">{b_status} {b_time:.2f}s{b_obj_str}</td>
            <td class="{winner_class}">{winner}</td>
        </tr>""")

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>Cumulative bump_activity A/B Test</title>
    <style>
        body {{
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            max-width: 1200px; margin: 0 auto; padding: 20px; background: #f5f5f5;
        }}
        h1 {{ color: #333; border-bottom: 2px solid #4285f4; padding-bottom: 10px; }}
        .summary {{
            background: white; padding: 20px; border-radius: 8px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1); margin-bottom: 20px; display: flex; gap: 20px;
        }}
        .stat-card {{ text-align: center; padding: 15px 30px; background: #f8f9fa; border-radius: 6px; }}
        .stat-value {{ font-size: 2em; font-weight: bold; color: #4285f4; }}
        .stat-label {{ color: #666; margin-top: 5px; }}
        table {{
            width: 100%; border-collapse: collapse; background: white;
            border-radius: 8px; overflow: hidden; box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }}
        th, td {{ padding: 12px 15px; text-align: left; border-bottom: 1px solid #eee; }}
        th {{ background: #4285f4; color: white; }}
        tr:hover {{ background: #f8f9fa; }}
        .success {{ color: #28a745; font-weight: bold; }}
        .timeout {{ color: #dc3545; }}
        .error {{ color: #6c757d; }}
        .a-win {{ color: #28a745; font-weight: bold; }}
        .b-win {{ color: #dc3545; font-weight: bold; }}
        .meta {{ color: #666; font-size: 0.9em; margin-bottom: 20px; }}
    </style>
</head>
<body>
    <h1>Cumulative bump_activity A/B Test</h1>
    <div class="meta">
        Generated: {now} | Timeout: {TIMEOUT}s | Problems: {len(problems)}<br>
        <b>WithBump</b>: custom bump_activity (sibling + mandatory overlap) |
        <b>NoBump</b>: base class bump_activity only
    </div>
    <div class="summary">
        <div class="stat-card">
            <div class="stat-value" style="color: #28a745;">{a_wins}</div>
            <div class="stat-label">WithBump Wins</div>
        </div>
        <div class="stat-card">
            <div class="stat-value" style="color: #dc3545;">{b_wins}</div>
            <div class="stat-label">NoBump Wins</div>
        </div>
        <div class="stat-card">
            <div class="stat-value" style="color: #6c757d;">{ties}</div>
            <div class="stat-label">Ties</div>
        </div>
    </div>
    <table>
        <thead>
            <tr>
                <th>Problem</th>
                <th>Year</th>
                <th>WithBump</th>
                <th>NoBump</th>
                <th>Winner</th>
            </tr>
        </thead>
        <tbody>
            {''.join(rows)}
        </tbody>
    </table>
</body>
</html>
"""
    with open(output_path, 'w') as f:
        f.write(html)
    print(f"\nHTML report saved to: {output_path}")

def main():
    # Verify binaries exist
    for label, path in [("WithBump", BINARY_WITH_BUMP), ("NoBump", BINARY_NO_BUMP)]:
        if not os.path.isfile(path):
            print(f"ERROR: {label} binary not found: {path}")
            sys.exit(1)
    print(f"WithBump: {BINARY_WITH_BUMP}")
    print(f"NoBump:   {BINARY_NO_BUMP}")

    cleanup_stale_processes()

    # Collect cumulative problems
    problems = []
    prob_years = {}
    prob_dirs_map = {}

    for year, probs_dir in sorted(PROB_DIRS.items()):
        if not probs_dir.exists():
            print(f"WARNING: {probs_dir} does not exist, skipping")
            continue
        for p in sorted(probs_dir.iterdir(), key=lambda x: natural_sort_key(x.name)):
            if not p.is_dir():
                continue
            if has_cumulative(p):
                prob_name = p.name
                if prob_name in prob_dirs_map:
                    prob_name = f"{p.name}-{year}"
                problems.append(prob_name)
                prob_years[prob_name] = year
                prob_dirs_map[prob_name] = p

    print(f"\nFound {len(problems)} problems with cumulative constraint:")
    for prob in problems:
        print(f"  [{prob_years[prob]}] {prob}")
    print()

    # Compile all .fzn files first
    print("Compiling .fzn files...")
    fzn_map = {}
    prob_types = {}
    data_labels = {}
    to_remove = []

    for prob in problems:
        prob_dir = prob_dirs_map[prob]
        instances = find_instances(prob_dir)
        if not instances:
            print(f"  WARNING: {prob} has no mzn files, skipping")
            to_remove.append(prob)
            continue

        mzn, data, label = instances[0]
        prob_types[prob] = detect_problem_type(mzn)
        if label:
            data_labels[prob] = label

        fzn_path = compile_fzn(mzn, data)
        if fzn_path is None:
            print(f"  WARNING: {prob} failed to compile, skipping")
            to_remove.append(prob)
            continue
        fzn_map[prob] = fzn_path

        # Override prob_type from .fzn (more reliable)
        prob_types[prob] = parse_fzn_solve_type(fzn_path)
        print(f"  {prob} ({prob_types[prob]}) -> {fzn_path}")

    for p in to_remove:
        problems.remove(p)

    print(f"\nCompiled {len(problems)} problems.")

    # Build task list: each problem x 2 solvers
    tasks = []
    for prob in problems:
        fzn = fzn_map[prob]
        tasks.append((prob, "WithBump", BINARY_WITH_BUMP, fzn))
        tasks.append((prob, "NoBump", BINARY_NO_BUMP, fzn))

    print(f"Running {len(tasks)} tasks with {MAX_WORKERS} workers...")
    print(f"Timeout: {TIMEOUT}s per instance")
    print("=" * 90)

    results = {}
    with ProcessPoolExecutor(max_workers=MAX_WORKERS) as executor:
        futures = {executor.submit(run_fzn_solver, *t): t for t in tasks}

        for future in as_completed(futures):
            prob, solver, status, elapsed, obj, stats = future.result()
            results[(prob, solver)] = (status, elapsed, obj, stats)

            obj_str = f" obj={obj}" if obj is not None else ""
            print(f"{prob:30s} {solver:10s} -> {status:8s} {elapsed:6.2f}s{obj_str}")

    # Console summary
    print("\n" + "=" * 90)
    print(f"{'Problem':<35s} {'Year':>4s} {'WithBump':>22s} {'NoBump':>22s} {'Winner':>10s}")
    print("-" * 95)

    a_wins = b_wins = ties = 0
    for prob in problems:
        a_res = results.get((prob, "WithBump"), ("?", 0, None, {}))
        b_res = results.get((prob, "NoBump"), ("?", 0, None, {}))

        ptype = prob_types.get(prob, "?")
        year = prob_years.get(prob, "")
        a_obj_str = f" obj={a_res[2]}" if a_res[2] is not None else ""
        b_obj_str = f" obj={b_res[2]}" if b_res[2] is not None else ""
        a_str = f"{a_res[0]} {a_res[1]:.1f}s{a_obj_str}"
        b_str = f"{b_res[0]} {b_res[1]:.1f}s{b_obj_str}"

        winner, _ = judge_winner(a_res[0], a_res[1], a_res[2], b_res[0], b_res[1], b_res[2], ptype)
        if winner == "WithBump":
            a_wins += 1
        elif winner == "NoBump":
            b_wins += 1
        elif winner == "Tie":
            ties += 1

        if winner == "Tie":
            winner = "~"

        dlabel = data_labels.get(prob, "")
        prob_label = f"{prob} ({ptype})"
        data_str = f" [{dlabel}]" if dlabel else ""
        print(f"{prob_label:<35s} {year:>4s}{data_str:>12s} {a_str:>22s} {b_str:>22s} {winner:>10s}")

    print("-" * 95)
    print(f"Wins: WithBump {a_wins}  /  NoBump {b_wins}  /  Tie {ties}")

    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    output_path = str(BASE_DIR / f"sabori_benchmark_cumulative_bump_{ts}.html")
    generate_html(results, problems, output_path, prob_types, data_labels, prob_years)

    # Cleanup .fzn temp files
    for fzn_path in fzn_map.values():
        try:
            os.unlink(fzn_path)
        except OSError:
            pass

if __name__ == "__main__":
    main()
