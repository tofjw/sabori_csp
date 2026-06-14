#!/usr/bin/env python3
"""sabori base + lcg を obj 込みで再取得 (bench3 は obj 未保存だったため)。
sabori は cwd 固定で決定論的なので status/obj は bench3 と一致する。
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
    ('base', '/home/tofjw/develop/cp/sabori_csp/build/share/minizinc/solvers/sabori_csp.msc'),
    ('lcg', '/tmp/sabori_csp_learn.msc'),
]
OUT = Path(__file__).with_name(
    'sabori_obj_large.tsv' if os.environ.get('INST') == 'large' else 'sabori_obj.tsv')


def nat(s):
    return [int(t) if t.isdigit() else t.lower() for t in re.split('([0-9]+)', str(s))]


def detect_type(mzn):
    txt = '\n'.join(l for l in Path(mzn).read_text().splitlines()
                    if not l.strip().startswith('%'))
    if re.search(r'\bminimize\b', txt):
        return 'MIN'
    if re.search(r'\bmaximize\b', txt):
        return 'MAX'
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
        out, _ = proc.communicate(timeout=TIMEOUT + 45)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except OSError:
            pass
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
    row = {'problem': prob, 'type': ptype}
    for cname, msc in CONFIGS:
        st, el, obj = run(msc, mzn, data, ptype)
        row[cname] = (st, el, obj)
    return row


def main():
    years = sys.argv[1:] or ['2022', '2023', '2024', '2025']
    for n in ['fzn_sabori', 'fzn-cp-sat', 'minizinc']:
        subprocess.run(['pkill', '-x', n], capture_output=True)
    time.sleep(0.5)
    tasks = []
    for y in years:
        for d in sorted((CH / f'mznc{y}_probs').glob('*/'), key=nat):
            inst = find_instance(d)
            if inst:
                mzn, data = inst
                tasks.append((f'{y}/{d.name.rstrip("/")}', mzn, data, detect_type(mzn)))
    print(f"{len(tasks)} problems x {len(CONFIGS)} configs (obj capture)")
    with open(OUT, 'w') as fo:
        fo.write('problem\ttype\tbase_st\tbase_t\tbase_obj\tlcg_st\tlcg_t\tlcg_obj\n')
        with ProcessPoolExecutor(WORKERS) as ex:
            for row in ex.map(process, tasks):
                b, l = row['base'], row['lcg']
                fo.write(f"{row['problem']}\t{row['type']}\t"
                         f"{b[0]}\t{b[1]}\t{b[2]}\t{l[0]}\t{l[1]}\t{l[2]}\n")
                print(f"{row['problem']:<34} base:{b[0]}/{b[2]} lcg:{l[0]}/{l[2]}", flush=True)
    print(f"wrote {OUT}")


if __name__ == '__main__':
    main()
