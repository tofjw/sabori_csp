#!/usr/bin/env python3
"""Sabori vs CP-SAT ベンチマーク比較スクリプト (MiniZinc Challenge)

Usage:
    python3 bench_compare.py 2024
    python3 bench_compare.py 2022 2023 2024 2025
"""
import subprocess
import sys
import os
import re
import time
import signal
import tempfile
from pathlib import Path
from datetime import datetime
from concurrent.futures import ProcessPoolExecutor, as_completed

BASE_DIR = Path(__file__).resolve().parent
MINIZINC = str(BASE_DIR / "squashfs-root/usr/bin/minizinc")
TIMEOUT = 30  # seconds
MAX_WORKERS = 4

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

def kill_process_tree(proc):
    """プロセスツリー全体を確実に殺し、zombie を回収する。

    proc: subprocess.Popen オブジェクト
    """
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
    # zombie 回収（wait しないと Z 状態で残る）
    try:
        proc.wait(timeout=5)
    except Exception:
        pass

def cleanup_stale_processes():
    """既存のベンチマークプロセスを停止（snapfuse 等の無関係なプロセスを巻き込まない）"""
    for name in ["fzn_sabori", "fzn-cp-sat"]:
        subprocess.run(["pkill", "-x", name], capture_output=True)
    # minizinc は実行ファイル名でマッチ（-f だと snapfuse にもマッチするため -x を使う）
    subprocess.run(["pkill", "-x", "minizinc"], capture_output=True)
    time.sleep(0.5)

def detect_problem_type(mzn_path):
    """mznファイルのsolve文からSAT/MIN/MAXを判定"""
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
    """問題ディレクトリからインスタンス一覧を返す。

    通常: 1つの.mzn + 複数の.dzn/.json → 最小データファイル1つ
    word-equations型: 複数の.mzn + データなし → 各.mznがインスタンス

    Returns: list of (mzn_path, data_path_or_None, instance_label)
    """
    prob_dir = Path(prob_dir)
    mzn_files = sorted(prob_dir.glob("*.mzn"), key=natural_sort_key)
    dzn_files = sorted(prob_dir.glob("*.dzn"), key=natural_sort_key)
    json_files = sorted(prob_dir.glob("*.json"), key=natural_sort_key)
    data_files = dzn_files + json_files

    if not mzn_files:
        return []

    if len(mzn_files) > 1 and not data_files:
        # 複数mzn、データなし → 各mznがインスタンス（最小1つ）
        mzn = mzn_files[0]
        return [(str(mzn), None, mzn.stem)]

    # 通常パターン: 1つのmzn + 最小データファイル
    mzn = mzn_files[0]
    if data_files:
        inst = data_files[0]
        return [(str(mzn), str(inst), inst.stem)]
    else:
        return [(str(mzn), None, mzn.stem)]

def parse_last_solution(output):
    """DZN出力から最終解ブロックを抽出し、変数名→値文字列の辞書を返す。

    解がない場合はNoneを返す。
    """
    # ---------- で区切られたブロックのうち最後のものを取得
    blocks = re.split(r'^----------$', output, flags=re.MULTILINE)
    # blocks[-1] は最後の ---------- の後（空 or ========== 行）
    # blocks[-2] が最終解ブロック（存在すれば）
    if len(blocks) < 2:
        return None

    last_block = blocks[-2]
    solution = {}
    # 複数行にまたがる代入文を結合してパース
    # まず行を結合（セミコロンで終わるまで）
    joined = re.sub(r'\n\s+', ' ', last_block)
    for m in re.finditer(r'(\w+)\s*=\s*(.+?)\s*;', joined):
        varname = m.group(1)
        value = m.group(2).strip()
        if varname == '_objective':
            continue
        solution[varname] = value

    return solution if solution else None


def verify_solution(mzn, data, solution, timeout=10):
    """saboriの解をGecodeで検証する。

    Returns: "CHECK_OK" / "CHECK_FAIL" / "CHECK_TIMEOUT" / "CHECK_SKIP"
    """
    if not solution:
        return "CHECK_SKIP"

    mzn_abs = str(Path(mzn).resolve())

    try:
        with tempfile.TemporaryDirectory() as tmpdir:
            checker_path = Path(tmpdir) / "checker.mzn"
            lines = [f'include "{mzn_abs}";']
            for varname, value in solution.items():
                lines.append(f'constraint {varname} = {value};')
            checker_path.write_text('\n'.join(lines) + '\n')

            cmd = [MINIZINC, "--solver", "gecode", checker_path]
            if data:
                cmd.append(data)

            proc = subprocess.run(
                cmd, capture_output=True, text=True, timeout=timeout
            )
            stdout = proc.stdout
            stderr = proc.stderr
            if "----------" in stdout:
                return "CHECK_OK"
            elif "=====UNSATISFIABLE=====" in stdout:
                return "CHECK_FAIL"
            elif proc.returncode != 0:
                # Gecode error (compilation error, crash, etc.) — retry with Chuffed
                cmd_chuffed = [MINIZINC, "--solver", "chuffed", str(checker_path)]
                if data:
                    cmd_chuffed.append(data)
                proc2 = subprocess.run(
                    cmd_chuffed, capture_output=True, text=True, timeout=timeout
                )
                if "----------" in proc2.stdout:
                    return "CHECK_OK"
                elif "=====UNSATISFIABLE=====" in proc2.stdout:
                    return "CHECK_FAIL"
                else:
                    return "CHECK_SKIP"
            else:
                return "CHECK_FAIL"
    except subprocess.TimeoutExpired:
        return "CHECK_TIMEOUT"
    except Exception:
        return "CHECK_SKIP"


def run_solver(problem, solver_name, solver_id, mzn, data, prob_type="SAT"):
    """ソルバーを実行して結果を返す"""
    data_label = Path(data).stem if data else None
    cmd = [MINIZINC, "--solver", solver_id]
    if prob_type != "SAT":
        cmd.append("-a")
    cmd.extend(["--output-objective", "--output-mode", "dzn", mzn])
    if data:
        cmd.append(data)

    start = time.monotonic()
    proc = None
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, start_new_session=True)
        try:
            stdout, stderr = proc.communicate(timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            kill_process_tree(proc)
            stdout, _ = proc.communicate()

            obj = None
            if stdout:
                for line in reversed(stdout.split('\n')):
                    m = re.search(r'_objective\s*=\s*(-?\d+)', line)
                    if m:
                        obj = int(m.group(1))
                        break

            check = None
            if solver_name == "Sabori" and stdout:
                sol = parse_last_solution(stdout)
                if sol:
                    check = verify_solution(mzn, data, sol)

            return (problem, solver_name, "TIMEOUT", TIMEOUT, obj, None, data_label, check)

        elapsed = time.monotonic() - start
        output = stdout

        if "==========\n" in output:
            status = "OPTIMAL"
        elif "----------\n" in output:
            status = "SOL"
        elif "=====UNSATISFIABLE=====" in output:
            status = "UNSAT"
        else:
            status = "UNKNOWN"

        obj = None
        for line in reversed(output.split('\n')):
            m = re.search(r'_objective\s*=\s*(-?\d+)', line)
            if m:
                obj = int(m.group(1))
                break

        check = None
        if solver_name == "Sabori" and status in ("OPTIMAL", "SOL"):
            sol = parse_last_solution(output)
            if sol:
                check = verify_solution(mzn, data, sol)

        return (problem, solver_name, status, elapsed, obj, None, data_label, check)

    except Exception as e:
        return (problem, solver_name, "ERROR", 0, None, str(e), data_label, None)
    finally:
        if proc is not None:
            kill_process_tree(proc)

def judge_winner(s_status, s_time, s_obj, c_status, c_time, c_obj, prob_type="SAT"):
    """勝者判定（MIN/MAX/SAT対応）"""
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
        # 両方解あり: OPTIMAL > SOL/UNSAT
        if s_status == "OPTIMAL" and c_status != "OPTIMAL":
            return "Sabori", "sabori-win"
        if c_status == "OPTIMAL" and s_status != "OPTIMAL":
            return "CP-SAT", "cpsat-win"
        # 同ステータス: 目的関数値で比較
        if s_obj is not None and c_obj is not None and s_obj != c_obj:
            return ("Sabori", "sabori-win") if obj_better(s_obj, c_obj) else ("CP-SAT", "cpsat-win")
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
    if s_has_sol and c_has_sol:
        if s_obj != c_obj:
            return ("Sabori", "sabori-win") if obj_better(s_obj, c_obj) else ("CP-SAT", "cpsat-win")
        return "Tie", ""
    if s_has_sol and not c_has_sol:
        return "Sabori", "sabori-win"
    if c_has_sol and not s_has_sol:
        return "CP-SAT", "cpsat-win"
    return "", ""

def generate_html(results, problems, year, output_path=None, prob_types=None, data_labels=None):
    """HTML レポートを生成"""
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    if output_path is None:
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")
        output_path = f"sabori_benchmark_{year}_{ts}.html"

    sabori_wins = 0
    cpsat_wins = 0
    ties = 0
    sabori_timeouts = 0
    cpsat_timeouts = 0
    check_fails = 0

    rows = []
    for prob in problems:
        s_res = results.get((prob, "Sabori"), ("?", 0, None, None, None))
        c_res = results.get((prob, "CP-SAT"), ("?", 0, None, None, None))

        s_status, s_time, s_obj, _, s_check = s_res
        c_status, c_time, c_obj, _ = c_res[:4]

        ptype = prob_types.get(prob, "?") if prob_types else "?"
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
        if s_check == "CHECK_FAIL":
            check_fails += 1

        s_obj_str = f" (obj={s_obj})" if s_obj is not None else ""
        c_obj_str = f" (obj={c_obj})" if c_obj is not None else ""

        s_solved = s_status in ("OPTIMAL", "SOL", "UNSAT")
        c_solved = c_status in ("OPTIMAL", "SOL", "UNSAT")
        s_class = "success" if s_solved else ("timeout" if s_status == "TIMEOUT" else "error")
        c_class = "success" if c_solved else ("timeout" if c_status == "TIMEOUT" else "error")
        dlabel = (data_labels or {}).get(prob, "")
        data_str = f" [{dlabel}]" if dlabel else ""

        check_map = {
            "CHECK_OK": ("check-ok", "OK"),
            "CHECK_FAIL": ("check-fail", "FAIL"),
            "CHECK_TIMEOUT": ("check-timeout", "T/O"),
            "CHECK_SKIP": ("check-skip", "-"),
            None: ("check-skip", "-"),
        }
        check_class, check_label = check_map.get(s_check, ("check-skip", "-"))

        rows.append(f"""
        <tr>
            <td>{prob} ({ptype}){data_str}</td>
            <td class="{s_class}">{s_status} {s_time:.2f}s{s_obj_str}</td>
            <td class="{c_class}">{c_status} {c_time:.2f}s{c_obj_str}</td>
            <td class="{check_class}">{check_label}</td>
            <td class="{winner_class}">{winner}</td>
        </tr>""")

    html = f"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <title>Sabori vs CP-SAT Benchmark (MiniZinc Challenge {year})</title>
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
        .check-ok {{ color: #28a745; font-weight: bold; }}
        .check-fail {{ color: #dc3545; font-weight: bold; }}
        .check-timeout {{ color: #e67e22; }}
        .check-skip {{ color: #adb5bd; }}
        .meta {{
            color: #666;
            font-size: 0.9em;
            margin-bottom: 20px;
        }}
    </style>
</head>
<body>
    <h1>Sabori vs CP-SAT Benchmark (MiniZinc Challenge {year})</h1>
    <div class="meta">
        Generated: {now} | Timeout: {TIMEOUT}s | Problems: {len(problems)}
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
        <div class="stat-card">
            <div class="stat-value" style="color: {'#dc3545' if check_fails > 0 else '#28a745'};">{check_fails}</div>
            <div class="stat-label">Check Fails</div>
        </div>
    </div>

    <table>
        <thead>
            <tr>
                <th>Problem</th>
                <th>Sabori</th>
                <th>CP-SAT</th>
                <th>Check</th>
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

MZN_CHALLENGE_REPO = "https://github.com/MiniZinc/mzn-challenge.git"

def fetch_problems(year):
    """MiniZinc Challenge リポジトリから問題セットを取得"""
    probs_dir = BASE_DIR / f"mznc{year}_probs"
    print(f"Fetching MiniZinc Challenge {year} problems...")

    # sparse checkout で指定年度のみ取得
    with tempfile.TemporaryDirectory() as tmpdir:
        tmp = Path(tmpdir) / "mzn-challenge"
        subprocess.run(
            ["git", "clone", "--filter=blob:none", "--sparse", "--branch", "develop",
             MZN_CHALLENGE_REPO, str(tmp)],
            check=True, capture_output=True, text=True
        )
        subprocess.run(
            ["git", "-C", str(tmp), "sparse-checkout", "set", str(year)],
            check=True, capture_output=True, text=True
        )
        src = tmp / str(year)
        if not src.exists():
            print(f"ERROR: Year {year} not found in mzn-challenge repository")
            return False

        import shutil
        shutil.copytree(src, probs_dir)

    print(f"Downloaded to {probs_dir}")
    return True

def run_year(year):
    """指定年のベンチマークを実行"""
    probs_dir = BASE_DIR / f"mznc{year}_probs"
    if not probs_dir.exists():
        if not fetch_problems(year):
            return

    problems = sorted(
        [p.name for p in probs_dir.iterdir() if p.is_dir()],
        key=natural_sort_key
    )

    results = {}
    tasks = []
    prob_types = {}
    data_labels = {}

    for prob in problems:
        prob_dir = probs_dir / prob
        instances = find_instances(prob_dir)
        if not instances:
            print(f"WARNING: {prob} has no mzn files, skipping")
            continue

        mzn, data, label = instances[0]
        ptype = detect_problem_type(mzn)
        prob_types[prob] = ptype
        tasks.append((prob, "Sabori", "sabori_csp", mzn, data, ptype))
        tasks.append((prob, "CP-SAT", "cp-sat", mzn, data, ptype))

    print(f"\n{'=' * 80}")
    print(f"MiniZinc Challenge {year}")
    print(f"{'=' * 80}")
    print(f"Running {len(tasks)} tasks with {MAX_WORKERS} workers...")
    print(f"Timeout: {TIMEOUT}s per instance")
    print(f"Problems directory: {probs_dir}")
    print("=" * 80)

    with ProcessPoolExecutor(max_workers=MAX_WORKERS) as executor:
        futures = {executor.submit(run_solver, *t): t for t in tasks}

        for future in as_completed(futures):
            prob, solver, status, elapsed, obj, err, data_label, check = future.result()
            key = (prob, solver)
            results[key] = (status, elapsed, obj, err, check)
            if data_label and prob not in data_labels:
                data_labels[prob] = data_label

            obj_str = f" obj={obj}" if obj is not None else ""
            err_str = f" ({err})" if err else ""
            check_str = f" [{check}]" if check else ""
            print(f"{prob:25s} {solver:8s} -> {status:8s} {elapsed:6.2f}s{obj_str}{err_str}{check_str}")

    # TIMEOUT集計（解なしのみ）
    sabori_timeouts = sum(1 for p in problems
                          if results.get((p, "Sabori"), ("?", 0, None, None, None))[0] == "TIMEOUT"
                          and results.get((p, "Sabori"), ("?", 0, None, None, None))[2] is None)
    cpsat_timeouts = sum(1 for p in problems
                         if results.get((p, "CP-SAT"), ("?", 0, None, None, None))[0] == "TIMEOUT"
                         and results.get((p, "CP-SAT"), ("?", 0, None, None, None))[2] is None)

    # コンソール出力
    print("\n" + "=" * 80)
    print(f"Summary ({year}):")
    print("=" * 80)
    print(f"{'Problem':<30s} {'Sabori':>20s} {'CP-SAT':>20s} {'Check':>8s} {'Winner':>10s}")
    print("-" * 95)

    sabori_wins = 0
    cpsat_wins = 0
    ties = 0
    check_fail_count = 0
    for prob in problems:
        s_res = results.get((prob, "Sabori"), ("?", 0, None, None, None))
        c_res = results.get((prob, "CP-SAT"), ("?", 0, None, None, None))

        ptype = prob_types.get(prob, "?")
        s_obj_str = f" obj={s_res[2]}" if s_res[2] is not None else ""
        c_obj_str = f" obj={c_res[2]}" if c_res[2] is not None else ""
        s_str = f"{s_res[0]} {s_res[1]:.1f}s{s_obj_str}"
        c_str = f"{c_res[0]} {c_res[1]:.1f}s{c_obj_str}"

        s_check = s_res[4] if len(s_res) > 4 else None
        check_str = s_check if s_check else "-"
        if s_check == "CHECK_FAIL":
            check_fail_count += 1

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
        print(f"{prob_label:<30s}{data_str:>15s} {s_str:>20s} {c_str:>20s} {check_str:>8s} {winner:>10s}")

    print("-" * 95)
    print(f"Wins:     Sabori {sabori_wins}  /  CP-SAT {cpsat_wins}  /  Tie {ties}")
    print(f"Timeouts: Sabori {sabori_timeouts}  /  CP-SAT {cpsat_timeouts}")
    if check_fail_count > 0:
        print(f"CHECK FAILS: {check_fail_count}")

    generate_html(results, problems=problems, year=year, prob_types=prob_types, data_labels=data_labels)

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 bench_compare.py <year> [year ...]")
        print("Example: python3 bench_compare.py 2024")
        print("         python3 bench_compare.py 2022 2023 2024 2025")
        sys.exit(1)

    years = sys.argv[1:]
    for y in years:
        if not re.match(r'^\d{4}$', y):
            print(f"ERROR: invalid year '{y}'")
            sys.exit(1)

    cleanup_stale_processes()

    for year in years:
        run_year(year)

if __name__ == "__main__":
    main()
