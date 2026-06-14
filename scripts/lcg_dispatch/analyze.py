#!/usr/bin/env python3
"""features.tsv の各特徴が LEARN+ / LEARN- をどれだけ分離するかを評価。

指標: AUC = P(LEARN+ の特徴値 > LEARN- の特徴値)  (Mann-Whitney U / ランクベース)
  - 0.5  = 無情報
  - >0.5 = 値が大きいほど LEARN+ (full-L が勝つ)
  - <0.5 = 値が小さいほど LEARN+
スケール不変なので span 等の歪んだ特徴もそのまま扱える。
"""
from pathlib import Path

TSV = Path(__file__).with_name('features.tsv')


def load():
    lines = TSV.read_text().splitlines()
    hdr = lines[0].split('\t')
    cols = hdr[3:]
    pos, neg = {c: [] for c in cols}, {c: [] for c in cols}
    for ln in lines[1:]:
        f = ln.split('\t')
        label = f[2]
        bucket = pos if label == 'LEARN+' else neg
        for c, v in zip(cols, f[3:]):
            bucket[c].append(float(v))
    return cols, pos, neg


def auc(p, n):
    """P(random pos > random neg)。タイは 0.5。"""
    if not p or not n:
        return 0.5
    allv = sorted([(v, 1) for v in p] + [(v, 0) for v in n])
    # ランク付け (タイは平均ランク)
    ranks = {}
    i = 0
    rsum_pos = 0.0
    # 平均ランク法で U を計算
    vals = [v for v, _ in allv]
    labels = [l for _, l in allv]
    i = 0
    rank = 1
    avg_ranks = [0.0] * len(vals)
    while i < len(vals):
        j = i
        while j + 1 < len(vals) and vals[j + 1] == vals[i]:
            j += 1
        avg = (rank + (rank + (j - i))) / 2.0
        for k in range(i, j + 1):
            avg_ranks[k] = avg
        rank += (j - i + 1)
        i = j + 1
    rsum_pos = sum(r for r, l in zip(avg_ranks, labels) if l == 1)
    n_pos = len(p)
    n_neg = len(n)
    u = rsum_pos - n_pos * (n_pos + 1) / 2.0
    return u / (n_pos * n_neg)


def main():
    cols, pos, neg = load()
    rows = []
    for c in cols:
        a = auc(pos[c], neg[c])
        sep = abs(a - 0.5)
        mp = sorted(pos[c])[len(pos[c]) // 2] if pos[c] else 0
        mn = sorted(neg[c])[len(neg[c]) // 2] if neg[c] else 0
        rows.append((sep, a, c, mp, mn))
    rows.sort(reverse=True)
    print(f"{'feature':<22}{'AUC':>7}{'sep':>7}   {'med(L+)':>14}{'med(L-)':>14}  direction")
    print('-' * 86)
    for sep, a, c, mp, mn in rows:
        d = 'big=>LEARN+' if a > 0.5 else 'small=>LEARN+'
        print(f"{c:<22}{a:>7.3f}{sep:>7.3f}   {mp:>14.4g}{mn:>14.4g}  {d}")


if __name__ == '__main__':
    main()
