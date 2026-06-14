#!/usr/bin/env python3
"""int_lin_* の項の並び順 (SABORI_LIN_ORDER) の比較実験。

キャッシュ済み .fzn のうち int_lin 制約が多い問題を対象に、
mode 0(初出順)/1(|c|降順)/2(|c|昇順)/3(|c|×幅 降順)/4(|c|×幅 昇順) を
fzn_sabori 直接実行で比較する（固定シードなので各モード決定論的）。
"""
import os
import re
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent
FZN_SABORI = str(BASE_DIR.parent.parent / "build" / "src" / "fzn" / "fzn_sabori")
TIMEOUT = 30
MAX_WORKERS = 4
MODES = [0, 1, 2, 3, 4]
MIN_LIN_COUNT = 500

PROB_DIRS = [BASE_DIR / f"mznc{y}_probs" for y in (2022, 2023, 2024, 2025)]

# 全モードで 30s 内に解が見つからず比較不能だった問題（初回計測で確認済み）
SKIP = {"2022/blocks-world", "2023/code-generator", "2023/evm-super-compilation",
        "2024/network_50_cstr", "2025/gt-sort"}


def cleanup_stale_processes():
    for name in ("fzn_sabori", "fzn-cp-sat", "minizinc"):
        subprocess.run(["pkill", "-x", name], capture_output=True)
    time.sleep(1)


def discover_problems():
    """int_lin 制約 >= MIN_LIN_COUNT のキャッシュ済み fzn を列挙"""
    probs = []
    for d in PROB_DIRS:
        for fzn in sorted(d.glob("*/*.fzn")):
            cnt = subprocess.run(
                ["grep", "-c", r"int_lin_eq\|int_lin_le\|int_lin_ne", str(fzn)],
                capture_output=True, text=True).stdout.strip()
            cnt = int(cnt) if cnt.isdigit() else 0
            if cnt < MIN_LIN_COUNT:
                continue
            goal = subprocess.run(
                ["grep", "-o", "-m1", r"minimize\|maximize\|satisfy", str(fzn)],
                capture_output=True, text=True).stdout.strip() or "?"
            year = d.name.replace("mznc", "").replace("_probs", "")
            name = f"{year}/{fzn.parent.name}"
            if name in SKIP:
                continue
            probs.append((name, str(fzn), goal, cnt))
    return probs


def run_one(task):
    name, fzn, goal, mode = task
    env = dict(os.environ, SABORI_LIN_ORDER=str(mode))
    t0 = time.monotonic()
    try:
        r = subprocess.run([FZN_SABORI, "-s", "-t", str(TIMEOUT), fzn],
                           capture_output=True, text=True,
                           timeout=TIMEOUT + 30, env=env)
        out = r.stdout + r.stderr
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
    wall = time.monotonic() - t0

    objs = re.findall(r"% obj = (-?\d+)", out) or re.findall(r"objective = (-?\d+)", out)
    obj = int(objs[-1]) if objs else None
    if "=====UNSATISFIABLE=====" in out:
        status = "UNSAT"
    elif "==========" in out:
        status = "COMPLETE"
    elif "=====TIMEOUT=====" in out:
        status = "SAT" if (obj is not None or "----------" in out) else "TIMEOUT"
    elif "----------" in out:
        status = "SAT-END"  # satisfaction: 最初の解で終了
    elif "=====ERROR" in out or "Error" in out:
        status = "ERROR"
    else:
        status = "NOSOLN"
    return name, mode, status, obj, wall


def main():
    import argparse
    global MODES, MAX_WORKERS
    ap = argparse.ArgumentParser()
    ap.add_argument("--modes", default=None, help="comma-separated modes")
    ap.add_argument("--workers", type=int, default=None)
    ap.add_argument("--only", default=None, help="comma-separated substrings of problem names")
    opts = ap.parse_args()
    if opts.modes:
        MODES = [int(x) for x in opts.modes.split(",")]
    if opts.workers:
        MAX_WORKERS = opts.workers
    cleanup_stale_processes()
    probs = discover_problems()
    if opts.only:
        keys = opts.only.split(",")
        probs = [p for p in probs if any(k in p[0] for k in keys)]
    print(f"problems: {len(probs)}, runs: {len(probs) * len(MODES)}")
    tasks = [(name, fzn, goal, m) for (name, fzn, goal, _cnt) in probs for m in MODES]
    results = {}
    with ProcessPoolExecutor(MAX_WORKERS) as ex:
        for name, mode, status, obj, wall in ex.map(run_one, tasks):
            results[(name, mode)] = (status, obj, wall)
            print(f"  done {name} mode={mode}: {status} obj={obj} {wall:.1f}s", flush=True)

    goal_of = {name: goal for (name, _f, goal, _c) in probs}
    print("\n=== 結果 (status/obj/time) ===")
    hdr = "problem".ljust(40) + "goal".ljust(10) + "".join(f"m{m}".ljust(22) for m in MODES)
    print(hdr)
    for (name, _f, goal, cnt) in probs:
        row = name.ljust(40) + goal.ljust(10)
        for m in MODES:
            st, obj, wall = results.get((name, m), ("MISSING", None, 0.0))
            cell = f"{st[:4]} {obj if obj is not None else '-'} {wall:.0f}s"
            row += cell.ljust(22)
        print(row)

    # mode0 を基準に勝敗を付ける
    print("\n=== mode0 比較 (better/worse/same) ===")
    for m in MODES[1:]:
        better = worse = same = 0
        details = []
        for (name, _f, goal, _c) in probs:
            s0, o0, w0 = results[(name, 0)]
            sm, om, wm = results[(name, m)]
            cmp = compare(goal, s0, o0, w0, sm, om, wm)
            if cmp > 0:
                better += 1
                details.append(f"+{name}")
            elif cmp < 0:
                worse += 1
                details.append(f"-{name}")
            else:
                same += 1
        print(f"mode{m}: better={better} worse={worse} same={same}  {' '.join(details)}")


def compare(goal, s0, o0, w0, sm, om, wm):
    """mode m が mode0 より良ければ +1, 悪ければ -1, 同等 0"""
    rank = {"COMPLETE": 3, "UNSAT": 3, "SAT-END": 3, "SAT": 2, "TIMEOUT": 1,
            "NOSOLN": 1, "ERROR": 0, "MISSING": 0}
    r0, rm = rank.get(s0, 0), rank.get(sm, 0)
    if rm != r0:
        return 1 if rm > r0 else -1
    if r0 == 3:  # 両方完了: 時間で比較 (2秒以上の差のみ有意)
        if abs(wm - w0) >= 2.0:
            return 1 if wm < w0 else -1
        return 0
    if o0 is None or om is None:
        return 0
    if om == o0:
        return 0
    if goal == "minimize":
        return 1 if om < o0 else -1
    if goal == "maximize":
        return 1 if om > o0 else -1
    return 0


if __name__ == "__main__":
    main()
