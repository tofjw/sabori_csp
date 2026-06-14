# リファクタリング検証 baseline (2026-06-14)

フェーズ0 成果物。以降の全フェーズはこの baseline に対する**非劣化**で合否判定する。
計画: [refactoring-plan-2026-06.md](refactoring-plan-2026-06.md)

## 対象バイナリ
- ブランチ: `main`
- コミット: `0bc7a86 Batch-schedule disjunctive bounds propagation and edge-finding`
- ビルド: Release (`cmake --build build`)、2026-06-14 クリーンリビルド
- 対戦相手: OR Tools CP-SAT 9.15（minizinc 経由）

## ゲーティングベンチ（非劣化ゲートの基準）

各スクリプトは起動時に自前で stale プロセスを停止、4並列。**汚染回避のため3本は逐次実行**
（並行実行するとマージナル行が contention で反転する。cf memory: bench_circuit_volatility）。

| ベンチ | TIMEOUT | Sabori Wins | CP-SAT Wins | Tie | Sabori TO | CP-SAT TO |
|--------|--------:|------------:|------------:|----:|----------:|----------:|
| bench_alldiff  | 40s | **6**  | 10 | 2 | 0 | 1 |
| bench_circuit  | 30s | **9**  | 5  | 2 | 0 | 2 |
| bench_diffn    | 30s | **7**  | 6  | 8 | 2 | 3 |

**ゲート判定基準**: 各ベンチで Sabori Wins 同等以上 かつ Sabori Timeouts 同等以下。
マージナル差はノイズなので、際どい場合は同一 fzn の直接実行で決定論比較すること。

> 注: 本日午前に並行実行で取得した汚染版は alldiff 5/diffn 6 と各1勝低く出ていた。
> 上表は逐次クリーン実行の正値。

## ctest
- 結果: **200 tests passed, 0 failed**
- 実行時間: **1.69s**（Release ビルド）

## ゴールデンマスター
- `tests/golden/run_golden.sh`（record/check/list）、corpus 182 fzn、`expected/*.txt`
- 検証: `check` で **pass=182 / fail=0（ALL GREEN）**（現行 main バイナリと一致）
- 指紋 = `-a -s` の全解出力 + `% Stats:` / `% NG length` 行（timing 非含・決定論的）
- 純リファクタの合否はこれの完全一致で判定

## 未取得（フェーズ0 残）
- gprof 再プロファイル（別途、docs-dev に記録予定）
- `bench_compare.py` 全問題セット baseline（重いため任意。必要時に追加）
