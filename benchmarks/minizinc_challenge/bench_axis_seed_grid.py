#!/usr/bin/env python3
"""軸×シード分離グリッド: ポートフォリオ多様化軸の寄与をシード効果と分離して測る。

背景 (2026-07-02 S(k) 計測の発見):
- 旧 bench_portfolio_diversity.py の CONFIGS は軸ごとに別シードを割り当てていた
  ため、貪欲順の「軸の Δ」にシード運が混入していた (solbat14 の -j8 解禁は
  worker5 の軸 gradient-off ではなくシード 399622595 単独で再現)。
- 旧 bench_restart_sat.py は逆に全構成が base シード固定（シード多様性ゼロ）。
- さらに 2026-07-02 の決定性修正 (int_lin_base のポインタ順→初出順) で全探索
  軌道が変わり、2026-06-30 の VBS 分析データは失効している。

設計:
- 軸 × 4シード の全組合せを単一スレッド・fzn直接・固定シードで評価（決定論）。
- 軸の真の寄与 = mean_s[score(axis,s) − score(base,s)]（同シード差分）。
- 貪欲 VBS ラダーは (軸,シード) ペアの候補プール全体から構成
  （worker0 = base@S0 固定: 「単一スレッドより悪くならない」保証を維持）。

使い方:
    python3 bench_axis_seed_grid.py                  # opt セット計測 → 分析
    python3 bench_axis_seed_grid.py --set sat        # SAT セット計測 → 分析
    python3 bench_axis_seed_grid.py [--set sat] --analyze   # JSON から分析のみ
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
OUT = Path("/tmp/bench_axis_seed")
OUT.mkdir(exist_ok=True)

MAX_PARALLEL = 4

# デプロイと同じ導出式のシード (12345678 + i*2654435761 mod 2^32)
SEEDS = [12345678, 2666781439, 1026249904, 3680685665]

# 軸 (env 差分)。act_first は旧分析で不採用(最弱)なので除外。
# off(restart無効) は SAT 専用軸（最適化では no-op に近い）。
AXES = {
    "base":        {},
    "sc8":         {"SABORI_RESTART_SCALE": "8"},
    "conflict":    {"SABORI_CONFLICT": "1"},
    "conf_sc8":    {"SABORI_CONFLICT": "1", "SABORI_RESTART_SCALE": "8"},
    "mrv":         {"SABORI_FIX_MIXP": "0"},
    "no_nogood":   {"SABORI_NOGOOD": "0"},
    "no_gradient": {"SABORI_GRADIENT": "0"},
    "no_probe":    {"SABORI_PROBE": "0"},
    "no_temporal": {"SABORI_TEMPORAL": "0"},
    "off":         {"SABORI_RESTART": "0"},
}

SETS = {
    "opt": {
        "work": Path("/tmp/portfolio_div"),   # bench_portfolio_diversity の fzn キャッシュ
        "instances": [
            "amaze12", "celar13", "fillomino14", "mario14", "openshop14",
            "multiknap14", "roadcons14", "tppv14", "depot16", "carpet16",
            "spot5_14", "cyclrcpsp14", "shipsch14", "solbat14", "celar16",
            "elitser14",
        ],
        "time_limit": 12,
        "sat_time_score": False,   # satisfy は解けた/解けないの2値
        "axes": [a for a in AXES if a != "off"],
        "json": OUT / "results.json",
        # 現行 make_portfolio_configs (optimize) の k<=4 相当
        "old_ladder": [("base", 0), ("conflict", 1), ("mrv", 2), ("no_nogood", 3)],
    },
    "sat": {
        "work": Path("/tmp/restart_sat"),     # bench_restart_sat の fzn キャッシュ
        "instances": [
            "costas16", "costas17", "costas19", "we01_123", "we01_140",
            "we02_8", "we03_133", "we03_31", "fillomino", "solbat", "monomatch",
        ],
        "time_limit": 15,
        "sat_time_score": True,    # SAT は速さも評価 (1 - 0.5*t/T)
        "axes": list(AXES),
        "json": OUT / "results_sat.json",
        # 現行 make_portfolio_configs (SAT) の k<=4 相当
        "old_ladder": [("base", 0), ("sc8", 1), ("conf_sc8", 2), ("mrv", 3)],
    },
}

OBJ_RE = re.compile(r"^% objective =\s*(-?\d+)", re.M)


def cfg_key(axis, si):
    return f"{axis}@S{si}"


def cleanup():
    for n in ("fzn_sabori", "minizinc"):
        subprocess.run(["pkill", "-x", n], capture_output=True)
    time.sleep(0.5)


def solve_kind(fzn):
    txt = fzn.read_text(errors="ignore")
    m = re.search(r"^solve\b.*\b(minimize|maximize|satisfy)\b", txt, re.M)
    return m.group(1) if m else "satisfy"


def run_one(fzn, kind, env_add, time_limit):
    env = dict(os.environ)
    env["SABORI_PRINT_OBJ"] = "1"
    env.update(env_add)
    cmd = [str(FZN), "-s", "-t", str(time_limit), "-j", "1", str(fzn)]
    t0 = time.time()
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, env=env,
                           timeout=time_limit + 15)
    except subprocess.TimeoutExpired:
        return {"status": "HANG", "obj": None, "time": time_limit, "optimal": False}
    elapsed = time.time() - t0
    optimal = "==========" in p.stdout
    solved = "----------" in p.stdout
    unsat = "UNSATISFIABLE" in p.stdout
    if kind in ("minimize", "maximize"):
        objs = OBJ_RE.findall(p.stderr)
        obj = int(objs[-1]) if objs else None
        status = ("UNSAT" if unsat else
                  ("OPT" if optimal else "FEAS") if obj is not None else "NOSOL")
        return {"status": status, "obj": obj, "time": round(elapsed, 2), "optimal": optimal}
    status = "UNSAT" if unsat else ("SAT" if solved else "NOSOL")
    return {"status": status, "obj": None, "time": round(elapsed, 2),
            "optimal": optimal or solved}


def main():
    set_name = "sat" if "--set" in sys.argv and \
        sys.argv[sys.argv.index("--set") + 1] == "sat" else "opt"
    cfg = SETS[set_name]

    if "--analyze" in sys.argv and cfg["json"].exists():
        analyze(json.loads(cfg["json"].read_text()), cfg)
        return

    cleanup()
    instances = {}
    for name in cfg["instances"]:
        fzn = cfg["work"] / f"{name}.fzn"
        if not fzn.exists() or fzn.stat().st_size == 0:
            print(f"[skip] {name}: fzn cache missing", file=sys.stderr)
            continue
        instances[name] = {"fzn": str(fzn), "kind": solve_kind(fzn)}

    jobs = []
    for iname, info in instances.items():
        for axis in cfg["axes"]:
            for si, seed in enumerate(SEEDS):
                e = dict(AXES[axis])
                e["SABORI_SEED"] = str(seed)
                jobs.append((iname, info, cfg_key(axis, si), e))

    results = {i: {} for i in instances}
    done, total = 0, len(jobs)

    def work(job):
        iname, info, ck, e = job
        return iname, ck, run_one(Path(info["fzn"]), info["kind"], e,
                                  cfg["time_limit"])

    with ThreadPoolExecutor(max_workers=MAX_PARALLEL) as ex:
        for iname, ck, r in ex.map(work, jobs):
            results[iname][ck] = r
            done += 1
            print(f"  [{done}/{total}] {iname}/{ck}: {r['status']} "
                  f"obj={r['obj']} t={r['time']}", file=sys.stderr)

    data = {"set": set_name,
            "instances": {i: v["kind"] for i, v in instances.items()},
            "axes": cfg["axes"], "n_seeds": len(SEEDS), "seeds": SEEDS,
            "time_limit": cfg["time_limit"],
            "sat_time_score": cfg["sat_time_score"],
            "results": results}
    cfg["json"].write_text(json.dumps(data, indent=2))
    print(f"\nsaved -> {cfg['json']}", file=sys.stderr)
    analyze(data, cfg)


def make_score(data):
    tl = data["time_limit"]
    sat_time = data.get("sat_time_score", False)

    def score(kind, r, best, worst):
        if kind == "satisfy":
            if r["status"] not in ("SAT", "UNSAT"):
                return 0.0
            return (1.0 - 0.5 * r["time"] / tl) if sat_time else 1.0
        if r["obj"] is None or r["status"] in ("UNSAT", "NOSOL", "HANG"):
            return 0.0
        if best == worst:
            base = 1.0
        elif kind == "minimize":
            base = (worst - r["obj"]) / (worst - best)
        else:
            base = (r["obj"] - worst) / (best - worst)
        return min(1.0, base * 0.97 + (0.03 if r["optimal"] else 0.0))
    return score


def analyze(data, cfg):
    data.setdefault("set", "opt")  # 旧 JSON 互換
    kinds = data["instances"]
    axes = data["axes"]
    n_seeds = data["n_seeds"]
    results = data["results"]
    score = make_score(data)
    configs = [cfg_key(a, s) for a in axes for s in range(n_seeds)]

    ranges = {}
    for i, kind in kinds.items():
        objs = [r["obj"] for r in results[i].values()
                if r["obj"] is not None and r["status"] in ("OPT", "FEAS")]
        if not objs:
            ranges[i] = (0, 0)
        elif kind == "minimize":
            ranges[i] = (min(objs), max(objs))
        else:
            ranges[i] = (max(objs), min(objs))

    sc = {i: {c: score(kinds[i], results[i][c], *ranges[i]) for c in configs}
          for i in kinds}

    all_is = list(kinds)

    # 1. 軸の真の寄与（同シード差分の平均）
    print(f"\n=== [{data['set']}] 軸の真の寄与: mean[score(axis,s) − score(base,s)] ===")
    print(f"{'axis':13s} {'Δmean':>8s}   per-seed Δ")
    for a in axes:
        if a == "base":
            continue
        deltas = [[sc[i][cfg_key(a, s)] - sc[i][cfg_key("base", s)] for i in all_is]
                  for s in range(n_seeds)]
        per_seed = " ".join(f"{sum(d)/len(d):+.3f}" for d in deltas)
        flat = [v for d in deltas for v in d]
        print(f"{a:13s} {sum(flat)/len(flat):+8.3f}   [{per_seed}]")

    # 2. 純シード変種のみの VBS(k)
    print(f"\n=== [{data['set']}] 純シード変種のみの VBS(k)（base@S* 貪欲追加）===")
    cur = {i: sc[i][cfg_key("base", 0)] for i in all_is}
    picks = [("base@S0", sum(cur.values()) / len(all_is))]
    rem = [cfg_key("base", s) for s in range(1, n_seeds)]
    while rem:
        bc, bg = None, -1
        for c in rem:
            g = sum(max(cur[i], sc[i][c]) for i in all_is) - sum(cur.values())
            if g > bg:
                bc, bg = c, g
        rem.remove(bc)
        for i in all_is:
            cur[i] = max(cur[i], sc[i][bc])
        picks.append((bc, sum(cur.values()) / len(all_is)))
    print("  " + " -> ".join(f"{c}({v:.3f})" for c, v in picks))

    # 3. 全候補プールでの貪欲 VBS ラダー（worker0 = base@S0 固定）
    print(f"\n=== [{data['set']}] 貪欲 VBS ラダー（候補 = 全 軸×シード, worker0=base@S0 固定）===")
    cur = {i: sc[i][cfg_key("base", 0)] for i in all_is}
    print(f"  worker0: base@S0          VBS={sum(cur.values())/len(all_is):.3f}")
    rem = set(configs) - {cfg_key("base", 0)}
    for w in range(1, 8):
        bc, bg = None, -1e9
        for c in rem:
            g = sum(max(cur[i], sc[i][c]) for i in all_is) - sum(cur.values())
            if g > bg:
                bc, bg = c, g
        rem.discard(bc)
        for i in all_is:
            cur[i] = max(cur[i], sc[i][bc])
        print(f"  worker{w}: {bc:16s} VBS={sum(cur.values())/len(all_is):.3f}  "
              f"(Δ={bg/len(all_is):+.3f})")

    # 4. 現行ラダーの同データ VBS 予測（k<=4）
    print(f"\n=== [{data['set']}] 参考: 現行ラダーの VBS 予測（同データ, k<=4）===")
    cur = {i: 0.0 for i in all_is}
    for k, (a, s) in enumerate(cfg["old_ladder"], 1):
        c = cfg_key(a, s)
        for i in all_is:
            cur[i] = max(cur[i], sc[i][c])
        print(f"  -j{k} ({c:16s}): VBS={sum(cur.values())/len(all_is):.3f}")


if __name__ == "__main__":
    main()
