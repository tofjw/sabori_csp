#!/usr/bin/env python3
"""Real-parallel A/B: new optimize ladder (promote@worker1) vs old ladder.

For each optimize problem, run the actual ParallelSolver at -jN with the NEW ladder
and with SABORI_LADDER_OLD=1 (OLD ladder). Compare proved-optimal + objective
(direction-aware). Highlights the robust wins (proved-opt flips, feasible flips,
large objective gaps) that survive parallel timing noise.
"""
import os, re, subprocess, time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BIN = ROOT.parent.parent / "build/src/fzn/fzn_sabori"
TIMEOUT = int(os.environ.get("LP_TIMEOUT", "15"))
THREADS = int(os.environ.get("LP_THREADS", "4"))
OUTER = int(os.environ.get("LP_OUTER", "4"))   # 並列 bench プロセス数
REPS = int(os.environ.get("LP_REPS", "3"))     # 反復回数（並列は非決定的なので best-of で比較）
YEARS = ["mznc2022_probs", "mznc2023_probs", "mznc2024_probs", "mznc2025_probs"]


def cleanup():
    for p in ("fzn_sabori", "minizinc"):
        subprocess.run(["pkill", "-x", p], capture_output=True)


def pick():
    out = []
    for y in YEARS:
        yd = ROOT / y
        if not yd.is_dir():
            continue
        for d in sorted(p for p in yd.iterdir() if p.is_dir()):
            fz = sorted(d.glob("*.fzn"), key=lambda p: p.stat().st_size)
            if not fz:
                continue
            txt = fz[0].read_text(errors="ignore")
            m = re.search(r"solve.*?(minimize|maximize)", txt, re.S)
            if not m:
                continue
            out.append((f"{y[4:8]}:{d.name}", fz[0], "min" if m.group(1) == "minimize" else "max"))
    return out


def run1(fzn, old, seed):
    env = dict(os.environ, SABORI_THREADS=str(THREADS), SABORI_PRINT_OBJ="1", SABORI_SEED=seed)
    if old:
        env["SABORI_LADDER_OLD"] = "1"
    try:
        p = subprocess.run([str(BIN), "-t", str(TIMEOUT), str(fzn)],
                           capture_output=True, text=True, env=env, timeout=TIMEOUT + 20)
    except subprocess.TimeoutExpired:
        return {"obj": None, "opt": False}
    objs = re.findall(r"% objective = (-?\d+)", p.stderr)
    return {"obj": int(objs[-1]) if objs else None, "opt": "==========" in p.stdout}


REP_SEEDS = ["12345678", "777", "42", "2024", "99"][:REPS]


def run(fzn, old, d):
    """REPS 反復し direction-aware best(capability)を返す。"""
    runs = [run1(fzn, old, s) for s in REP_SEEDS]
    opt = any(r["opt"] for r in runs)
    vals = [r["obj"] for r in runs if r["obj"] is not None]
    if not vals:
        return {"obj": None, "opt": False}
    best = min(vals) if d == "min" else max(vals)
    return {"obj": best, "opt": opt}


def better(x, y, d):
    """is x strictly better than y? (direction-aware; opt beats non-opt at same obj)."""
    if x["opt"] and y["opt"]:
        return False  # both proved -> same optimum, tie
    if x["obj"] is None and y["obj"] is None:
        return False
    if x["obj"] is None:
        return False
    if y["obj"] is None:
        return True
    if x["obj"] == y["obj"]:
        return x["opt"] and not y["opt"]
    return (x["obj"] < y["obj"]) if d == "min" else (x["obj"] > y["obj"])


def main():
    cleanup()
    probs = pick()
    print(f"# {len(probs)} opt problems, -j{THREADS}, timeout={TIMEOUT}s, outer={OUTER}, "
          f"reps={REPS}(best-of)", flush=True)
    res = {}
    with ThreadPoolExecutor(max_workers=OUTER) as ex:
        futs = {}
        for name, fzn, d in probs:
            futs[ex.submit(run, fzn, False, d)] = (name, "new")
            futs[ex.submit(run, fzn, True, d)] = (name, "old")
        for f in as_completed(futs):
            name, k = futs[f]
            res.setdefault(name, {})[k] = f.result()

    new_win, old_win = [], []
    for name, fzn, d in probs:
        r = res.get(name, {})
        n, o = r.get("new"), r.get("old")
        if not n or not o:
            continue
        if better(n, o, d):
            new_win.append((name, d, o, n))
        elif better(o, n, d):
            old_win.append((name, d, o, n))

    def fmt(x):
        return f"{x['obj']}{'*' if x['opt'] else ''}"
    print("\n===== NEW ladder wins (promote@worker1) =====")
    for name, d, o, n in new_win:
        print(f"  {name} ({d}): old={fmt(o)} -> new={fmt(n)}")
    print("===== OLD ladder wins (regressions) =====")
    for name, d, o, n in old_win:
        print(f"  {name} ({d}): new={fmt(n)} -> old={fmt(o)}")
    print(f"\nSUMMARY (-j{THREADS}): new-wins={len(new_win)}  old-wins={len(old_win)}  "
          f"ties={len(probs)-len(new_win)-len(old_win)}")


if __name__ == "__main__":
    main()
