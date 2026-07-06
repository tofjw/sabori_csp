#!/usr/bin/env python3
"""SAT (satisfaction) A/B for SABORI_PROMOTE_DEF_BOOL.

Reuses bench_restart_sat.py's INSTANCES + flatten/run. SAT metric: solved(=----------)
within timeout, and time. Portfolio value: instances promote solves that base doesn't
(over seeds), or where promote's best time beats base's best time.
"""
import os, subprocess
from concurrent.futures import ThreadPoolExecutor, as_completed
import bench_restart_sat as sat

SEEDS = os.environ.get("PS_SEEDS", "12345678,777,42,2024,99").split(",")
TIMEOUT = int(os.environ.get("PS_TIMEOUT", "20"))
sat.TIME_LIMIT = TIMEOUT  # run_one が参照する上限を上書き
MAXPROC = 4


def run(fzn, prom, seed):
    cenv = {"SABORI_THREADS": "1", "SABORI_SEED": seed}
    if prom:
        cenv["SABORI_PROMOTE_DEF_BOOL"] = "1"
    return sat.run_one(fzn, cenv)


def best_time(runs):
    """min solve time among solved runs; None if never solved."""
    ts = [r["time"] for r in runs if r and r["solved"]]
    return min(ts) if ts else None


def main():
    sat.cleanup()
    print(f"# SAT A/B, seeds={SEEDS}, timeout={TIMEOUT}s", flush=True)
    fzns = {}
    for name, mzn, dzn in sat.INSTANCES:
        f = sat.flatten(name, mzn, dzn)
        if f:
            fzns[name] = f
        else:
            print(f"  (flatten failed: {name})")
    print(f"# {len(fzns)} instances flattened", flush=True)

    res = {}
    with ThreadPoolExecutor(max_workers=MAXPROC) as ex:
        futs = {}
        for name, f in fzns.items():
            for s in SEEDS:
                futs[ex.submit(run, f, False, s)] = (name, s, "base")
                futs[ex.submit(run, f, True, s)] = (name, s, "prom")
        for fut in as_completed(futs):
            name, s, k = futs[fut]
            res.setdefault(name, {}).setdefault(k, []).append(fut.result())

    print(f"\n{'instance':<14} {'base solved/N  best':<24} {'prom solved/N  best':<24} verdict")
    print("-" * 78)
    prom_add, base_only = [], []
    for name in fzns:
        b = res[name]["base"]
        p = res[name]["prom"]
        bn = sum(1 for r in b if r["solved"])
        pn = sum(1 for r in p if r["solved"])
        bt, pt = best_time(b), best_time(p)
        v = ""
        # ポートフォリオ VBS: promote が解ける instance を base が全 seed で解けない
        if pt is not None and bt is None:
            v = "  <<PROM adds (base never solves)"; prom_add.append(name)
        elif bt is not None and pt is None:
            v = "  base only (prom never solves)"; base_only.append(name)
        elif bt is not None and pt is not None:
            if pt < bt * 0.8:
                v = f"  prom faster ({pt}<{bt})"; prom_add.append(name)
            elif bt < pt * 0.8:
                v = f"  base faster ({bt}<{pt})"; base_only.append(name)
        bs = f"{bn}/{len(SEEDS)}  {bt if bt is not None else '-'}"
        ps = f"{pn}/{len(SEEDS)}  {pt if pt is not None else '-'}"
        print(f"{name:<14} {bs:<24} {ps:<24}{v}")
    print("-" * 78)
    print(f"portfolio: promote が足す(解ける/速い)={len(prom_add)} {prom_add}")
    print(f"           base のみ(promote 及ばず)={len(base_only)} {base_only}")


if __name__ == "__main__":
    main()
