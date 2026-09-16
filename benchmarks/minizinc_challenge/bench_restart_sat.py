#!/usr/bin/env python3
"""SAT インスタンスでの restart 変種比較（多様化チューニングの restart スロット用）。

最適化では restart 緩和(scale>1)が逆効果だったが、SAT では restart_enabled=off が
実際に効く（最適化と違い no-op でない）。SAT を増やして
  base / off(RESTART=0) / scale{2,4,8,16}
を比較し、restart スロットの最良設定を決める。

SAT の評価: solved(=----------) なら time が小さいほど良い。未解は 0 点。
VBS = 各インスタンスで最良スコアの構成。
"""
import os
import re
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
MINIZINC = Path("/snap/bin/minizinc")
FZN = ROOT / "build/src/fzn/fzn_sabori"
WORK = Path("/tmp/restart_sat")
WORK.mkdir(exist_ok=True)

TIME_LIMIT = 15
MAX_PARALLEL = 4

# (名前, mzn 相対, dzn 相対 or None)
INSTANCES = [
    ("costas16",  "mznc2010_probs/costas_array/CostasArray.mzn", "mznc2010_probs/costas_array/16.dzn"),
    ("costas17",  "mznc2010_probs/costas_array/CostasArray.mzn", "mznc2010_probs/costas_array/17.dzn"),
    ("costas18",  "mznc2010_probs/costas_array/CostasArray.mzn", "mznc2010_probs/costas_array/18.dzn"),
    ("costas19",  "mznc2010_probs/costas_array/CostasArray.mzn", "mznc2010_probs/costas_array/19.dzn"),
    ("we01_123",  "mznc2024_probs/word-equations/word_equations_01_track_123-int.mzn", None),
    ("we01_140",  "mznc2024_probs/word-equations/word_equations_01_track_140-int.mzn", None),
    ("we02_8",    "mznc2024_probs/word-equations/word_equations_02_track_8-int.mzn", None),
    ("we03_133",  "mznc2024_probs/word-equations/word_equations_03_track_133-int.mzn", None),
    ("we03_31",   "mznc2024_probs/word-equations/word_equations_03_track_31-int.mzn", None),
    ("fillomino", "mznc2014_probs/fillomino/fillomino.mzn", None),  # dzn 自動
    ("solbat",    "mznc2014_probs/solbat/sb.mzn", None),
    ("monomatch", "mznc2021_probs/monomatch/monomatch.mzn", None),
]

CONFIGS = {
    "base":   {},
    "off":    {"SABORI_RESTART": "0"},
    "sc2":    {"SABORI_RESTART_SCALE": "2"},
    "sc4":    {"SABORI_RESTART_SCALE": "4"},
    "sc8":    {"SABORI_RESTART_SCALE": "8"},
    "sc16":   {"SABORI_RESTART_SCALE": "16"},
    "conf":   {"SABORI_CONFLICT": "1"},                          # conflict 学習（SAT で効くか）
    "conf_sc8": {"SABORI_CONFLICT": "1", "SABORI_RESTART_SCALE": "8"},  # conflict + restart緩和の併用
}


def cleanup():
    for n in ("fzn_sabori", "minizinc"):
        subprocess.run(["pkill", "-x", n], capture_output=True)


def pick_dzn(prob_dir):
    dzns = sorted((HERE / prob_dir).glob("*.dzn"), key=lambda p: p.stat().st_size)
    return dzns[len(dzns) // 2] if dzns else None  # 中央サイズ


def flatten(name, mzn_rel, dzn_rel):
    out = WORK / f"{name}.fzn"
    if out.exists() and out.stat().st_size > 0:
        return out
    mzn = HERE / mzn_rel
    if not mzn.exists():
        return None
    dzn = (HERE / dzn_rel) if dzn_rel else pick_dzn(str(Path(mzn_rel).parent))
    cmd = [str(MINIZINC), "--solver", "sabori_csp", "--compile", "--fzn", str(out), str(mzn)]
    if dzn:
        cmd.append(str(dzn))
    try:
        subprocess.run(cmd, capture_output=True, timeout=120, cwd=str(HERE))
    except subprocess.TimeoutExpired:
        return None
    return out if out.exists() and out.stat().st_size > 0 else None


def run_one(fzn, cenv):
    env = dict(os.environ)
    env.update(cenv)
    t0 = time.time()
    try:
        p = subprocess.run([str(FZN), "-t", str(TIME_LIMIT), str(fzn)],
                           capture_output=True, text=True, env=env, timeout=TIME_LIMIT + 10)
    except subprocess.TimeoutExpired:
        return {"solved": False, "time": TIME_LIMIT, "unsat": False}
    elapsed = time.time() - t0
    solved = "----------" in p.stdout
    unsat = "UNSATISFIABLE" in p.stdout
    return {"solved": solved, "time": round(elapsed, 2), "unsat": unsat}


def main():
    cleanup()
    instances = {}
    for name, mzn, dzn in INSTANCES:
        f = flatten(name, mzn, dzn)
        if f:
            instances[name] = f
            print(f"[ok] {name}")
        else:
            print(f"[skip] {name}")

    jobs = [(i, f, c, e) for i, f in instances.items() for c, e in CONFIGS.items()]
    results = {i: {} for i in instances}

    def work(job):
        i, f, c, e = job
        return i, c, run_one(f, e)

    done, total = 0, len(jobs)
    with ThreadPoolExecutor(max_workers=MAX_PARALLEL) as ex:
        for i, c, r in ex.map(work, jobs):
            results[i][c] = r
            done += 1
            st = "SAT %.1fs" % r["time"] if r["solved"] else ("UNSAT" if r["unsat"] else "timeout")
            print(f"  [{done}/{total}] {i}/{c}: {st}")

    # スコア: solved なら 1 - 0.5*time/T（速いほど高い）、未解 0。
    def score(r):
        if not r["solved"]:
            return 0.0
        return 1.0 - 0.5 * r["time"] / TIME_LIMIT

    print("\n=== インスタンス別 solved 時間（s, '.'=未解）===")
    print(f"{'instance':12s} " + " ".join(f"{c:>7s}" for c in CONFIGS))
    for i in instances:
        cells = []
        for c in CONFIGS:
            r = results[i][c]
            cells.append(f"{r['time']:7.1f}" if r["solved"] else f"{'.':>7s}")
        print(f"{i:12s} " + " ".join(cells))

    print("\n=== 構成別 solved 数 / 平均スコア ===")
    for c in CONFIGS:
        ns = sum(1 for i in instances if results[i][c]["solved"])
        avg = sum(score(results[i][c]) for i in instances) / max(1, len(instances))
        print(f"  {c:6s} solved={ns}/{len(instances)}  avg_score={avg:.3f}")

    print("\n=== 貪欲 VBS（restart スロットに最良な変種を選ぶ）===")
    sc = {i: {c: score(results[i][c]) for c in CONFIGS} for i in instances}
    cur = {i: 0.0 for i in instances}
    remaining = set(CONFIGS)
    while remaining:
        bestc, bestgain = None, -1
        for c in remaining:
            gain = sum(max(cur[i], sc[i][c]) for i in instances) - sum(cur.values())
            if gain > bestgain:
                bestc, bestgain = c, gain
        for i in instances:
            cur[i] = max(cur[i], sc[i][bestc])
        remaining.discard(bestc)
        print(f"  +{bestc:6s} VBS={sum(cur.values())/len(instances):.3f} (Δ={bestgain/len(instances):+.3f})")


if __name__ == "__main__":
    main()
