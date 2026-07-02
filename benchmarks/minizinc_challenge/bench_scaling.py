#!/usr/bin/env python3
"""S(k) スケーリング計測: -j k (k=1,2,4,8) の実並列 wall-clock ベンチ。

bench_portfolio_diversity.py が単一スレッド決定論で予測した VBS が、実際の
`-j k` 並列実行でどこまで実現するかを測る。曲線が平らになる k を特定し、
候補プール(A) / シード循環(B) / 協調不足(C) のどこがボトルネックか判別する。

- 問題セット・fzn キャッシュ・スコア正規化は bench_portfolio_diversity.py を流用。
- k=1 は決定的(2026-07-02 のポインタ順修正後)なので 1 回、k>1 は非決定なので反復。
- wall-clock 計測のため k ごとに逐次フェーズ実行し、フェーズ内の同時実行は
  総スレッド数 <= 16 (32コアの半分) かつ最大4プロセスに制限してノイズを抑える。

使い方:
    python3 bench_scaling.py             # 計測 → JSON → 分析
    python3 bench_scaling.py --analyze   # 既存 JSON から分析のみ
"""
import json
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
FZN = ROOT / "build/src/fzn/fzn_sabori"
WORK = Path("/tmp/portfolio_div")   # bench_portfolio_diversity.py の fzn キャッシュを流用
OUT = Path("/tmp/bench_scaling")
OUT.mkdir(exist_ok=True)
RESULTS_JSON = OUT / "results.json"

TIME_LIMIT = 12          # 秒/run（VBS 分析と同条件）
KS = [1, 2, 4, 8]
REPS = {1: 1, 2: 3, 4: 3, 8: 3}          # k=1 は決定的なので1回
MAX_THREADS_TOTAL = 16                    # フェーズ内の総スレッド上限（ノイズ抑制）
MAX_PROCS = 4                             # メモリ規約: 最大4プロセス

INSTANCES = [
    "amaze12", "celar13", "fillomino14", "mario14", "openshop14",
    "multiknap14", "roadcons14", "tppv14", "depot16", "carpet16",
    "spot5_14", "cyclrcpsp14", "shipsch14", "solbat14", "celar16",
    "elitser14",
]

OBJ_RE = re.compile(r"^% objective =\s*(-?\d+)", re.M)


def cleanup():
    for n in ("fzn_sabori", "minizinc"):
        subprocess.run(["pkill", "-x", n], capture_output=True)
    time.sleep(0.5)


def solve_kind(fzn):
    txt = fzn.read_text(errors="ignore")
    m = re.search(r"^solve\b.*\b(minimize|maximize|satisfy)\b", txt, re.M)
    return m.group(1) if m else "satisfy"


def run_one(fzn, kind, k):
    env = dict(os.environ)
    env["SABORI_PRINT_OBJ"] = "1"
    cmd = [str(FZN), "-s", "-t", str(TIME_LIMIT), "-j", str(k), str(fzn)]
    t0 = time.time()
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, env=env,
                           timeout=TIME_LIMIT + 15)
    except subprocess.TimeoutExpired:
        return {"status": "HANG", "obj": None, "time": TIME_LIMIT, "optimal": False}
    elapsed = time.time() - t0
    optimal = "==========" in p.stdout
    solved = "----------" in p.stdout
    unsat = "UNSATISFIABLE" in p.stdout
    if kind in ("minimize", "maximize"):
        objs = OBJ_RE.findall(p.stderr)
        obj = int(objs[-1]) if objs else None
        if unsat:
            status = "UNSAT"
        elif obj is not None:
            status = "OPT" if optimal else "FEAS"
        else:
            status = "NOSOL"
        return {"status": status, "obj": obj, "time": round(elapsed, 2), "optimal": optimal}
    else:
        if unsat:
            status = "UNSAT"
        elif solved:
            status = "SAT"
        else:
            status = "NOSOL"
        return {"status": status, "obj": None, "time": round(elapsed, 2),
                "optimal": optimal or solved}


def main():
    if "--analyze" in sys.argv and RESULTS_JSON.exists():
        analyze(json.loads(RESULTS_JSON.read_text()))
        return

    cleanup()
    instances = {}
    for name in INSTANCES:
        fzn = WORK / f"{name}.fzn"
        if not fzn.exists() or fzn.stat().st_size == 0:
            print(f"[skip] {name}: fzn cache missing (run bench_portfolio_diversity.py first)",
                  file=sys.stderr)
            continue
        instances[name] = {"fzn": str(fzn), "kind": solve_kind(fzn)}

    # results[iname][str(k)] = [run結果 × reps]
    results = {i: {str(k): [] for k in KS} for i in instances}

    for k in KS:  # k ごとに逐次フェーズ（負荷を一定に保つ）
        par = max(1, min(MAX_PROCS, MAX_THREADS_TOTAL // k))
        jobs = [(i, rep) for rep in range(REPS[k]) for i in instances]
        print(f"\n=== phase -j {k}: {len(jobs)} runs, {par} procs in parallel ===",
              file=sys.stderr)

        def work(job):
            iname, rep = job
            info = instances[iname]
            r = run_one(Path(info["fzn"]), info["kind"], k)
            return iname, rep, r

        done = 0
        with ThreadPoolExecutor(max_workers=par) as ex:
            for iname, rep, r in ex.map(work, jobs):
                results[iname][str(k)].append(r)
                done += 1
                print(f"  [{done}/{len(jobs)}] -j{k} {iname} rep{rep}: {r['status']} "
                      f"obj={r['obj']} t={r['time']}", file=sys.stderr)

    data = {"instances": {i: v["kind"] for i, v in instances.items()},
            "ks": KS, "reps": REPS, "time_limit": TIME_LIMIT,
            "results": results}
    RESULTS_JSON.write_text(json.dumps(data, indent=2))
    print(f"\nsaved -> {RESULTS_JSON}", file=sys.stderr)
    analyze(data)


def score(kind, r, best, worst):
    """bench_portfolio_diversity.py と同じ [0,1] スコア。"""
    if kind == "satisfy":
        return 1.0 if r["status"] in ("SAT", "UNSAT") else 0.0
    if r["obj"] is None or r["status"] in ("UNSAT", "NOSOL", "HANG"):
        return 0.0
    if best == worst:
        base = 1.0
    elif kind == "minimize":
        base = (worst - r["obj"]) / (worst - best)
    else:
        base = (r["obj"] - worst) / (best - worst)
    return min(1.0, base * 0.97 + (0.03 if r["optimal"] else 0.0))


def analyze(data):
    kinds = data["instances"]
    ks = data["ks"]
    results = data["results"]

    # 正規化範囲: 全 (k, rep) の目的値から
    ranges = {}
    for i, kind in kinds.items():
        objs = [r["obj"] for k in ks for r in results[i][str(k)]
                if r["obj"] is not None and r["status"] in ("OPT", "FEAS")]
        if not objs:
            ranges[i] = (0, 0)
        elif kind == "minimize":
            ranges[i] = (min(objs), max(objs))
        else:
            ranges[i] = (max(objs), min(objs))

    print(f"\n{'=' * 72}")
    print(f"S(k) scaling  (timeout {data['time_limit']}s, reps: "
          f"{ {int(a): b for a, b in data['reps'].items()} })")
    print(f"{'=' * 72}")

    # 問題×k の詳細テーブル
    print(f"\n{'instance':14s} {'kind':9s}", end="")
    for k in ks:
        print(f"  {'-j' + str(k):>16s}", end="")
    print()
    for i, kind in kinds.items():
        print(f"{i:14s} {kind[:9]:9s}", end="")
        for k in ks:
            rs = results[i][str(k)]
            ss = [score(kind, r, *ranges[i]) for r in rs]
            mean_s = sum(ss) / len(ss)
            # 表示: 平均スコア + 代表 status/time
            best_r = max(zip(ss, rs), key=lambda t: t[0])[1]
            cell = f"{mean_s:.2f} {best_r['status'][:4]:4s}{best_r['time']:5.1f}s"
            print(f"  {cell:>16s}", end="")
        print()

    # S(k) サマリ
    print(f"\n--- S(k) summary (mean score over instances; reps averaged) ---")
    sat_is = [i for i, kd in kinds.items() if kd == "satisfy"]
    opt_is = [i for i, kd in kinds.items() if kd != "satisfy"]

    def mean_score(inames, k):
        vals = []
        for i in inames:
            rs = results[i][str(k)]
            ss = [score(kinds[i], r, *ranges[i]) for r in rs]
            vals.append(sum(ss) / len(ss))
        return sum(vals) / len(vals) if vals else 0.0

    def solved_count(inames, k):
        """反復の過半数で解けた問題数"""
        n = 0
        for i in inames:
            rs = results[i][str(k)]
            ok = sum(1 for r in rs if r["status"] in ("SAT", "OPT", "FEAS", "UNSAT"))
            if ok * 2 > len(rs):
                n += 1
        return n

    def proved_count(inames, k):
        n = 0
        for i in inames:
            rs = results[i][str(k)]
            ok = sum(1 for r in rs if r["optimal"])
            if ok * 2 > len(rs):
                n += 1
        return n

    hdr = f"{'k':>3s} {'S_all':>7s} {'S_opt':>7s} {'S_sat':>7s} " \
          f"{'solved':>7s} {'proved':>7s}"
    print(hdr)
    prev = None
    for k in ks:
        s_all = mean_score(list(kinds), k)
        s_opt = mean_score(opt_is, k)
        s_sat = mean_score(sat_is, k)
        sv = solved_count(list(kinds), k)
        pv = proved_count(list(kinds), k)
        delta = f"  (ΔS={s_all - prev:+.3f})" if prev is not None else ""
        print(f"{k:>3d} {s_all:>7.3f} {s_opt:>7.3f} {s_sat:>7.3f} "
              f"{sv:>4d}/{len(kinds):<2d} {pv:>4d}/{len(kinds):<2d}{delta}")
        prev = s_all


if __name__ == "__main__":
    main()
