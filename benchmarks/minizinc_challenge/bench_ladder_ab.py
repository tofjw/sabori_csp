#!/usr/bin/env python3
"""スレッドラダー変更の実並列 A/B（2 バイナリ × -jN × 同一シードペア）。

目的:
    make_portfolio_configs（スレッド間ラダー）の変更を、単スレッド greedy VBS ではなく
    実際の ParallelSolver（bound 共有・非決定的なスレッド間相互作用込み）で判定する。
    単スレッド VBS は実並列で符号ごと反転した前例がある（promote-def-bool）。

方式:
    bench_ab_perf.py の FZN キャッシュ（.fzn_cache_abperf）を使い、同一 FZN を
    old/new 2 バイナリ × 複数シードで直接実行する。シードは SABORI_SEED（base.seed。
    ワーカー1以降のシードもここから導出される）。同一 (問題, シード) の old/new を
    隣接して投入し、時間ドリフトを差として拾わないようにする。
    勝敗は judge_winner（new を "Sabori" 側に置く）。平均に薄まらないよう、
    尾の指標（解なし数・最適性証明数）も併記する（multistart-portfolio-design.md §2(c)）。

使い方:
    python3 bench_ladder_ab.py --old /path/old/fzn_sabori --threads 4
    python3 bench_ladder_ab.py --old ... --threads 8 --seeds 3 --timeout 30 --out res.json
    python3 bench_ladder_ab.py --old ... --type opt     # 最適化問題のみ（既定）
    # 同一バイナリで環境変数だけ変えるアーム比較（例: 適応的ファンアウト vs 固定 n=1）
    python3 bench_ladder_ab.py --old build/src/fzn/fzn_sabori --threads 1 \
        --new-env SABORI_MULTISTART_N=3,SABORI_MULTISTART_ADAPT=1000
"""
import argparse
import json
import os
import re
import subprocess
import time
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

from lib_benchmark import BASE_DIR, kill_process_tree, cleanup_stale_processes, judge_winner

REPO_ROOT = BASE_DIR.parent.parent
BIN_NEW_DEFAULT = str(REPO_ROOT / "build" / "src" / "fzn" / "fzn_sabori")
FZN_CACHE = BASE_DIR / ".fzn_cache_abperf"
SEEDS = [12345678, 987654321, 555555555, 42424242, 31415926]
OBJ_RE = re.compile(r"% objective = (-?\d+)")
SOLVE_RE = re.compile(r"^solve.*$", re.M)


def prob_type_of(fzn):
    lines = SOLVE_RE.findall(fzn.read_text(errors="replace"))
    last = lines[-1] if lines else ""
    return "MIN" if "minimize" in last else "MAX" if "maximize" in last else "SAT"


def parse_env(spec):
    """"K=V,K=V" を dict に（空なら {}）。"""
    out = {}
    for item in filter(None, (spec or "").split(",")):
        k, _, v = item.partition("=")
        out[k.strip()] = v.strip()
    return out


def run_one(job):
    key, side, binpath, fzn, seed, threads, timeout, side_env = job
    env = dict(os.environ, SABORI_SEED=str(seed), SABORI_PRINT_OBJ="1", **side_env)
    cmd = [binpath, "-j", str(threads), "-t", str(timeout), str(fzn)]
    t0 = time.monotonic()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, env=env, start_new_session=True)
    try:
        out, err = proc.communicate(timeout=timeout + 20)
    except subprocess.TimeoutExpired:
        kill_process_tree(proc)
        return key, side, seed, ("ERR", timeout + 20, None)
    wall = time.monotonic() - t0
    objs = OBJ_RE.findall(err)
    obj = int(objs[-1]) if objs else None
    if "=====UNSATISFIABLE=====" in out:
        st = "UNSAT"
    elif "==========" in out:
        st = "OPTIMAL"
    elif "=====TIMEOUT=====" in out:
        st = "TIMEOUT"
    elif "----------" in out:
        st = "SOL"
    elif "=====UNKNOWN=====" in out:
        st = "UNKNOWN"
    else:
        st = "ERR"
    return key, side, seed, (st, wall, obj)


def has_sol(r):
    return r[0] in ("OPTIMAL", "SOL", "UNSAT") or (r[0] == "TIMEOUT" and r[2] is not None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--old", required=True, help="比較元バイナリ")
    ap.add_argument("--new", default=BIN_NEW_DEFAULT, help="比較先バイナリ (既定: build/)")
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--type", choices=["opt", "sat", "all"], default="opt")
    ap.add_argument("--outer", type=int, default=None,
                    help="同時実行プロセス数 (既定: min(4, 24 // threads))")
    ap.add_argument("--limit", type=int, default=None)
    ap.add_argument("--cache", default=str(FZN_CACHE),
                    help="FZN キャッシュディレクトリ (既定: .fzn_cache_abperf。ホールドアウト検証用に差し替え)")
    ap.add_argument("--old-env", default="", help="old 側だけに渡す環境変数 (K=V,K=V)")
    ap.add_argument("--new-env", default="", help="new 側だけに渡す環境変数 (K=V,K=V)")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()
    side_envs = {"old": parse_env(args.old_env), "new": parse_env(args.new_env)}

    outer = args.outer or max(1, min(4, 24 // args.threads))
    fzns = sorted(Path(args.cache).glob("*.fzn"))
    ptypes = {f.stem: prob_type_of(f) for f in fzns}
    if args.type == "opt":
        fzns = [f for f in fzns if ptypes[f.stem] != "SAT"]
    elif args.type == "sat":
        fzns = [f for f in fzns if ptypes[f.stem] == "SAT"]
    if args.limit:
        fzns = fzns[:args.limit]
    seeds = SEEDS[:args.seeds]

    jobs = []
    for f in fzns:
        for s in seeds:
            for side, b in (("old", args.old), ("new", args.new)):
                jobs.append((f.stem, side, b, f, s, args.threads, args.timeout, side_envs[side]))
    print(f"problems={len(fzns)} seeds={len(seeds)} -j{args.threads} outer={outer} "
          f"timeout={args.timeout}s runs={len(jobs)} "
          f"(~{len(jobs) * args.timeout / outer / 60:.0f} min)", flush=True)

    cleanup_stale_processes()
    results = {}
    with ProcessPoolExecutor(max_workers=outer) as ex:
        futs = [ex.submit(run_one, j) for j in jobs]
        for i, fut in enumerate(as_completed(futs), 1):
            key, side, seed, r = fut.result()
            results[(key, side, seed)] = r
            if i % 50 == 0 or i == len(jobs):
                print(f"  {i}/{len(jobs)}", flush=True)

    w = l = t = 0
    both_proved_ratio = []  # 両側とも証明したペアの wall 比 new/old（分割税の目安）
    per_prob = defaultdict(lambda: [0, 0])
    tail = {"old": defaultdict(int), "new": defaultdict(int)}
    for f in fzns:
        key = f.stem
        for s in seeds:
            o, n = results.get((key, "old", s)), results.get((key, "new", s))
            if not o or not n:
                continue
            for side, r in (("old", o), ("new", n)):
                tail[side]["nosol"] += not has_sol(r)
                tail[side]["proved"] += r[0] in ("OPTIMAL", "UNSAT")
                tail[side]["err"] += r[0] == "ERR"
            if o[0] in ("OPTIMAL", "UNSAT") and n[0] in ("OPTIMAL", "UNSAT"):
                both_proved_ratio.append(n[1] / max(o[1], 1e-3))
            win, _ = judge_winner(n[0], n[1], n[2], o[0], o[1], o[2], ptypes[key])
            if win == "Sabori":
                w += 1
                per_prob[key][0] += 1
            elif win == "CP-SAT":
                l += 1
                per_prob[key][1] += 1
            else:
                t += 1
    pairs = w + l + t
    print(f"\n=== ladder A/B -j{args.threads} (new vs old, 同一シードペア) ===")
    print(f"  Δ={(w - l) / pairs if pairs else 0:+.3f}  W{w} L{l} T{t} (pairs={pairs})")
    all_w = sorted(k for k, (a, b) in per_prob.items() if a == len(seeds))
    all_l = sorted(k for k, (a, b) in per_prob.items() if b == len(seeds))
    flip = sorted(k for k, (a, b) in per_prob.items() if a and b)
    print(f"  問題単位: 全シードnew勝ち {len(all_w)} / 全シードold勝ち {len(all_l)} / シードで反転 {len(flip)}")
    print(f"    new全勝: {' '.join(all_w)}")
    print(f"    old全勝: {' '.join(all_l)}")
    if both_proved_ratio:
        r = sorted(both_proved_ratio)
        print(f"  両側証明ペアの時間比 new/old: 中央値 {r[len(r) // 2]:.3f}  "
              f"平均 {sum(r) / len(r):.3f}  (n={len(r)})")
    for side in ("old", "new"):
        tl = tail[side]
        print(f"  {side}: 解なし {tl['nosol']}  証明(OPT/UNSAT) {tl['proved']}  ERR {tl['err']}")

    if args.out:
        Path(args.out).write_text(json.dumps({
            "meta": {"old": args.old, "new": args.new, "threads": args.threads,
                     "old_env": side_envs["old"], "new_env": side_envs["new"],
                     "seeds": seeds, "timeout": args.timeout,
                     "problems": [f.stem for f in fzns]},
            "summary": {"wins": w, "losses": l, "ties": t, "pairs": pairs},
            "runs": {f"{k}|{sd}|{s}": v for (k, sd, s), v in results.items()},
        }, indent=1))


if __name__ == "__main__":
    main()
