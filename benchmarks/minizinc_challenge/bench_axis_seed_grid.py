#!/usr/bin/env python3
"""軸×シード分離グリッド: ポートフォリオ・ラダー再チューニング用の限界効果計測。

目的:
    多様化軸それぞれの「同一シードでの限界効果」を base との対比で測る。
    2026-07-03 の教訓（memory: portfolio-ladder-seed-first）:
      - 軸ごとに別シードで測ると「シード運」が軸の効果に混入する。
        必ず base と同一シードのペアで比較する（軸×シード分離）。
      - 1スレッドの処理方式が変わったらラダーは要再チューニング。
        本スクリプトは feature/mp-port（2026-09 の perf 群 + build_order
        post-presolve 後の軌道）でのラダー値決定に使う。

方式:
    .fzn_cache_abperf の FZN を fzn_sabori で直接実行（flatten ノイズなし、-j 1）。
    base（軸なし）と各軸を同一 SABORI_SEED で走らせ、lib_benchmark.judge_winner で
    ペア勝敗を判定。Δ = (wins - losses) / pairs を軸ごとに集計する。

使い方:
    python3 bench_axis_seed_grid.py --seeds 3 --timeout 30          # 全77問
    python3 bench_axis_seed_grid.py --years 2024 2025 --limit 10    # サブセット
    python3 bench_axis_seed_grid.py --axes conflict rs4 bisect_low  # 軸を絞る
    結果: /tmp/axis_seed_grid.json + stdout のサマリ表

注意:
    - 旧 bench_axis_seed_grid.py（2026-07、未追跡で消失）の再構築。
      旧版の time.time() クロックジャンプバグは time.monotonic() で回避済み。
    - 実行前の常駐プロセス掃除は cleanup_stale_processes()（pkill -x 相当）。
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

import lib_benchmark
from lib_benchmark import (
    BASE_DIR,
    cleanup_stale_processes,
    judge_winner,
    kill_process_tree,
    natural_sort_key,
)

REPO_ROOT = BASE_DIR.parent.parent
BIN = os.environ.get("SABORI_BIN", str(REPO_ROOT / "build" / "src" / "fzn" / "fzn_sabori"))
FZN_CACHE = BASE_DIR / ".fzn_cache_abperf"

SOLVE_RE = re.compile(r"solve\s+(?:::\s*[^;]*\s+)?(minimize|maximize)\b")
OBJ_RE = re.compile(r"%\s*objective\s*=\s*(-?\d+)")

# 軸定義: name -> (env dict, 追加 CLI 引数)
# ラダー候補（スレッド間=構造系 / スロット間=初期分散系）を両方測る。
AXES = {
    # --- スレッド間（構造的戦略）候補 ---
    "conflict":       ({}, ["-C"]),
    "nogood_off":     ({"SABORI_NOGOOD": "0"}, []),
    "mrv":            ({"SABORI_FIX_MIXP": "0"}, []),
    "gradient_off":   ({"SABORI_GRADIENT": "0"}, []),
    "probe_off":      ({"SABORI_PROBE": "0"}, []),
    "probe_root":     ({"SABORI_PROBE_ROOT": "1"}, []),
    "bottomup":       ({"SABORI_BOTTOMUP": "1"}, []),
    "promote_impact": ({"SABORI_PROMOTE_IMPACT": "1"}, []),
    # --- スロット間（初期分散）候補 ---
    "rs2":            ({"SABORI_RESTART_SCALE": "2"}, []),
    "rs4":            ({"SABORI_RESTART_SCALE": "4"}, []),
    "rs8":            ({"SABORI_RESTART_SCALE": "8"}, []),
    "bisect_low":     ({"SABORI_BISECT_DIR": "low"}, []),
    "bisect_high":    ({"SABORI_BISECT_DIR": "high"}, []),
}

SEEDS = [12345678, 987654321, 555555555, 42424242, 31415926]


def prob_type_of(fzn_path):
    txt = Path(fzn_path).read_text(errors="replace")
    m = SOLVE_RE.search(txt)
    if not m:
        return "SAT"
    return "MIN" if m.group(1) == "minimize" else "MAX"


def run_one(fzn, env_extra, cli_extra, seed, timeout):
    """1 run。戻り値 (status, wall_time, obj)。status は judge_winner の語彙。"""
    env = os.environ.copy()
    env.update(env_extra)
    env["SABORI_SEED"] = str(seed)
    env["SABORI_PRINT_OBJ"] = "1"
    cmd = [BIN, "-j", "1", "-t", str(timeout)] + cli_extra + [str(fzn)]
    t0 = time.monotonic()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, env=env, start_new_session=True)
    try:
        out, err = proc.communicate(timeout=timeout + 15)
    except subprocess.TimeoutExpired:
        kill_process_tree(proc)
        return "ERR", timeout + 15, None
    wall = time.monotonic() - t0

    obj = None
    m = OBJ_RE.findall(err)
    if m:
        obj = int(m[-1])
    if "=====UNSATISFIABLE=====" in out:
        return "UNSAT", wall, None
    if "==========" in out:
        return "OPTIMAL", wall, obj
    if "=====TIMEOUT=====" in out:
        return "TIMEOUT", wall, obj
    if "----------" in out:
        return "SOL", wall, obj
    if "=====UNKNOWN=====" in out:
        return "UNKNOWN", wall, None
    return "ERR", wall, None


def worker(job):
    key, axis, seed, fzn, timeout = job
    env_extra, cli_extra = AXES[axis] if axis != "base" else ({}, [])
    status, wall, obj = run_one(fzn, env_extra, cli_extra, seed, timeout)
    return key, axis, seed, status, wall, obj


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seeds", type=int, default=3, help="シード数 (最大 %d)" % len(SEEDS))
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--years", nargs="*", default=None, help="対象年 (キャッシュ命名の prefix)")
    ap.add_argument("--limit", type=int, default=0, help="問題数上限 (0=全部)")
    ap.add_argument("--axes", nargs="*", default=None, help="測る軸 (既定: 全軸)")
    ap.add_argument("--out", default="/tmp/axis_seed_grid.json")
    ap.add_argument("--jobs", type=int, default=4, help="並列プロセス数 (上限4厳守)")
    args = ap.parse_args()

    axes = args.axes if args.axes else list(AXES)
    for a in axes:
        if a not in AXES:
            sys.exit(f"unknown axis: {a} (choose from {list(AXES)})")
    seeds = SEEDS[: args.seeds]
    jobs_n = min(args.jobs, 4)

    fzns = sorted(FZN_CACHE.glob("*.fzn"), key=lambda p: natural_sort_key(p.name))
    if args.years:
        fzns = [f for f in fzns if any(f.name.startswith(y + "__") for y in args.years)]
    if args.limit:
        fzns = fzns[: args.limit]
    if not fzns:
        sys.exit("no cached fzn found (bench_ab_perf.py を先に走らせてキャッシュを作る)")

    ptypes = {f.stem: prob_type_of(f) for f in fzns}
    cleanup_stale_processes()

    jobs = []
    for f in fzns:
        for s in seeds:
            for a in ["base"] + axes:
                jobs.append((f.stem, a, s, f, args.timeout))
    total = len(jobs)
    print(f"problems={len(fzns)} axes={len(axes)} seeds={len(seeds)} runs={total} "
          f"timeout={args.timeout}s jobs={jobs_n}", flush=True)

    results = {}  # (key, axis, seed) -> (status, wall, obj)
    done = 0
    with ProcessPoolExecutor(max_workers=jobs_n) as ex:
        futs = [ex.submit(worker, j) for j in jobs]
        for fut in as_completed(futs):
            key, axis, seed, status, wall, obj = fut.result()
            results[(key, axis, seed)] = (status, wall, obj)
            done += 1
            if done % 50 == 0 or done == total:
                print(f"  {done}/{total}", flush=True)

    # 集計: 軸ごとに base との同一シードペア勝敗
    summary = {}
    detail = defaultdict(list)
    for a in axes:
        w = l = t = n = 0
        for f in fzns:
            key = f.stem
            pt = ptypes[key]
            for s in seeds:
                b = results.get((key, "base", s))
                x = results.get((key, a, s))
                if not b or not x:
                    continue
                n += 1
                winner, _ = judge_winner(x[0], x[1], x[2], b[0], b[1], b[2], pt)
                if winner == "Sabori":
                    w += 1
                    detail[a].append((key, s, "win", x, b))
                elif winner == "CP-SAT":
                    l += 1
                    detail[a].append((key, s, "loss", x, b))
                else:
                    t += 1
        summary[a] = {"wins": w, "losses": l, "ties": t, "pairs": n,
                      "delta": (w - l) / n if n else 0.0}

    print("\n=== 軸×シード分離グリッド (Δ = (win-loss)/pairs, base と同一シードペア比較) ===")
    for a in sorted(axes, key=lambda a: -summary[a]["delta"]):
        s = summary[a]
        print(f"  {a:16s} Δ={s['delta']:+.3f}  W{s['wins']:3d} L{s['losses']:3d} T{s['ties']:3d} (n={s['pairs']})")

    dump = {
        "meta": {"bin": BIN, "timeout": args.timeout, "seeds": seeds,
                 "axes": axes, "problems": [f.stem for f in fzns]},
        "summary": summary,
        "runs": {f"{k}|{a}|{s}": v for (k, a, s), v in results.items()},
        "flips": {a: [(k, s, r) for (k, s, r, _, _) in detail[a]] for a in axes},
    }
    Path(args.out).write_text(json.dumps(dump, indent=1))
    print(f"\nraw -> {args.out}")


if __name__ == "__main__":
    main()
