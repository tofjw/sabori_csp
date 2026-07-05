#!/usr/bin/env python3
"""A/B: single-thread build_order pre-presolve (baseline) vs post-presolve (variant).

variant = SABORI_BUILDORDER_POSTPRESOLVE=1, which makes single-thread build the
variable order on the presolved model (matching parallel worker0).

Metric per problem (single-thread, fixed timeout):
  - proved optimal?  (stdout "==========")
  - best objective   (stderr "% objective = N"), direction-aware
  - wall time to finish (only meaningful when proved optimal)

Winner rules (direction-aware, min: lower better / max: higher better):
  - both optimal:    faster wall time wins (else tie)
  - one optimal only: that one wins
  - neither optimal: strictly better objective wins (else tie/both-none)
"""
import os, re, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BIN = ROOT.parent.parent / "build/src/fzn/fzn_sabori"
TIMEOUT = int(os.environ.get("AB_TIMEOUT", "12"))
YEARS = ["mznc2022_probs", "mznc2023_probs", "mznc2024_probs", "mznc2025_probs"]
MAXPROC = 4


def cleanup():
    for pat in ("fzn_sabori", "minizinc"):
        subprocess.run(["pkill", "-x", pat], capture_output=True)


def pick_problems():
    probs = []
    for y in YEARS:
        ydir = ROOT / y
        if not ydir.is_dir():
            continue
        for d in sorted(p for p in ydir.iterdir() if p.is_dir()):
            fzns = sorted(d.glob("*.fzn"), key=lambda p: p.stat().st_size)
            if not fzns:
                continue
            f = fzns[0]  # smallest instance in the dir
            txt = f.read_text(errors="ignore")
            m = re.search(r"solve.*?(minimize|maximize)", txt, re.S)
            if not m:
                continue
            direction = "min" if m.group(1) == "minimize" else "max"
            probs.append((f"{y.split('_')[0][4:]}:{d.name}", f, direction))
    return probs


def run_one(fzn, variant):
    env = dict(os.environ, SABORI_THREADS="1", SABORI_PRINT_OBJ="1")
    if variant:
        env["SABORI_BUILDORDER_POSTPRESOLVE"] = "1"
    t0 = time.time()
    try:
        p = subprocess.run(
            [str(BIN), "-t", str(TIMEOUT), str(fzn)],
            capture_output=True, text=True, env=env, timeout=TIMEOUT + 15,
        )
    except subprocess.TimeoutExpired:
        return {"obj": None, "optimal": False, "err": True, "t": time.time() - t0}
    dt = time.time() - t0
    out, err = p.stdout, p.stderr
    if "=====UNSATISFIABLE=====" in out or "Error" in err[:2000] and "objective" not in err:
        # unsupported constraint or unsat: mark err unless it produced an objective
        pass
    objs = re.findall(r"% objective = (-?\d+)", err)
    obj = int(objs[-1]) if objs else None
    optimal = "==========" in out
    err_flag = obj is None and not optimal and ("Error" in err[:3000])
    return {"obj": obj, "optimal": optimal, "err": err_flag, "t": dt}


def better(a, b, direction):
    """return 'A','B','tie' for objective a vs b."""
    if a is None and b is None:
        return "tie"
    if a is None:
        return "B"
    if b is None:
        return "A"
    if a == b:
        return "tie"
    if direction == "min":
        return "A" if a < b else "B"
    return "A" if a > b else "B"


def decide(base, var, direction):
    if base["err"] and var["err"]:
        return "skip"
    if base["optimal"] and var["optimal"]:
        # both proved optimal; faster wins (10% margin)
        if base["t"] < var["t"] * 0.9:
            return "base"
        if var["t"] < base["t"] * 0.9:
            return "var"
        return "tie"
    if base["optimal"] != var["optimal"]:
        return "base" if base["optimal"] else "var"
    r = better(base["obj"], var["obj"], direction)
    return {"A": "base", "B": "var", "tie": "tie"}[r]


def main():
    cleanup()
    probs = pick_problems()
    print(f"# {len(probs)} problems, timeout={TIMEOUT}s, single-thread A/B", flush=True)
    results = {}

    def task(name, fzn, direction, variant):
        return (name, direction, variant, run_one(fzn, variant))

    jobs = []
    with ThreadPoolExecutor(max_workers=MAXPROC) as ex:
        for name, fzn, direction in probs:
            for variant in (False, True):
                jobs.append(ex.submit(task, name, fzn, direction, variant))
        for fut in as_completed(jobs):
            name, direction, variant, res = fut.result()
            results.setdefault(name, {"dir": direction})["var" if variant else "base"] = res

    tally = {"base": 0, "var": 0, "tie": 0, "skip": 0}
    rows = []
    for name, fzn, direction in probs:
        r = results.get(name, {})
        if "base" not in r or "var" not in r:
            continue
        w = decide(r["base"], r["var"], direction)
        tally[w] += 1
        rows.append((name, direction, r["base"], r["var"], w))

    def fmt(d):
        o = d["obj"]
        tag = "OPT" if d["optimal"] else ("ERR" if d["err"] else "   ")
        return f"{str(o):>10} {tag} {d['t']:5.1f}s"

    print(f"\n{'problem':<32} {'dir':<4} {'baseline':>20} {'variant':>20}  winner")
    print("-" * 105)
    for name, direction, b, v, w in rows:
        star = "" if w in ("tie", "skip") else "  <<<"
        print(f"{name:<32} {direction:<4} {fmt(b):>20} {fmt(v):>20}  {w}{star}")

    print("-" * 105)
    print(f"WINS  baseline(pre-presolve)={tally['base']}  "
          f"variant(post-presolve)={tally['var']}  tie={tally['tie']}  skip={tally['skip']}")


if __name__ == "__main__":
    main()
