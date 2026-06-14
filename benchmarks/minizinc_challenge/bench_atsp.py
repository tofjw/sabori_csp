#!/usr/bin/env python3
"""Sabori vs CP-SAT ベンチマーク比較: atsp 全インスタンス"""
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
PROBLEM = "atsp"
PROB_DIR = BASE_DIR / "mznc2025_probs" / PROBLEM

def natural_sort_key(s):
    return [int(t) if t.isdigit() else t.lower() for t in re.split('([0-9]+)', str(s))]

def get_descendant_pids(pid):
    """/proc を使ってpidの子孫プロセスを再帰的に取得"""
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
    """プロセスツリー全体を確実に殺す（子孫→親の順）"""
    descendants = get_descendant_pids(pid)
    # 子孫を先に殺す（葉から根へ）
    for dpid in descendants:
        try:
            os.kill(dpid, signal.SIGKILL)
        except OSError:
            pass
    # 親プロセスグループも殺す
    try:
        os.killpg(os.getpgid(pid), signal.SIGKILL)
    except OSError:
        pass
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass

def cleanup_stale_processes():
    """既存のベンチマークプロセスを停止"""
    subprocess.run(["pkill", "-f", "fzn_sabori|fzn-cp-sat|minizinc"],
                   capture_output=True)
    time.sleep(0.5)

def run_solver(instance_name, inst_path, solver_name, solver_id):
    """ソルバーを実行して結果を返す"""
    mzn = str(PROB_DIR / f"{PROBLEM}.mzn")

    # minizinc 自前の -t で graceful にタイムアウトさせる。communicate(timeout=…)
    # を SIGKILL の起点にすると未フラッシュ出力（obj/解）が消えるため。
    cmd = [MINIZINC, "--solver", solver_id, "-t", str(TIMEOUT * 1000), "-a",
           "--output-objective", "--output-mode", "dzn", mzn, str(inst_path)]

    start = time.time()
    is_to = False
    proc = None
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, start_new_session=True)
        try:
            stdout, stderr = proc.communicate(timeout=TIMEOUT + 10)
        except subprocess.TimeoutExpired:
            kill_process_tree(proc.pid)
            stdout, _ = proc.communicate()
            is_to = True

        elapsed = time.time() - start
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

        return (instance_name, solver_name, status, elapsed, obj, None)

    except Exception as e:
        return (instance_name, solver_name, "ERROR", 0, None, str(e))
    finally:
        # 正常終了・異常終了問わず残存プロセスを確実に殺す
        if proc is not None:
            kill_process_tree(proc.pid)

def judge_winner(s_status, s_time, s_obj, c_status, c_time, c_obj):
    """勝者判定（MIN問題用）"""
    s_ok = s_status in ("OPTIMAL", "SOL", "UNSAT")
    c_ok = c_status in ("OPTIMAL", "SOL", "UNSAT")
    s_has_sol = s_status in ("OPTIMAL", "SOL", "UNSAT", "TIMEOUT") and s_obj is not None
    c_has_sol = c_status in ("OPTIMAL", "SOL", "UNSAT", "TIMEOUT") and c_obj is not None

    if s_ok and c_ok:
        # 両方解あり: OPTIMAL > SOL/UNSAT
        if s_status == "OPTIMAL" and c_status != "OPTIMAL":
            return "Sabori", "sabori-win"
        if c_status == "OPTIMAL" and s_status != "OPTIMAL":
            return "CP-SAT", "cpsat-win"
        # 同ステータス: 目的関数値で比較
        if s_obj is not None and c_obj is not None and s_obj != c_obj:
            return ("Sabori", "sabori-win") if s_obj < c_obj else ("CP-SAT", "cpsat-win")
        # 同値: 時間で比較
        if s_time < c_time * 0.8:
            return "Sabori", "sabori-win"
        if c_time < s_time * 0.8:
            return "CP-SAT", "cpsat-win"
        return "Tie", ""
    if s_ok and not c_ok:
        return "Sabori", "sabori-win"
    if c_ok and not s_ok:
        return "CP-SAT", "cpsat-win"
    # 両方TIMEOUT等: 途中解の目的関数値で比較
    if s_has_sol and c_has_sol and s_obj != c_obj:
        return ("Sabori", "sabori-win") if s_obj < c_obj else ("CP-SAT", "cpsat-win")
    if s_has_sol and not c_has_sol:
        return "Sabori", "sabori-win"
    if c_has_sol and not s_has_sol:
        return "CP-SAT", "cpsat-win"
    return "", ""

def generate_html(results, instances, output_path="bench_atsp.html"):
    """HTML レポートを生成"""
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    sabori_wins = 0
    cpsat_wins = 0
    ties = 0

    rows = []
    for inst in instances:
        s_res = results.get((inst, "Sabori"), ("?", 0, None, None))
        c_res = results.get((inst, "CP-SAT"), ("?", 0, None, None))

        s_status, s_time, s_obj, _ = s_res
        c_status, c_time, c_obj, _ = c_res

        winner, winner_class = judge_winner(s_status, s_time, s_obj, c_status, c_time, c_obj)
        if winner == "Sabori":
            sabori_wins += 1
        elif winner == "CP-SAT":
            cpsat_wins += 1
        elif winner == "Tie":
            ties += 1

        s_obj_str = f" (obj={s_obj})" if s_obj is not None else ""
        c_obj_str = f" (obj={c_obj})" if c_obj is not None else ""

        s_solved = s_status in ("OPTIMAL", "SOL", "UNSAT")
        c_solved = c_status in ("OPTIMAL", "SOL", "UNSAT")
        s_class = "success" if s_solved else ("timeout" if s_status == "TIMEOUT" else "error")
        c_class = "success" if c_solved else ("timeout" if c_status == "TIMEOUT" else "error")

        rows.append(f"""
        <tr>
            <td>{inst}</td>
            <td class="{s_class}">{s_status} {s_time:.2f}s{s_obj_str}</td>
            <td class="{c_class}">{c_status} {c_time:.2f}s{c_obj_str}</td>
            <td class="{winner_class}">{winner}</td>
        </tr>""")

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>ATSP Benchmark: Sabori vs CP-SAT</title>
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
    <h1>ATSP Benchmark: Sabori vs CP-SAT</h1>
    <div class="meta">
        Generated: {now} | Timeout: {TIMEOUT}s | Instances: {len(instances)}
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
    # 既存プロセスを停止
    cleanup_stale_processes()

    # dznファイルを列挙
    dzn_files = sorted(PROB_DIR.glob("*.dzn"), key=natural_sort_key)
    if not dzn_files:
        print(f"No .dzn files found in {PROB_DIR}")
        sys.exit(1)

    instances = [f.stem for f in dzn_files]

    results = {}
    tasks = []

    for dzn in dzn_files:
        inst_name = dzn.stem
        tasks.append((inst_name, dzn, "Sabori", SABORI_MSC))
        tasks.append((inst_name, dzn, "CP-SAT", "cp-sat"))

    print(f"Problem: {PROBLEM} (MIN)")
    print(f"Running {len(tasks)} tasks ({len(instances)} instances x 2 solvers) with {MAX_WORKERS} workers...")
    print(f"Timeout: {TIMEOUT}s per instance")
    print("=" * 80)

    with ProcessPoolExecutor(max_workers=MAX_WORKERS) as executor:
        futures = {executor.submit(run_solver, *t): t for t in tasks}

        for future in as_completed(futures):
            inst, solver, status, elapsed, obj, err = future.result()
            key = (inst, solver)
            results[key] = (status, elapsed, obj, err)

            obj_str = f" obj={obj}" if obj is not None else ""
            err_str = f" ({err})" if err else ""
            print(f"{inst:30s} {solver:8s} -> {status:8s} {elapsed:6.2f}s{obj_str}{err_str}")

    # コンソール出力
    print("\n" + "=" * 80)
    print(f"ATSP Benchmark Summary:")
    print("=" * 80)
    print(f"{'Instance':<35s} {'Sabori':>20s} {'CP-SAT':>20s} {'Winner':>10s}")
    print("-" * 90)

    for inst in instances:
        s_res = results.get((inst, "Sabori"), ("?", 0, None, None))
        c_res = results.get((inst, "CP-SAT"), ("?", 0, None, None))

        s_obj_str = f" obj={s_res[2]}" if s_res[2] is not None else ""
        c_obj_str = f" obj={c_res[2]}" if c_res[2] is not None else ""
        s_str = f"{s_res[0]} {s_res[1]:.1f}s{s_obj_str}"
        c_str = f"{c_res[0]} {c_res[1]:.1f}s{c_obj_str}"

        winner, _ = judge_winner(s_res[0], s_res[1], s_res[2], c_res[0], c_res[1], c_res[2])
        if winner == "Tie":
            winner = "~"

        print(f"{inst:<35s} {s_str:>20s} {c_str:>20s} {winner:>10s}")

    generate_html(results, instances)

if __name__ == "__main__":
    main()
