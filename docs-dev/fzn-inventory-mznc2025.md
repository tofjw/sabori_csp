# FZN 棚卸: MiniZinc Challenge 2025 の負け・両者タイムアウト問題 (2026-07-06)

## 背景

シングルスレッド CP-SAT (`-p 1`) との 2025 年比較 (60s, データファイル名前順中央インスタンス,
`sabori_benchmark_2025_20260706_205650.html`) の結果:
**Sabori 11 勝 / CP-SAT 4 勝 / Tie 2 / 両者解なし 3**。
パターンは既知テーゼどおり「改善で勝ち、証明で負ける」。

負け・両者タイムアウトの 6 問について、「どの global がどう分解されているか」を
sabori mznlib と CP-SAT mznlib の両方で FZN にコンパイルして比較した。
狙いは、負けを (a) 証明支配 / (b) 分解・伝播 / (c) obj 停滞 / (d) スループット に
分類し、(b) だけを native propagator 実装の候補にすること。

補足: fzn-cp-sat は `-p` 省略時も `num_workers: 1` に解決される（`--threads` default 0 → 1、
警告つき）。つまり従来の bench_compare 結果も実質シングルスレッド CP-SAT との比較だった。

## 手法

```bash
minizinc --solver <msc> -c --fzn out.fzn --no-output-ozn <mzn> <data>
grep -oP '^constraint \K\w+' out.fzn | sort | uniq -c | sort -rn
```

- sabori 側: `build/share/minizinc/solvers/sabori_csp.msc`（feature/more_constraint、
  bin_packing_load native 追加後、reconfigure 済み）
- CP-SAT 側: `--solver cp-sat`（/snap/minizinc 同梱 OR-Tools 9.15）

## 結果一覧

| 問題 | sabori FZN | CP-SAT FZN | 診断 |
|---|---|---|---|
| gt-sort | **flatten 550s 打ち切りでも未完**（`nosets.mzn` で set→bool 分解爆発） | 19,348 制約。set_card/set_in/set_union が native | set 変数非対応が敗因のすべて。60s ベンチでは flatten 段階で死亡 |
| mondoku | 2,421 制約。GCC→`fzn_count_eq`×408 + int_eq_reif/bool_clause 網 | 1,178 制約。`ortools_global_cardinality`×48 native | **global_cardinality 欠落**。モデルは GCC 中心 (`mondoku-gcc-model-balance.mzn`) |
| groupsplitter | 168 制約、`sabori_table_int`×75 native、15.7MB | 168 制約、`ortools_table_int`×75。**構成が完全一致** | 分解差ゼロ。table は Compact Table 実装済 → 純粋に探索側の負け |
| hitori | 6,386 制約（`int_eq_reif`×1687） | 6,008 制約（`int_eq_imp`×1484 = half-reified） | ほぼ同一。差は half-reification のみ |
| ihtc-2024-marte | 29,384 制約（fzn_nvalue×151, fzn_cumulative×10, sabori_bin_packing_load×8 native） | 25,434 制約 | native カバレッジ良好。両者 TIMEOUT = 単純に難問 |
| products-and-shelves | 195,652 制約（`int_lin_le_imp`×146,985） | 187,447 制約（同 ×146,994） | 両者とも `fzn_diffn_nonstrict_k`(3D) を O(n²) pairwise 分解。両者 TIMEOUT |

### 詳細メモ

- **gt-sort** は sorting network の merge を `array[L,_] of var set` + `card()` +
  union で書いた set モデル。`redefinitions.mzn` 冒頭の `include "nosets.mzn"` で
  bool 配列に落ちるが、set_union×3657 + set_card×3718 相当の分解が flatten を殺す。
  CP-SAT は数秒で flatten + 6.1s で OPTIMAL 証明。
- **int_eq_imp** は parser/registry には実装済み (`make_int_eq_imp`,
  `bool_eq_imp` も同じ factory) だが、redefinitions.mzn の predicate 宣言が
  コメントアウトされている (f8c701b 2026-03-05 "little bit slow and disabled")。
  当時から propagator 核は大きく変わっており再計測候補。
- **products-and-shelves** は `diffn_nonstrict_k`（3 次元・棚ごと）で、std 分解が
  box ペアごとに `int_lin_le_imp`×4 + bool_clause を生む。CP-SAT も native の
  k 次元版を持たず同じ分解 → 差別化機会ではあるが両者未達の難問。

## 負け分類と対処方針

| 分類 | 問題 | 対処 |
|---|---|---|
| (a) 証明・パラダイム支配 | gt-sort (set), mondoku の証明部分 | 射程外。外部ポートフォリオ方針 ([[solver-positioning-activity-vs-external]]) |
| (b) 分解・伝播 | **mondoku (GCC)**, hitori (half-reif 微差) | native 実装候補 |
| (c) 探索 | **groupsplitter**（FZN 同一で負け） | `-v`/`-c` で巨大 table 上の activity 挙動を調査 |
| (d) 両者難問 | ihtc-2024-marte, products-and-shelves | 静観 |

## 提案リスト（優先度順）

1. **`fzn_global_cardinality` native 実装** — 小粒で確実。counting 系
   (bin_packing_load と同型)。mondoku の FZN が半減、count_eq×408 の reified 網が
   propagator×48 に置換。→ **本日着手**
2. **`int_eq_imp` 再有効化の再計測** — 実装済み。宣言 1 行 + A/B ベンチ。
3. **set 変数対応は「やらない」を明示** — 大工事かつ CP-SAT/Gecode の土俵。
   頻出 `fzn_set_*` サブセットだけ効率的 bool 分解に差し替える中間案のみ残す。
4. **`fzn_diffn_nonstrict_k` は見送り** — CP-SAT も同分解で同じく TIMEOUT。

## 教訓

- 「未サポート global の棚卸」は機械的にできて、提案が (a)〜(d) に自動分類される。
  LCG 系提案バイアス（文献平均への回帰）を避けて、パラダイム内の改善候補だけを
  抽出する方法として有効だった。
- FZN が byte 単位で同一なのに負ける問題（groupsplitter）が見つかるのが副産物。
  制約実装ではなく探索調査に回せる。
