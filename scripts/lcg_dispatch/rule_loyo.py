#!/usr/bin/env python3
"""linear_frac (および補助特徴) の閾値ルールを leave-one-year-out で検証。

各 fold: ある年を hold out。残り年で最良の閾値 (train accuracy 最大) を学習し、
hold out 年に適用して test accuracy を測る。LOYO で base (多数決) を上回るか。
"""
from pathlib import Path

TSV = Path(__file__).with_name('features.tsv')


def load():
    lines = TSV.read_text().splitlines()
    hdr = lines[0].split('\t')
    idx = {c: i for i, c in enumerate(hdr)}
    data = []
    for ln in lines[1:]:
        f = ln.split('\t')
        year = f[0].split('/')[0]
        y = 1 if f[2] == 'LEARN+' else 0
        data.append((year, y, {c: float(f[idx[c]]) for c in hdr[3:]}))
    return data


def best_threshold(train, feat, direction):
    """direction=-1: 値<=t を LEARN+ と予測 (linear_frac 小=>L+)。+1: 値>=t。"""
    vals = sorted(set(d[2][feat] for d in train))
    best_acc, best_t = -1, vals[0]
    for t in vals:
        correct = 0
        for _, y, ft in train:
            pred = 1 if (ft[feat] <= t if direction < 0 else ft[feat] >= t) else 0
            correct += (pred == y)
        acc = correct / len(train)
        if acc > best_acc:
            best_acc, best_t = acc, t
    return best_t, best_acc


def loyo(data, feat, direction):
    years = sorted(set(d[0] for d in data))
    tot_correct = tot = 0
    rows = []
    for hy in years:
        train = [d for d in data if d[0] != hy]
        test = [d for d in data if d[0] == hy]
        if not test:
            continue
        t, tr_acc = best_threshold(train, feat, direction)
        correct = 0
        for _, y, ft in test:
            pred = 1 if (ft[feat] <= t if direction < 0 else ft[feat] >= t) else 0
            correct += (pred == y)
        maj = sum(y for _, y, _ in train) / len(train)
        maj_pred = 1 if maj >= 0.5 else 0
        maj_correct = sum(1 for _, y, _ in test if y == maj_pred)
        rows.append((hy, len(test), t, correct, maj_correct))
        tot_correct += correct
        tot += len(test)
    return rows, tot_correct, tot


def main():
    data = load()
    base_pos = sum(y for _, y, _ in data)
    print(f"N={len(data)}  LEARN+={base_pos}  LEARN-={len(data)-base_pos}  "
          f"majority baseline acc={max(base_pos, len(data)-base_pos)/len(data):.3f}\n")

    for feat, direction in [('linear_frac', -1), ('bool_frac', +1),
                            ('bool_reif_density', +1)]:
        rows, tc, tt = loyo(data, feat, direction)
        print(f"=== {feat} (dir={'<=t' if direction<0 else '>=t'}) ===")
        print(f"{'year':>6}{'n':>4}{'thr':>9}{'rule_ok':>9}{'maj_ok':>8}")
        for hy, n, t, c, m in rows:
            flag = '  <' if c < m else ('  =' if c == m else '')
            print(f"{hy:>6}{n:>4}{t:>9.4g}{c:>9}{m:>8}{flag}")
        print(f"  LOYO rule acc = {tc/tt:.3f}  ({tc}/{tt})\n")


if __name__ == '__main__':
    main()
