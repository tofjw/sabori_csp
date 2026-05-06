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
MINIZINC = "/snap/bin/minizinc"
SABORI_MSC = str(Path(__file__).resolve().parent.parent.parent / "build" / "share" / "minizinc" / "solvers" / "sabori_csp.msc")
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


def _run_checker(cmd, timeout):
    """checker プロセスを実行し、タイムアウト時もプロセスツリーを確実に停止する。

    Returns: (stdout, returncode) or None on timeout/error.
    """
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, start_new_session=True)
    try:
        stdout, _ = proc.communicate(timeout=timeout)
        return stdout, proc.returncode
    except subprocess.TimeoutExpired:
        kill_process_tree(proc)
        proc.wait()
        return None, None
    except Exception:
        kill_process_tree(proc)
        proc.wait()
        return None, None
    finally:
        if proc.poll() is None:
            kill_process_tree(proc)
            proc.wait()


def _verify_mzn(mzn, data, solution, timeout):
    """MZN レベルで `include + constraint var = value` 方式で検証する。

    enum / opt int / set 等の MZN 固有の値もそのまま扱える代わりに、元モデル
    の `is_fixed()` 分岐などで誤検出 (例: connect) が出ることがある。
    """
    mzn_abs = str(Path(mzn).resolve())
    try:
        with tempfile.TemporaryDirectory() as tmpdir:
            checker_path = Path(tmpdir) / "checker.mzn"
            lines = [f'include "{mzn_abs}";']
            for varname, value in solution.items():
                lines.append(f'constraint {varname} = {value};')
            checker_path.write_text('\n'.join(lines) + '\n')

            cmd = [MINIZINC, "--solver", "gecode", str(checker_path)]
            if data:
                cmd.append(str(Path(data).resolve()))
            stdout, rc = _run_checker(cmd, timeout)
            if stdout is None:
                return "CHECK_TIMEOUT"
            if "----------" in stdout:
                return "CHECK_OK"
            if "=====UNSATISFIABLE=====" in stdout:
                return "CHECK_FAIL"
            return "CHECK_SKIP"
    except Exception:
        return "CHECK_SKIP"


def _verify_fzn(mzn, data, solution, timeout):
    """FZN レベルで int_eq / bool_eq 制約として固定して Gecode で検証する。

    手順:
      1. 元の .mzn を gecode 用に flatten して .fzn と .ozn を生成
      2. .ozn から `inFlow = array2d(...,X_INTRODUCED_57_)` 等の MZN→FZN
         エイリアスを抽出
      3. sabori の output_var 値を int_eq / bool_eq 制約として FZN に注入
      4. fzn-gecode で解いて SAT / UNSAT を判定

    MZN レベル方式が `is_fixed()` 分岐などで誤検出する problem の救済用。
    enum / opt int / set 等の値は固定できないので SKIP となる。
    """
    mzn_abs = str(Path(mzn).resolve())
    data_abs = str(Path(data).resolve()) if data else None

    try:
        with tempfile.TemporaryDirectory() as tmpdir:
            base_fzn = Path(tmpdir) / "base.fzn"
            base_ozn = Path(tmpdir) / "base.ozn"

            compile_cmd = [MINIZINC, "--solver", "gecode", "--compile",
                           "-o", str(base_fzn),
                           "--output-ozn-to-file", str(base_ozn),
                           mzn_abs]
            if data_abs:
                compile_cmd.append(data_abs)
            stdout, rc = _run_checker(compile_cmd, timeout)
            if stdout is None:
                return "CHECK_TIMEOUT"
            if rc != 0 or not base_fzn.exists() or base_fzn.stat().st_size == 0:
                return "CHECK_SKIP"

            fzn_text = base_fzn.read_text()
            declared = set()
            for m in re.finditer(r'^\s*var\s+\S[^:]*:\s*(\w+)',
                                 fzn_text, re.MULTILINE):
                declared.add(m.group(1))
            for m in re.finditer(r'^\s*array\s*\[[^\]]*\]\s*of\s+\S[^:]*:\s*(\w+)',
                                 fzn_text, re.MULTILINE):
                declared.add(m.group(1))

            # MZN レベル名 → FZN レベル名 のエイリアス。.ozn には
            #   `array [...] of int: inFlow = array2d(1..7,1..4,X_INTRODUCED_57_);`
            #   `var int: nMdl_11__Z = nMdl_14__Z;`
            # のような宣言がある。複雑な式 (連結など) は最後の引数が単純な
            # 識別子にならないので自動的にスキップされる。
            alias_map = {}
            ozn_text = base_ozn.read_text() if base_ozn.exists() else ''
            ident = re.compile(r'^\w+$')
            for m in re.finditer(
                r'array\s*\[[^\]]+\]\s*of\s+(?:var\s+)?[^:]+:\s*(\w+)\s*=\s*'
                r'array\d+d?\((.*?)\)\s*;', ozn_text, re.DOTALL):
                mzn_name, args = m.group(1), m.group(2)
                last = args.rsplit(',', 1)[-1].strip()
                if ident.match(last) and last in declared:
                    alias_map[mzn_name] = last
            for m in re.finditer(
                r'(?:var\s+)?(?:int|bool)\s*:\s*(\w+)\s*=\s*(\w+)\s*;', ozn_text):
                mzn_name, target = m.group(1), m.group(2)
                if target in declared:
                    alias_map[mzn_name] = target

            int_lit = re.compile(r'^-?\d+$')

            def emit_eq(name, item):
                if item == 'true':
                    return f'constraint bool_eq({name}, true);'
                if item == 'false':
                    return f'constraint bool_eq({name}, false);'
                if int_lit.match(item):
                    return f'constraint int_eq({name}, {item});'
                return None  # unsupported (enum, <>, set, float, etc.)

            extra = []
            n_fixed_vars = 0
            for varname, value in solution.items():
                # FZN に存在しない MZN 名は .ozn 経由で X_INTRODUCED_ などの
                # 別名に飛ばす（`inFlow → X_INTRODUCED_57_` 等）。
                fzn_name = varname if varname in declared else alias_map.get(varname)
                if fzn_name is None:
                    continue
                v = value.strip()
                # MZN は多次元配列を `array2d(idx, idx, [...])` 等で出力する。
                # FZN レベルでは 1D row-major なので末尾の `[...]` を抽出する。
                m = re.match(r'^array\d+d?\(.*?(\[[^\]]*\])\s*\)\s*$', v, re.DOTALL)
                if m:
                    v = m.group(1)
                if v.startswith('['):
                    # 1D `[a,b,c]` も 2D `[| a,b | c,d |]` も `|` を `,` に
                    # 置き換えてから 1D に潰す（FZN は常に 1D row-major）
                    flat = v.replace('|', ',').replace('[', '').replace(']', '')
                    items = [x.strip() for x in flat.split(',') if x.strip()]
                    cs = [emit_eq(f'{fzn_name}[{i}]', item)
                          for i, item in enumerate(items, 1)]
                    # サポートできない値が一つでもあればこの変数全体を諦める
                    if any(c is None for c in cs):
                        continue
                    extra.extend(cs)
                    n_fixed_vars += 1
                else:
                    c = emit_eq(fzn_name, v)
                    if c is None:
                        continue
                    extra.append(c)
                    n_fixed_vars += 1

            # カバレッジが低い (= enum / opt / set 等で半数以上の変数を固定
            # できなかった) と検証が「ほぼ再探索」になり TIMEOUT で潰れるので、
            # その場合は SKIP として早期に返す。
            if n_fixed_vars * 2 < len(solution) or not extra:
                return "CHECK_SKIP"

            patched, n_sub = re.subn(r'^solve\s',
                                     '\n'.join(extra) + '\nsolve ',
                                     fzn_text, count=1, flags=re.MULTILINE)
            if n_sub == 0:
                return "CHECK_SKIP"
            checker_fzn = Path(tmpdir) / "checker.fzn"
            checker_fzn.write_text(patched)

            # CHECK_OK は秒未満、CHECK_FAIL は初期伝播で即検出される。
            # solve に長時間かかる = 元の決定変数が aliasing で固定し切れず
            # 「ほぼ再探索」になっているケース。10s で打ち切って TIMEOUT にする。
            cmd = [MINIZINC, "--solver", "gecode", str(checker_fzn)]
            stdout, rc = _run_checker(cmd, min(timeout, 10))
            if stdout is None:
                return "CHECK_TIMEOUT"
            if "----------" in stdout:
                return "CHECK_OK"
            if "=====UNSATISFIABLE=====" in stdout:
                return "CHECK_FAIL"
            return "CHECK_SKIP"
    except Exception:
        return "CHECK_SKIP"


def verify_solution(mzn, data, solution, timeout=30):
    """sabori の解を Gecode で検証する。MZN レベル → FZN レベルのハイブリッド。

      1. MZN レベル方式 (`include + constraint var = value`) で検証
      2. CHECK_FAIL なら FZN レベル方式 (int_eq 注入) でセカンドオピニオン
         - FZN OK → MZN の検証は誤検出 (例: `is_fixed()` 起因) → CHECK_OK
         - FZN も FAIL → 本物の不整合 → CHECK_FAIL のまま

    enum / opt int / set 等は MZN レベルで処理されるためカバレッジを失わない。
    Returns: "CHECK_OK" / "CHECK_FAIL" / "CHECK_TIMEOUT" / "CHECK_SKIP"
    """
    if not solution:
        return "CHECK_SKIP"
    res = _verify_mzn(mzn, data, solution, timeout)
    if res != "CHECK_FAIL":
        return res
    fzn_res = _verify_fzn(mzn, data, solution, timeout)
    if fzn_res == "CHECK_OK":
        return "CHECK_OK"
    return "CHECK_FAIL"


def run_solver(problem, solver_name, solver_id, mzn, data, prob_type="SAT"):
    """ソルバーを実行して結果を返す"""
    data_label = Path(data).stem if data else None
    # cwd を challenge ルート（BASE_DIR）に固定して、引数は BASE_DIR からの
    # 相対パスで渡す。理由は 2 つ:
    # (1) MiniZinc 2.9.5 は特定の (mznlib, mzn, dzn) 組み合わせで .dzn を絶対
    #     パスで渡すと flatten 中に SIGABRT することがある（例: mznc2016 の
    #     gfd-schedule n25f5d20m10k3.dzn + sabori_csp.msc）。相対パスにすると
    #     回避できる（詳細: repro_gfd_segv/README.md）。
    # (2) MiniZinc は cwd / 引数の文字列によって変数導入順序が変わり、生成
    #     される FZN の X_INTRODUCED_… 番号にずれが出る。これが探索軌跡を
    #     左右する（mznc2019 median-string で obj=122 vs 210 と顕著）。
    #     全問題で cwd を一定（BASE_DIR）にすれば結果が決定的かつ手動再現と
    #     一致する。
    mzn_arg = str(Path(mzn).resolve().relative_to(BASE_DIR))
    data_arg = str(Path(data).resolve().relative_to(BASE_DIR)) if data else None
    cwd = str(BASE_DIR)

    # minizinc 自身の `-t` を使ってタイムアウトを処理させる。Python 側の
    # communicate(timeout=…) を SIGKILL の起点にしてしまうと、fzn_sabori →
    # solns2out → minizinc → Python のパイプライン上に未フラッシュのデータが
    # 残ったまま落ち、最後に出力された obj/解が捕捉できない（バッファ消失）。
    # minizinc 自前のタイムアウトは graceful 終了でバッファを保全する。
    cmd = [MINIZINC, "--solver", solver_id, "-t", str(TIMEOUT * 1000)]
    if prob_type != "SAT":
        cmd.append("-a")
    cmd.extend(["--output-objective", "--output-mode", "dzn", mzn_arg])
    if data_arg:
        cmd.append(data_arg)

    start = time.monotonic()
    proc = None
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, start_new_session=True, cwd=cwd)
        try:
            # Popen 側の communicate timeout は安全網。minizinc が graceful
            # に終わらない（パイプ詰まり等）ケースでのみ発火させたいので
            # TIMEOUT より十分長く取る。
            stdout, stderr = proc.communicate(timeout=TIMEOUT + 10)
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
        elif elapsed >= TIMEOUT * 0.9:
            # minizinc 自身の -t で時間切れになり、何も解を出さずに終了した
            # ケース（UNKNOWN マーカーすら出さないこともある）は TIMEOUT 扱い。
            status = "TIMEOUT"
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

def judge_winner(s_status, s_time, s_obj, c_status, c_time, c_obj, prob_type="SAT", s_check=None):
    """勝者判定（MIN/MAX/SAT対応）"""
    # CHECK_FAIL は sabori の解が gecode で再検証して FZN 制約を満たさなかった
    # ことを意味する（verify_solution は FZN レベルで int_eq を積む方式）。
    # 真のバグなので解を無効化する。
    if s_check == "CHECK_FAIL":
        s_status = "UNKNOWN"
        s_obj = None
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
        winner, winner_class = judge_winner(s_status, s_time, s_obj, c_status, c_time, c_obj, ptype, s_check=s_check)
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
        tasks.append((prob, "Sabori", SABORI_MSC, mzn, data, ptype))
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

        winner, _ = judge_winner(s_res[0], s_res[1], s_res[2], c_res[0], c_res[1], c_res[2], ptype, s_check=s_check)
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
