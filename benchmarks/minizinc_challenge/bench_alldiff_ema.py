#!/usr/bin/env python3
"""Activity jitter A/B test: with vs without jitter on activity bump.
Uses all_different problems (broad coverage). Runs fzn_sabori directly."""
import subprocess, sys, os, re, time, signal, json
from pathlib import Path
from datetime import datetime
from concurrent.futures import ProcessPoolExecutor, as_completed

BASE_DIR = Path(__file__).resolve().parent
MINIZINC = "/snap/bin/minizinc"
SABORI_MSC = str(Path(__file__).resolve().parent.parent.parent / "build" / "share" / "minizinc" / "solvers" / "sabori_csp.msc")
TIMEOUT = 60
MAX_WORKERS = 4

BINARY_EMA = str(BASE_DIR / "../../build/src/fzn/fzn_sabori")
BINARY_NO_EMA = str(BASE_DIR / "../../.claude/worktrees/agent-a866e2c4/build/src/fzn/fzn_sabori")

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
        return [(str(mzn), str(data_files[0]), data_files[0].stem)]
    return [(str(mzn), None, mzn.stem)]

def detect_problem_type(mzn_path):
    content = Path(mzn_path).read_text()
    lines = [l for l in content.splitlines() if not l.strip().startswith('%')]
    text = '\n'.join(lines)
    if re.search(r'\bminimize\b', text): return "MIN"
    elif re.search(r'\bmaximize\b', text): return "MAX"
    return "SAT"

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
        if a_status == "OPTIMAL" and b_status != "OPTIMAL": return "EMA", "a-win"
        if b_status == "OPTIMAL" and a_status != "OPTIMAL": return "NoEMA", "b-win"
        if a_obj is not None and b_obj is not None and a_obj != b_obj:
            return ("EMA", "a-win") if obj_better(a_obj, b_obj) else ("NoEMA", "b-win")
        if a_time < b_time * 0.8: return "EMA", "a-win"
        if b_time < a_time * 0.8: return "NoEMA", "b-win"
        return "Tie", ""
    if a_ok and not b_ok: return "EMA", "a-win"
    if b_ok and not a_ok: return "NoEMA", "b-win"
    a_has = a_obj is not None
    b_has = b_obj is not None
    if a_has and b_has:
        if a_obj != b_obj:
            return ("EMA", "a-win") if obj_better(a_obj, b_obj) else ("NoEMA", "b-win")
        return "Tie", ""
    if a_has and not b_has: return "EMA", "a-win"
    if b_has and not a_has: return "NoEMA", "b-win"
    return "", ""

def generate_html(results, problems, output_path, prob_types, data_labels, prob_years):
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    a_wins = b_wins = ties = 0
    rows = []
    for prob in problems:
        a_res = results.get((prob, "EMA"), ("?", 0, None))
        b_res = results.get((prob, "NoEMA"), ("?", 0, None))
        a_status, a_time, a_obj = a_res
        b_status, b_time, b_obj = b_res
        ptype = prob_types.get(prob, "?")
        year = prob_years.get(prob, "")
        winner, winner_class = judge_winner(a_status, a_time, a_obj, b_status, b_time, b_obj, ptype)
        if winner == "EMA": a_wins += 1
        elif winner == "NoEMA": b_wins += 1
        elif winner == "Tie": ties += 1
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
    <title>Activity EMA A/B Test</title>
    <style>
        body {{ font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
               max-width: 1200px; margin: 0 auto; padding: 20px; background: #f5f5f5; }}
        h1 {{ color: #333; border-bottom: 2px solid #4285f4; padding-bottom: 10px; }}
        .summary {{ background: white; padding: 20px; border-radius: 8px;
                    box-shadow: 0 2px 4px rgba(0,0,0,0.1); margin-bottom: 20px; display: flex; gap: 20px; }}
        .stat-card {{ text-align: center; padding: 15px 30px; background: #f8f9fa; border-radius: 6px; }}
        .stat-value {{ font-size: 2em; font-weight: bold; color: #4285f4; }}
        .stat-label {{ color: #666; margin-top: 5px; }}
        table {{ width: 100%; border-collapse: collapse; background: white;
                 border-radius: 8px; overflow: hidden; box-shadow: 0 2px 4px rgba(0,0,0,0.1); }}
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
    <h1>EMA Gradient A/B Test (all_different problems)</h1>
    <div class="meta">
        Generated: {now} | Timeout: {TIMEOUT}s | Problems: {len(problems)}<br>
        <b>Jitter</b>: gradient EMA enabled |
        <b>NoJitter</b>: gradient EMA disabled
    </div>
    <div class="summary">
        <div class="stat-card">
            <div class="stat-value" style="color: #28a745;">{a_wins}</div>
            <div class="stat-label">EMA Wins</div>
        </div>
        <div class="stat-card">
            <div class="stat-value" style="color: #dc3545;">{b_wins}</div>
            <div class="stat-label">NoEMA Wins</div>
        </div>
        <div class="stat-card">
            <div class="stat-value" style="color: #6c757d;">{ties}</div>
            <div class="stat-label">Ties</div>
        </div>
    </div>
    <table>
        <thead><tr><th>Problem</th><th>Year</th><th>Jitter</th><th>NoJitter</th><th>Winner</th></tr></thead>
        <tbody>{''.join(rows)}</tbody>
    </table>
</body>
</html>"""
    with open(output_path, 'w') as f:
        f.write(html)
    print(f"\nHTML report saved to: {output_path}")

def main():
    for label, path in [("EMA", BINARY_EMA), ("NoEMA", BINARY_NO_EMA)]:
        if not os.path.isfile(path):
            print(f"ERROR: {label} binary not found: {path}")
            sys.exit(1)
    print(f"EMA:   {BINARY_EMA}")
    print(f"NoEMA: {BINARY_NO_EMA}")

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
            if has_alldifferent(p):
                name = p.name
                if name in prob_dirs_map:
                    name = f"{p.name}-{year}"
                problems.append(name)
                prob_years[name] = year
                prob_dirs_map[name] = p

    print(f"\nFound {len(problems)} all_different problems")

    # Compile all .fzn
    print("Compiling .fzn files...")
    fzn_map = {}
    prob_types = {}
    data_labels = {}
    to_remove = []
    for prob in problems:
        instances = find_instances(prob_dirs_map[prob])
        if not instances:
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
        prob_types[prob] = parse_fzn_solve_type(fzn_path)
        print(f"  {prob} ({prob_types[prob]})")
    for p in to_remove:
        problems.remove(p)

    print(f"\nCompiled {len(problems)} problems.")

    tasks = []
    for prob in problems:
        fzn = fzn_map[prob]
        tasks.append((prob, "EMA", BINARY_EMA, fzn))
        tasks.append((prob, "NoEMA", BINARY_NO_EMA, fzn))

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
    print(f"{'Problem':<35s} {'Year':>4s} {'Jitter':>22s} {'NoJitter':>22s} {'Winner':>10s}")
    print("-" * 100)

    a_wins = b_wins = ties = 0
    for prob in problems:
        a_res = results.get((prob, "EMA"), ("?", 0, None))
        b_res = results.get((prob, "NoEMA"), ("?", 0, None))
        ptype = prob_types.get(prob, "?")
        year = prob_years.get(prob, "")
        a_obj_str = f" obj={a_res[2]}" if a_res[2] is not None else ""
        b_obj_str = f" obj={b_res[2]}" if b_res[2] is not None else ""
        a_str = f"{a_res[0]} {a_res[1]:.1f}s{a_obj_str}"
        b_str = f"{b_res[0]} {b_res[1]:.1f}s{b_obj_str}"
        winner, _ = judge_winner(a_res[0], a_res[1], a_res[2], b_res[0], b_res[1], b_res[2], ptype)
        if winner == "EMA": a_wins += 1
        elif winner == "NoEMA": b_wins += 1
        elif winner == "Tie": ties += 1
        if winner == "Tie": winner = "~"
        dlabel = data_labels.get(prob, "")
        prob_label = f"{prob} ({ptype})"
        data_str = f" [{dlabel}]" if dlabel else ""
        print(f"{prob_label:<35s} {year:>4s}{data_str:>12s} {a_str:>22s} {b_str:>22s} {winner:>10s}")

    print("-" * 100)
    print(f"Wins: EMA {a_wins}  /  NoEMA {b_wins}  /  Tie {ties}")

    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    output_path = str(BASE_DIR / f"sabori_benchmark_alldiff_ema_{ts}.html")
    generate_html(results, problems, output_path, prob_types, data_labels, prob_years)

    # Cleanup
    for fzn_path in fzn_map.values():
        try: os.unlink(fzn_path)
        except OSError: pass

if __name__ == "__main__":
    main()
