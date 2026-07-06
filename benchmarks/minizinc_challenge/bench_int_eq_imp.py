#!/usr/bin/env python3
"""int_eq_imp / bool_eq_imp 宣言再有効化の A/B 性能ベンチマーク。

背景:
    f8c701b (2026-03-05) で IntEqImpConstraint を実装したが「少し遅い」として
    redefinitions.mzn の predicate 宣言をコメントアウトした。宣言がないと
    MiniZinc は half-reification できず int_eq_reif (full reif) に展開する。
    当時から propagator 核は大きく変わったため再計測する
    (docs-dev/fzn-inventory-mznc2025.md 提案 #2。hitori では CP-SAT が
    int_eq_imp×1484 を使うのに対し sabori は int_eq_reif×1687 だった)。

方式:
    - baseline: 現行 build mznlib (宣言コメントアウトのまま)
    - imp: build mznlib のコピーに int_eq_imp / bool_eq_imp の宣言を足した変種
    両方で各問題を FZN にコンパイルし、imp 側に int_eq_imp / bool_eq_imp が
    1つも現れない問題は FZN 同一の no-op なので除外。残りを同一バイナリで
    fzn 直接実行 (flatten ノイズ排除・決定論) × 複数シードで比較する。

判定は解の目的値 (タイムは使わない)。同 obj なら最適性証明の有無。
net = imp の勝ち − 負け。

使い方:
    python3 bench_int_eq_imp.py                          # 2022-2025, 5シード, 30s
    python3 bench_int_eq_imp.py --years 2023 2024
    python3 bench_int_eq_imp.py --timeout 60 --seeds 1 2 3
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from collections import defaultdict
from concurrent.futures import ProcessPoolExecutor, as_completed
from datetime import datetime
from pathlib import Path

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
BIN = str(REPO_ROOT / "build" / "src" / "fzn" / "fzn_sabori")
BUILD_MZNLIB = REPO_ROOT / "build" / "share" / "minizinc" / "sabori_csp"

CACHE = BASE_DIR / ".fzn_cache_eqimp"
COMPILE_TIMEOUT = 90
MAX_WORKERS = 4

SOLVE_RE = re.compile(r"\b(minimize|maximize)\s+([A-Za-z_][A-Za-z0-9_]*)")
IMP_RE = re.compile(r"^constraint (?:int|bool)_eq_imp", re.MULTILINE)


def setup_imp_variant():
    """build mznlib をコピーし宣言を有効化した imp 変種と .msc を作る。"""
    lib_dir = CACHE / "mznlib_imp"
    if lib_dir.exists():
        shutil.rmtree(lib_dir)
    shutil.copytree(BUILD_MZNLIB, lib_dir)

    redef = lib_dir / "redefinitions.mzn"
    text = redef.read_text()
    for pred in ("int_eq_imp", "bool_eq_imp"):
        commented = f"% predicate {pred}("
        if commented not in text:
            print(f"ERROR: redefinitions.mzn に '{commented}' が見つからない", file=sys.stderr)
            sys.exit(2)
        text = text.replace(commented, f"predicate {pred}(")
    redef.write_text(text)

    msc = json.loads(Path(SABORI_MSC).read_text())
    msc["id"] = "io.github.tofjw.sabori_csp_eqimp"
    msc["name"] = "sabori_csp_eqimp"
    msc["mznlib"] = str(lib_dir)
    msc_path = CACHE / "sabori_csp_eqimp.msc"
    msc_path.write_text(json.dumps(msc, indent=2))
    return str(msc_path)


def ensure_output_objective(fzn_path):
    """目的変数の宣言に :: output_var を足す (無い場合)。両変種に同一適用なので公平。

    FZN 直接実行では output_var 注釈の変数しか出力されず、minizinc が導入した
    目的変数は注釈を持たないことがある。obj を読めないと A/B 判定が status
    比較に退化するため、キャッシュ後に一度だけ注釈を付与する。
    """
    text = Path(fzn_path).read_text()
    m = SOLVE_RE.search(text)
    if not m:
        return
    var = m.group(2)
    # 定義付き宣言 (= rhs) は対象外 ('=' を含む行はスキップ)。名前は完全一致。
    decl_re = re.compile(
        rf"^(var [^:=]+: {re.escape(var)})((?:\s*::\s*[^;=]+)?);\s*$", re.MULTILINE)
    dm = decl_re.search(text)
    if dm is None or "output_var" in (dm.group(2) or ""):
        return
    text = decl_re.sub(r"\1\2 :: output_var;", text, count=1)
    Path(fzn_path).write_text(text)


def compile_one(task):
    """(key, variant, msc, mzn, data) -> (key, variant, fzn_path or None)"""
    key, variant, msc, mzn, data = task
    out = CACHE / variant / f"{key}.fzn"
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.exists() and out.stat().st_size > 0:
        return key, variant, str(out)
    cmd = [MINIZINC, "-c", "--solver", msc, "--fzn", str(out),
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
        return key, variant, None
    if proc.returncode != 0 or not out.exists() or out.stat().st_size == 0:
        if out.exists():
            out.unlink()
        return key, variant, None
    ensure_output_objective(out)
    return key, variant, str(out)


def parse_status(stdout):
    if "=====UNSATISFIABLE=====" in stdout:
        return "UNSAT"
    if "=====TIMEOUT=====" in stdout:
        return "TIMEOUT"
    if "=====UNKNOWN=====" in stdout:
        return "UNKNOWN"
    if "==========" in stdout:
        return "OPTIMAL"
    if "----------" in stdout:
        return "SOL"
    return "UNKNOWN"


def run_one(task):
    """(key, variant, fzn, seed, timeout) -> (key, variant, seed, status, obj)"""
    key, variant, fzn, seed, timeout = task
    obj_var, direction = get_objective(fzn)
    cmd = [BIN, "-t", str(timeout), fzn]
    env = dict(os.environ)
    env["SABORI_SEED"] = str(seed)
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                            text=True, start_new_session=True, env=env)
    try:
        stdout, _ = proc.communicate(timeout=timeout + 15)
    except subprocess.TimeoutExpired:
        kill_process_tree(proc)
        stdout, _ = proc.communicate()
    status = parse_status(stdout)
    obj = None
    if obj_var:
        objs = re.findall(rf"\b{re.escape(obj_var)}\s*=\s*(-?\d+)", stdout)
        obj = int(objs[-1]) if objs else None
    return key, variant, seed, status, obj


def get_objective(fzn_path):
    text = Path(fzn_path).read_text()
    m = SOLVE_RE.search(text)
    if not m:
        return None, None
    return m.group(2), m.group(1)


def judge(direction, s_a, o_a, s_b, o_b):
    """baseline(a) vs imp(b)。戻り値: 'base' / 'imp' / 'tie'"""
    solved_a = s_a in ("OPTIMAL", "SOL", "UNSAT")
    solved_b = s_b in ("OPTIMAL", "SOL", "UNSAT")
    if o_a is not None and o_b is not None:
        if o_a != o_b:
            if direction == "minimize":
                return "base" if o_a < o_b else "imp"
            return "base" if o_a > o_b else "imp"
        # 同 obj: 最適性証明の有無
        if (s_a == "OPTIMAL") != (s_b == "OPTIMAL"):
            return "base" if s_a == "OPTIMAL" else "imp"
        return "tie"
    if o_a is not None:
        return "base"
    if o_b is not None:
        return "imp"
    if solved_a != solved_b:
        return "base" if solved_a else "imp"
    return "tie"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--years", nargs="+", default=["2022", "2023", "2024", "2025"])
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--seeds", nargs="+", type=int, default=[1, 2, 3, 4, 5])
    args = ap.parse_args()

    cleanup_stale_processes()
    CACHE.mkdir(exist_ok=True)
    imp_msc = setup_imp_variant()

    # 候補収集 (各問題の先頭インスタンス)
    cands = []
    for year in args.years:
        probs_dir = PROB_DIRS.get(year)
        if probs_dir is None or not probs_dir.exists():
            print(f"WARNING: {year} なし、スキップ", file=sys.stderr)
            continue
        for p in sorted(probs_dir.iterdir(), key=lambda x: natural_sort_key(x.name)):
            if not p.is_dir():
                continue
            inst = find_instances(p)
            if not inst:
                continue
            mzn, data, _ = inst[0]
            cands.append((f"{year}__{p.name}", mzn, data))
    print(f"candidates: {len(cands)} problems ({', '.join(args.years)})")

    # 両変種でコンパイル
    tasks = []
    for key, mzn, data in cands:
        tasks.append((key, "base", SABORI_MSC, mzn, data))
        tasks.append((key, "imp", imp_msc, mzn, data))
    fzn = {}
    with ProcessPoolExecutor(max_workers=MAX_WORKERS) as ex:
        futs = {ex.submit(compile_one, t): t for t in tasks}
        for fut in as_completed(futs):
            key, variant, path = fut.result()
            fzn[(key, variant)] = path

    # 対象化: imp FZN に int_eq_imp / bool_eq_imp が現れる問題のみ
    affected = []
    for key, mzn, data in cands:
        fa, fb = fzn.get((key, "base")), fzn.get((key, "imp"))
        if not fa or not fb:
            continue
        n_imp = len(IMP_RE.findall(Path(fb).read_text()))
        if n_imp > 0:
            affected.append((key, fa, fb, n_imp))
    print(f"affected: {len(affected)} problems (imp FZN に *_eq_imp あり)")
    for key, _, _, n in affected:
        print(f"  {key}: {n} imp constraints")
    if not affected:
        print("対象問題なし")
        return

    # A/B 実行
    run_tasks = []
    for key, fa, fb, _ in affected:
        for seed in args.seeds:
            run_tasks.append((key, "base", fa, seed, args.timeout))
            run_tasks.append((key, "imp", fb, seed, args.timeout))
    print(f"\nrunning {len(run_tasks)} tasks "
          f"({len(affected)} probs x 2 x {len(args.seeds)} seeds, "
          f"timeout {args.timeout}s, workers {MAX_WORKERS})")
    results = {}
    with ProcessPoolExecutor(max_workers=MAX_WORKERS) as ex:
        futs = {ex.submit(run_one, t): t for t in run_tasks}
        done = 0
        for fut in as_completed(futs):
            key, variant, seed, status, obj = fut.result()
            results[(key, variant, seed)] = (status, obj)
            done += 1
            if done % 20 == 0:
                print(f"  {done}/{len(run_tasks)} done")

    # 集計
    print("\n" + "=" * 100)
    print(f"int_eq_imp A/B  (base=宣言なし/full-reif, imp=宣言あり/half-reif)  "
          f"{datetime.now().strftime('%Y-%m-%d %H:%M')}")
    print(f"timeout {args.timeout}s, seeds {args.seeds}")
    print("=" * 100)
    total = defaultdict(int)
    per_prob = {}
    for key, fa, fb, n_imp in affected:
        direction = get_objective(fa)[1]
        w = defaultdict(int)
        cells = []
        for seed in args.seeds:
            s_a, o_a = results.get((key, "base", seed), ("?", None))
            s_b, o_b = results.get((key, "imp", seed), ("?", None))
            verdict = judge(direction, s_a, o_a, s_b, o_b)
            w[verdict] += 1
            total[verdict] += 1
            def fmt(s, o):
                return f"{o}" if o is not None else s[:4]
            cells.append(f"{fmt(s_a, o_a)}/{fmt(s_b, o_b)}")
        per_prob[key] = w
        net = w["imp"] - w["base"]
        print(f"{key:42s} imp={n_imp:5d}  net={net:+d}  "
              f"[base/imp per seed: {' '.join(cells)}]")
    print("-" * 100)
    net = total["imp"] - total["base"]
    print(f"TOTAL: imp_win={total['imp']}  base_win={total['base']}  "
          f"tie={total['tie']}  net(imp)={net:+d}")


if __name__ == "__main__":
    main()
