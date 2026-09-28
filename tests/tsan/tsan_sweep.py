#!/usr/bin/env python3
"""TSan ビルドの fzn_sabori で golden コーパスを並列探索させ、データ競合を検出する。

`-DSABORI_SANITIZE=thread` でビルドしたバイナリ専用。各 fzn を並列構成
（既定: ``-j 4`` / ``-j 8`` / ``-j 4`` + ``SABORI_MULTISTART_N=2`` / -j 無しの既定マルチスタート）で解き、

- stderr に ThreadSanitizer のレポートが出た
- 異常終了した（シグナル死、TSan の exitcode、タイムアウト以外の非 0）

のどちらかがあれば失敗とする。解の正しさはここでは見ない（clone の解一致は
test_fzn_corpus ``[clone]``、解の健全性は ``[resolve]`` と golden が担う）。

並列経路を通すため ``-a`` は付けない（SAT の ``-a`` は単スレッド経路になる）。

使い方::

    tests/tsan/tsan_sweep.py --bin build-tsan/src/fzn/fzn_sabori
    tests/tsan/tsan_sweep.py --bin ... --configs j4 --limit 20   # 部分実行
    # golden コーパスは小問題ばかりで探索がすぐ終わる。長く走る経路（bound 共有・
    # ラウンドロビン・リスタート）まで通すにはベンチの fzn キャッシュを使う:
    tests/tsan/tsan_sweep.py --bin ... --dir benchmarks/minizinc_challenge/.fzn_cache_abperf \
        --configs j4 --timeout 10 --jobs 1

注意:
- この環境の TSan は ASLR 有効だと即死するので ``setarch <arch> -R`` 越しに起動する
- TSan は 1 プロセスで十数 GB 使うことがある。``--jobs`` は既定 2、上げすぎないこと
"""

from __future__ import annotations

import argparse
import os
import platform
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parents[2]
CORPUS = ROOT / "tests" / "golden" / "corpus.txt"

#: 構成名 → (追加引数, 追加環境変数)
CONFIGS: Dict[str, Tuple[List[str], Dict[str, str]]] = {
    "j4": (["-j", "4"], {}),
    "j8": (["-j", "8"], {}),
    "j4ms2": (["-j", "4"], {"SABORI_MULTISTART_N": "2"}),
    # -j 無し = 既定の適応的ファンアウト付きマルチスタート（3 スロット）。
    # 予算を 1 にして最初のリスタートで全スロットを有効にし、ターン受け渡しを踏ませる。
    "default": ([], {"SABORI_MULTISTART_ADAPT": "1"}),
}

TSAN_EXITCODE = 66
TSAN_MARKER = "WARNING: ThreadSanitizer"


@dataclass
class RunResult:
    """1 回の実行結果。"""

    fzn: str
    config: str
    returncode: Optional[int]
    reports: int
    wall: float
    stderr_tail: str

    @property
    def failed(self) -> bool:
        """TSan レポートあり、または異常終了なら True。"""
        if self.reports > 0:
            return True
        if self.returncode is None:  # 外側タイムアウト = ハング
            return True
        return self.returncode != 0


def load_corpus() -> List[str]:
    """golden コーパスの fzn 一覧（ROOT 相対）を返す。"""
    lines = CORPUS.read_text().splitlines()
    return [ln.strip() for ln in lines if ln.strip() and not ln.startswith("#")]


def run_one(
    binary: Path, fzn: str, config: str, solve_timeout: int, hard_timeout: int
) -> RunResult:
    """1 fzn × 1 構成を TSan 下で実行する。"""
    args, extra_env = CONFIGS[config]
    cmd = ["setarch", platform.machine(), "-R", str(binary), *args,
           "-t", str(solve_timeout), fzn]
    env = dict(os.environ)
    env.update(extra_env)
    # 既存の TSAN_OPTIONS を尊重しつつ、判定に要る設定を足す
    opts = env.get("TSAN_OPTIONS", "")
    env["TSAN_OPTIONS"] = f"{opts} halt_on_error=0 exitcode={TSAN_EXITCODE}".strip()
    t0 = time.monotonic()
    try:
        proc = subprocess.run(cmd, cwd=ROOT, env=env, stdout=subprocess.DEVNULL,
                              stderr=subprocess.PIPE, text=True, errors="replace",
                              timeout=hard_timeout)
        rc: Optional[int] = proc.returncode
        err = proc.stderr
    except subprocess.TimeoutExpired as e:
        rc = None
        raw = e.stderr or b""
        err = raw.decode(errors="replace") if isinstance(raw, bytes) else raw
    wall = time.monotonic() - t0
    return RunResult(fzn, config, rc, err.count(TSAN_MARKER), wall,
                     "\n".join(err.splitlines()[-40:]))


def main() -> int:
    """エントリポイント。失敗があれば 1 を返す。"""
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", type=Path,
                    default=ROOT / "build-tsan" / "src" / "fzn" / "fzn_sabori",
                    help="TSan ビルドの fzn_sabori")
    ap.add_argument("--configs", default=",".join(CONFIGS),
                    help=f"実行する構成（カンマ区切り、既定 {','.join(CONFIGS)}）")
    ap.add_argument("--jobs", type=int, default=2, help="同時実行数（既定 2）")
    ap.add_argument("--timeout", type=int, default=10,
                    help="fzn_sabori の -t 秒（既定 10）")
    ap.add_argument("--dir", type=Path, default=None,
                    help="コーパスの代わりにこのディレクトリ以下の *.fzn を使う")
    ap.add_argument("--hard-timeout", type=int, default=0,
                    help="1 実行の打ち切り秒（超過はハング扱いで失敗）。"
                         "0 = max(300, 10 × --timeout)。TSan 下は構築が数倍遅い")
    ap.add_argument("--limit", type=int, default=0, help="先頭 N 問だけ（0 = 全部）")
    args = ap.parse_args()

    if not args.bin.is_file():
        print(f"fzn_sabori が見つからない: {args.bin}", file=sys.stderr)
        return 2
    configs = [c for c in args.configs.split(",") if c]
    unknown = [c for c in configs if c not in CONFIGS]
    if unknown:
        print(f"未知の構成: {unknown}（候補 {list(CONFIGS)}）", file=sys.stderr)
        return 2

    if args.dir is not None:
        corpus = sorted(str(p.resolve()) for p in args.dir.rglob("*.fzn"))
        if not corpus:
            print(f"*.fzn が無い: {args.dir}", file=sys.stderr)
            return 2
    else:
        corpus = load_corpus()
    if args.limit > 0:
        corpus = corpus[: args.limit]
    tasks = [(f, c) for c in configs for f in corpus]
    hard_timeout = args.hard_timeout or max(300, 10 * args.timeout)

    print(f"TSan sweep: {len(corpus)} fzn × {configs} = {len(tasks)} runs, "
          f"jobs={args.jobs}, -t {args.timeout}", flush=True)
    t0 = time.monotonic()
    failures: List[RunResult] = []
    done = 0
    with ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = [ex.submit(run_one, args.bin, f, c, args.timeout, hard_timeout)
                for f, c in tasks]
        for fut in futs:
            r = fut.result()
            done += 1
            if r.failed:
                failures.append(r)
                rc = "HARD-TIMEOUT" if r.returncode is None else r.returncode
                print(f"FAIL [{r.config}] {r.fzn}: rc={rc} "
                      f"reports={r.reports} wall={r.wall:.1f}s", flush=True)
            elif done % 50 == 0:
                print(f"  {done}/{len(tasks)} ({time.monotonic() - t0:.0f}s)",
                      flush=True)

    elapsed = time.monotonic() - t0
    for r in failures:
        print(f"\n===== [{r.config}] {r.fzn} (rc={r.returncode}) =====\n{r.stderr_tail}")
    print(f"\n{len(tasks)} runs, {len(failures)} failed, {elapsed:.0f}s")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
