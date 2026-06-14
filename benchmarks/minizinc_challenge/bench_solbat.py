#!/usr/bin/env python3
"""Sabori vs CP-SAT ベンチマーク比較: solbat 全インスタンス (2012/2014/2016)"""
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
TIMEOUT = 120  # seconds
MAX_WORKERS = 4

# solbat 問題ディレクトリ（年ごと）
PROB_DIRS = {
    "2012": BASE_DIR / "mznc2012_probs" / "solbat",
    "2014": BASE_DIR / "mznc2014_probs" / "solbat",
    "2016": BASE_DIR / "mznc2016_probs" / "solbat",
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

def kill_process_tree(pid):
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

def cleanup_stale_processes():
    for name in ["fzn_sabori", "fzn-cp-sat"]:
        subprocess.run(["pkill", "-x", name], capture_output=True)
    subprocess.run(["pkill", "-x", "minizinc"], capture_output=True)
    time.sleep(0.5)

def run_solver(instance_name, mzn_path, dzn_path, solver_name, solver_id):
    """ソルバーを実行して結果を返す（SAT問題なので -a なし）"""
    # minizinc 自前の -t で graceful にタイムアウトさせる。communicate(timeout=…)
    # を SIGKILL の起点にすると未フラッシュ出力（obj/解）が消えるため。
    cmd = [MINIZINC, "--solver", solver_id, "-t", str(TIMEOUT * 1000),
           "--output-mode", "dzn", str(mzn_path), str(dzn_path)]

    start = time.monotonic()
    is_to = False
    proc = None
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, start_new_session=True)
        try:
            stdout, stderr = proc.communicate(timeout=TIMEOUT + 10)
        except subprocess.TimeoutExpired:
            kill_process_tree(proc.pid)
            proc.wait()
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

        return (instance_name, solver_name, status, elapsed, None)

    except Exception as e:
        return (instance_name, solver_name, "ERROR", 0, str(e))
    finally:
        if proc is not None:
            kill_process_tree(proc.pid)

def judge_winner(s_status, s_time, c_status, c_time):
    """勝者判定（SAT問題用）"""
    s_ok = s_status in ("OPTIMAL", "SOL", "UNSAT")
    c_ok = c_status in ("OPTIMAL", "SOL", "UNSAT")

    if s_ok and c_ok:
        if s_time < c_time * 0.8:
            return "Sabori", "sabori-win"
        if c_time < s_time * 0.8:
            return "CP-SAT", "cpsat-win"
        return "Tie", ""
    if s_ok and not c_ok:
        return "Sabori", "sabori-win"
    if c_ok and not s_ok:
        return "CP-SAT", "cpsat-win"
    return "", ""

def generate_html(results, instances, output_path=None):
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    if output_path is None:
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        output_path = f"sabori_benchmark_solbat_{ts}.html"

    sabori_wins = 0
    cpsat_wins = 0
    ties = 0

    rows = []
    for inst in instances:
        s_res = results.get((inst, "Sabori"), ("?", 0, None))
        c_res = results.get((inst, "CP-SAT"), ("?", 0, None))

        s_status, s_time, _ = s_res
        c_status, c_time, _ = c_res

        winner, winner_class = judge_winner(s_status, s_time, c_status, c_time)
        if winner == "Sabori":
            sabori_wins += 1
        elif winner == "CP-SAT":
            cpsat_wins += 1
        elif winner == "Tie":
            ties += 1

        s_solved = s_status in ("OPTIMAL", "SOL", "UNSAT")
        c_solved = c_status in ("OPTIMAL", "SOL", "UNSAT")
        s_class = "success" if s_solved else ("timeout" if s_status == "TIMEOUT" else "error")
        c_class = "success" if c_solved else ("timeout" if c_status == "TIMEOUT" else "error")

        rows.append(f"""
        <tr>
            <td>{inst}</td>
            <td class="{s_class}">{s_status} {s_time:.2f}s</td>
            <td class="{c_class}">{c_status} {c_time:.2f}s</td>
            <td class="{winner_class}">{winner}</td>
        </tr>""")

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>Solbat Benchmark: Sabori vs CP-SAT</title>
    <style>
        body {{
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            max-width: 1200px;
            margin: 0 auto;
            padding: 20px;
            background: #f5f5f5;
        }}
        h1 {{
            color: #333;
            border-bottom: 2px solid #4285f4;
            padding-bottom: 10px;
        }}
        .summary {{
            background: white;
            padding: 20px;
            border-radius: 8px;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
            margin-bottom: 20px;
            display: flex;
            gap: 20px;
        }}
        .stat-card {{
            text-align: center;
            padding: 15px 30px;
            background: #f8f9fa;
            border-radius: 6px;
        }}
        .stat-value {{
            font-size: 2em;
            font-weight: bold;
            color: #4285f4;
        }}
        .stat-label {{
            color: #666;
            margin-top: 5px;
        }}
        table {{
            width: 100%;
            border-collapse: collapse;
            background: white;
            border-radius: 8px;
            overflow: hidden;
            box-shadow: 0 2px 4px rgba(0,0,0,0.1);
        }}
        th, td {{
            padding: 12px 15px;
            text-align: left;
            border-bottom: 1px solid #eee;
        }}
        th {{
            background: #4285f4;
            color: white;
        }}
        tr:hover {{
            background: #f8f9fa;
        }}
        .success {{ color: #28a745; font-weight: bold; }}
        .timeout {{ color: #dc3545; }}
        .error {{ color: #6c757d; }}
        .sabori-win {{ color: #28a745; font-weight: bold; }}
        .cpsat-win {{ color: #dc3545; font-weight: bold; }}
        .meta {{
            color: #666;
            font-size: 0.9em;
            margin-bottom: 20px;
        }}
    </style>
</head>
<body>
    <h1>Solbat Benchmark: Sabori vs CP-SAT</h1>
    <div class="meta">
        Generated: {now} | Timeout: {TIMEOUT}s | Instances: {len(instances)} (from MiniZinc Challenge 2012/2014/2016)
    </div>

    <div class="summary">
        <div class="stat-card">
            <div class="stat-value" style="color: #28a745;">{sabori_wins}</div>
            <div class="stat-label">Sabori Wins</div>
        </div>
        <div class="stat-card">
            <div class="stat-value" style="color: #dc3545;">{cpsat_wins}</div>
            <div class="stat-label">CP-SAT Wins</div>
        </div>
        <div class="stat-card">
            <div class="stat-value" style="color: #6c757d;">{ties}</div>
            <div class="stat-label">Ties</div>
        </div>
    </div>

    <table>
        <thead>
            <tr>
                <th>Instance</th>
                <th>Sabori</th>
                <th>CP-SAT</th>
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
    cleanup_stale_processes()

    # 全年度から solbat インスタンスを収集
    instances = []
    tasks = []

    for year, prob_dir in sorted(PROB_DIRS.items()):
        if not prob_dir.exists():
            print(f"WARNING: {prob_dir} does not exist, skipping")
            continue
        mzn = prob_dir / "sb.mzn"
        if not mzn.exists():
            print(f"WARNING: {mzn} not found, skipping")
            continue
        dzn_files = sorted(prob_dir.glob("*.dzn"), key=natural_sort_key)
        for dzn in dzn_files:
            inst_name = f"[{year}] {dzn.stem}"
            instances.append(inst_name)
            tasks.append((inst_name, mzn, dzn, "Sabori", SABORI_MSC))
            tasks.append((inst_name, mzn, dzn, "CP-SAT", "cp-sat"))

    print(f"Solbat Benchmark (SAT)")
    print(f"Running {len(tasks)} tasks ({len(instances)} instances x 2 solvers) with {MAX_WORKERS} workers...")
    print(f"Timeout: {TIMEOUT}s per instance")
    print("=" * 80)

    results = {}
    with ProcessPoolExecutor(max_workers=MAX_WORKERS) as executor:
        futures = {executor.submit(run_solver, *t): t for t in tasks}

        for future in as_completed(futures):
            inst, solver, status, elapsed, err = future.result()
            key = (inst, solver)
            results[key] = (status, elapsed, err)

            err_str = f" ({err})" if err else ""
            print(f"{inst:40s} {solver:8s} -> {status:8s} {elapsed:6.2f}s{err_str}")

    # コンソール出力
    print("\n" + "=" * 80)
    print("Solbat Benchmark Summary:")
    print("=" * 80)
    print(f"{'Instance':<40s} {'Sabori':>15s} {'CP-SAT':>15s} {'Winner':>10s}")
    print("-" * 85)

    sabori_wins = 0
    cpsat_wins = 0
    ties = 0
    for inst in instances:
        s_res = results.get((inst, "Sabori"), ("?", 0, None))
        c_res = results.get((inst, "CP-SAT"), ("?", 0, None))

        s_str = f"{s_res[0]} {s_res[1]:.1f}s"
        c_str = f"{c_res[0]} {c_res[1]:.1f}s"

        winner, _ = judge_winner(s_res[0], s_res[1], c_res[0], c_res[1])
        if winner == "Sabori":
            sabori_wins += 1
        elif winner == "CP-SAT":
            cpsat_wins += 1
        elif winner == "Tie":
            ties += 1

        if winner == "Tie":
            winner = "~"

        print(f"{inst:<40s} {s_str:>15s} {c_str:>15s} {winner:>10s}")

    print("-" * 85)
    print(f"Wins: Sabori {sabori_wins}  /  CP-SAT {cpsat_wins}  /  Tie {ties}")

    generate_html(results, instances)

if __name__ == "__main__":
    main()
