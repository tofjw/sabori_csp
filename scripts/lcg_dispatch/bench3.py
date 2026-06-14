#!/usr/bin/env python3
"""3 構成の速度比較: main / branch+L / branch base.

各問題を minizinc 経由で 3 構成実行し、status/time/obj を記録。
winner 判定: status rank (UNKNOWN<SOL<OPTIMAL/UNSAT) → obj 質 → time。
構成間 SOL↔UNSAT 矛盾は健全性赤旗として明示。

Usage: python3 bench3.py 2022 2023 2024 2025
"""
import os
import re
import signal
import subprocess
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

CH = Path('/home/tofjw/develop/cp/sabori_csp/benchmarks/minizinc_challenge')
MINIZINC = '/snap/bin/minizinc'
TIMEOUT = 60
WORKERS = 4
CONFIGS = [
    ('main', '/tmp/msc-main.msc'),
    ('base', '/home/tofjw/develop/cp/sabori_csp/build/share/minizinc/solvers/sabori_csp.msc'),
    ('lcg', '/tmp/sabori_csp_learn.msc'),
]
STAT_RANK = {'TIMEOUT': 0, 'UNKNOWN': 0, 'ERROR': 0, 'SOL': 1, 'UNSAT': 2, 'OPTIMAL': 2}


def nat(s):
    return [int(t) if t.isdigit() else t.lower() for t in re.split('([0-9]+)', str(s))]


def cleanup():
    for n in ['fzn_sabori', 'fzn-cp-sat', 'minizinc']:
        subprocess.run(['pkill', '-x', n], capture_output=True)
    time.sleep(0.5)


def kill_tree(proc):
    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
    except OSError:
        pass


def detect_type(mzn):
    txt = '\n'.join(l for l in Path(mzn).read_text().splitlines()
                    if not l.strip().startswith('%'))
    if re.search(r'\bminimize\b', txt):
        return 'MIN'
    if re.search(r'\bmaximize\b', txt):
        return 'MAX'
    return 'SAT'


def find_instance(prob_dir):
    mzns = sorted(Path(prob_dir).glob('*.mzn'), key=nat)
    if not mzns:
        return None
    datas = sorted(Path(prob_dir).glob('*.dzn'), key=nat) + \
        sorted(Path(prob_dir).glob('*.json'), key=nat)
    if len(mzns) > 1 and not datas:
        return str(mzns[0]), None
    return str(mzns[0]), (str(datas[0]) if datas else None)


def run(msc, mzn, data, ptype):
    mzn_arg = str(Path(mzn).resolve().relative_to(CH))
    data_arg = str(Path(data).resolve().relative_to(CH)) if data else None
    cmd = [MINIZINC, '--solver', msc, '-t', str(TIMEOUT * 1000)]
    if ptype != 'SAT':
        cmd.append('-a')
    cmd += ['--output-objective', '--output-mode', 'dzn', mzn_arg]
    if data_arg:
        cmd.append(data_arg)
    start = time.monotonic()
    is_to = False
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, start_new_session=True, cwd=str(CH))
    try:
        out, _ = proc.communicate(timeout=TIMEOUT + 15)
    except subprocess.TimeoutExpired:
        kill_tree(proc)
        out, _ = proc.communicate()
        is_to = True
    el = time.monotonic() - start
    if '==========\n' in out:
        st = 'OPTIMAL'
    elif '=====UNSATISFIABLE=====' in out:
        st = 'UNSAT'
    elif '----------\n' in out:
        st = 'SOL'
    elif is_to or el >= TIMEOUT * 0.9:
        st = 'TIMEOUT'
    else:
        st = 'UNKNOWN'
    obj = None
    for line in reversed(out.split('\n')):
        m = re.search(r'_objective\s*=\s*(-?\d+)', line)
        if m:
            obj = int(m.group(1))
            break
    return st, round(el, 2), obj


def process(task):
    prob, mzn, data, ptype = task
    res = {}
    for cname, msc in CONFIGS:
        res[cname] = run(msc, mzn, data, ptype)
    return prob, ptype, res


def better(ptype, a, b):
    """a が b より良ければ +1, 悪ければ -1, 互角 0。(status→obj→time)"""
    sa, ta, oa = a
    sb, tb, ob = b
    ra, rb = STAT_RANK[sa], STAT_RANK[sb]
    if ra != rb:
        return 1 if ra > rb else -1
    if oa is not None and ob is not None and oa != ob:
        if ptype == 'MAX':
            return 1 if oa > ob else -1
        return 1 if oa < ob else -1
    # 同 status・同 obj → 速い方 (有意差 >0.5s のみ)
    if abs(ta - tb) > 0.5:
        return 1 if ta < tb else -1
    return 0


def main():
    years = sys.argv[1:] or ['2022', '2023', '2024', '2025']
    cleanup()
    tasks = []
    for y in years:
        for d in sorted((CH / f'mznc{y}_probs').glob('*/'), key=nat):
            inst = find_instance(d)
            if inst:
                mzn, data = inst
                tasks.append((f'{y}/{d.name.rstrip("/")}', mzn, data, detect_type(mzn)))
    print(f"{len(tasks)} problems x 3 configs, timeout={TIMEOUT}s, workers={WORKERS}\n")
    results = []
    with ProcessPoolExecutor(WORKERS) as ex:
        for prob, ptype, res in ex.map(process, tasks):
            results.append((prob, ptype, res))
            cells = '  '.join(f"{c}:{res[c][0]}/{res[c][1]}s" for c, _ in CONFIGS)
            print(f"{prob:<34}{ptype:<4} {cells}", flush=True)

    # 集計
    print("\n" + "=" * 90)
    names = [c for c, _ in CONFIGS]
    wins = {c: 0 for c in names}          # 単独最良
    contradictions = []
    solved = {c: 0 for c in names}        # SOL/OPTIMAL/UNSAT 到達
    opt = {c: 0 for c in names}
    proof_time = {c: 0.0 for c in names}  # 全構成 OPTIMAL の問題での合計証明時間
    all_opt_probs = 0
    for prob, ptype, res in results:
        sts = {c: res[c][0] for c in names}
        for c in names:
            if sts[c] in ('SOL', 'OPTIMAL', 'UNSAT'):
                solved[c] += 1
            if sts[c] in ('OPTIMAL', 'UNSAT'):
                opt[c] += 1
        # 健全性: ある構成 UNSAT, 別構成 SOL/OPTIMAL は矛盾
        has_unsat = any(sts[c] == 'UNSAT' for c in names)
        has_sol = any(sts[c] in ('SOL', 'OPTIMAL') for c in names)
        if has_unsat and has_sol:
            contradictions.append((prob, sts))
        # 単独最良の判定
        best = []
        for c in names:
            dominates = all(better(ptype, res[c], res[o]) >= 0 for o in names)
            strictly = any(better(ptype, res[c], res[o]) > 0 for o in names)
            if dominates and strictly:
                best.append(c)
        if len(best) == 1:
            wins[best[0]] += 1
        # 全構成 OPTIMAL なら証明時間を比較
        if all(sts[c] == 'OPTIMAL' for c in names):
            all_opt_probs += 1
            for c in names:
                proof_time[c] += res[c][1]

    print(f"problems: {len(results)}\n")
    print(f"{'config':<8}{'solved':>8}{'opt/unsat':>11}{'sole-best':>11}")
    for c in names:
        print(f"{c:<8}{solved[c]:>8}{opt[c]:>11}{wins[c]:>11}")
    print(f"\n全構成 OPTIMAL の {all_opt_probs} 問での合計証明時間 (短いほど速い):")
    for c in names:
        print(f"  {c:<8}{proof_time[c]:>8.1f}s")
    if contradictions:
        print(f"\n!!! 健全性矛盾 (SOL vs UNSAT) {len(contradictions)} 件 !!!")
        for prob, sts in contradictions:
            print(f"  {prob}: {sts}")
    else:
        print("\n健全性矛盾なし (SOL↔UNSAT)")


if __name__ == '__main__':
    main()
