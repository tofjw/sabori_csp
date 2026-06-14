#!/usr/bin/env python3
"""dispatch ルールを「accuracy」でなく「regret(誤判定で失う質)」で評価。

rows.txt: name type <base:STATUS VAL TIME> <L:STATUS VAL TIME> [label]
各 nontie 問題で L と base の差をインパクト階層に分類:
  T3 (solve級, w=10): status が解到達レベルで異なる (OPTIMAL vs SOL/UNKNOWN 等)
  T2 (objective, w=3): 両者 SOL だが目的値が異なる
  T1 (time-only, w=1): 同 status・同目的値、時間差のみ (ノイズ源)

regret(戦略) = Σ_{予測を誤った問題} magnitude
  always-L  : base が勝つ問題で損 (s=-1)
  always-base: L が勝つ問題で損 (s=+1)
  dispatch  : LOYO 予測が外れた問題で損
低いほど良い。
"""
import re
from pathlib import Path

ROOT = Path(__file__).parent
ROWS = Path('/tmp/rows.txt')
TSV = ROOT / 'features.tsv'
W = {3: 10.0, 2: 3.0, 1: 1.0}


def parse_rows():
    """name -> (type, base, L) ; base/L = (status, val_or_None, time_sec)"""
    out = {}
    pat = re.compile(
        r'^(\S+)\s+(MIN|MAX|SAT)\s+'
        r'(OPTIMAL|SOL|UNKNOWN|UNSAT|ERROR)\s+(\S+)\s+(\d+)s\s+'
        r'(OPTIMAL|SOL|UNKNOWN|UNSAT|ERROR)\s+(\S+)\s+(\d+)s')
    for ln in ROWS.read_text().splitlines():
        m = pat.match(ln)
        if not m:
            continue
        name, typ = m.group(1), m.group(2)
        bs, bv, bt = m.group(3), m.group(4), int(m.group(5))
        ls, lv, lt = m.group(6), m.group(7), int(m.group(8))
        bv = None if bv == '-' else int(bv)
        lv = None if lv == '-' else int(lv)
        out[name] = (typ, (bs, bv, bt), (ls, lv, lt))
    return out


STAT_RANK = {'UNKNOWN': 0, 'ERROR': 0, 'SOL': 1, 'UNSAT': 2, 'OPTIMAL': 2}


def impact(typ, base, L):
    """return (sign, tier). sign=+1 なら L が良い, -1 なら base が良い, 0 なら互角。"""
    bs, bv, bt = base
    ls, lv, lt = L
    br, lr = STAT_RANK[bs], STAT_RANK[ls]
    # T3: 解到達レベルが異なる
    if br != lr:
        return (1 if lr > br else -1), 3
    # 同レベル。両者 SOL/OPTIMAL で目的値あり → T2 (目的値比較)
    if bv is not None and lv is not None and bv != lv:
        better_L = (lv > bv) if typ == 'MAX' else (lv < bv)
        # ただし OPTIMAL 同士で値が違うのは異常(健全性) -> それでも値で判定
        return (1 if better_L else -1), 2
    # 同 status・同目的値 → T1 (時間差のみ)
    if bt != lt:
        return (1 if lt < bt else -1), 1
    return 0, 1


def load_features():
    lines = TSV.read_text().splitlines()
    hdr = lines[0].split('\t')
    idx = {c: i for i, c in enumerate(hdr)}
    rows = []
    for ln in lines[1:]:
        f = ln.split('\t')
        rows.append((f[0], f[0].split('/')[0], f[2],
                     float(f[idx['linear_frac']])))
    return rows


def loyo_predict(rows):
    """linear_frac<=t を LEARN+ 予測。年 hold out で各問題に予測を付与。"""
    years = sorted(set(r[1] for r in rows))
    pred = {}
    for hy in years:
        train = [r for r in rows if r[1] != hy]
        vals = sorted(set(r[3] for r in train))
        best_acc, best_t = -1, vals[0]
        for t in vals:
            c = sum(1 for _, _, lab, v in train
                    if (1 if v <= t else 0) == (1 if lab == 'LEARN+' else 0))
            if c > best_acc:
                best_acc, best_t = c, t
        for name, y, lab, v in rows:
            if y == hy:
                pred[name] = 1 if v <= best_t else 0
    return pred


def main():
    rd = parse_rows()
    feats = load_features()
    pred = loyo_predict(feats)

    reg_L = reg_base = reg_disp = 0.0
    tier_count = {1: 0, 2: 0, 3: 0}
    sign_tier = {}
    n = 0
    for name, year, lab, v in feats:
        if name not in rd:
            continue
        typ, base, L = rd[name]
        s, tier = impact(typ, base, L)
        if s == 0:
            continue
        n += 1
        tier_count[tier] += 1
        mag = W[tier]
        sign_tier[name] = (s, tier)
        # always-L: base が良い(s=-1)のに L を選び損
        if s < 0:
            reg_L += mag
        else:
            reg_base += mag
        # dispatch
        chosen_L = pred.get(name, 1)
        # 選んだ側が悪ければ mag 損
        if (chosen_L and s < 0) or (not chosen_L and s > 0):
            reg_disp += mag

    print(f"evaluated {n} nontie problems")
    print(f"tier counts: T3(solve)={tier_count[3]}  T2(obj)={tier_count[2]}  T1(time)={tier_count[1]}\n")
    print(f"{'strategy':<14}{'regret':>10}")
    print('-' * 24)
    print(f"{'always-base':<14}{reg_base:>10.1f}")
    print(f"{'always-L':<14}{reg_L:>10.1f}")
    print(f"{'dispatch(LOYO)':<14}{reg_disp:>10.1f}")
    print(f"\nbest possible (oracle) regret = 0")
    print(f"dispatch improves over always-L by {reg_L-reg_disp:+.1f}, "
          f"over always-base by {reg_base-reg_disp:+.1f}")

    # T3 だけ見る (solve級だけ): dispatch はここを当てられているか
    print("\n--- T3 (solve級) のみ内訳 ---")
    for name, year, lab, v in feats:
        if name in sign_tier and sign_tier[name][1] == 3:
            s, _ = sign_tier[name]
            chosen = 'L' if pred.get(name, 1) else 'base'
            winner = 'L' if s > 0 else 'base'
            ok = 'OK' if chosen == winner else 'MISS'
            print(f"  {name:<34} winner={winner:<5} dispatch={chosen:<5} {ok}")


if __name__ == '__main__':
    main()
