#!/usr/bin/env python3
"""activity bump 源の「相互補償・要・全部」を確認する ablation。

Same binary, env で切替（既定=全部 on）。全構成を同一 run に並べ、単体 off と全部 off を直接比較する:
  - full       : 既定（learn/prop/decvar すべて on）
  - learn_off  : SABORI_NG_LEARN_BUMP=0           （学習時 bump 0.01 のみ off）
  - prop_off   : SABORI_NG_PROP_BUMP=0            （伝播時 bump フルスケール/n のみ off）
  - decvar_off : SABORI_DECVAR_BUMP=0             （決定変数 bump フルスケール のみ off）
  - all3_off   : 上記3つ同時 off                  （activity bump 源をまとめて停止）

仮説（相互補償説）: 単体 off はどれも ≈0 だが all3_off だけ大きく崩れる
  → 「どの源でも1つ残っていれば足り、全部抜くとダメ」= activity 源は冗長・代替可能。
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
CONFIGS = [
    ("full", {}),
    ("learn_off", {"SABORI_NG_LEARN_BUMP": "0"}),
    ("prop_off", {"SABORI_NG_PROP_BUMP": "0"}),
    ("decvar_off", {"SABORI_DECVAR_BUMP": "0"}),
    ("all3_off", {"SABORI_NG_LEARN_BUMP": "0", "SABORI_NG_PROP_BUMP": "0", "SABORI_DECVAR_BUMP": "0"}),
    ("everything_off", {"SABORI_NG_LEARN_BUMP": "0", "SABORI_NG_PROP_BUMP": "0", "SABORI_DECVAR_BUMP": "0", "SABORI_BUMP_MODE": "0"}),
]
# pairwise: full vs each（単体3つ + 全部同時 + 制約bumpも含め全停止）
# 相互補償なら単体≈0・all3_off だけ大。all3_off も小さければ制約bumpが補償 → everything_off で確認。
PAIRS = [
    ("full", "learn_off"),
    ("full", "prop_off"),
    ("full", "decvar_off"),
    ("full", "all3_off"),
    ("full", "everything_off"),
]

def natural_sort_key(s):
    return [int(t) if t.isdigit() else t.lower() for t in re.split('([0-9]+)', str(s))]

def get_descendant_pids(pid):
    out = []
    try:
        children = subprocess.run(["pgrep", "-P", str(pid)], capture_output=True, text=True).stdout.strip().split('\n')
        for c in children:
            if c:
                cp = int(c)
                out.extend(get_descendant_pids(cp))
                out.append(cp)
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
        mzn = mzn_files[0]
        return [(str(mzn), None, mzn.stem)]
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
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
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

def run_fzn_solver(prob, cfg_name, env_extra, seed, fzn_path):
    # -v: stderr に "new best objective: N" / restart 行の "best=N"（暫定目的値）が出る。
    # これはモデル出力に依存せず全最適化問題で目的値を取得できる唯一の経路。
    cmd = [BINARY, "-a", "-v", "-t", str(TIMEOUT), fzn_path]
    env = dict(os.environ)
    env.update(env_extra)
    env["SABORI_SEED"] = str(seed)
    start = time.time()
    is_to = False
    proc = None
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, start_new_session=True, env=env)
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
        # 目的値: stderr の最後の "new best objective: N" を最優先、無ければ最後の "best=N"
        obj = None
        err = stderr or ""
        for line in reversed(err.split('\n')):
            m = re.search(r'new best objective:\s*(-?\d+)', line)
            if m:
                obj = int(m.group(1)); break
        if obj is None:
            for line in reversed(err.split('\n')):
                m = re.search(r'\bbest=(-?\d+)', line)
                if m:
                    obj = int(m.group(1)); break
        return (prob, cfg_name, seed, status, max(elapsed, 0.0), obj)
    except Exception:
        return (prob, cfg_name, seed, "ERROR", 0, None)
    finally:
        if proc is not None:
            kill_process_tree(proc)

def cmp_pair(a, b, ptype):
    """a vs b. returns 'A' / 'B' / 'T'.

    時間は使わない（クロックスキュー/ジッタで反転するため）。
    最適化: 目的値の良し悪し → 同値なら最適性証明の有無で判定。
    SAT: 解/UNSAT を出せたか（証明完了 OPTIMAL/UNSAT > 解のみ）。
    """
    a_status, _a_time, a_obj = a
    b_status, _b_time, b_obj = b

    def obj_better(x, y):
        if ptype == "MIN": return x < y
        if ptype == "MAX": return x > y
        return False

    if ptype in ("MIN", "MAX"):
        a_has, b_has = a_obj is not None, b_obj is not None
        if a_has and b_has:
            if a_obj != b_obj:
                return "A" if obj_better(a_obj, b_obj) else "B"
            # 同じ目的値: 最適性を証明できた方を上に
            if a_status == "OPTIMAL" and b_status != "OPTIMAL": return "A"
            if b_status == "OPTIMAL" and a_status != "OPTIMAL": return "B"
            return "T"
        if a_has and not b_has: return "A"
        if b_has and not a_has: return "B"
        return "T"
    else:  # SAT
        rank = {"OPTIMAL": 2, "UNSAT": 2, "SOL": 1}
        ra, rb = rank.get(a_status, 0), rank.get(b_status, 0)
        if ra > rb: return "A"
        if rb > ra: return "B"
        return "T"

def main():
    if not os.path.isfile(BINARY):
        print(f"ERROR: binary not found: {BINARY}"); sys.exit(1)
    print(f"Binary: {BINARY}")
    print(f"Configs: {[c[0] for c in CONFIGS]}")
    print(f"Timeout: {TIMEOUT}s  Workers: {MAX_WORKERS}  Years: {YEARS}")

    subprocess.run(["pkill", "-x", "fzn_sabori"], capture_output=True)
    subprocess.run(["pkill", "-x", "minizinc"], capture_output=True)
    time.sleep(0.5)

    problems, prob_dirs_map, prob_years = [], {}, {}
    for year in YEARS:
        pdir = BASE_DIR / f"mznc{year}_probs"
        if not pdir.exists(): continue
        for p in sorted(pdir.iterdir(), key=lambda x: natural_sort_key(x.name)):
            if not p.is_dir(): continue
            name = p.name if p.name not in prob_dirs_map else f"{p.name}-{year}"
            problems.append(name)
            prob_dirs_map[name] = p
            prob_years[name] = year

    print(f"\nCompiling {len(problems)} problems...")
    fzn_map, prob_types = {}, {}
    for prob in list(problems):
        inst = find_instances(prob_dirs_map[prob])
        if not inst:
            problems.remove(prob); continue
        mzn, data, _ = inst[0]
        fzn = compile_fzn(mzn, data)
        if fzn is None:
            print(f"  SKIP (compile failed): {prob}")
            problems.remove(prob); continue
        fzn_map[prob] = fzn
        prob_types[prob] = parse_fzn_solve_type(fzn)
    print(f"Compiled {len(problems)} problems.")

    tasks = []
    for prob in problems:
        for cfg_name, env_extra in CONFIGS:
            for seed in SEEDS:
                tasks.append((prob, cfg_name, env_extra, seed, fzn_map[prob]))

    print(f"Running {len(tasks)} tasks ({len(problems)} probs x {len(CONFIGS)} cfgs x {len(SEEDS)} seeds)...\n" + "=" * 90)
    results = {}
    done = 0
    with ProcessPoolExecutor(max_workers=MAX_WORKERS) as ex:
        futures = {ex.submit(run_fzn_solver, *t): t for t in tasks}
        for fut in as_completed(futures):
            prob, cfg, seed, status, elapsed, obj = fut.result()
            results[(prob, cfg, seed)] = (status, elapsed, obj)
            done += 1
            if done % 20 == 0:
                print(f"  ... {done}/{len(tasks)} done")

    # ---- pairwise tally over (problem x seed) ----
    # 各 (問題, シード) を独立データ点として勝敗集計。シードごとの内訳も出す。
    print("\n" + "=" * 90)
    print("PAIRWISE (集計: 問題 x シード を独立データ点):")
    for (ca, cb) in PAIRS:
        total = [0, 0, 0]
        per_seed = []
        for seed in SEEDS:
            s = [0, 0, 0]
            for prob in problems:
                ptype = prob_types.get(prob, "SAT")
                ra = results.get((prob, ca, seed), ("?", 0, None))
                rb = results.get((prob, cb, seed), ("?", 0, None))
                res = cmp_pair(ra, rb, ptype)
                idx = 0 if res == "A" else (1 if res == "B" else 2)
                s[idx] += 1; total[idx] += 1
            per_seed.append(s)
        w, l, t = total
        print(f"  {ca:11s} vs {cb:11s}:  {ca}_win={w}  {cb}_win={l}  tie={t}   net={w-l:+d}")
        for seed, s in zip(SEEDS, per_seed):
            print(f"      seed={seed}: {ca}={s[0]} {cb}={s[1]} tie={s[2]}")
    print(f"\nProblems: {len(problems)}  Seeds: {SEEDS}  Timeout: {TIMEOUT}s  Generated: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")

    # cleanup temp fzn
    for f in fzn_map.values():
        try: os.unlink(f)
        except OSError: pass

if __name__ == "__main__":
    main()
