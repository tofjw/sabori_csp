#!/usr/bin/env python3
"""probe_feature_study.py の出力を 3 つのゲートで判定する。

Gate 1  余地（headroom）: アーム選択で得られる上限。VBS（問題ごとに最良アーム）が
        最良の単一静的アームをどれだけ上回るか。ここが小さければ特徴量の質以前に
        選択する意味がない。
Gate 2  勝者の安定性: 同じ問題で「勝つアーム」がシードをまたいで一貫しているか。
        シードで入れ替わるなら、どんな特徴量でも予測できない
        （coin-flip hotness が敏感なモデルを特定できなかったのと同じ形）。
Gate 3  特徴量の判別力: プローブ統計が「base 以外のアームが勝つか」を分離できるか。
        各特徴について、勝ち群と負け群の分布差（AUC）を出す。

使い方: python3 probe_feature_analyze.py [/tmp/probe_study.json]
"""
import json
import statistics
import sys
from collections import defaultdict

RANK = {"UNKNOWN": 0, "SOL": 1, "OPTIMAL": 2, "UNSAT": 2}


def better(a, b, direction):
    """run a が run b より良ければ 1、悪ければ -1、同等なら 0。"""
    ra, rb = RANK.get(a["status"], 0), RANK.get(b["status"], 0)
    if ra != rb:
        return 1 if ra > rb else -1
    if ra == 0:
        return 0
    if a["obj"] is not None and b["obj"] is not None and a["obj"] != b["obj"]:
        if direction == "minimize":
            return 1 if a["obj"] < b["obj"] else -1
        return 1 if a["obj"] > b["obj"] else -1
    if ra == 2:  # 証明完了同士は時間
        ta, tb = a["time"], b["time"]
        if ta > 0.3 or tb > 0.3:
            if ta < tb * 0.85:
                return 1
            if tb < ta * 0.85:
                return -1
    return 0


def auc(pos, neg):
    """Mann-Whitney U から AUC。0.5 = 判別力なし、1.0/0.0 = 完全分離。"""
    if not pos or not neg:
        return None
    wins = ties = 0
    for p in pos:
        for n in neg:
            if p > n:
                wins += 1
            elif p == n:
                ties += 1
    return (wins + 0.5 * ties) / (len(pos) * len(neg))


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/probe_study.json"
    D = json.load(open(path))
    arms = D["config"]["arms"]
    seeds = D["config"]["seeds"]
    probes = D["probes"]

    runs = defaultdict(dict)  # name -> (arm, seed) -> run
    for r in D["runs"]:
        runs[r["name"]][(r["arm"], r["seed"])] = r["run"]
    names = sorted(runs)
    print(f"対象 {len(names)} 問 / アーム {len(arms)} / シード {seeds}"
          f"（probe で証明完了のため除外: {len(D['trivial'])} 問）\n")

    # ---- Gate 1: headroom -------------------------------------------------
    # 各 (問題, シード) で、アーム間の順位を「勝ち数」で決める（総当たり）。
    win_count = defaultdict(int)     # arm -> 単独最良だった (問題,シード) 数
    arm_points = defaultdict(int)    # arm -> 総当たりの勝ち点
    vbs_vs_arm = defaultdict(int)    # arm -> VBS がそのアームに勝った (問題,シード) 数
    per_inst_best = defaultdict(set)
    for n in names:
        direction = probes[n]["static"].get("direction")
        for s in range(1, seeds + 1):
            rs = {a: runs[n].get((a, s)) for a in arms}
            if any(v is None for v in rs.values()):
                continue
            for a in arms:
                for b in arms:
                    if a != b:
                        arm_points[a] += max(0, better(rs[a], rs[b], direction))
            best = [a for a in arms if all(better(rs[a], rs[b], direction) >= 0 for b in arms)]
            strict = [a for a in best if any(better(rs[a], rs[b], direction) > 0 for b in arms)]
            for a in (strict or best):
                per_inst_best[n].add(a)
            if len(strict) == 1:
                win_count[strict[0]] += 1
            for a in arms:
                if any(better(rs[b], rs[a], direction) > 0 for b in arms):
                    vbs_vs_arm[a] += 1

    total_cells = sum(1 for n in names for s in range(1, seeds + 1))
    print("=== Gate 1: 余地（headroom）===")
    print(f"{'arm':<16}{'総当たり勝ち点':>14}{'単独最良':>10}{'VBS に負けたセル':>18}")
    for a in sorted(arms, key=lambda x: -arm_points[x]):
        print(f"{a:<16}{arm_points[a]:>14}{win_count[a]:>10}{vbs_vs_arm[a]:>18} / {total_cells}")
    best_arm = max(arms, key=lambda a: arm_points[a])
    print(f"\n最良の単一静的アーム: {best_arm}")
    print(f"  そのアームが VBS に負けるセル: {vbs_vs_arm[best_arm]} / {total_cells} "
          f"({100*vbs_vs_arm[best_arm]/max(1,total_cells):.1f}%)  <- 選択で取りに行ける上限")

    # ---- Gate 2: 勝者の安定性 ---------------------------------------------
    print("\n=== Gate 2: 勝者の安定性（シード間）===")
    stable = flip = nodiff = 0
    flip_examples = []
    for n in names:
        direction = probes[n]["static"].get("direction")
        winners = []
        for s in range(1, seeds + 1):
            rs = {a: runs[n].get((a, s)) for a in arms}
            if any(v is None for v in rs.values()):
                continue
            strict = [a for a in arms
                      if all(better(rs[a], rs[b], direction) >= 0 for b in arms)
                      and any(better(rs[a], rs[b], direction) > 0 for b in arms)]
            winners.append(frozenset(strict))
        if not winners:
            continue
        if all(not w for w in winners):
            nodiff += 1
        elif len(set(winners)) == 1:
            stable += 1
        else:
            flip += 1
            if len(flip_examples) < 8:
                flip_examples.append((n, [sorted(w) or ["-"] for w in winners]))
    print(f"  全シードで勝者一致        : {stable}")
    print(f"  シードで勝者が入れ替わる  : {flip}")
    print(f"  そもそもアーム間に差なし  : {nodiff}")
    for n, w in flip_examples:
        print(f"    {n}: {w}")

    # ---- Gate 3: 特徴量の判別力 -------------------------------------------
    print("\n=== Gate 3: 特徴量の判別力（AUC, 0.5=判別力なし）===")
    feats = {}
    for n in names:
        p = probes[n]
        st = p["probe"]["stats"]
        f = {}
        for k in ("fails", "restarts", "max_depth", "avg_depth", "nogoods",
                  "ng_check", "ng_domain", "ng_prune", "ng_noop",
                  "bisect", "enumerate", "ng_len_mean"):
            if k in st:
                f[k] = float(st[k])
        fails = max(1.0, f.get("fails", 1.0))
        chk = max(1.0, f.get("ng_check", 1.0))
        br = f.get("bisect", 0.0) + f.get("enumerate", 0.0)
        f["prune_rate"] = f.get("ng_prune", 0.0) / chk
        f["noop_rate"] = f.get("ng_noop", 0.0) / chk
        f["ng_per_fail"] = f.get("nogoods", 0.0) / fails
        f["restart_rate"] = f.get("restarts", 0.0) / fails
        f["bisect_ratio"] = f.get("bisect", 0.0) / br if br else 0.0
        f["depth_ratio"] = f.get("avg_depth", 0.0) / max(1.0, f.get("max_depth", 1.0))
        f["probe_found_sol"] = 1.0 if p["probe"]["status"] == "SOL" else 0.0
        f["probe_improve"] = float(p["probe"]["n_improve"])
        f["n_constraint"] = float(p["static"].get("n_constraint", 0))
        f["n_kind"] = float(p["static"].get("n_kind", 0))
        f["fzn_bytes"] = float(p["static"].get("fzn_bytes", 0))
        f["is_optimize"] = 1.0 if p["static"].get("direction") else 0.0
        feats[n] = f

    for target in [a for a in arms if a != "base"] + ["ANY_NONBASE"]:
        pos, neg = [], []
        for n in names:
            direction = probes[n]["static"].get("direction")
            votes = 0
            for s in range(1, seeds + 1):
                rs = {a: runs[n].get((a, s)) for a in arms}
                if any(v is None for v in rs.values()):
                    continue
                if target == "ANY_NONBASE":
                    if any(better(rs[a], rs["base"], direction) > 0 for a in arms if a != "base"):
                        votes += 1
                else:
                    if better(rs[target], rs["base"], direction) > 0:
                        votes += 1
            (pos if votes > seeds / 2 else neg).append(n)
        if not pos or len(pos) < 3:
            print(f"\n[{target}] base に勝つ問題 {len(pos)} 件（少なすぎて判定不能）")
            continue
        print(f"\n[{target}] base に勝つ {len(pos)} 問 vs 勝たない {len(neg)} 問")
        rows = []
        for k in sorted(next(iter(feats.values()))):
            a = auc([feats[n].get(k, 0.0) for n in pos], [feats[n].get(k, 0.0) for n in neg])
            if a is not None:
                rows.append((abs(a - 0.5), a, k))
        rows.sort(reverse=True)
        for d, a, k in rows[:6]:
            print(f"    {k:<18} AUC={a:.3f}  (|AUC-0.5|={d:.3f})")
        # 並べ替え検定: ラベルをシャッフルして「全特徴の最大 |AUC-0.5|」の帰無分布を作る。
        # 特徴が約20個ある分の多重比較も同時に補正できる。
        import random
        rng = random.Random(12345)
        labels = [1] * len(pos) + [0] * len(neg)
        allnames = pos + neg
        keys = sorted(next(iter(feats.values())))
        observed = rows[0][0]
        null = []
        for _ in range(2000):
            rng.shuffle(labels)
            p2 = [allnames[i] for i, v in enumerate(labels) if v]
            n2 = [allnames[i] for i, v in enumerate(labels) if not v]
            best = 0.0
            for k in keys:
                a2 = auc([feats[x].get(k, 0.0) for x in p2], [feats[x].get(k, 0.0) for x in n2])
                if a2 is not None:
                    best = max(best, abs(a2 - 0.5))
            null.append(best)
        pval = (sum(1 for v in null if v >= observed) + 1) / (len(null) + 1)
        print(f"    -> 並べ替え検定(2000回, 全特徴の最大|AUC-0.5|): "
              f"観測 {observed:.3f}, 帰無の中央値 {statistics.median(null):.3f}, p={pval:.3f}"
              f"  {'有意' if pval < 0.05 else '有意でない'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
