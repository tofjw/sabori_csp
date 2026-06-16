#!/usr/bin/env python3
"""one_hot_channel (IntOneHotChannel) 集約が発火する問題に絞った
before/after A/B 性能ベンチマーク。

目的:
    TODO「n_bids カウンタのトレイル化 (one_hot_channel)」の効果測定。
    `IntOneHotChannelConstraint::bump_activity` の n_bids 計算を
    O(N) ループ → トレイル付きカウンタ参照に置き換えた変更 (commit 0fcd895) の
    定数倍効果を、同一探索木上で時間比較する。

なぜ before/after の 2 バイナリか:
    bump_activity が計算する n_bids 値は両実装で完全に一致する（カウンタは
    ループ結果と等価）ため、変数 activity → 変数選択 → 探索木は不変。
    つまり結果(obj/status)は一致し、差は実行時間のみに現れる。これを A/B で測る。

なぜ対象問題の「検出」が要るか:
    one_hot_channel は FlatZinc に明示的には現れず、fzn_sabori の presolve 内
    OneHotChannelAggregator が定数値 int_eq_reif 群を実行時に集約して初めて生成される。
    そのため MZN/FZN の静的 grep では判定できない。短時間 `-v` 実行で stderr の
        % [verbose] OneHotChannelAggregator: N int_eq_reif -> M IntOneHotChannel
    を拾い、M>0 を対象とする。

実行経路:
    各問題を minizinc で FZN にコンパイル（redefinitions 適用済み・サポート制約のみ）
    してキャッシュし、その FZN を fzn_sabori で直接実行する。flatten ノイズを排除し
    定数倍差をクリーンに測る。

使い方:
    python3 bench_one_hot_channel.py                 # 全年スキャン → 検出 → A/B
    python3 bench_one_hot_channel.py --years 2022 2023 2024 2025
    python3 bench_one_hot_channel.py --timeout 60 --limit 20
    SABORI_BIN_BEFORE=/path SABORI_BIN_AFTER=/path python3 bench_one_hot_channel.py
"""
import argparse
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
    detect_problem_type,
)

REPO_ROOT = BASE_DIR.parent.parent

# 既定バイナリ: after=現行 HEAD (build/)、before=worktree (.wt-before/build/)。
# 環境変数で差し替え可能。
BIN_AFTER = os.environ.get(
    "SABORI_BIN_AFTER", str(REPO_ROOT / "build" / "src" / "fzn" / "fzn_sabori")
)
BIN_BEFORE = os.environ.get(
    "SABORI_BIN_BEFORE",
    str(REPO_ROOT / ".wt-before" / "build" / "src" / "fzn" / "fzn_sabori"),
)

FZN_CACHE = BASE_DIR / ".fzn_cache_one_hot"
COMPILE_TIMEOUT = 90  # minizinc flatten 上限
DETECT_TIMEOUT = 5    # aggregator は presolve で発火するので短時間で十分
TIMEOUT = 30          # 各ソルバ実行の制限時間
DETECT_WORKERS = 4

# OneHotChannelAggregator の verbose 行: "... -> M IntOneHotChannel"
AGG_RE = re.compile(r"->\s*(\d+)\s+IntOneHotChannel")
# solve 行から目的変数と方向を抽出（minimize/maximize VAR）
SOLVE_RE = re.compile(r"\b(minimize|maximize)\s+([A-Za-z_][A-Za-z0-9_]*)")
# -s の "% Stats: fails=N restarts=N ..." 行（探索量の説明指標）
FAILS_RE = re.compile(r"fails=(\d+)")
RESTART_RE = re.compile(r"restarts=(\d+)")


def prob_key(year, name):
    return f"{year}__{name}"


def compile_fzn(year, name, mzn, data):
    """MZN を FZN にコンパイルしてキャッシュ。パスを返す（失敗時 None）。"""
    FZN_CACHE.mkdir(exist_ok=True)
    out = FZN_CACHE / f"{prob_key(year, name)}.fzn"
    if out.exists() and out.stat().st_size > 0:
        return str(out)
    cmd = [MINIZINC, "-c", "--solver", SABORI_MSC, "--fzn", str(out), mzn]
    if data:
        cmd.append(data)
    proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        start_new_session=True,
    )
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


def detect_one_hot(fzn_path, detect_timeout):
    """fzn_sabori -v を短時間実行し、集約された IntOneHotChannel 数を返す。"""
    cmd = [BIN_AFTER, "-v", "-t", str(detect_timeout), fzn_path]
    proc = subprocess.Popen(
        cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
        start_new_session=True,
    )
    try:
        _, stderr = proc.communicate(timeout=detect_timeout + 10)
    except subprocess.TimeoutExpired:
        kill_process_tree(proc)
        _, stderr = proc.communicate()
    m = AGG_RE.search(stderr or "")
    return int(m.group(1)) if m else 0


def get_objective_var(fzn_path):
    """FZN の solve 行から (obj_var, direction) を返す。satisfy なら (None, None)。"""
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
    if "=====TIMEOUT=====" in stdout:
        return "TIMEOUT"
    if "=====UNKNOWN=====" in stdout:
        return "UNKNOWN"
    if "==========" in stdout:
        return "OPTIMAL"
    if "----------" in stdout:
        return "SOL"
    return "UNKNOWN"


def run_bin(bin_path, fzn_path, timeout, obj_var):
    """fzn_sabori を直接実行。(status, elapsed, obj, fails, restarts) を返す。"""
    cmd = [bin_path, "-s", "-t", str(timeout), fzn_path]
    start = time.monotonic()
    proc = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        start_new_session=True,
    )
    try:
        stdout, stderr = proc.communicate(timeout=timeout + 15)
    except subprocess.TimeoutExpired:
        kill_process_tree(proc)
        stdout, stderr = proc.communicate()
    elapsed = time.monotonic() - start

    status = parse_status(stdout)
    obj = None
    if obj_var:
        objs = re.findall(rf"\b{re.escape(obj_var)}\s*=\s*(-?\d+)", stdout)
        obj = int(objs[-1]) if objs else None
    fm = FAILS_RE.search(stderr or "")
    rm = RESTART_RE.search(stderr or "")
    fails = int(fm.group(1)) if fm else None
    restarts = int(rm.group(1)) if rm else None
    return status, elapsed, obj, fails, restarts


def gather_candidates(years):
    """対象年の各問題の先頭インスタンスを (year, name, mzn, data) で返す。"""
    cands = []
    for year in years:
        probs_dir = PROB_DIRS.get(year)
        if probs_dir is None or not probs_dir.exists():
            print(f"WARNING: year {year} dir missing, skip", file=sys.stderr)
            continue
        for p in sorted(probs_dir.iterdir(), key=lambda x: natural_sort_key(x.name)):
            if not p.is_dir():
                continue
            inst = find_instances(p)
            if not inst:
                continue
            mzn, data, _ = inst[0]
            cands.append((year, p.name, mzn, data))
    return cands


def main():
    ap = argparse.ArgumentParser(description="one_hot_channel before/after A/B benchmark")
    ap.add_argument("--years", nargs="+", default=sorted(PROB_DIRS.keys()),
                    help="対象年 (既定: 全年)")
    ap.add_argument("--timeout", type=int, default=TIMEOUT, help="各実行の制限秒")
    ap.add_argument("--detect-timeout", type=int, default=DETECT_TIMEOUT,
                    help="検出実行の制限秒")
    ap.add_argument("--limit", type=int, default=0,
                    help="検出後に先頭 N 問だけベンチ (0=全件)")
    ap.add_argument("--rescan", action="store_true",
                    help="FZN キャッシュを無視して再コンパイル")
    args = ap.parse_args()
    detect_timeout = args.detect_timeout

    for label, b in (("before", BIN_BEFORE), ("after", BIN_AFTER)):
        if not Path(b).exists():
            print(f"ERROR: {label} binary not found: {b}", file=sys.stderr)
            sys.exit(1)
    print(f"before binary: {BIN_BEFORE}")
    print(f"after  binary: {BIN_AFTER}")

    if args.rescan and FZN_CACHE.exists():
        for f in FZN_CACHE.glob("*.fzn"):
            f.unlink()

    cleanup_stale_processes()

    cands = gather_candidates(args.years)
    print(f"\n=== Phase 1: compile {len(cands)} problems to FZN (cached) ===")
    compiled = []  # (year, name, fzn)
    with ProcessPoolExecutor(max_workers=DETECT_WORKERS) as ex:
        futs = {ex.submit(compile_fzn, y, n, m, d): (y, n)
                for (y, n, m, d) in cands}
        for fut in as_completed(futs):
            y, n = futs[fut]
            fzn = fut.result()
            if fzn:
                compiled.append((y, n, fzn))
            else:
                print(f"  [{y}] {n}: compile failed/timeout, skip")
    print(f"  compiled {len(compiled)}/{len(cands)}")

    print(f"\n=== Phase 2: detect IntOneHotChannel (fzn_sabori -v -t {detect_timeout}) ===")
    targets = []  # (year, name, fzn, count)
    with ProcessPoolExecutor(max_workers=DETECT_WORKERS) as ex:
        futs = {ex.submit(detect_one_hot, fzn, detect_timeout): (y, n, fzn)
                for (y, n, fzn) in compiled}
        for fut in as_completed(futs):
            y, n, fzn = futs[fut]
            count = fut.result()
            if count > 0:
                targets.append((y, n, fzn, count))

    targets.sort(key=lambda t: (t[0], natural_sort_key(t[1])))
    print(f"\nFound {len(targets)} problems that generate IntOneHotChannel:")
    for y, n, _, c in targets:
        print(f"  [{y}] {n:35s} channels={c}")
    if not targets:
        print("No target problems. Nothing to benchmark.")
        return

    if args.limit > 0:
        targets = targets[: args.limit]
        print(f"\n(limited to first {len(targets)} problems)")

    print(f"\n=== Phase 3: A/B benchmark (timeout={args.timeout}s, sequential) ===")
    print("=" * 100)
    results = []
    for y, n, fzn, c in targets:
        ptype = detect_problem_type(
            str(next(iter(find_instances(PROB_DIRS[y] / n)))[0])
        ) if (PROB_DIRS[y] / n).exists() else "?"
        obj_var, _ = get_objective_var(fzn)
        # before → after の順で同一プロセス資源条件下に逐次実行
        b_status, b_time, b_obj, b_fails, b_rst = run_bin(BIN_BEFORE, fzn, args.timeout, obj_var)
        a_status, a_time, a_obj, a_fails, a_rst = run_bin(BIN_AFTER, fzn, args.timeout, obj_var)

        # 健全性: 同一探索木のはず → obj/status/fails 一致を確認
        mismatch = ""
        if b_obj != a_obj or b_status != a_status:
            mismatch = "  <<< MISMATCH (status/obj differ!)"

        speed = ""
        if a_time > 0:
            speed = f"x{b_time / a_time:.2f}"

        results.append({
            "year": y, "name": n, "ptype": ptype, "channels": c,
            "b_status": b_status, "b_time": b_time, "b_obj": b_obj,
            "a_status": a_status, "a_time": a_time, "a_obj": a_obj,
            "fails": a_fails, "restarts": a_rst,
            "speed": speed, "mismatch": bool(mismatch),
        })
        print(f"[{y}] {n:30s} ({ptype}) ch={c:<4d} fails={a_fails} | "
              f"before {b_status:8s} {b_time:6.2f}s obj={b_obj} | "
              f"after {a_status:8s} {a_time:6.2f}s obj={a_obj} | {speed}{mismatch}")

    # サマリ
    print("\n" + "=" * 100)
    print("Summary (before O(N)-loop  vs  after trailed-counter)")
    print("=" * 100)
    print(f"{'Problem':<34s}{'ch':>5s}{'fails':>10s}"
          f"{'before(s)':>11s}{'after(s)':>10s}{'speedup':>9s}{'obj==':>7s}")
    print("-" * 100)
    tot_b = tot_a = 0.0
    n_faster = n_slower = n_mismatch = 0
    for r in results:
        same = "OK" if not r["mismatch"] else "DIFF"
        if r["mismatch"]:
            n_mismatch += 1
        tot_b += r["b_time"]
        tot_a += r["a_time"]
        if r["a_time"] < r["b_time"]:
            n_faster += 1
        elif r["a_time"] > r["b_time"]:
            n_slower += 1
        label = f"[{r['year']}] {r['name']}"
        print(f"{label:<34s}{r['channels']:>5d}{str(r['fails']):>10s}"
              f"{r['b_time']:>11.2f}{r['a_time']:>10.2f}{r['speed']:>9s}{same:>7s}")
    print("-" * 100)
    print(f"Total before={tot_b:.2f}s  after={tot_a:.2f}s  "
          f"overall speedup x{(tot_b / tot_a):.3f}" if tot_a > 0 else "")
    print(f"after faster: {n_faster}  slower: {n_slower}  mismatches: {n_mismatch}")

    # TIMEOUT 問題は both=timeout 秒で構造的に同値 → timing 信号ゼロ。
    # 完走（OPTIMAL/SOL/UNSAT）かつ before が制限時間に達していない問題のみで
    # 真の timing 比較を再集計する。
    done = [r for r in results
            if r["b_status"] in ("OPTIMAL", "SOL", "UNSAT")
            and r["b_time"] < timeout * 0.95]
    cb = sum(r["b_time"] for r in done)
    ca = sum(r["a_time"] for r in done)
    df = sum(1 for r in done if r["a_time"] < r["b_time"])
    ds = sum(1 for r in done if r["a_time"] > r["b_time"])
    print("-" * 100)
    print(f"[completed-only] {len(done)} problems with real timing signal "
          f"(TIMEOUT 問題を除外):")
    if ca > 0:
        print(f"  before={cb:.2f}s  after={ca:.2f}s  speedup x{(cb / ca):.3f}  "
              f"(after faster {df} / slower {ds})")
    if n_mismatch:
        print("WARNING: status/obj mismatch detected — 探索木が一致していない可能性。"
              "トレイル化に健全性バグがあるかもしれない。")

    write_html(results, tot_b, tot_a, args.timeout)


def write_html(results, tot_b, tot_a, timeout):
    now = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    out = BASE_DIR / f"sabori_benchmark_one_hot_ab_{ts}.html"
    rows = []
    for r in results:
        cls = "mismatch" if r["mismatch"] else ""
        speed_cls = ""
        if r["a_time"] and r["b_time"]:
            speed_cls = "faster" if r["a_time"] < r["b_time"] else (
                "slower" if r["a_time"] > r["b_time"] else "")
        rows.append(f"""
        <tr class="{cls}">
            <td>[{r['year']}] {r['name']} ({r['ptype']})</td>
            <td>{r['channels']}</td>
            <td>{r['fails']}</td>
            <td>{r['b_status']} {r['b_time']:.2f}s (obj={r['b_obj']})</td>
            <td>{r['a_status']} {r['a_time']:.2f}s (obj={r['a_obj']})</td>
            <td class="{speed_cls}">{r['speed']}</td>
            <td>{'DIFF' if r['mismatch'] else 'OK'}</td>
        </tr>""")
    overall = f"x{(tot_b / tot_a):.3f}" if tot_a > 0 else "-"
    html = f"""<!DOCTYPE html>
<html lang="ja"><head><meta charset="UTF-8">
<title>one_hot_channel trail before/after A/B</title>
<style>
 body{{font-family:-apple-system,Segoe UI,Roboto,sans-serif;max-width:1200px;margin:0 auto;padding:20px;background:#f5f5f5;}}
 h1{{color:#333;border-bottom:2px solid #4285f4;padding-bottom:10px;}}
 .meta{{color:#666;font-size:.9em;margin-bottom:20px;}}
 table{{width:100%;border-collapse:collapse;background:#fff;border-radius:8px;overflow:hidden;box-shadow:0 2px 4px rgba(0,0,0,.1);}}
 th,td{{padding:10px 14px;text-align:left;border-bottom:1px solid #eee;}}
 th{{background:#4285f4;color:#fff;}}
 tr:hover{{background:#f8f9fa;}}
 .faster{{color:#28a745;font-weight:bold;}}
 .slower{{color:#dc3545;font-weight:bold;}}
 tr.mismatch{{background:#fff3cd;}}
</style></head><body>
<h1>one_hot_channel: n_bids トレイル化 before/after</h1>
<div class="meta">Generated: {now} | Timeout: {timeout}s | Problems: {len(results)}<br>
 before = O(N) ループ版 (commit 0fcd895 の親相当) / after = トレイル付きカウンタ版 (現 HEAD)<br>
 Total before={tot_b:.2f}s / after={tot_a:.2f}s / overall speedup {overall}</div>
<table><thead><tr>
 <th>Problem</th><th>channels</th><th>fails(after)</th>
 <th>before</th><th>after</th><th>speedup</th><th>obj==</th>
</tr></thead><tbody>{''.join(rows)}</tbody></table>
</body></html>"""
    out.write_text(html)
    print(f"\nHTML report saved to: {out}")


if __name__ == "__main__":
    main()
