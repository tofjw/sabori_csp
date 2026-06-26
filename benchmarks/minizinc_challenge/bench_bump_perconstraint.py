#!/usr/bin/env python3
"""制約別 structural bump 寄与の切り分け。

baseline = 全制約 base（SABORI_BUMP_STRUCT_ONLY=__nomatch__）。
各制約 C について「baseline + C だけ structural」(SABORI_BUMP_STRUCT_ONLY=<C>) を、
**C を含む問題でだけ**実行し、baseline と比較する。
（C を含まない問題では C 構成 = baseline で必ず tie になるため走らせない。）

これで「どの制約の構造特化が baseline に対してプラス/マイナスか」を制約単位で集計する。
"""
import subprocess, sys, os, re, time, signal
from pathlib import Path
from datetime import datetime
from concurrent.futures import ProcessPoolExecutor, as_completed

BASE_DIR = Path(__file__).resolve().parent
MINIZINC = "/snap/bin/minizinc"
SABORI_MSC = str(BASE_DIR.parent.parent / "build" / "share" / "minizinc" / "solvers" / "sabori_csp.msc")
BINARY = str(BASE_DIR.parent.parent / "build" / "src" / "fzn" / "fzn_sabori")
TIMEOUT = 30
MAX_WORKERS = 4
YEARS = ["2023", "2024"]
SEEDS = [1, 2, 3, 4, 5]

# (label, toggle substring for name(), fzn grep pattern to detect occurrence)
CONSTRAINTS = [
    ("cumulative",      "fzn_cumulative",        "fzn_cumulative"),
    ("disjunctive",     "fzn_disjunctive",       "fzn_disjunctive"),
    ("circuit",         "circuit",               "circuit"),
    ("regular",         "regular",               "regular"),
    ("arr_var_elem",    "array_var_int_element", "array_var_int_element"),
    ("int_lin_eq",      "int_lin_eq",            "int_lin_eq"),
    ("int_lin_le",      "int_lin_le",            "int_lin_le"),
    ("array_bool_and",  "array_bool_and",        "array_bool_and"),
]
NOMATCH = "__nomatch__zzz"  # baseline: matches no constraint name → 全制約 base

def natural_sort_key(s):
    return [int(t) if t.isdigit() else t.lower() for t in re.split('([0-9]+)', str(s))]

def get_descendant_pids(pid):
    out = []
    try:
        children = subprocess.run(["pgrep", "-P", str(pid)], capture_output=True, text=True).stdout.strip().split('\n')
        for c in children:
            if c:
                cp = int(c)
                out.extend(get_descendant_pids(cp)); out.append(cp)
    except (ValueError, OSError):
        pass
    return out

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
    data_files = sorted(prob_dir.glob("*.dzn"), key=natural_sort_key) + sorted(prob_dir.glob("*.json"), key=natural_sort_key)
    if not mzn_files:
        return []
    if len(mzn_files) > 1 and not data_files:
        mzn = mzn_files[0]; return [(str(mzn), None, mzn.stem)]
    mzn = mzn_files[0]
    if data_files:
        return [(str(mzn), str(data_files[0]), data_files[0].stem)]
    return [(str(mzn), None, mzn.stem)]

def compile_fzn(mzn, data):
    import tempfile
    fzn_fd, fzn_path = tempfile.mkstemp(suffix=".fzn"); os.close(fzn_fd)
    cmd = [MINIZINC, "--compile", "--solver", SABORI_MSC, "-o", fzn_path, mzn]
    if data: cmd.append(data)
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if result.returncode != 0:
            os.unlink(fzn_path); return None
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

def fzn_text(fzn_path):
    try:
        with open(fzn_path) as f: return f.read()
    except OSError:
        return ""

def run_fzn_solver(prob, cfg_name, struct_only, seed, fzn_path):
    cmd = [BINARY, "-a", "-v", "-t", str(TIMEOUT), fzn_path]
    env = dict(os.environ)
    env["SABORI_BUMP_STRUCT_ONLY"] = struct_only
    env["SABORI_SEED"] = str(seed)
    start = time.time(); is_to = False; proc = None
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, start_new_session=True, env=env)
        try:
            stdout, stderr = proc.communicate(timeout=TIMEOUT + 5)
        except subprocess.TimeoutExpired:
            kill_process_tree(proc); stdout, stderr = proc.communicate(); is_to = True
        elapsed = time.time() - start
        output = stdout or ""
        if "==========\n" in output: status = "OPTIMAL"
        elif "----------\n" in output: status = "SOL"
        elif "=====UNSATISFIABLE=====" in output: status = "UNSAT"
        elif is_to or elapsed >= TIMEOUT - 0.5: status = "TIMEOUT"
        else: status = "UNKNOWN"
        obj = None; err = stderr or ""
        for line in reversed(err.split('\n')):
            m = re.search(r'new best objective:\s*(-?\d+)', line)
            if m: obj = int(m.group(1)); break
        if obj is None:
            for line in reversed(err.split('\n')):
                m = re.search(r'\bbest=(-?\d+)', line)
                if m: obj = int(m.group(1)); break
        return (prob, cfg_name, seed, status, max(elapsed, 0.0), obj)
    except Exception:
        return (prob, cfg_name, seed, "ERROR", 0, None)
    finally:
        if proc is not None: kill_process_tree(proc)

def cmp_pair(a, b, ptype):
    """a vs b → 'A'/'B'/'T'. 時間は使わない。最適化=目的値, SAT=証明完了優先。"""
    a_status, _at, a_obj = a; b_status, _bt, b_obj = b
    def obj_better(x, y):
        if ptype == "MIN": return x < y
        if ptype == "MAX": return x > y
        return False
    if ptype in ("MIN", "MAX"):
        a_has, b_has = a_obj is not None, b_obj is not None
        if a_has and b_has:
            if a_obj != b_obj: return "A" if obj_better(a_obj, b_obj) else "B"
            if a_status == "OPTIMAL" and b_status != "OPTIMAL": return "A"
            if b_status == "OPTIMAL" and a_status != "OPTIMAL": return "B"
            return "T"
        if a_has and not b_has: return "A"
        if b_has and not a_has: return "B"
        return "T"
    else:
        rank = {"OPTIMAL": 2, "UNSAT": 2, "SOL": 1}
        ra, rb = rank.get(a_status, 0), rank.get(b_status, 0)
        if ra > rb: return "A"
        if rb > ra: return "B"
        return "T"

def main():
    if not os.path.isfile(BINARY):
        print(f"ERROR: binary not found: {BINARY}"); sys.exit(1)
    print(f"Binary: {BINARY}\nTimeout: {TIMEOUT}s  Workers: {MAX_WORKERS}  Years: {YEARS}")
    subprocess.run(["pkill", "-x", "fzn_sabori"], capture_output=True)
    subprocess.run(["pkill", "-x", "minizinc"], capture_output=True)
    time.sleep(0.5)

    problems, prob_dirs_map = [], {}
    for year in YEARS:
        pdir = BASE_DIR / f"mznc{year}_probs"
        if not pdir.exists(): continue
        for p in sorted(pdir.iterdir(), key=lambda x: natural_sort_key(x.name)):
            if not p.is_dir(): continue
            name = p.name if p.name not in prob_dirs_map else f"{p.name}-{year}"
            problems.append(name); prob_dirs_map[name] = p

    print(f"\nCompiling {len(problems)} problems...")
    fzn_map, prob_types, occ = {}, {}, {c[0]: set() for c in CONSTRAINTS}
    for prob in list(problems):
        inst = find_instances(prob_dirs_map[prob])
        if not inst:
            problems.remove(prob); continue
        mzn, data, _ = inst[0]
        fzn = compile_fzn(mzn, data)
        if fzn is None:
            print(f"  SKIP (compile failed): {prob}"); problems.remove(prob); continue
        fzn_map[prob] = fzn
        prob_types[prob] = parse_fzn_solve_type(fzn)
        txt = fzn_text(fzn)
        for label, _toggle, pat in CONSTRAINTS:
            if pat in txt:
                occ[label].add(prob)
    print(f"Compiled {len(problems)} problems.")
    print("Occurrence (problems containing each constraint):")
    for label, _t, _p in CONSTRAINTS:
        print(f"  {label:16s} {len(occ[label])}")

    # tasks: baseline on all; each C only on occurring problems; 全 seed
    tasks = []
    for seed in SEEDS:
        for prob in problems:
            tasks.append((prob, "base", NOMATCH, seed, fzn_map[prob]))
        for label, toggle, _pat in CONSTRAINTS:
            for prob in problems:
                if prob in occ[label]:
                    tasks.append((prob, label, toggle, seed, fzn_map[prob]))

    print(f"\nRunning {len(tasks)} tasks ({len(SEEDS)} seeds)...\n" + "=" * 90)
    results = {}
    done = 0
    with ProcessPoolExecutor(max_workers=MAX_WORKERS) as ex:
        futures = {ex.submit(run_fzn_solver, *t): t for t in tasks}
        for fut in as_completed(futures):
            prob, cfg, seed, status, elapsed, obj = fut.result()
            results[(prob, cfg, seed)] = (status, elapsed, obj)
            done += 1
            if done % 40 == 0:
                print(f"  ... {done}/{len(tasks)} done")

    # per-constraint tally vs baseline, over (occurring problems x seeds)
    print("\n" + "=" * 90)
    print("PER-CONSTRAINT (structural-for-C vs all-base, over 問題xシード, C を含む問題のみ):")
    print(f"{'constraint':16s} {'#prob':>5s} {'pts':>4s} {'C_win':>6s} {'base_win':>9s} {'tie':>4s} {'net':>5s}")
    for label, _toggle, _pat in CONSTRAINTS:
        w = l = t = 0
        for seed in SEEDS:
            for prob in problems:
                if prob not in occ[label]:
                    continue
                ptype = prob_types.get(prob, "SAT")
                c = results.get((prob, label, seed), ("?", 0, None))
                b = results.get((prob, "base", seed), ("?", 0, None))
                res = cmp_pair(c, b, ptype)
                if res == "A": w += 1
                elif res == "B": l += 1
                else: t += 1
        pts = len(occ[label]) * len(SEEDS)
        print(f"{label:16s} {len(occ[label]):>5d} {pts:>4d} {w:>6d} {l:>9d} {t:>4d} {w-l:>+5d}")

    print(f"\nProblems: {len(problems)}  Seeds: {SEEDS}  Timeout: {TIMEOUT}s  Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    for f in fzn_map.values():
        try: os.unlink(f)
        except OSError: pass

if __name__ == "__main__":
    main()
