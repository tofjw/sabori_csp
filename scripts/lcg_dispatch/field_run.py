#!/usr/bin/env python3
"""任意のフィールドソルバーを bench3 と同条件で実行し <name>.tsv 出力。
Usage: python3 field_run.py <solver_id> <threads:0=既定> <short_name> [years...]
例: python3 field_run.py gecode 0 gecode 2022 2023 2024 2025
"""
import os, re, signal, subprocess, sys, time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

CH = Path('/home/tofjw/develop/cp/sabori_csp/benchmarks/minizinc_challenge')
MINIZINC = '/snap/bin/minizinc'
TIMEOUT = 60

def nat(s):
    return [int(t) if t.isdigit() else t.lower() for t in re.split('([0-9]+)', str(s))]

def detect_type(mzn):
    txt = '\n'.join(l for l in Path(mzn).read_text().splitlines() if not l.strip().startswith('%'))
    if re.search(r'\bminimize\b', txt): return 'MIN'
    if re.search(r'\bmaximize\b', txt): return 'MAX'
    return 'SAT'

def find_instance(prob_dir):
    """INST=large でファイルサイズ最大インスタンス、既定は最小(nat順先頭)。"""
    import os as _os
    mzns = sorted(Path(prob_dir).glob('*.mzn'), key=nat)
    if not mzns:
        return None
    datas = sorted(Path(prob_dir).glob('*.dzn'), key=nat) + \
        sorted(Path(prob_dir).glob('*.json'), key=nat)
    if len(mzns) > 1 and not datas:
        return str(mzns[0]), None
    if not datas:
        return str(mzns[0]), None
    if _os.environ.get('INST') == 'large':
        data = max(datas, key=lambda p: p.stat().st_size)
    else:
        data = datas[0]
    return str(mzns[0]), str(data)

SOLVER = sys.argv[1]
THREADS = int(sys.argv[2])
NAME = sys.argv[3]
WORKERS = 4 if THREADS <= 1 else 2
OUT = Path(__file__).with_name(f'{NAME}.tsv')

def run(mzn, data, ptype):
    mzn_arg = str(Path(mzn).resolve().relative_to(CH))
    data_arg = str(Path(data).resolve().relative_to(CH)) if data else None
    cmd = [MINIZINC, '--solver', SOLVER, '-t', str(TIMEOUT*1000)]
    if THREADS > 1: cmd += ['-p', str(THREADS)]
    if ptype != 'SAT': cmd.append('-a')
    cmd += ['--output-objective', '--output-mode', 'dzn', mzn_arg]
    if data_arg: cmd.append(data_arg)
    start = time.monotonic(); is_to = False
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, start_new_session=True, cwd=str(CH))
    try:
        out, _ = proc.communicate(timeout=TIMEOUT+45)
    except subprocess.TimeoutExpired:
        try: os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except OSError: pass
        out, _ = proc.communicate(); is_to = True
    el = time.monotonic() - start
    if '==========\n' in out: st = 'OPTIMAL'
    elif '=====UNSATISFIABLE=====' in out: st = 'UNSAT'
    elif '----------\n' in out: st = 'SOL'
    elif is_to or el >= TIMEOUT*0.9: st = 'TIMEOUT'
    else: st = 'UNKNOWN'
    obj = None
    for line in reversed(out.split('\n')):
        m = re.search(r'_objective\s*=\s*(-?\d+)', line)
        if m: obj = int(m.group(1)); break
    return st, round(el, 2), obj

def process(task):
    prob, mzn, data, ptype = task
    return (prob, ptype) + run(mzn, data, ptype)

def main():
    years = sys.argv[4:] or ['2022','2023','2024','2025']
    for n in ['fzn_sabori','fzn-cp-sat','minizinc','fzn-gecode']:
        subprocess.run(['pkill','-x',n], capture_output=True)
    time.sleep(0.5)
    tasks = []
    for y in years:
        for d in sorted((CH/f'mznc{y}_probs').glob('*/'), key=nat):
            inst = find_instance(d)
            if inst:
                mzn, data = inst
                tasks.append((f'{y}/{d.name.rstrip("/")}', mzn, data, detect_type(mzn)))
    print(f"{len(tasks)} problems, solver={SOLVER} threads={THREADS} workers={WORKERS} timeout={TIMEOUT}s")
    with open(OUT, 'w') as fo:
        fo.write('problem\ttype\tstatus\ttime\tobj\n')
        with ProcessPoolExecutor(WORKERS) as ex:
            for prob, ptype, st, el, obj in ex.map(process, tasks):
                fo.write(f"{prob}\t{ptype}\t{st}\t{el}\t{obj}\n")
                print(f"{prob:<34}{ptype:<4} {NAME}:{st}/{el}s obj={obj}", flush=True)
    print(f"wrote {OUT}")

if __name__ == '__main__':
    main()
