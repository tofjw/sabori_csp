#!/usr/bin/env python3
"""A/B for SABORI_PROMOTE_DEF_BOOL: promote defined bool vars to the decision tier.

Portfolio framing: we care about BIG WINS (a config solving/optimizing much better
than the other on some problem), not average. A double-edged arm with big wins is
valuable in a portfolio (VBS takes the best).

Per (problem, seed), single-thread, fixed timeout:
  base = normal, prom = SABORI_PROMOTE_DEF_BOOL=1
Big win for X over Y (direction-aware, min: lower better):
  - X proves optimal, Y does not; OR
  - X finds a solution, Y finds none; OR
  - both feasible, X objective better than Y by > BIGWIN_FRAC (default 15%).
"""
import os, re, subprocess, time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

ROOT = Path(__file__).resolve().parent
BIN = ROOT.parent.parent / "build/src/fzn/fzn_sabori"
TIMEOUT = int(os.environ.get("PB_TIMEOUT", "12"))
SEEDS = os.environ.get("PB_SEEDS", "12345678,777").split(",")
BIGWIN = float(os.environ.get("PB_BIGWIN", "0.15"))
YEARS = ["mznc2022_probs", "mznc2023_probs", "mznc2024_probs", "mznc2025_probs"]
MAXPROC = 4


def cleanup():
    for p in ("fzn_sabori", "minizinc"):
        subprocess.run(["pkill", "-x", p], capture_output=True)


FOCUS = [s for s in os.environ.get("PB_FOCUS", "").split(",") if s]


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
            name = f"{y[4:8]}:{d.name}"
            if FOCUS and not any(s in name for s in FOCUS):
                continue
            out.append((name, fz[0], "min" if m.group(1) == "minimize" else "max"))
    return out


def vbs(objs, direction):
    """best objective over a list of run results (None-tolerant)."""
    vals = [o["obj"] for o in objs if o and o["obj"] is not None]
    if not vals:
        return None
    return min(vals) if direction == "min" else max(vals)


def cmp_better(a, b, direction):
    """is a strictly better than b (direction-aware)? None = worst."""
    if a is None:
        return False
    if b is None:
        return True
    return (a < b) if direction == "min" else (a > b)


def run(fzn, prom, seed):
    env = dict(os.environ, SABORI_THREADS="1", SABORI_PRINT_OBJ="1", SABORI_SEED=seed)
    if prom:
        env["SABORI_PROMOTE_DEF_BOOL"] = "1"
    try:
        p = subprocess.run([str(BIN), "-t", str(TIMEOUT), str(fzn)],
                           capture_output=True, text=True, env=env, timeout=TIMEOUT + 15)
    except subprocess.TimeoutExpired:
        return {"obj": None, "opt": False}
    objs = re.findall(r"% objective = (-?\d+)", p.stderr)
    return {"obj": int(objs[-1]) if objs else None, "opt": "==========" in p.stdout}


def bigwin(x, y, direction):
    """Does X have a big win over Y?"""
    if x["opt"] and not y["opt"]:
        return True
    if x["obj"] is not None and y["obj"] is None:
        return True
    if x["obj"] is None or y["obj"] is None:
        return False
    if x["obj"] == y["obj"]:
        return False
    better = (x["obj"] < y["obj"]) if direction == "min" else (x["obj"] > y["obj"])
    if not better:
        return False
    denom = max(1, abs(y["obj"]))
    return abs(x["obj"] - y["obj"]) / denom > BIGWIN


def main():
    cleanup()
    probs = pick()
    print(f"# {len(probs)} problems, seeds={SEEDS}, timeout={TIMEOUT}s, bigwin>{BIGWIN:.0%}", flush=True)
    res = {}
    with ThreadPoolExecutor(max_workers=MAXPROC) as ex:
        futs = {}
        for name, fzn, d in probs:
            for s in SEEDS:
                futs[ex.submit(run, fzn, False, s)] = (name, s, "base")
                futs[ex.submit(run, fzn, True, s)] = (name, s, "prom")
        for f in as_completed(futs):
            name, s, k = futs[f]
            res.setdefault((name, s), {})[k] = f.result()

    prom_wins, base_wins = [], []
    rows = []
    for name, fzn, d in probs:
        pw = bw = None
        detail = []
        for s in SEEDS:
            r = res.get((name, s), {})
            b, p = r.get("base"), r.get("prom")
            if not b or not p:
                continue
            if bigwin(p, b, d):
                pw = (s, b, p)
            if bigwin(b, p, d):
                bw = (s, b, p)
            detail.append(f"s{s[:3]}:b={b['obj']}{'*' if b['opt'] else ''}/p={p['obj']}{'*' if p['opt'] else ''}")
        if pw:
            prom_wins.append((name, d, pw))
        if bw:
            base_wins.append((name, d, bw))
        tag = "  <<PROM BIGWIN" if pw else ("  <<base bigwin" if bw else "")
        rows.append((name, d, " ".join(detail), tag))

    print(f"\n{'problem':<32} {'dir':<4} detail")
    for name, d, detail, tag in rows:
        if tag:
            print(f"{name:<32} {d:<4} {detail}{tag}")
    print("\n===== PROMOTE BIG WINS (portfolio value) =====")
    for name, d, (s, b, p) in prom_wins:
        print(f"  {name} ({d}) seed {s[:4]}: base obj={b['obj']} opt={b['opt']} -> prom obj={p['obj']} opt={p['opt']}")
    print("===== BASELINE BIG WINS (promote regressions) =====")
    for name, d, (s, b, p) in base_wins:
        print(f"  {name} ({d}) seed {s[:4]}: prom obj={p['obj']} opt={p['opt']} -> base obj={b['obj']} opt={b['opt']}")
    print(f"\nSUMMARY: promote big-wins={len(prom_wins)}  baseline big-wins={len(base_wins)}")

    # ===== VBS 分析（軸×シード分離）: N シードで promote がラダーに新規 VBS を足すか =====
    # base_vbs = N シードの baseline 最良、prom_vbs = N シードの promote 最良。
    # promote が base_vbs を厳密に上回る = シード運と分離した構造的 VBS 貢献。
    print(f"\n===== VBS over {len(SEEDS)} seeds (seed-luck separated) =====")
    prom_adds, base_only = [], []
    for name, fzn, d in probs:
        bo = [res.get((name, s), {}).get("base") for s in SEEDS]
        po = [res.get((name, s), {}).get("prom") for s in SEEDS]
        # 最適証明も VBS の一部: どちらかが opt を出せたか
        b_opt = any(x and x["opt"] for x in bo)
        p_opt = any(x and x["opt"] for x in po)
        bv, pv = vbs(bo, d), vbs(po, d)
        if cmp_better(pv, bv, d) or (p_opt and not b_opt):
            prom_adds.append((name, d, bv, pv, b_opt, p_opt))
        elif cmp_better(bv, pv, d) or (b_opt and not p_opt):
            base_only.append((name, d, bv, pv, b_opt, p_opt))
    print(f"promote が VBS を足す問題（base の {len(SEEDS)} シード最良を上回る）: {len(prom_adds)}")
    for name, d, bv, pv, bopt, popt in prom_adds:
        print(f"  + {name} ({d}): base_vbs={bv}{'*' if bopt else ''} -> prom_vbs={pv}{'*' if popt else ''}")
    print(f"promote が及ばない問題（base のみ）: {len(base_only)}")
    for name, d, bv, pv, bopt, popt in base_only:
        print(f"  - {name} ({d}): base_vbs={bv}{'*' if bopt else ''} vs prom_vbs={pv}{'*' if popt else ''}")


if __name__ == "__main__":
    main()
