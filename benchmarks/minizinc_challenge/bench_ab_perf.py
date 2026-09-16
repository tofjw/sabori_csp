#!/usr/bin/env python3
"""main (base) vs main+perf群 (perf) の 2 バイナリ A/B ベンチマーク。

目的:
    feature/more_constraint の perf 群（cumulative TTEF / int_lin no-op skip /
    table flat trail + batch filter / all_different flat GAC / entailment dispatch /
    par 定数配列 / hash-consing / Variable order 修正 / SABORI_MARCH）を
    main にマージするかどうかを、実測で判定する。

方式:
    各問題を minizinc で FZN にコンパイルしてキャッシュし（flatten ノイズ排除）、
    同一 FZN を 2 バイナリ × 複数シードで直接実行する。
    シードは SABORI_SEED 環境変数で与える。

使い方:
    python3 bench_ab_perf.py                          # 2022-2025, 5 seeds, 30s
    python3 bench_ab_perf.py --years 2022 2023 --seeds 3 --timeout 60
    SABORI_BIN_BASE=/path SABORI_BIN_PERF=/path python3 bench_ab_perf.py
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path
from concurrent.futures import ProcessPoolExecutor, as_completed

import lib_benchmark
from lib_benchmark import (
    BASE_DIR,
    MINIZINC,
    SABORI_MSC,
    PROB_DIRS,
    natural_sort_key,
    kill_process_tree,
    cleanup_stale_processes,
    find_instances,
)

REPO_ROOT = BASE_DIR.parent.parent

BIN_BASE = os.environ.get("SABORI_BIN_BASE", str(REPO_ROOT / "build" / "src" / "fzn" / "fzn_sabori"))
BIN_PERF = os.environ.get(
    "SABORI_BIN_PERF",
    str(REPO_ROOT.parent / "sabori_perf" / "build" / "src" / "fzn" / "fzn_sabori"),
)

FZN_CACHE = BASE_DIR / ".fzn_cache_abperf"
COMPILE_TIMEOUT = 120

SOLVE_RE = re.compile(r"solve\s+(?:::\s*\S+\s+)*(minimize|maximize)\s+([A-Za-z_][A-Za-z0-9_]*)")
FAILS_RE = re.compile(r"fails[=:]\s*(\d+)")


def prob_key(year, name):
    return f"{year}__{name}"


def compile_fzn(year, name, mzn, data):
    FZN_CACHE.mkdir(exist_ok=True)
    out = FZN_CACHE / f"{prob_key(year, name)}.fzn"
    if out.exists() and out.stat().st_size > 0:
        return str(out)
    cmd = [MINIZINC, "-c", "--solver", SABORI_MSC, "--fzn", str(out), mzn]
    if data:
        cmd.append(data)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, start_new_session=True)
    try:
        proc.communicate(timeout=COMPILE_TIMEOUT)
    except subprocess.TimeoutExpired:
        kill_process_tree(proc)
        if out.exists():
            out.unlink()
        return None
    if proc.returncode != 0 or not out.exists() or out.stat().st_size == 0:
        if out.exists():
            out.unlink()
        return None
    return str(out)


def get_objective(fzn_path):
    try:
        text = Path(fzn_path).read_text()
    except OSError:
        return None, None
    m = SOLVE_RE.search(text)
    if not m:
        return None, None
    return m.group(2), m.group(1)


def parse_status(stdout):
    if "=====UNSATISFIABLE=====" in stdout:
        return "UNSAT"
    if "=====ERROR=====" in stdout or "Unsupported" in stdout:
        return "ERR"
    if "==========" in stdout:
        return "OPTIMAL"
    if "----------" in stdout:
        return "SOL"
    return "UNKNOWN"


def run_bin(bin_path, fzn_path, timeout, obj_var, seed):
    env = dict(os.environ)
    env["SABORI_SEED"] = str(seed)
    cmd = [bin_path, "-s", "-t", str(timeout), fzn_path]
    start = time.monotonic()
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, start_new_session=True, env=env)
    try:
        stdout, stderr = proc.communicate(timeout=timeout + 20)
    except subprocess.TimeoutExpired:
        kill_process_tree(proc)
        try:
            stdout, stderr = proc.communicate(timeout=10)
        except Exception:
            stdout, stderr = "", ""
    elapsed = time.monotonic() - start
    status = parse_status(stdout or "")
    obj = None
    if obj_var and stdout:
        objs = re.findall(rf"\b{re.escape(obj_var)}\s*=\s*(-?\d+)", stdout)
        obj = int(objs[-1]) if objs else None
    fm = FAILS_RE.search(stderr or "")
    return {
        "status": status,
        "time": round(elapsed, 2),
        "obj": obj,
        "fails": int(fm.group(1)) if fm else None,
    }


def run_pair(task):
    year, name, fzn, obj_var, direction, timeout, seed = task
    base = run_bin(BIN_BASE, fzn, timeout, obj_var, seed)
    perf = run_bin(BIN_PERF, fzn, timeout, obj_var, seed)
    return {
        "year": year, "name": name, "seed": seed,
        "direction": direction, "base": base, "perf": perf,
    }


STATUS_RANK = {"ERR": -1, "UNKNOWN": 0, "SOL": 1, "OPTIMAL": 2, "UNSAT": 2}


def judge(rec):
    """1 実行ペアの判定。'perf' / 'base' / 'tie' を返す。"""
    b, p = rec["base"], rec["perf"]
    rb, rp = STATUS_RANK.get(b["status"], 0), STATUS_RANK.get(p["status"], 0)
    if rp != rb:
        return "perf" if rp > rb else "base"
    if rb <= 0:
        return "tie"
    # 目的値の比較（両方が解を持つ場合）
    if b["obj"] is not None and p["obj"] is not None and b["obj"] != p["obj"]:
        if rec["direction"] == "minimize":
            return "perf" if p["obj"] < b["obj"] else "base"
        return "perf" if p["obj"] > b["obj"] else "base"
    # 同じ結果なら時間（証明完了 = OPTIMAL/UNSAT のときのみ意味がある）
    if rb == 2:
        tb, tp = b["time"], p["time"]
        if tb > 0.3 or tp > 0.3:  # 極短時間はノイズなので tie
            if tp < tb * 0.85:
                return "perf"
            if tb < tp * 0.85:
                return "base"
    return "tie"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--years", nargs="+", default=["2022", "2023", "2024", "2025"])
    ap.add_argument("--seeds", type=int, default=5)
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--workers", type=int, default=lib_benchmark.MAX_WORKERS)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    for b in (BIN_BASE, BIN_PERF):
        if not Path(b).exists():
            print(f"ERROR: binary not found: {b}", file=sys.stderr)
            return 1

    cleanup_stale_processes()

    print(f"base = {BIN_BASE}")
    print(f"perf = {BIN_PERF}")
    print(f"years={args.years} seeds={args.seeds} timeout={args.timeout}s workers={args.workers}")

    # 1) FZN コンパイル（キャッシュ）
    cands = []
    for year in args.years:
        d = PROB_DIRS.get(year)
        if d is None or not d.exists():
            print(f"WARNING: year {year} missing", file=sys.stderr)
            continue
        for p in sorted(d.iterdir(), key=lambda x: natural_sort_key(x.name)):
            if not p.is_dir():
                continue
            inst = find_instances(p)
            if inst:
                mzn, data, _ = inst[0]
                cands.append((year, p.name, mzn, data))
    print(f"candidates: {len(cands)}")

    tasks = []
    compiled = 0
    for year, name, mzn, data in cands:
        fzn = compile_fzn(year, name, mzn, data)
        if not fzn:
            print(f"  SKIP(compile) {year}/{name}")
            continue
        obj_var, direction = get_objective(fzn)
        compiled += 1
        for seed in range(1, args.seeds + 1):
            tasks.append((year, name, fzn, obj_var, direction, args.timeout, seed))
    print(f"compiled: {compiled}, runs: {len(tasks)} pairs")
    sys.stdout.flush()

    results = []
    done = 0
    with ProcessPoolExecutor(max_workers=args.workers) as ex:
        futs = {ex.submit(run_pair, t): t for t in tasks}
        for fut in as_completed(futs):
            try:
                rec = fut.result()
            except Exception as e:
                print(f"  ERROR: {e}", file=sys.stderr)
                continue
            rec["judge"] = judge(rec)
            results.append(rec)
            done += 1
            if done % 10 == 0 or done == len(tasks):
                print(f"  {done}/{len(tasks)}")
                sys.stdout.flush()

    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    out = args.out or str(BASE_DIR / f"abperf_{stamp}.json")
    Path(out).write_text(json.dumps(results, indent=1))
    print(f"\nraw -> {out}")

    # 集計
    tally = {"perf": 0, "base": 0, "tie": 0}
    for r in results:
        tally[r["judge"]] += 1
    print(f"\n=== 実行単位 (problem x seed): perf勝 {tally['perf']} / base勝 {tally['base']} / tie {tally['tie']}")

    # 問題単位: シード多数決
    byprob = {}
    for r in results:
        byprob.setdefault((r["year"], r["name"]), []).append(r)
    p_win = b_win = t_win = 0
    lines = []
    for key, recs in sorted(byprob.items()):
        c = {"perf": 0, "base": 0, "tie": 0}
        for r in recs:
            c[r["judge"]] += 1
        if c["perf"] > c["base"]:
            verdict = "perf"; p_win += 1
        elif c["base"] > c["perf"]:
            verdict = "base"; b_win += 1
        else:
            verdict = "tie"; t_win += 1
        if verdict != "tie":
            lines.append(f"  {key[0]}/{key[1]:<34} {verdict:<5} (perf{c['perf']}/base{c['base']}/tie{c['tie']})")
    print(f"=== 問題単位 (多数決): perf {p_win} / base {b_win} / tie {t_win}")
    for line in lines:
        print(line)

    # ステータス階層の変化のみ抽出
    print("\n=== ステータス階層が動いた問題 ===")
    moved = 0
    for key, recs in sorted(byprob.items()):
        for r in recs:
            rb = STATUS_RANK.get(r["base"]["status"], 0)
            rp = STATUS_RANK.get(r["perf"]["status"], 0)
            if rb != rp:
                print(f"  {key[0]}/{key[1]} seed{r['seed']}: "
                      f"{r['base']['status']} -> {r['perf']['status']}")
                moved += 1
    if not moved:
        print("  なし")
    return 0


if __name__ == "__main__":
    sys.exit(main())
