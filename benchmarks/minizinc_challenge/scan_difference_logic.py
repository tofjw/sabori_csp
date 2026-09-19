#!/usr/bin/env python3
"""FZN コーパスを走査して Difference Logic (x - y <= c) の密度を測る。

目的: DL 専用伝播（制約グラフ上の負閉路検出 / 全点対最短路）を入れる余地が
      あるかを、実際のモデルの構成比から判断する。

数えるもの:
  hard   : int_le / int_lt / 係数 {1,-1} の int_lin_le・int_lin_eq
           （= 無条件の差分制約。DL グラフの辺になる）
  reified: 上記の _reif / _imp 版（= 条件付き辺。disjunctive scheduling の本体）
  他     : それ以外の制約

出力: モデルごとの (全制約数, hard DL 数, reified DL 数, DL 変数数, 密度)。

使い方:
    python3 scan_difference_logic.py [.fzn_cache_one_hot]
    python3 scan_difference_logic.py --top 30
"""
import argparse
import re
import sys
from pathlib import Path

ARR_DECL = re.compile(
    r"array\s*\[[^\]]*\]\s*of\s+int\s*:\s*([A-Za-z_][A-Za-z0-9_]*)\s*=\s*\[([^\]]*)\]\s*;")
CONSTRAINT = re.compile(r"^constraint\s+([A-Za-z_][A-Za-z0-9_]*)\s*\((.*)\)\s*(?:::.*)?;\s*$")

LIN_LE = {"int_lin_le", "int_lin_eq"}
LIN_COND = {"int_lin_le_reif", "int_lin_le_imp", "int_lin_eq_reif", "int_lin_eq_imp"}
CMP_HARD = {"int_le", "int_lt"}
CMP_COND = {"int_le_reif", "int_lt_reif", "int_le_imp", "int_lt_imp"}


def split_args(s):
    """トップレベルのカンマで分割（[] の中は無視）。"""
    out, depth, cur = [], 0, []
    for ch in s:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
        if ch == "," and depth == 0:
            out.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    if cur:
        out.append("".join(cur).strip())
    return out


def parse_coeffs(tok, arrays):
    """係数配列トークンを int のリストにする（解決できなければ None）。"""
    tok = tok.strip()
    if tok.startswith("["):
        body = tok[1:tok.rfind("]")]
        try:
            return [int(x.strip()) for x in body.split(",") if x.strip()]
        except ValueError:
            return None
    return arrays.get(tok)


def is_difference(coeffs):
    return coeffs is not None and len(coeffs) == 2 and sorted(coeffs) == [-1, 1]


def var_tokens(tok):
    tok = tok.strip()
    if tok.startswith("["):
        body = tok[1:tok.rfind("]")]
        return [x.strip() for x in body.split(",") if x.strip()]
    return [tok]


def scan(path):
    try:
        text = Path(path).read_text(errors="replace")
    except OSError:
        return None
    arrays = {}
    for m in ARR_DECL.finditer(text):
        try:
            arrays[m.group(1)] = [int(x.strip()) for x in m.group(2).split(",") if x.strip()]
        except ValueError:
            pass
    total = hard = cond = 0
    dl_vars = set()
    kinds = {}
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith("constraint"):
            continue
        m = CONSTRAINT.match(line)
        if not m:
            total += 1
            continue
        name, argstr = m.group(1), m.group(2)
        total += 1
        kinds[name] = kinds.get(name, 0) + 1
        args = split_args(argstr)
        if name in CMP_HARD and len(args) == 2:
            hard += 1
            dl_vars.update(a for a in args if not a.lstrip("-").isdigit())
        elif name in CMP_COND and len(args) == 3:
            cond += 1
            dl_vars.update(a for a in args[:2] if not a.lstrip("-").isdigit())
        elif name in LIN_LE and len(args) >= 3:
            if is_difference(parse_coeffs(args[0], arrays)):
                hard += 1
                dl_vars.update(v for v in var_tokens(args[1]) if not v.lstrip("-").isdigit())
        elif name in LIN_COND and len(args) >= 4:
            if is_difference(parse_coeffs(args[0], arrays)):
                cond += 1
                dl_vars.update(v for v in var_tokens(args[1]) if not v.lstrip("-").isdigit())
    return {"total": total, "hard": hard, "cond": cond,
            "dl_vars": len(dl_vars), "kinds": kinds}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", nargs="?", default=".fzn_cache_one_hot")
    ap.add_argument("--top", type=int, default=25)
    args = ap.parse_args()

    files = sorted(Path(args.dir).glob("*.fzn")) + sorted(Path(args.dir).glob("*/*.fzn"))
    if not files:
        print(f"no fzn under {args.dir}", file=sys.stderr)
        return 1
    rows = []
    for f in files:
        r = scan(f)
        if r and r["total"]:
            r["name"] = f.stem
            r["dl_ratio"] = (r["hard"] + r["cond"]) / r["total"]
            rows.append(r)
    print(f"走査 {len(rows)} モデル（{args.dir}）\n")

    n_any = sum(1 for r in rows if r["hard"] + r["cond"] > 0)
    n_hard10 = sum(1 for r in rows if r["hard"] >= 10)
    n_r25 = sum(1 for r in rows if r["dl_ratio"] >= 0.25)
    n_r50 = sum(1 for r in rows if r["dl_ratio"] >= 0.50)
    print("=== 全体 ===")
    print(f"  差分制約を1本でも含む     : {n_any}/{len(rows)}")
    print(f"  hard 差分制約 10本以上     : {n_hard10}/{len(rows)}")
    print(f"  差分制約が全制約の25%以上  : {n_r25}/{len(rows)}")
    print(f"  差分制約が全制約の50%以上  : {n_r50}/{len(rows)}")
    tot_c = sum(r["total"] for r in rows)
    tot_h = sum(r["hard"] for r in rows)
    tot_d = sum(r["cond"] for r in rows)
    print(f"  コーパス全体の制約数       : {tot_c}")
    print(f"    うち hard 差分           : {tot_h} ({100*tot_h/tot_c:.1f}%)")
    print(f"    うち reified/imp 差分    : {tot_d} ({100*tot_d/tot_c:.1f}%)")

    print(f"\n=== DL 密度 上位 {args.top} ===")
    print(f"{'model':<40}{'total':>8}{'hard':>8}{'cond':>8}{'DLvars':>8}{'ratio':>8}")
    for r in sorted(rows, key=lambda x: -x["dl_ratio"])[:args.top]:
        print(f"{r['name']:<40}{r['total']:>8}{r['hard']:>8}{r['cond']:>8}"
              f"{r['dl_vars']:>8}{r['dl_ratio']:>8.2f}")

    print(f"\n=== hard 差分制約の本数 上位 {args.top} ===")
    print(f"{'model':<40}{'total':>8}{'hard':>8}{'cond':>8}{'DLvars':>8}{'ratio':>8}")
    for r in sorted(rows, key=lambda x: -x["hard"])[:args.top]:
        print(f"{r['name']:<40}{r['total']:>8}{r['hard']:>8}{r['cond']:>8}"
              f"{r['dl_vars']:>8}{r['dl_ratio']:>8.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
