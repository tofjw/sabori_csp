#!/usr/bin/env python3
"""対フィールド (CP-SAT) で base vs lcg の取り逃しを見積もる。

入力: sabori_obj.tsv (base/lcg, obj 込み) + cpsat.tsv
各問題で sabori 構成 vs CP-SAT を MiniZinc Challenge 流に採点:
  1. 解到達レベル (見つけた/証明した) が上 → 勝ち (score 1)
  2. 両者解あり・最適化問題 → 目的値の良い方が勝ち。同値なら OPTIMAL(証明) が SOL に勝つ
  3. 質が完全同点 → 時間スコア t_cpsat/(t_self+t_cpsat) (0.5=同速, →1=自分が速い)
  4. 両者解なし → 0.5

出力: base/lcg の対CP-SAT スコア合計、勝/分/負カウント、flip 一覧 (取り逃し)。
"""
import sys
from pathlib import Path

ROOT = Path(__file__).parent
SAB = ROOT / 'sabori_obj.tsv'
# 比較対象フィールドの tsv (既定 cpsat.tsv)。argv[1] で gecode.tsv 等に差し替え可。
CPS = ROOT / (sys.argv[1] if len(sys.argv) > 1 else 'cpsat.tsv')
FIELD = CPS.stem
FOUND = {'OPTIMAL': 2, 'UNSAT': 2, 'SOL': 1, 'TIMEOUT': 0, 'UNKNOWN': 0, 'ERROR': 0}


def pobj(s):
    return None if s in ('None', '', None) else int(s)


def load_sab():
    out = {}
    lines = SAB.read_text().splitlines()[1:]
    for ln in lines:
        f = ln.split('\t')
        out[f[0]] = dict(type=f[1],
                         base=(f[2], float(f[3]), pobj(f[4])),
                         lcg=(f[5], float(f[6]), pobj(f[7])))
    return out


def load_cps():
    out = {}
    for ln in CPS.read_text().splitlines()[1:]:
        f = ln.split('\t')
        out[f[0]] = (f[2], float(f[3]), pobj(f[4]))
    return out


def score(ptype, s, c):
    """sabori s が CP-SAT c に対して取るスコア [0,1] と離散判定 (W/T/L)。"""
    ss, st, so = s
    cs, ct, co = c
    fs, fc = FOUND[ss], FOUND[cs]
    # 解到達レベルが違う
    if (fs > 0) != (fc > 0):
        return (1.0, 'W') if fs > fc else (0.0, 'L')
    if fs == 0 and fc == 0:
        return (0.5, 'T')   # 両者解なし
    # 両者解あり
    if ptype != 'SAT' and so is not None and co is not None and so != co:
        better = (so > co) if ptype == 'MAX' else (so < co)
        return (1.0, 'W') if better else (0.0, 'L')
    # 目的値同値 (or SAT) → 証明の有無
    if fs != fc:   # 一方 OPTIMAL/UNSAT(2), 他方 SOL(1)
        return (1.0, 'W') if fs > fc else (0.0, 'L')
    # 質完全同点 → 時間スコア
    tscore = ct / (st + ct) if (st + ct) > 0 else 0.5
    disc = 'W' if st < ct - 0.5 else ('L' if st > ct + 0.5 else 'T')
    return (round(tscore, 3), disc)


def main():
    sab = load_sab()
    cps = load_cps()
    probs = [p for p in sab if p in cps]
    agg = {'base': [0.0, {'W': 0, 'T': 0, 'L': 0}],
           'lcg': [0.0, {'W': 0, 'T': 0, 'L': 0}]}
    detail = []
    for p in probs:
        ptype = sab[p]['type']
        c = cps[p]
        sc = {}
        for cfg in ('base', 'lcg'):
            val, disc = score(ptype, sab[p][cfg], c)
            agg[cfg][0] += val
            agg[cfg][1][disc] += 1
            sc[cfg] = (val, disc)
        detail.append((p, ptype, sc, sab[p], c))

    print(f"対 {FIELD}, {len(probs)} 問, timeout 60s\n")
    print(f"{'config':<8}{'score':>8}{'W':>5}{'T':>5}{'L':>5}   (score: 1=sabori勝 0.5=分 0=負)")
    for cfg in ('base', 'lcg'):
        s, wtl = agg[cfg]
        print(f"{cfg:<8}{s:>8.2f}{wtl['W']:>5}{wtl['T']:>5}{wtl['L']:>5}")
    print(f"\n対フィールド スコア差 (lcg - base) = {agg['lcg'][0]-agg['base'][0]:+.2f}")

    # flip: 離散判定が base と lcg で変わる問題 = 取り逃し/取り返し
    print(f"\n=== flip (対{FIELD} 判定が base→lcg で変化) ===")
    rank = {'L': 0, 'T': 1, 'W': 2}
    gain = loss = 0
    for p, ptype, sc, s, c in detail:
        bd, ld = sc['base'][1], sc['lcg'][1]
        if bd == ld:
            continue
        arrow = '取り返し(gain)' if rank[ld] > rank[bd] else '取り逃し→lcg悪化(loss)'
        if rank[ld] > rank[bd]:
            gain += 1
        else:
            loss += 1
        b, l = s['base'], s['lcg']
        print(f"  {p:<30}{ptype:<4} base:{b[0]}/{b[2]}({bd}) lcg:{l[0]}/{l[2]}({ld}) "
              f"{FIELD}:{c[0]}/{c[2]}  {arrow}")
    print(f"\nlcg が {FIELD} 相手に base より良くなった問題: {gain}")
    print(f"lcg が {FIELD} 相手に base より悪くなった問題: {loss}")
    print(f"正味 flip: {gain - loss:+d}")


if __name__ == '__main__':
    main()
