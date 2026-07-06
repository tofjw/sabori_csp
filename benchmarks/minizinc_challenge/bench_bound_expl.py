#!/usr/bin/env python3
"""Verify + A/B for SABORI_BOUND_EXPL (complete-box bound-literal conflict learning).

Two things:
  1. SOUNDNESS: on instances where BOTH the trusted ground-only baseline and the
     bound_expl config prove optimal, their optima must match exactly. Any mismatch
     is a soundness violation. (ground Eq-nogood vs bound-box-nogood are independent
     explanation code paths over the same .fzn, so agreement is meaningful.)
  2. PERFORMANCE: proved-optimal count and time; incumbent objective at timeout.

Configs (all single-thread, -C conflict learning ON):
  base : ground-only (SABORI_BOUND_EXPL unset)  -> trusted sound reference
  box  : SABORI_BOUND_EXPL=1 (complete box)      -> candidate
"""
import os, re, subprocess, time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BIN = ROOT.parent.parent / "build/src/fzn/fzn_sabori"
TIMEOUT = int(os.environ.get("BE_TIMEOUT", "15"))
THREADS = int(os.environ.get("BE_THREADS", "1"))  # SABORI_THREADS per solver run
YEARS = ["mznc2022_probs", "mznc2023_probs", "mznc2024_probs", "mznc2025_probs"]
# keep total worker threads bounded (<=4): fewer concurrent instances when each is parallel
MAXPROC = max(1, 4 // THREADS)


def cleanup():
    for pat in ("fzn_sabori", "minizinc", "fzn-chuffed"):
        subprocess.run(["pkill", "-x", pat], capture_output=True)


def pick():
    probs = []
    for y in YEARS:
        yd = ROOT / y
        if not yd.is_dir():
            continue
        for d in sorted(p for p in yd.iterdir() if p.is_dir()):
            fz = sorted(d.glob("*.fzn"), key=lambda p: p.stat().st_size)
            if not fz:
                continue
            f = fz[0]
            txt = f.read_text(errors="ignore")
            if not re.search(r"solve.*?(minimize|maximize)", txt, re.S):
                continue
            probs.append((f"{y[4:8]}:{d.name}", f))
    return probs


BOX_MODE = os.environ.get("BE_MODE", "1")  # 1=complete box, 2=Geq only, 3=complete+neq-guard


def run(fzn, box):
    env = dict(os.environ, SABORI_THREADS=str(THREADS), SABORI_PRINT_OBJ="1")
    if box:
        env["SABORI_BOUND_EXPL"] = BOX_MODE
    t0 = time.time()
    try:
        p = subprocess.run([str(BIN), "-t", str(TIMEOUT), "-C", str(fzn)],
                           capture_output=True, text=True, env=env, timeout=TIMEOUT + 15)
    except subprocess.TimeoutExpired:
        return {"obj": None, "opt": False, "t": time.time() - t0, "err": True}
    dt = time.time() - t0
    objs = re.findall(r"% objective = (-?\d+)", p.stderr)
    obj = int(objs[-1]) if objs else None
    opt = "==========" in p.stdout
    err = obj is None and not opt and "Error" in p.stderr[:3000]
    return {"obj": obj, "opt": opt, "t": dt, "err": err}


def main():
    cleanup()
    probs = pick()
    print(f"# {len(probs)} instances, timeout={TIMEOUT}s, SABORI_THREADS={THREADS}, "
          f"concurrency={MAXPROC}, -C", flush=True)
    res = {}
    with ThreadPoolExecutor(max_workers=MAXPROC) as ex:
        futs = {}
        for name, fzn in probs:
            futs[ex.submit(run, fzn, False)] = (name, "base")
            futs[ex.submit(run, fzn, True)] = (name, "box")
        for fut in as_completed(futs):
            name, k = futs[fut]
            res.setdefault(name, {})[k] = fut.result()

    viol = []
    both_opt = 0
    base_faster = box_faster = tie = 0
    rows = []
    for name, fzn in probs:
        r = res.get(name, {})
        b, x = r.get("base"), r.get("box")
        if not b or not x:
            continue
        # soundness: both proved optimal -> objectives must match
        sv = ""
        if b["opt"] and x["opt"]:
            both_opt += 1
            if b["obj"] != x["obj"]:
                sv = "  !!!SOUNDNESS VIOLATION!!!"
                viol.append((name, b["obj"], x["obj"]))
            # perf among both-optimal: faster wins (10% margin)
            if b["t"] < x["t"] * 0.9:
                base_faster += 1
            elif x["t"] < b["t"] * 0.9:
                box_faster += 1
            else:
                tie += 1
        rows.append((name, b, x, sv))

    def f(d):
        tag = "OPT" if d["opt"] else ("ERR" if d.get("err") else "   ")
        return f"{str(d['obj']):>9} {tag} {d['t']:5.1f}s"

    print(f"\n{'instance':<34} {'base(ground)':>18} {'box(bound_expl)':>18}")
    print("-" * 78)
    for name, b, x, sv in rows:
        print(f"{name:<34} {f(b):>18} {f(x):>18}{sv}")
    print("-" * 78)
    print(f"SOUNDNESS: both-optimal={both_opt}  violations={len(viol)}")
    if viol:
        for n, bo, xo in viol:
            print(f"   VIOLATION {n}: base={bo} box={xo}")
    print(f"PERF(among both-optimal): base_faster={base_faster} box_faster={box_faster} tie={tie}")


if __name__ == "__main__":
    main()
