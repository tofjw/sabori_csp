#!/usr/bin/env python3
"""案2 の調査: 短いプローブの統計が「どのアームが勝つか」を予測できるかを測る。

設計メモ docs-dev/multistart-portfolio-design.md §2(d) 案2 に対応。

手順:
  Phase 1  全インスタンスを既定構成で短時間（--probe-timeout）走らせ、
           `-s` の Stats を特徴量として記録する。プローブ中に証明まで
           終わる問題は「選択の余地なし」として以降から外す。
  Phase 2  残りを 6 アーム × 複数シードでフル実行する。
  出力     JSON（features と runs）。判定・相関の分析は別途行う。

アーム（いずれも現 main の env フラグ。既定はすべて OFF）:
  base            既定構成
  bottomup        SABORI_BOTTOMUP=1        (budget=2000 / cutoff=8 / iso=off)
  probe_root      SABORI_PROBE_ROOT=1      (budget=2000, failed-literal 検出)
  promote_impact  SABORI_PROMOTE_IMPACT=1  (K=32, root probing を暗黙に有効化)
  clause_witness  SABORI_CLAUSE_WITNESS=1  (最小節長 8, 1WL)
  bisect_low      SABORI_BISECT_DIR=low

使い方:
    python3 probe_feature_study.py                        # 既定: 3s probe, 30s 本番, 3 seeds
    python3 probe_feature_study.py --timeout 20 --seeds 2
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path
from concurrent.futures import ProcessPoolExecutor, as_completed

import lib_benchmark
from lib_benchmark import BASE_DIR, kill_process_tree, cleanup_stale_processes

REPO_ROOT = BASE_DIR.parent.parent
BIN = os.environ.get("SABORI_BIN", str(REPO_ROOT / "build" / "src" / "fzn" / "fzn_sabori"))
FZN_CACHE = BASE_DIR / ".fzn_cache_abperf"

ARMS = {
    "base": {},
    "bottomup": {"SABORI_BOTTOMUP": "1"},
    "probe_root": {"SABORI_PROBE_ROOT": "1"},
    "promote_impact": {"SABORI_PROMOTE_IMPACT": "1"},
    "clause_witness": {"SABORI_CLAUSE_WITNESS": "1"},
    "bisect_low": {"SABORI_BISECT_DIR": "low"},
}

STATS_RE = re.compile(r"^% Stats: (.*)$", re.M)
NGLEN_RE = re.compile(r"^% NG length distribution: (.*)$", re.M)
SOLVE_RE = re.compile(r"solve\s+(?:::\s*\S+\s+)*(minimize|maximize)\s+([A-Za-z_][A-Za-z0-9_]*)")


def parse_status(stdout):
    if "=====UNSATISFIABLE=====" in stdout:
        return "UNSAT"
    if "==========" in stdout:
        return "OPTIMAL"
    if "----------" in stdout:
        return "SOL"
    return "UNKNOWN"


def parse_stats(stderr):
    out = {}
    m = STATS_RE.search(stderr or "")
    if m:
        for kv in m.group(1).split():
            if "=" in kv:
                k, v = kv.split("=", 1)
                try:
                    out[k] = int(v)
                except ValueError:
                    pass
    m = NGLEN_RE.search(stderr or "")
    if m:
        tot = cnt = 0
        for part in m.group(1).split():
            if ":" in part:
                ln, c = part.split(":", 1)
                try:
                    ln, c = int(ln), int(c)
                except ValueError:
                    continue
                tot += ln * c
                cnt += c
        if cnt:
            out["ng_len_mean"] = round(tot / cnt, 2)
            out["ng_len_count"] = cnt
    return out


def run_once(fzn, timeout, env_extra, obj_var, seed=None):
    env = dict(os.environ)
    env.update(env_extra)
    if seed is not None:
        env["SABORI_SEED"] = str(seed)
    cmd = [BIN, "-s", "-t", str(timeout), fzn]
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
    objs = []
    if obj_var and stdout:
        objs = [int(x) for x in re.findall(rf"\b{re.escape(obj_var)}\s*=\s*(-?\d+)", stdout)]
    return {
        "status": status,
        "time": round(elapsed, 2),
        "obj": objs[-1] if objs else None,
        "n_improve": len(objs),
        "stats": parse_stats(stderr),
    }


def static_features(fzn):
    """FZN のテキストから静的特徴を取る。"""
    try:
        text = Path(fzn).read_text()
    except OSError:
        return {}
    lines = text.splitlines()
    n_var = sum(1 for l in lines if l.startswith("var "))
    n_arr = sum(1 for l in lines if l.startswith("array"))
    n_con = sum(1 for l in lines if l.startswith("constraint"))
    kinds = {}
    for l in lines:
        if l.startswith("constraint "):
            m = re.match(r"constraint\s+([A-Za-z_][A-Za-z0-9_]*)", l)
            if m:
                kinds[m.group(1)] = kinds.get(m.group(1), 0) + 1
    m = SOLVE_RE.search(text)
    top = sorted(kinds.items(), key=lambda kv: -kv[1])[:5]
    return {
        "n_var_decl": n_var, "n_array_decl": n_arr, "n_constraint": n_con,
        "n_kind": len(kinds), "top_kinds": top,
        "direction": m.group(1) if m else None,
        "obj_var": m.group(2) if m else None,
        "fzn_bytes": len(text),
    }


def probe_task(args):
    name, fzn, ptimeout = args
    sf = static_features(fzn)
    r = run_once(fzn, ptimeout, {}, sf.get("obj_var"))
    return {"name": name, "static": sf, "probe": r}


def arm_task(args):
    name, fzn, arm, seed, timeout, obj_var = args
    r = run_once(fzn, timeout, ARMS[arm], obj_var, seed)
    return {"name": name, "arm": arm, "seed": seed, "run": r}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe-timeout", type=int, default=3)
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--workers", type=int, default=lib_benchmark.MAX_WORKERS)
    ap.add_argument("--out", default="/tmp/probe_study.json")
    args = ap.parse_args()

    if not Path(BIN).exists():
        print(f"ERROR: binary not found: {BIN}", file=sys.stderr)
        return 1
    fzns = sorted(FZN_CACHE.glob("*.fzn"))
    if not fzns:
        print(f"ERROR: no cached FZN in {FZN_CACHE}", file=sys.stderr)
        return 1
    cleanup_stale_processes()
    print(f"bin={BIN}\ninstances={len(fzns)} probe={args.probe_timeout}s "
          f"timeout={args.timeout}s seeds={args.seeds} workers={args.workers}")
    sys.stdout.flush()

    # Phase 1: probe
    probes = {}
    tasks = [(f.stem, str(f), args.probe_timeout) for f in fzns]
    with ProcessPoolExecutor(max_workers=args.workers) as ex:
        for fut in as_completed([ex.submit(probe_task, t) for t in tasks]):
            rec = fut.result()
            probes[rec["name"]] = rec
    trivial = [n for n, r in probes.items() if r["probe"]["status"] in ("OPTIMAL", "UNSAT")]
    target = [n for n in probes if n not in trivial]
    print(f"Phase1 done: probe で証明完了 {len(trivial)} 件を除外、対象 {len(target)} 件")
    sys.stdout.flush()

    # Phase 2: arms
    atasks = []
    for n in sorted(target):
        fzn = str(FZN_CACHE / f"{n}.fzn")
        ov = probes[n]["static"].get("obj_var")
        for arm in ARMS:
            for seed in range(1, args.seeds + 1):
                atasks.append((n, fzn, arm, seed, args.timeout, ov))
    print(f"Phase2: {len(atasks)} runs")
    sys.stdout.flush()

    runs = []
    done = 0
    with ProcessPoolExecutor(max_workers=args.workers) as ex:
        for fut in as_completed([ex.submit(arm_task, t) for t in atasks]):
            try:
                runs.append(fut.result())
            except Exception as e:
                print(f"  ERROR: {e}", file=sys.stderr)
            done += 1
            if done % 25 == 0 or done == len(atasks):
                print(f"  {done}/{len(atasks)}")
                sys.stdout.flush()

    Path(args.out).write_text(json.dumps(
        {"probes": probes, "trivial": trivial, "runs": runs,
         "config": {"probe_timeout": args.probe_timeout, "timeout": args.timeout,
                    "seeds": args.seeds, "arms": list(ARMS)}}, indent=1))
    print(f"\n-> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
