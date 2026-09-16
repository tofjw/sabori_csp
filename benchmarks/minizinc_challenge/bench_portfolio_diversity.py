#!/usr/bin/env python3
"""ポートフォリオ多様化チューニング用 決定論ベンチ harness。

各候補ワーカー構成を **単一スレッド・fzn直接実行・固定シード** で評価し（並列実行の
wall-clock ノイズを排除）、VBS(virtual best = ワーカーの min) でポートフォリオ性能を
予測してから貪欲に構成を選ぶ。

- 構成は env 変数（SABORI_*）+ CLI で駆動する。
- 最適化問題の目的値は `-v` の "new best objective" 行からパースする（出力変数不要）。
- 結果は JSON にキャッシュし、再分析を高速化する。

使い方:
    python3 bench_portfolio_diversity.py            # フラット化→評価→VBS分析
    python3 bench_portfolio_diversity.py --analyze  # 既存JSONから分析のみ
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
MINIZINC = Path("/snap/bin/minizinc")
FZN = ROOT / "build/src/fzn/fzn_sabori"
WORK = Path("/tmp/portfolio_div")
WORK.mkdir(exist_ok=True)
RESULTS_JSON = WORK / "results.json"

TIME_LIMIT = 12          # 秒/run
MAX_PARALLEL = 4         # メモリ規約: 最大4プロセス

# 候補ワーカー構成プール。各構成は (env追加, CLI追加フラグ) を持つ。
# 非base構成にも個別シードを与える（実デプロイ同様に seed と ablation の両方で多様化）。
S = [12345678, 2666781439, 1026249904, 3680685665,
     2040154130, 399622595, 3054058356, 1413526821,
     1500000001, 2200000002]
CONFIGS = {
    "base":       {"SABORI_SEED": str(S[0])},
    "seedA":      {"SABORI_SEED": str(S[1])},
    "seedB":      {"SABORI_SEED": str(S[2])},
    "mrv_first":  {"SABORI_SEED": str(S[3]), "SABORI_FIX_MIXP": "0"},
    "act_first":  {"SABORI_SEED": str(S[4]), "SABORI_FIX_MIXP": "4"},
    "no_nogood":  {"SABORI_SEED": str(S[5]), "SABORI_NOGOOD": "0"},
    "no_gradient":{"SABORI_SEED": str(S[6]), "SABORI_GRADIENT": "0"},
    "no_probe":   {"SABORI_SEED": str(S[7]), "SABORI_PROBE": "0"},
    "no_temporal":{"SABORI_SEED": str(S[8]), "SABORI_TEMPORAL": "0"},
    "no_restart": {"SABORI_SEED": str(S[9]), "SABORI_RESTART": "0"},
    "conflict":   {"SABORI_SEED": str(S[1]), "SABORI_CONFLICT": "1"},  # conflict 学習を多様化軸として評価
}

# 評価インスタンス: (名前, prob_dir 相対パス, dzn選択)。
#   dzn選択: "min"=最小（速い） / "max"=最大（難しい）。
# 識別力（構成で結果が変わる難しめ）を狙って、難問は max を選ぶ。
PROB_DIRS = [
    ("amaze12",      "mznc2012_probs/amaze",          "min"),
    ("celar13",      "mznc2013_probs/celar",          "max"),
    ("fillomino14",  "mznc2014_probs/fillomino",      "max"),
    ("mario14",      "mznc2014_probs/mario",          "max"),
    ("openshop14",   "mznc2014_probs/openshop",       "max"),
    ("multiknap14",  "mznc2014_probs/multi-knapsack", "max"),
    ("roadcons14",   "mznc2014_probs/road-cons",      "max"),
    ("tppv14",       "mznc2014_probs/traveling-tppv", "max"),
    ("depot16",      "mznc2016_probs/depot-placement","max"),
    ("carpet16",     "mznc2016_probs/carpet-cutting", "max"),
    ("spot5_14",     "mznc2014_probs/spot5",          "min"),
    ("cyclrcpsp14",  "mznc2014_probs/cyclic-rcpsp",   "max"),
    ("shipsch14",    "mznc2014_probs/ship-schedule",  "max"),
    ("solbat14",     "mznc2014_probs/solbat",         "max"),
    ("celar16",      "mznc2016_probs/celar",          "max"),
    ("elitser14",    "mznc2014_probs/elitserien",     "max"),
]


def cleanup():
    for n in ("fzn_sabori", "minizinc"):
        subprocess.run(["pkill", "-x", n], capture_output=True)


def pick_instance(prob_dir, which="min"):
    d = HERE / prob_dir
    mzns = sorted(d.glob("*.mzn"))
    dzns = sorted(d.glob("*.dzn"), key=lambda p: p.stat().st_size)
    if not mzns:
        return None
    mzn = mzns[0]
    if not dzns:
        dzn = None
    elif which == "max":
        dzn = dzns[-1]
    else:
        dzn = dzns[0]
    return mzn, dzn


def flatten(name, mzn, dzn):
    out = WORK / f"{name}.fzn"
    if out.exists() and out.stat().st_size > 0:
        return out
    cmd = [str(MINIZINC), "--solver", "sabori_csp", "--compile", "--fzn", str(out), str(mzn)]
    if dzn:
        cmd.append(str(dzn))
    try:
        subprocess.run(cmd, capture_output=True, timeout=120, cwd=str(HERE))
    except subprocess.TimeoutExpired:
        return None
    return out if out.exists() and out.stat().st_size > 0 else None


def solve_kind(fzn):
    txt = fzn.read_text(errors="ignore")
    m = re.search(r"^solve\b.*\b(minimize|maximize|satisfy)\b", txt, re.M)
    if not m:
        # satisfy がデフォルト
        return "satisfy"
    return m.group(1)


OBJ_RE = re.compile(r"^% objective =\s*(-?\d+)", re.M)


def run_one(fzn, kind, cfg_env):
    env = dict(os.environ)
    env["SABORI_PRINT_OBJ"] = "1"  # 目的値を stderr に出させる（golden 不変のため専用 env）
    env.update(cfg_env)
    cmd = [str(FZN), "-s", "-t", str(TIME_LIMIT), str(fzn)]
    t0 = time.time()
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, env=env,
                           timeout=TIME_LIMIT + 10)
    except subprocess.TimeoutExpired:
        return {"status": "HANG", "obj": None, "time": TIME_LIMIT, "optimal": False}
    elapsed = time.time() - t0
    out = p.stdout + p.stderr
    optimal = "==========" in p.stdout
    solved = "----------" in p.stdout
    unsat = "UNSATISFIABLE" in p.stdout
    out = p.stderr  # "% objective" は stderr
    if kind in ("minimize", "maximize"):
        objs = OBJ_RE.findall(out)
        obj = int(objs[-1]) if objs else None
        if unsat:
            status = "UNSAT"
        elif obj is not None:
            status = "OPT" if optimal else "FEAS"
        else:
            status = "NOSOL"
        return {"status": status, "obj": obj, "time": round(elapsed, 2), "optimal": optimal}
    else:  # satisfy
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
        data = json.loads(RESULTS_JSON.read_text())
        analyze(data)
        return

    cleanup()
    # フラット化
    instances = {}
    for name, prob, which in PROB_DIRS:
        picked = pick_instance(prob, which)
        if not picked:
            print(f"[skip] {name}: no mzn", file=sys.stderr)
            continue
        mzn, dzn = picked
        fzn = flatten(name, mzn, dzn)
        if not fzn:
            print(f"[skip] {name}: flatten failed", file=sys.stderr)
            continue
        kind = solve_kind(fzn)
        instances[name] = {"fzn": str(fzn), "kind": kind}
        print(f"[ok] {name}: {kind}  ({fzn.stat().st_size} B)", file=sys.stderr)

    # 評価グリッド（config × instance）を並列実行
    jobs = []
    for iname, info in instances.items():
        for cname, cenv in CONFIGS.items():
            jobs.append((iname, info, cname, cenv))

    results = {iname: {} for iname in instances}
    done = 0
    total = len(jobs)

    def work(job):
        iname, info, cname, cenv = job
        r = run_one(Path(info["fzn"]), info["kind"], cenv)
        return iname, cname, r

    with ThreadPoolExecutor(max_workers=MAX_PARALLEL) as ex:
        for iname, cname, r in ex.map(work, jobs):
            results[iname][cname] = r
            done += 1
            print(f"  [{done}/{total}] {iname}/{cname}: {r['status']} "
                  f"obj={r['obj']} t={r['time']}", file=sys.stderr)

    data = {"instances": {k: v["kind"] for k, v in instances.items()},
            "configs": list(CONFIGS),
            "time_limit": TIME_LIMIT,
            "results": results}
    RESULTS_JSON.write_text(json.dumps(data, indent=2))
    print(f"\nsaved -> {RESULTS_JSON}", file=sys.stderr)
    analyze(data)


def score(kind, r, best, worst):
    """(config,instance) の [0,1] スコア。VBS 用。best/worst はそのインスタンスの全構成の目的値範囲。"""
    if kind == "satisfy":
        if r["status"] == "SAT":
            return 1.0
        return 0.0
    # optimize
    if r["obj"] is None or r["status"] in ("UNSAT", "NOSOL", "HANG"):
        return 0.0
    if best == worst:
        base = 1.0
    elif kind == "minimize":
        base = (worst - r["obj"]) / (worst - best)
    else:  # maximize
        base = (r["obj"] - worst) / (best - worst)
    # 最適性証明にボーナス（同値でも証明済みを優先）
    return min(1.0, base * 0.97 + (0.03 if r["optimal"] else 0.0))


def analyze(data):
    kinds = data["instances"]
    configs = data["configs"]
    results = data["results"]

    # 各インスタンスの best/worst 目的値
    inst_score = {}  # iname -> {config: score}
    for iname, kind in kinds.items():
        rs = results[iname]
        objs = [rr["obj"] for rr in rs.values()
                if rr["obj"] is not None and rr["status"] in ("OPT", "FEAS")]
        if kind == "minimize":
            best, worst = (min(objs), max(objs)) if objs else (0, 0)
        elif kind == "maximize":
            best, worst = (max(objs), min(objs)) if objs else (0, 0)
        else:
            best = worst = 0
        inst_score[iname] = {c: score(kind, rs[c], best, worst) for c in configs}

    # 構成ごとの平均スコア（単体性能）
    print("\n=== 構成単体の平均スコア（高いほど単体で強い）===")
    avg = {c: sum(inst_score[i][c] for i in kinds) / len(kinds) for c in configs}
    for c in sorted(configs, key=lambda c: -avg[c]):
        print(f"  {c:14s} {avg[c]:.3f}")

    # 貪欲 VBS ポートフォリオ構成
    print("\n=== 貪欲 VBS ポートフォリオ（順に追加して合計 VBS が最大化される順）===")
    selected = []
    remaining = set(configs)
    cur = {i: 0.0 for i in kinds}
    order = []
    while remaining:
        bestc, bestgain, besttot = None, -1, None
        for c in remaining:
            tot = sum(max(cur[i], inst_score[i][c]) for i in kinds)
            gain = tot - sum(cur.values())
            if gain > bestgain:
                bestc, bestgain, besttot = c, gain, tot
        selected.append(bestc)
        remaining.discard(bestc)
        for i in kinds:
            cur[i] = max(cur[i], inst_score[i][bestc])
        vbs = besttot / len(kinds)
        order.append((bestc, vbs, bestgain / len(kinds)))
        print(f"  +{bestc:14s} VBS={vbs:.3f}  (Δ={bestgain/len(kinds):+.3f})")

    print("\n=== 推奨ワーカー順（build_worker_configs の worker0,1,2,... に対応）===")
    print("  " + " -> ".join(c for c, _, _ in order))

    # 各 K での VBS（-j K の見込み性能）
    print("\n=== -j K の見込み VBS ===")
    for k in (1, 2, 3, 4, 6, 8):
        if k <= len(order):
            print(f"  -j{k}: VBS={order[k-1][1]:.3f}")


if __name__ == "__main__":
    main()
