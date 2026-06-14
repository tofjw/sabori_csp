#!/usr/bin/env python3
"""LCG up-front dispatch 用の静的特徴抽出。

入力: nontie.txt  (各行 "<year>/<prob> <MIN|MAX|SAT> <LEARN+|LEARN->")
       LEARN+ = full-L (-L) が base に勝つ問題、LEARN- = base 勝ち。
出力: features.tsv (ヘッダ付き TSV、全数値列。相関分析にそのまま投入できる)

特徴の出所:
  - fzn 静的解析 (mzn コンパイル後・solver presolve 前)
  - solver の `-c -v` 静的レポート (VIG / modularity)

fzn のキャッシュは /tmp/fzn_cache、VIG レポートは /tmp/vig_cache に保存し再利用する。
"""
import re
import subprocess
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

ROOT = Path('/home/tofjw/develop/cp/sabori_csp')
BASE = ROOT / 'benchmarks/minizinc_challenge'
MSC = ROOT / 'build/share/minizinc/solvers/sabori_csp.msc'
FZN_SABORI = ROOT / 'build/src/fzn/fzn_sabori'
FZN_CACHE = Path('/tmp/fzn_cache')
VIG_CACHE = Path('/tmp/vig_cache')
LABELS = Path(__file__).with_name('nontie.txt')

# --- solver が explain (LCG nogood) を実装している fzn builtin ---
# 出所: src/core/constraints/{comparison,logical,global/int_lin_eq*,global/int_one_hot_channel}.cpp
EXPLAINABLE = {
    'int_eq_reif', 'int_ne_reif', 'int_le_reif',
    'int_lin_eq', 'int_lin_eq_reif',
    'array_bool_and', 'array_bool_or', 'bool_clause',
}

# --- グローバル制約 (mzn 分解を免れて fzn に残るもの) ---
GLOBAL_KEYS = {
    'all_different_int', 'fzn_all_different_int', 'alldifferent',
    'cumulative', 'fzn_cumulative', 'circuit', 'fzn_circuit',
    'subcircuit', 'table_int', 'fzn_table_int', 'table_bool',
    'global_cardinality', 'global_cardinality_low_up', 'count_eq',
    'count', 'inverse', 'fzn_inverse', 'bin_packing', 'diffn',
    'fzn_diffn', 'regular', 'fzn_regular', 'lex_less', 'lex_lesseq',
    'nvalue', 'fzn_nvalue', 'maximum_int', 'minimum_int',
}

LINEAR_KEYS = {
    'int_lin_eq', 'int_lin_le', 'int_lin_ne',
    'int_lin_eq_reif', 'int_lin_le_reif', 'int_lin_ne_reif',
    'bool_lin_eq', 'bool_lin_le',
}


def nat_key(s):
    return [int(t) if t.isdigit() else t.lower() for t in re.split(r'([0-9]+)', str(s))]


def compile_one(name):
    year, prob = name.split('/')
    d = BASE / f"mznc{year}_probs" / prob
    out = FZN_CACHE / f"{year}_{prob}.fzn"
    if out.exists():
        return str(out)
    mzns = sorted(d.glob('*.mzn'), key=nat_key)
    if not mzns:
        return None
    datas = sorted(d.glob('*.dzn'), key=nat_key) + sorted(d.glob('*.json'), key=nat_key)
    cmd = ['/snap/bin/minizinc', '--solver', str(MSC), '-c', '--output-objective', str(mzns[0])]
    if datas:
        cmd.append(str(datas[0]))
    cmd += ['-o', str(out)]
    try:
        subprocess.run(cmd, capture_output=True, timeout=180, cwd=str(BASE))
    except subprocess.TimeoutExpired:
        return None
    return str(out) if out.exists() else None


# var 宣言パターン
RE_BOOL = re.compile(r'var bool\s*:\s*(\w+)')
RE_RANGE = re.compile(r'var\s+(-?\d+)\.\.(-?\d+)\s*:\s*(\w+)')
RE_SET = re.compile(r'var\s+\{([0-9,\s-]+)\}\s*:\s*(\w+)')
RE_CONS = re.compile(r'constraint\s+([a-zA-Z_][a-zA-Z_0-9]*)\s*\(')
RE_SOLVE = re.compile(r'solve\s+(?:::.*?)?(minimize|maximize)\s+(\w+)')


def parse_fzn(path):
    n_bool = n_int = 0
    spans = []          # 各 int 変数の span = hi-lo+1
    cards = []          # 各 int 変数の取りうる値数 (range なら span、set なら要素数)
    var_span = {}       # name -> span
    var_lo = {}
    var_hi = {}
    cons_hist = {}
    obj_name = None
    with open(path) as f:
        for line in f:
            if line.startswith('var ') or ': var ' in line or line.lstrip().startswith('var '):
                m = RE_BOOL.search(line)
                if m:
                    n_bool += 1
                    var_span[m.group(1)] = 2
                    var_lo[m.group(1)] = 0
                    var_hi[m.group(1)] = 1
                    continue
                m = RE_RANGE.search(line)
                if m:
                    lo, hi = int(m.group(1)), int(m.group(2))
                    n_int += 1
                    sp = hi - lo + 1
                    spans.append(sp)
                    cards.append(sp)
                    var_span[m.group(3)] = sp
                    var_lo[m.group(3)] = lo
                    var_hi[m.group(3)] = hi
                    continue
                m = RE_SET.search(line)
                if m:
                    vals = [int(x) for x in m.group(1).split(',') if x.strip()]
                    if vals:
                        n_int += 1
                        sp = max(vals) - min(vals) + 1
                        spans.append(sp)
                        cards.append(len(vals))
                        var_span[m.group(2)] = sp
                        var_lo[m.group(2)] = min(vals)
                        var_hi[m.group(2)] = max(vals)
                    continue
            if 'constraint' in line:
                m = RE_CONS.search(line)
                if m:
                    cons_hist[m.group(1)] = cons_hist.get(m.group(1), 0) + 1
            if obj_name is None and 'solve' in line:
                m = RE_SOLVE.search(line)
                if m:
                    obj_name = m.group(2)

    n_vars = n_bool + n_int
    n_cons = sum(cons_hist.values())
    expl = sum(c for k, c in cons_hist.items() if k in EXPLAINABLE)
    reif = sum(c for k, c in cons_hist.items() if k.endswith('_reif'))
    n_global = sum(c for k, c in cons_hist.items() if k in GLOBAL_KEYS)
    n_linear = sum(c for k, c in cons_hist.items() if k in LINEAR_KEYS)

    # hole_frac: range 内の穴の割合 (set 変数の sparse 度). domain_vocabulary_fit 候補
    hole = [1 - c / s for c, s in zip(cards, spans) if s > 0]
    mean_hole = sum(hole) / len(hole) if hole else 0.0

    return dict(
        n_vars=n_vars, n_bool=n_bool, n_int=n_int,
        bool_frac=round(n_bool / n_vars, 4) if n_vars else 0.0,
        max_span=max(spans) if spans else 1,
        mean_span=round(sum(spans) / len(spans), 1) if spans else 1.0,
        obj_span=var_span.get(obj_name, 0) if obj_name else 0,
        n_cons=n_cons,
        explainable_weight=round(expl / n_cons, 4) if n_cons else 0.0,
        bool_reif_density=round(reif / n_cons, 4) if n_cons else 0.0,
        global_frac=round(n_global / n_cons, 4) if n_cons else 0.0,
        linear_frac=round(n_linear / n_cons, 4) if n_cons else 0.0,
        global_vs_linear=round(n_global / n_linear, 4) if n_linear else 0.0,
        mean_hole=round(mean_hole, 4),
    )


RE_VIG = re.compile(r'VIG:\s*(\d+)\s*vars,\s*(\d+)\s*edges,\s*avg_degree=([\d.]+),\s*max_degree=(\d+)')
RE_MOD = re.compile(r'Communities:\s*(\d+)\s*\(modularity Q=(-?[\d.]+)\)')
RE_SIZES = re.compile(r'Sizes:\s*\[([0-9,\s.]+)\]')
RE_INTRA = re.compile(r'Intra-edges:\s*\d+\s*\(([\d.]+)%\)')
RE_OBJC = re.compile(r'objcoupling\] obj_root_min=(-?\d+) obj_root_max=(-?\d+)')


def vig_features(name, fzn_path):
    cache = VIG_CACHE / f"{name.replace('/', '_')}.log"
    if cache.exists():
        txt = cache.read_text()
    else:
        try:
            r = subprocess.run([str(FZN_SABORI), '-c', '-v', '-t', '1', fzn_path],
                               capture_output=True, timeout=30, text=True)
            txt = r.stderr
        except subprocess.TimeoutExpired:
            txt = ''
        cache.write_text(txt)

    out = dict(vig_vars=0, vig_edges=0, vig_density=0.0, avg_degree=0.0,
               max_degree=0, modularity=0.0, num_comm=0,
               largest_comm_frac=0.0, intra_frac=0.0, obj_root_span=None)
    m = RE_VIG.search(txt)
    if m:
        v, e = int(m.group(1)), int(m.group(2))
        out['vig_vars'] = v
        out['vig_edges'] = e
        out['avg_degree'] = float(m.group(3))
        out['max_degree'] = int(m.group(4))
        if v > 1:
            out['vig_density'] = round(2 * e / (v * (v - 1)), 5)
    m = RE_MOD.search(txt)
    if m:
        out['num_comm'] = int(m.group(1))
        out['modularity'] = float(m.group(2))
    m = RE_SIZES.search(txt)
    if m and out['vig_vars']:
        sizes = [int(float(x)) for x in m.group(1).split(',') if x.strip() and x.strip() != '...']
        if sizes:
            out['largest_comm_frac'] = round(max(sizes) / out['vig_vars'], 4)
    m = RE_INTRA.search(txt)
    if m:
        out['intra_frac'] = round(float(m.group(1)) / 100.0, 4)
    m = RE_OBJC.search(txt)
    if m:
        out['obj_root_span'] = int(m.group(2)) - int(m.group(1)) + 1
    return out


def process(line):
    name, ptype, label = line.split()
    fzn = compile_one(name)
    if fzn is None:
        return name, ptype, label, None
    ft = parse_fzn(fzn)
    vf = vig_features(name, fzn)
    # objective_coupling = root 伝播が宣言 span をどれだけ縮めたか [0,1]
    decl = ft['obj_span']
    root = vf.pop('obj_root_span')
    if decl and decl > 1 and root is not None:
        ft['obj_coupling'] = round(max(0.0, min(1.0, 1.0 - root / decl)), 4)
    else:
        ft['obj_coupling'] = 0.0
    ft.update(vf)
    return name, ptype, label, ft


COLS = ['n_vars', 'n_bool', 'n_int', 'bool_frac', 'max_span', 'mean_span',
        'obj_span', 'n_cons', 'explainable_weight', 'bool_reif_density',
        'global_frac', 'linear_frac', 'global_vs_linear', 'mean_hole',
        'obj_coupling',
        'vig_vars', 'vig_edges', 'vig_density', 'avg_degree', 'max_degree',
        'modularity', 'num_comm', 'largest_comm_frac', 'intra_frac']


def main():
    FZN_CACHE.mkdir(exist_ok=True)
    VIG_CACHE.mkdir(exist_ok=True)
    lines = [l.strip() for l in open(LABELS) if l.strip()]
    out = Path(__file__).with_name('features.tsv')
    with open(out, 'w') as fo:
        fo.write('name\ttype\tlabel\t' + '\t'.join(COLS) + '\n')
        with ProcessPoolExecutor(4) as ex:
            for name, ptype, label, ft in ex.map(process, lines):
                if ft is None:
                    print(f"COMPILE_FAIL\t{name}")
                    continue
                row = [name, ptype, label] + [str(ft[c]) for c in COLS]
                fo.write('\t'.join(row) + '\n')
    print(f"wrote {out}")


if __name__ == '__main__':
    main()
