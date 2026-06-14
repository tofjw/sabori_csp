#!/usr/bin/env python3
"""CP-SAT が complete できない領域 (大インスタンス) での base/lcg/CP-SAT 挙動比較。

入力: sabori_obj_large.tsv + cpsat_large.tsv
CP-SAT が OPTIMAL/UNSAT に到達しなかった問題に絞り (=anytime/primal 競争域):
  - 各構成の目的値を並べ、解の質で勝敗
  - 特に base vs lcg: incomplete 域で -L は base を改善するか
  - 各 sabori 構成 vs CP-SAT: 解の質で勝てるか
"""
import sys
from pathlib import Path

ROOT = Path(__file__).parent
SAB = ROOT / 'sabori_obj_large.tsv'
CPS = ROOT / (sys.argv[1] if len(sys.argv) > 1 else 'cpsat_large.tsv')
CPLABEL = CPS.stem
FOUND = {'OPTIMAL': 2, 'UNSAT': 2, 'SOL': 1, 'TIMEOUT': 0, 'UNKNOWN': 0, 'ERROR': 0}


def pobj(s):
    return None if s in ('None', '', None) else int(s)


def load_sab():
    out = {}
    for ln in SAB.read_text().splitlines()[1:]:
        f = ln.split('\t')
        out[f[0]] = dict(type=f[1], base=(f[2], float(f[3]), pobj(f[4])),
                         lcg=(f[5], float(f[6]), pobj(f[7])))
    return out


def load_cps():
    out = {}
    for ln in CPS.read_text().splitlines()[1:]:
        f = ln.split('\t')
        out[f[0]] = (f[2], float(f[3]), pobj(f[4]))
    return out


def quality_cmp(ptype, a, b):
    """a,b=(st,t,obj)。解の質で a が b に勝てば +1, 負け -1, 互角 0。
    解あり>解なし。両者解あり最適化は目的値。SAT/同値は 0(質互角)。"""
    fa, fb = FOUND[a[0]], FOUND[b[0]]
    if (fa > 0) != (fb > 0):
        return 1 if fa > fb else -1
    if fa == 0 and fb == 0:
        return 0
    if ptype != 'SAT' and a[2] is not None and b[2] is not None and a[2] != b[2]:
        better = (a[2] > b[2]) if ptype == 'MAX' else (a[2] < b[2])
        return 1 if better else -1
    # 証明の有無 (OPTIMAL vs SOL 同値) は質互角扱い (complete でないので)
    return 0


def main():
    sab, cps = load_sab(), load_cps()
    probs = [p for p in sab if p in cps]
    # CP-SAT incomplete (証明できず) のみ
    region = [p for p in probs if FOUND[cps[p][0]] < 2 or cps[p][0] == 'SOL']
    # SOL は解はあるが最適性未証明 = incomplete。OPTIMAL/UNSAT のみ除外。
    region = [p for p in probs if cps[p][0] not in ('OPTIMAL', 'UNSAT')]

    print(f"全 {len(probs)} 問中、CP-SAT が complete できない領域 = {len(region)} 問\n")
    print(f"{'problem':<32}{'type':<5}{'base':>16}{'lcg':>16}{'cpsat':>16}")
    print('-' * 85)

    bl = {'base>lcg': 0, 'lcg>base': 0, 'eq': 0}
    vs_cp = {'base': {'W': 0, 'L': 0, 'T': 0}, 'lcg': {'W': 0, 'L': 0, 'T': 0}}
    for p in sorted(region):
        t = sab[p]['type']
        b, l, c = sab[p]['base'], sab[p]['lcg'], cps[p]

        def cell(x):
            return f"{x[0]}/{x[2]}"
        print(f"{p:<32}{t:<5}{cell(b):>16}{cell(l):>16}{cell(c):>16}")
        # base vs lcg
        d = quality_cmp(t, b, l)
        bl['base>lcg' if d > 0 else ('lcg>base' if d < 0 else 'eq')] += 1
        # vs cpsat
        for cfg, x in (('base', b), ('lcg', l)):
            dc = quality_cmp(t, x, c)
            vs_cp[cfg]['W' if dc > 0 else ('L' if dc < 0 else 'T')] += 1

    n = len(region)
    print("\n=== incomplete 領域での base vs lcg (解の質) ===")
    print(f"  base が良い: {bl['base>lcg']}   lcg が良い: {bl['lcg>base']}   互角: {bl['eq']}")
    print(f"  → -L は incomplete 域で base を {'改善' if bl['lcg>base']>bl['base>lcg'] else '改善せず(むしろ悪化)' if bl['lcg>base']<bl['base>lcg'] else '中立'}")

    print("\n=== incomplete 領域で sabori vs CP-SAT (解の質) ===")
    for cfg in ('base', 'lcg'):
        w = vs_cp[cfg]
        print(f"  {cfg:<5}: CP-SAT に 勝ち {w['W']} / 互角 {w['T']} / 負け {w['L']}  (n={n})")


if __name__ == '__main__':
    main()
