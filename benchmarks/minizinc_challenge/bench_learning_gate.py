#!/usr/bin/env python3
"""conflict learning (-L) の全年度ゲート: Sabori base vs Sabori -L

同一バイナリで -L あり/なしを minizinc 経由・各30秒で比較する。
判定: status (OPTIMAL/UNSAT > SOL > TIMEOUT/UNKNOWN) → obj (MIN/MAX) →
完了時間 (差2秒以上のみ有意)。

Usage:
    python3 bench_learning_gate.py            # 全年度
    python3 bench_learning_gate.py 2024 2025  # 指定年
"""
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from datetime import datetime
from pathlib import Path

import bench_compare as bc

BASE_DIR = Path(__file__).resolve().parent
LEARN_MSC = "/tmp/sabori_csp_learn.msc"
ALL_YEARS = ["2010", "2012", "2013", "2014", "2015", "2016", "2017", "2018",
             "2019", "2020", "2021", "2022", "2023", "2024", "2025"]


def run_one(task):
    tag, prob, solver_id, mzn, data, ptype = task
    r = bc.run_solver(prob, "Sabori", solver_id, mzn, data, ptype)
    # r = (problem, solver_name, status, elapsed, obj, err, data_label, check)
    return (tag, r[0], r[2], r[3], r[4], r[5], r[7])


def compare(ptype, sb, sl):
    """base vs -L: +1 = -L が良い, -1 = 悪い, 0 = 同等"""
    rank = {"OPTIMAL": 3, "UNSAT": 3, "SOL": 2,
            "TIMEOUT": 1, "UNKNOWN": 1, "ERROR": 0, "MISSING": 0}
    (st_b, t_b, o_b), (st_l, t_l, o_l) = sb, sl
    rb, rl = rank.get(st_b, 0), rank.get(st_l, 0)
    if rl != rb:
        return 1 if rl > rb else -1
    if rb == 3:
        if abs(t_l - t_b) >= 2.0:
            return 1 if t_l < t_b else -1
        return 0
    if o_b is None or o_l is None or o_b == o_l:
        return 0
    if ptype == "MIN":
        return 1 if o_l < o_b else -1
    if ptype == "MAX":
        return 1 if o_l > o_b else -1
    return 0


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("years", nargs="*", default=[])
    ap.add_argument("--inst", type=int, default=0,
                    help="使うデータファイルの番号 (0=最小=既定、1=第2 = holdout 検証用)")
    opts = ap.parse_args()
    years = opts.years or ALL_YEARS
    bc.cleanup_stale_processes()

    out_dir = BASE_DIR / f"{datetime.now():%Y%m%d}_learning_gate"
    out_dir.mkdir(exist_ok=True)
    out_lines = []

    def emit(line=""):
        print(line, flush=True)
        out_lines.append(line)

    tasks = []
    prob_types = {}
    for year in years:
        probs_dir = BASE_DIR / f"mznc{year}_probs"
        if not probs_dir.exists():
            emit(f"WARNING: {probs_dir} not found, skipping {year}")
            continue
        for p in sorted(probs_dir.iterdir(), key=bc.natural_sort_key):
            if not p.is_dir():
                continue
            if opts.inst == 0:
                instances = bc.find_instances(p)
                if not instances:
                    continue
                mzn, data, _label = instances[0]
            else:
                # holdout: 第 (inst+1) データファイル。なければスキップ
                mzns = sorted(p.glob("*.mzn"), key=bc.natural_sort_key)
                datas = (sorted(p.glob("*.dzn"), key=bc.natural_sort_key)
                         + sorted(p.glob("*.json"), key=bc.natural_sort_key))
                if mzns and len(datas) > opts.inst:
                    mzn, data = str(mzns[0]), str(datas[opts.inst])
                elif len(mzns) > opts.inst and not datas:
                    mzn, data = str(mzns[opts.inst]), None
                else:
                    continue
            ptype = bc.detect_problem_type(mzn)
            key = f"{year}/{p.name}"
            prob_types[key] = ptype
            tasks.append(("base", key, bc.SABORI_MSC, mzn, data, ptype))
            tasks.append(("learn", key, LEARN_MSC, mzn, data, ptype))

    emit(f"learning gate: {len(tasks) // 2} problems, {len(tasks)} runs, "
         f"{bc.MAX_WORKERS} workers, timeout {bc.TIMEOUT}s")
    t0 = time.monotonic()

    results = {}
    done = 0
    with ProcessPoolExecutor(max_workers=bc.MAX_WORKERS) as ex:
        futures = {ex.submit(run_one, t): t for t in tasks}
        for fut in as_completed(futures):
            tag, prob, status, elapsed, obj, err, check = fut.result()
            results[(prob, tag)] = (status, elapsed, obj, check)
            done += 1
            print(f"  [{done}/{len(tasks)}] {prob} {tag}: {status} obj={obj} "
                  f"{elapsed:.1f}s check={check}", flush=True)

    emit(f"\ntotal wall: {(time.monotonic() - t0) / 60:.1f} min")

    # ---- 結果表 ----
    emit("\n" + "=" * 100)
    emit(f"{'problem':<45}{'type':<6}{'base':<25}{'-L':<25}{'judge'}")
    emit("=" * 100)
    wins = losses = ties = 0
    check_fails = []
    year_stats = {}
    for key in sorted(prob_types, key=bc.natural_sort_key):
        ptype = prob_types[key]
        sb = results.get((key, "base"), ("MISSING", 0.0, None, None))
        sl = results.get((key, "learn"), ("MISSING", 0.0, None, None))
        for tag, r in (("base", sb), ("learn", sl)):
            if r[3] is False:
                check_fails.append(f"{key} [{tag}]")
        c = compare(ptype, (sb[0], sb[1], sb[2]), (sl[0], sl[1], sl[2]))
        judge = {1: "LEARN+", -1: "LEARN-", 0: ""}[c]
        wins += c == 1
        losses += c == -1
        ties += c == 0
        y = key.split("/")[0]
        ys = year_stats.setdefault(y, [0, 0, 0])
        ys[0 if c == 1 else (1 if c == -1 else 2)] += 1

        def cell(r):
            o = r[2] if r[2] is not None else "-"
            return f"{r[0][:7]} {o} {r[1]:.0f}s"
        emit(f"{key:<45}{ptype:<6}{cell(sb):<25}{cell(sl):<25}{judge}")

    emit("\n=== 年別 (LEARN+ / LEARN- / tie) ===")
    for y in sorted(year_stats):
        w, l, t = year_stats[y]
        emit(f"  {y}: +{w} / -{l} / ={t}")
    emit(f"\n=== 合計: -L wins={wins} losses={losses} ties={ties} ===")
    if check_fails:
        emit(f"\n!!! solution check FAILED: {check_fails}")
    else:
        emit("\nsolution checks: all passed (or skipped)")

    # 健全性: 片側が解を発見し他方が UNSAT を主張する矛盾を検出
    contradictions = []
    for key in prob_types:
        sb = results.get((key, "base"), ("MISSING",))[0]
        sl = results.get((key, "learn"), ("MISSING",))[0]
        if {sb, sl} & {"SOL", "OPTIMAL"} and "UNSAT" in (sb, sl):
            contradictions.append(f"{key} base={sb} learn={sl}")
    if contradictions:
        emit(f"\n!!! SOUNDNESS: SOL/UNSAT 矛盾: {contradictions}")
    else:
        emit("soundness: no SOL/UNSAT contradictions")

    out_path = out_dir / (f"results_inst{opts.inst}.txt" if opts.inst else "results.txt")
    out_path.write_text("\n".join(out_lines) + "\n")
    print(f"\nsaved: {out_path}")


if __name__ == "__main__":
    main()
