# find_all 解列挙 設計レビュー (2026-06-16)

リファクタ計画 v2 Phase 6 の保留項目「find_all + restart の解列挙設計レビュー」の結論。

## 対象

`Solver::solve_all`（`-a` 全解探索）は `restart_enabled_` で **2つの列挙エンジン** を切り替える。

| | restart_enabled_ = true（既定） | restart_enabled_ = false |
|---|---|---|
| 実装 | `search_with_restart` + solution NoGood | `run_search` 明示スタック DFS（Enumerate モード） |
| 仕組み | 解を見つける → permanent NoGood で封鎖 → root へ backtrack → 再探索（restart 込み） | 古典的 DFS。解に到達したら backtrack して次へ。NoGood 非依存 |
| 完全性の根拠 | NoGood が「見つけた解ちょうど1つ」を封鎖し、restart/GC を跨いで永続すること | 探索木の網羅（自明に完全・重複なし） |
| メモリ | **O(#解)**（permanent NoGood が解ごとに蓄積、GC されない） | O(探索深さ) |

`fzn_sabori -a` は既定 `restart_enabled_ = true` なので **restart+NoGood 経路** を使う。
golden master（182 fzn の `-a -s`）もこの経路を検証している。

## 健全性評価

### 完全性・重複なし: 検証済み（OK）
restart+NoGood 経路の主要な事故モード = 「同じ解を封鎖し損ねて無限に再発見」
（2026-06-10 の root 確定変数 watch 無限ループバグはこれ）。これを直接ガードする独立オラクル
照合を新設した（`tests/cpp/test_find_all_consistency.cpp`）:

- 2エンジン（restart on/off）が **同一の解集合** を返すことを REQUIRE。
- 各エンジンが **重複報告ゼロ**（報告数 == 集合サイズ）であることを REQUIRE。
- 解析的に既知のケースは解数も照合。

カバー: 単一自由変数（unit NoGood 縮退）/ all_different 順列（6・24 解, NoGood 大量蓄積）/
定数変数の NoGood 除外 / 線形等式 / presolve 一意確定（backtrack 後 all-assigned 終了路）/ UNSAT。
→ 241/241 green。**設計上の完全性・重複なしは独立オラクルで担保された。**

### 終了性（OK）
`handle_find_all_solution` の各終了路を確認:
- backtrack 後も全変数確定（presolve が解き切った／root 伝播で確定）→ 即 Stop（再代入余地なし）。
- unit NoGood 伝播で全確定する内側ループも、確定 → 検証 → 報告 → 封鎖 → 再 backtrack で進む。
- 解が尽きると `run_search` が UNSAT を返し（蓄積 NoGood が空間を潰す）ループ終了。

### restart との相互作用（OK）
solution NoGood は `permanent = true`。GC（`NoGoodManager::gc`）は permanent を削除せず
（`if (ng->permanent) return false` / 容量管理ループも permanent で break）、ソートで先頭に固定。
→ restart や NoGood GC を跨いでも封鎖済みの解は封鎖されたまま = 重複しない。

## 設計上の制約（既知・要判断）

**メモリと伝播コストが O(#解) でスケールしない。**
解ごとに permanent NoGood が増え、GC されない。さらに後続の探索ノードは蓄積した全 solution
NoGood に対して watch/bloom 照合を行うため、伝播コストも解数とともに増える。
→ **高解数の全解列挙には restart+NoGood 経路は不向き**（DFS 経路は O(深さ) メモリで素直）。

現状の許容理由:
- `restart_enabled_` はソルバ全体の既定フラグ（探索性能のため true）で、`solve_all` はそれを
  踏襲しているだけ。restart の activity 学習 + NoGood 学習は「充足解に到達しにくい難問」で
  素朴 DFS より速く解へ届けることがある。
- 実運用の `-a` は「全解を数える」より「代表解 / 最初の数解」用途が多く、解数は中規模に留まる。

## 推奨（将来の判断事項）

1. **【採用済み】独立オラクル照合テストを安全網として常設**（本コミットで完了）。
   今後 NoGood / restart / 列挙制御に触る変更のリグレッション網になる。
2. **【保留・要ユーザー判断】高解数列挙は DFS 経路へ切替える選択肢**。
   `solve_all` を find_all 時のみ `restart_enabled_` に関わらず DFS に固定すれば
   メモリ O(深さ)・重複ロジック非依存になる。ただし:
   - 解集合は不変（本テストで証明済み）だが **解の出力順が変わる** → golden 182 本の
     再録が必要。ユーザーが解順に依存していないかの確認も要る。
   - よって純リファクタの範囲外。性能/メモリが実問題になった時点で別途検討。

## 結論

restart+NoGood 経路の **find_all 列挙は完全・重複なし・終了する**（独立オラクルで担保）。
設計上の唯一の未解決点は「高解数でのメモリ/伝播コストの O(#解) スケール」で、これは
正しさの問題ではなく性能トレードオフ。対処（DFS 既定化）は golden 再録 + 解順変更を伴うため
ユーザー判断待ちの将来項目とする。**Phase 6 のレビュー項目はクローズ。**

## 関連
- `src/core/solver_search.cpp`: `search_with_restart` / `handle_find_all_solution`
- `src/core/solver_frame.cpp`: `run_search` / `handle_solution` / `try_enumerate_values`
- `src/core/nogood_manager.cpp`: `collect_solution_literals` / `add_solution_nogood` / `gc`
- `tests/cpp/test_find_all_consistency.cpp`（新規, commit 8eaa808）
