#!/usr/bin/env python3
"""bottom-up optimistic probe (SABORI_BOTTOMUP) のノブグリッド計測。

対象ノブ:
  - budget  (SABORI_BOTTOMUP):          probe 1 ステップの fail 予算
  - isolate (SABORI_BOTTOMUP_ISOLATE):  probe の activity 汚染を隔離するか
  - cutoff  (SABORI_BOTTOMUP_CUTOFF):   相転移カットオフの分母 (0=無効)

問題セットは G1 (ペナルティ和・下界崩壊型、docs-dev/loss-taxonomy-20260709.md) を
中心に、非 G1 の対照 2 問を加える。FZN 直接実行 (flatten ノイズ排除) × 複数シード、
判定は obj 品質。OFF (probe 無効) を基準に各構成の net を出す。

使い方:
    python3 bench_bottomup.py                     # 既定グリッド, 2シード, 30s
    python3 bench_bottomup.py --seeds 1 2 3 --timeout 60
"""
import argparse
import os
import re
import subprocess
import sys
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor, as_completed
from datetime import datetime
from pathlib import Path

from lib_benchmark import (
    BASE_DIR,
    MINIZINC,
    SABORI_MSC,
    kill_process_tree,
    cleanup_stale_processes,
    find_instances,
)

REPO_ROOT = BASE_DIR.parent.parent
BIN = str(REPO_ROOT / "build" / "src" / "fzn" / "fzn_sabori")
CACHE = BASE_DIR / ".fzn_cache_bottomup"
COMPILE_TIMEOUT = 120
MAX_WORKERS = 4

# G1 メンバー + 対照 (tsptw=通常最適化, rcpsp=G3)
PROBLEMS = [
    ("2015", "zephyrus"),
    ("2018", "gfd-schedule"),
    ("2022", "gfd-schedule"),
    ("2022", "arithmetic-target"),
    ("2023", "roster-shifts-bool"),
    ("2023", "code-generator"),
    ("2024", "network_50_cstr"),
    ("2020", "collaborative-construction"),
    ("2025", "tsptw"),
    ("2013", "rcpsp"),
]

# 構成グリッド: (label, env dict)。OFF が基準
CONFIGS = [
    ("off",          {}),
    ("b2k",          {"SABORI_BOTTOMUP": "2000"}),
    ("b2k-iso",      {"SABORI_BOTTOMUP": "2000", "SABORI_BOTTOMUP_ISOLATE": "1"}),
    ("b2k-nocut",    {"SABORI_BOTTOMUP": "2000", "SABORI_BOTTOMUP_CUTOFF": "0"}),
    ("b20k",         {"SABORI_BOTTOMUP": "20000"}),
    ("b20k-iso",     {"SABORI_BOTTOMUP": "20000", "SABORI_BOTTOMUP_ISOLATE": "1"}),
    ("b20k-nocut",   {"SABORI_BOTTOMUP": "20000", "SABORI_BOTTOMUP_CUTOFF": "0"}),
    ("b80k",         {"SABORI_BOTTOMUP": "80000"}),
    ("b80k-iso",     {"SABORI_BOTTOMUP": "80000", "SABORI_BOTTOMUP_ISOLATE": "1"}),
]

SOLVE_RE = re.compile(r"\b(minimize|maximize)\s+(\w+)")


def ensure_output_objective(fzn_path):
    """目的変数の宣言に :: output_var を付与 (bench_int_eq_imp.py と同じ)"""
    text = Path(fzn_path).read_text()
    m = SOLVE_RE.search(text)
    if not m:
        return
    var = m.group(2)
    decl_re = re.compile(
        rf"^(var [^:=]+: {re.escape(var)})((?:\s*::\s*[^;=]+)?);\s*$", re.MULTILINE)
    dm = decl_re.search(text)
    if dm is None or "output_var" in (dm.group(2) or ""):
        return
    text = decl_re.sub(r"\1\2 :: output_var;", text, count=1)
    Path(fzn_path).write_text(text)


def compile_one(task):
    key, mzn, data = task
    out = CACHE / f"{key}.fzn"
    if out.exists() and out.stat().st_size > 0:
        return key, str(out)
    cmd = [MINIZINC, "-c", "--solver", SABORI_MSC, "--fzn", str(out),
           "--no-output-ozn", mzn]
    if data:
        cmd.append(data)
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            text=True, start_new_session=True, cwd=str(BASE_DIR))
    try:
        proc.communicate(timeout=COMPILE_TIMEOUT)
    except subprocess.TimeoutExpired:
        kill_process_tree(proc)
        if out.exists():
            out.unlink()
        return key, None
    if proc.returncode != 0 or not out.exists() or out.stat().st_size == 0:
        if out.exists():
            out.unlink()
        return key, None
    ensure_output_objective(out)
    return key, str(out)


def get_objective(fzn_path):
    m = SOLVE_RE.search(Path(fzn_path).read_text())
    return (m.group(2), m.group(1)) if m else (None, None)


def run_one(task):
    key, cfg_label, cfg_env, fzn, seed, timeout = task
    obj_var, _ = get_objective(fzn)
    env = dict(os.environ)
    env["SABORI_SEED"] = str(seed)
    env.pop("SABORI_BOTTOMUP", None)
    env.pop("SABORI_BOTTOMUP_ISOLATE", None)
    env.pop("SABORI_BOTTOMUP_CUTOFF", None)
    env.update(cfg_env)
    cmd = [BIN, "-t", str(timeout), "-a", fzn]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            text=True, start_new_session=True, env=env)
    try:
        stdout, _ = proc.communicate(timeout=timeout + 15)
    except subprocess.TimeoutExpired:
        kill_process_tree(proc)
        stdout, _ = proc.communicate()
    obj = None
    if obj_var:
        objs = re.findall(rf"^{re.escape(obj_var)}\s*=\s*(-?\d+);", stdout, re.MULTILINE)
        obj = int(objs[-1]) if objs else None
    return key, cfg_label, seed, obj


def better(a, b, direction):
    """a が b より良ければ +1、悪ければ -1、同等/比較不能 0"""
    if a is None and b is None:
        return 0
    if a is None:
        return -1
    if b is None:
        return +1
    if a == b:
        return 0
    if direction == "minimize":
        return +1 if a < b else -1
    return +1 if a > b else -1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--seeds", nargs="+", type=int, default=[1, 2])
    args = ap.parse_args()

    cleanup_stale_processes()
    CACHE.mkdir(exist_ok=True)

    # コンパイル
    tasks = []
    for year, prob in PROBLEMS:
        inst = find_instances(str(BASE_DIR / f"mznc{year}_probs" / prob))
        if not inst:
            print(f"WARNING: {year}/{prob} なし", file=sys.stderr)
            continue
        mzn, data, _ = inst[0]
        tasks.append((f"{year}__{prob}", mzn, data))
    fzn = {}
    with ProcessPoolExecutor(max_workers=MAX_WORKERS) as ex:
        for fut in as_completed({ex.submit(compile_one, t): t for t in tasks}):
            key, path = fut.result()
            fzn[key] = path
    keys = [k for k, v in fzn.items() if v]
    print(f"problems: {len(keys)}, configs: {len(CONFIGS)}, seeds: {args.seeds}")

    # 実行
    run_tasks = []
    for key in keys:
        for label, env in CONFIGS:
            for seed in args.seeds:
                run_tasks.append((key, label, env, fzn[key], seed, args.timeout))
    print(f"running {len(run_tasks)} tasks (timeout {args.timeout}s, workers {MAX_WORKERS})")
    results = {}
    with ProcessPoolExecutor(max_workers=MAX_WORKERS) as ex:
        futs = {ex.submit(run_one, t): t for t in run_tasks}
        done = 0
        for fut in as_completed(futs):
            key, label, seed, obj = fut.result()
            results[(key, label, seed)] = obj
            done += 1
            if done % 20 == 0:
                print(f"  {done}/{len(run_tasks)}")

    # 集計: 各構成 vs off の net (問題×シード単位)
    print("\n" + "=" * 100)
    print(f"bottomup grid  {datetime.now().strftime('%Y-%m-%d %H:%M')}  "
          f"timeout {args.timeout}s seeds {args.seeds}")
    print("=" * 100)
    dirs = {k: get_objective(fzn[k])[1] for k in keys}
    G1 = {k for k in keys if not (k.endswith("tsptw") or k.endswith("rcpsp"))}
    header = f"{'config':12s} {'net(all)':>9s} {'net(G1)':>8s} {'win/loss/tie':>14s}"
    print(header)
    summary = []
    for label, _ in CONFIGS:
        if label == "off":
            continue
        net = net_g1 = w = l = t_ = 0
        for key in keys:
            for seed in args.seeds:
                r = better(results.get((key, label, seed)),
                           results.get((key, "off", seed)), dirs[key])
                net += r
                if key in G1:
                    net_g1 += r
                if r > 0: w += 1
                elif r < 0: l += 1
                else: t_ += 1
        summary.append((net_g1, net, label))
        print(f"{label:12s} {net:+9d} {net_g1:+8d} {w:5d}/{l}/{t_}")

    print("\n--- per-problem (config ごとの seed1 obj / off) ---")
    for key in keys:
        cells = []
        for label, _ in CONFIGS:
            o = results.get((key, label, args.seeds[0]))
            cells.append(f"{label.replace('b', '')}={o if o is not None else 'NA'}")
        print(f"{key:36s} [{dirs[key][:3]}] {' '.join(cells)}")

    best = max(summary)
    print(f"\nBEST (G1 net 優先): {best[2]}  net_G1={best[0]:+d} net_all={best[1]:+d}")


if __name__ == "__main__":
    main()
