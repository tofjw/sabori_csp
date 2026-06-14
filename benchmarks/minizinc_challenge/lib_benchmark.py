"""ベンチマーク共通ライブラリ (Sabori vs CP-SAT)。
bench_*.py が共有するプロセス管理 / run / judge / HTML を集約。
TIMEOUT は各スクリプトが lib_benchmark.TIMEOUT に設定する。
"""
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
TIMEOUT = 30  # 各スクリプトで lib_benchmark.TIMEOUT に上書き
MAX_WORKERS = 4

# circuit を含む問題ディレクトリ（年ごと）
PROB_DIRS = {
    "2012": BASE_DIR / "mznc2012_probs",
    "2013": BASE_DIR / "mznc2013_probs",
    "2014": BASE_DIR / "mznc2014_probs",
    "2015": BASE_DIR / "mznc2015_probs",
    "2016": BASE_DIR / "mznc2016_probs",
    "2017": BASE_DIR / "mznc2017_probs",
    "2018": BASE_DIR / "mznc2018_probs",
    "2019": BASE_DIR / "mznc2019_probs",
    "2020": BASE_DIR / "mznc2020_probs",
    "2021": BASE_DIR / "mznc2021_probs",
    "2022": BASE_DIR / "mznc2022_probs",
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


def judge_winner(s_status, s_time, s_obj, c_status, c_time, c_obj, prob_type="SAT"):
    s_ok = s_status in ("OPTIMAL", "SOL", "UNSAT")
    c_ok = c_status in ("OPTIMAL", "SOL", "UNSAT")
    s_has_sol = s_status in ("OPTIMAL", "SOL", "UNSAT", "TIMEOUT") and s_obj is not None
    c_has_sol = c_status in ("OPTIMAL", "SOL", "UNSAT", "TIMEOUT") and c_obj is not None

    def obj_better(a, b):
        if prob_type == "MIN":
            return a < b
        elif prob_type == "MAX":
            return a > b
        return False

    if s_ok and c_ok:
        if s_status == "OPTIMAL" and c_status != "OPTIMAL":
            return "Sabori", "sabori-win"
        if c_status == "OPTIMAL" and s_status != "OPTIMAL":
            return "CP-SAT", "cpsat-win"
        if s_obj is not None and c_obj is not None and s_obj != c_obj:
            return ("Sabori", "sabori-win") if obj_better(s_obj, c_obj) else ("CP-SAT", "cpsat-win")
        if s_time < c_time * 0.8:
            return "Sabori", "sabori-win"
        if c_time < s_time * 0.8:
            return "CP-SAT", "cpsat-win"
        return "Tie", ""
    if s_ok and not c_ok:
        return "Sabori", "sabori-win"
    if c_ok and not s_ok:
        return "CP-SAT", "cpsat-win"
    if s_has_sol and c_has_sol:
        if s_obj != c_obj:
            return ("Sabori", "sabori-win") if obj_better(s_obj, c_obj) else ("CP-SAT", "cpsat-win")
        return "Tie", ""
    if s_has_sol and not c_has_sol:
        return "Sabori", "sabori-win"
    if c_has_sol and not s_has_sol:
        return "CP-SAT", "cpsat-win"
    return "", ""


def generate_html(results, problems, label="benchmark", output_path=None, prob_types=None, data_labels=None, prob_years=None):
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    if output_path is None:
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        output_path = f"sabori_benchmark_{label}_{ts}.html"

    sabori_wins = 0
    cpsat_wins = 0
    ties = 0
    sabori_timeouts = 0
    cpsat_timeouts = 0

    rows = []
    for prob in problems:
        s_res = results.get((prob, "Sabori"), ("?", 0, None, None))
        c_res = results.get((prob, "CP-SAT"), ("?", 0, None, None))

        s_status, s_time, s_obj, _ = s_res
        c_status, c_time, c_obj, _ = c_res

        ptype = prob_types.get(prob, "?") if prob_types else "?"
        year = prob_years.get(prob, "") if prob_years else ""
        winner, winner_class = judge_winner(s_status, s_time, s_obj, c_status, c_time, c_obj, ptype)
        if winner == "Sabori":
            sabori_wins += 1
        elif winner == "CP-SAT":
            cpsat_wins += 1
        elif winner == "Tie":
            ties += 1

        if s_status == "TIMEOUT" and s_obj is None:
            sabori_timeouts += 1
        if c_status == "TIMEOUT" and c_obj is None:
            cpsat_timeouts += 1

        s_obj_str = f" (obj={s_obj})" if s_obj is not None else ""
        c_obj_str = f" (obj={c_obj})" if c_obj is not None else ""

        s_solved = s_status in ("OPTIMAL", "SOL", "UNSAT")
        c_solved = c_status in ("OPTIMAL", "SOL", "UNSAT")
        s_class = "success" if s_solved else ("timeout" if s_status == "TIMEOUT" else "error")
        c_class = "success" if c_solved else ("timeout" if c_status == "TIMEOUT" else "error")
        dlabel = (data_labels or {}).get(prob, "")
        data_str = f" [{dlabel}]" if dlabel else ""

        rows.append(f"""
        <tr>
            <td>{prob} ({ptype}){data_str}</td>
            <td>{year}</td>
            <td class="{s_class}">{s_status} {s_time:.2f}s{s_obj_str}</td>
            <td class="{c_class}">{c_status} {c_time:.2f}s{c_obj_str}</td>
            <td class="{winner_class}">{winner}</td>
        </tr>""")

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>Sabori vs CP-SAT: {label} Problems</title>
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
    <h1>Sabori vs CP-SAT: {label} Problems</h1>
    <div class="meta">
        Generated: {now} | Timeout: {TIMEOUT}s | Problems: {len(problems)} (from MiniZinc Challenge 2023/2024/2025)
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
        <div class="stat-card">
            <div class="stat-value" style="color: #e67e22;">{sabori_timeouts}</div>
            <div class="stat-label">Sabori Timeouts</div>
        </div>
        <div class="stat-card">
            <div class="stat-value" style="color: #e67e22;">{cpsat_timeouts}</div>
            <div class="stat-label">CP-SAT Timeouts</div>
        </div>
    </div>

    <table>
        <thead>
            <tr>
                <th>Problem</th>
                <th>Year</th>
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

