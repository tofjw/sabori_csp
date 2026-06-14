# MiniZinc Challenge 2025 必要制約一覧

更新日: 2026-02-04

## 概要

`benchmarks/minizinc_challenge_2025/mznc2025_probs/` 以下の20問題を解くために必要な制約を分析。

**調査方法**: `sabori_csp.msc` を使用して MiniZinc から FlatZinc に変換。未実装のグローバル制約は MiniZinc によって primitive な制約に分解されるため、実際に必要な制約のみが抽出される。

## 必要制約と実装状況

### 使用頻度順一覧（全20問題合計）

| 制約名 | 使用回数 | 実装状況 |
|--------|---------|---------|
| `int_le_reif` | 155,513 | ✅ 実装済 |
| `bool2int` | 150,294 | ✅ 実装済 |
| `array_bool_and` | 100,862 | ✅ 実装済 |
| `int_eq_reif` | 43,034 | ✅ 実装済 |
| `int_lin_le_reif` | 42,187 | ✅ 実装済 |
| `int_times` | 39,759 | ✅ 実装済 |
| `int_lin_eq_reif` | 26,099 | ✅ 実装済 |
| `int_lin_eq` | 21,947 | ✅ 実装済 |
| `bool_clause` | 19,925 | ✅ 実装済 |
| `set_card` | 16,982 | ❌ 未実装 (集合制約) |
| `set_in` | 16,942 | ❌ 未実装 (集合制約) |
| `set_union` | 16,898 | ❌ 未実装 (集合制約) |
| `int_lin_le` | 7,836 | ✅ 実装済 |
| `array_int_element` | 3,438 | ✅ 実装済 |
| `set_in_reif` | 1,638 | ❌ 未実装 (集合制約) |
| `int_max` | 1,521 | ✅ 実装済 |
| `int_lin_ne_reif` | 1,193 | ✅ 実装済 |
| `array_var_int_element` | 883 | ✅ 実装済 |
| `int_ne_reif` | 840 | ✅ 実装済 |
| `array_var_bool_element` | 220 | ✅ 実装済 |
| `bool_eq` | 192 | ✅ 実装済 |
| `bool_not` | 179 | ✅ 実装済 |
| `int_min` | 134 | ✅ 実装済 |
| `array_var_set_element` | 28 | ❌ 未実装 (集合制約) |
| `int_lin_ne` | 12 | ✅ 実装済 |
| `int_abs` | 12 | ✅ 実装済 |
| `int_ne` | 7 | ✅ 実装済 |
| `fzn_all_different_int` | 7 | ✅ 実装済 |
| `int_eq` | 3 | ✅ 実装済 |
| `fzn_circuit` | 2 | ✅ 実装済 |

---

## 未実装制約（優先度順）

### Priority 3: 集合制約（gt-sort 問題専用）

| 制約名 | 使用回数 | 説明 |
|--------|---------|------|
| `set_card` | 16,982 | \|S\| = n |
| `set_in` | 16,942 | x ∈ S |
| `set_union` | 16,898 | S1 ∪ S2 = S3 |
| `set_in_reif` | 1,638 | (x ∈ S) <-> b |
| `array_var_set_element` | 28 | 集合配列の可変インデックス |

**注**: 集合制約は gt-sort 問題でのみ使用。集合変数の実装が必要なため優先度は低い。

---

## 実装済み制約一覧

### 比較制約
- `int_eq`, `int_eq_reif`, `int_ne`, `int_ne_reif`, `int_lt`, `int_le`, `int_le_reif`, `int_max`, `int_min`

### 算術制約
- `int_times`, `int_abs`

### Bool 制約（int 制約のエイリアス）
- `bool2int`, `bool_eq`, `bool_ne`, `bool_lt`, `bool_le`
- `bool_eq_reif`, `bool_le_reif`
- `bool_lin_eq`, `bool_lin_le`

### 論理制約
- `array_bool_and`, `array_bool_or`, `bool_clause`, `bool_not`

### グローバル制約
- `fzn_all_different_int`, `fzn_circuit`
- `int_lin_eq`, `int_lin_eq_reif`, `int_lin_le`, `int_lin_le_imp`, `int_lin_le_reif`, `int_lin_ne`, `int_lin_ne_reif`
- `array_int_element`, `array_var_int_element`, `array_var_bool_element`, `array_int_maximum`, `array_int_minimum`

---

## 実装ロードマップ

### Phase 1: 基本 reified 制約
1. ✅ ~~`bool2int`~~ - 実装完了
2. ✅ ~~`int_lin_eq_reif`~~ - 実装完了
3. ✅ ~~`int_lin_ne_reif`~~ - 実装完了
4. ✅ ~~`int_ne_reif`~~ - 実装完了
5. ✅ ~~`bool_not`~~ - 実装完了

### Phase 2: 可変インデックス element 制約
1. ✅ ~~`array_var_int_element`~~ - 可変インデックス版（883回使用）実装完了
2. ✅ ~~`array_var_bool_element`~~ - Bool 配列版（220回使用）実装完了（int版を流用）

### Phase 3: その他
1. ✅ ~~`int_abs`~~ - 絶対値（12回使用）実装完了

### Phase 4: 集合制約（オプション）
- gt-sort 問題のみで使用
- 集合変数の実装が必要なため、優先度は低い

---

## 問題別の実行可能性

集合制約を除く全ての必要制約を実装すれば、**19/20 問題**が実行可能になる見込み。

| 問題 | 状況 |
|------|------|
| gt-sort | 集合制約が必要 |
| その他19問題 | Phase 1-3 の実装で対応可能 |

---

## 注記

- この分析は `sabori_csp.msc` を使用した FlatZinc 変換に基づく
- 未実装のグローバル制約は MiniZinc が自動的に primitive 制約に分解
- 使用回数は全20問題の合計（各問題1インスタンスのみ）
