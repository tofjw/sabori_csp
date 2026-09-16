# SABORI_* 環境変数フラグ棚卸し (2026-07-10)

環境変数で切り替えられる探索手法・アブレーション・計装の全一覧と得失の判定。
全ブランチ (main / feature/more_constraint / feature/mp / feature/lns /
feature/lcg) を `git grep '"SABORI_'` で横断し、docs-dev の計測記録と
memory の判定と突き合わせた結果。

**TL;DR**: 生きている玉は probe 系3アーム (特に `SABORI_PROBE_ROOT` p2k の
既定ON昇格判定) のみ。他の opt-in は全て「feature/mp ワーカー多様性部品」
としての価値が残るだけ。次の一手は個別アーム深掘りより mp のアーム割当設計。

## A. opt-in の探索アーム（feature/more_constraint）

| 変数 | 内容 | 判定・得失 |
|---|---|---|
| `SABORI_PROBE_ROOT` (=1→予算2000) | 探索前 failed-literal probing。dom=2 変数に両値仮置き→片側矛盾で root 確定。activity 順・最大3ラウンド | **生き残り筆頭**。prize-collecting 492/775 (63%) root 確定。p2k は負けセルなしで唯一の既定ON候補（広域14年ゲート待ち） |
| `SABORI_PROMOTE_IMPACT` / `_PERIOD` (=1→K=32, PERIOD 既定8) | probing 副産物（両分岐 trail 長）を impact 尺度に defined 変数を decision 層へ選別昇格。PROBE_ROOT を暗黙ON | opt-in arm 温存。prize-collecting NONE→SAT (20/31)。無差別昇格（promote_def_bool）の選別版で昇格系唯一の当たり |
| `SABORI_BOTTOMUP` / `_ISOLATE` / `_CUTOFF` (=1→budget2000) | bottom-up optimistic probe（obj 側からの destructive probing） | opt-in arm。グリッド (bench_bottomup.py 2026-07-09): budget=2000, cutoff=/8, isolate=off が G1 net+5・対照害なし。既定ONには届かず |
| `SABORI_CLAUSE_WITNESS` / `_PLAIN` | bool_clause に witness 変数 s（min{i: lit_i 真}）を導入、節への分岐ハンドル＋activity 集約。1WL イベント処理済み | **既定ON不可 確定**（1WL 14年ベンチ net −6、UNSAT 証明で遅延剪定が逆に損）。mp arm 候補として温存、「証明系ワーカーに載せない」前提。詳細: presolve-gap-analysis-20260709.md |
| `SABORI_RESTART_POLICY` | リスタート決定則差し替え (adaptive/inverted/prune_only/depth_only/always_*/scrambled/luby/geometric/constant) | 主にアブレーション用。出荷挙動は stale stats で事実上 always-widen、ライブ信号化しても tighten 不発（経験的 tighten 率 p=0.065）。手法というより診断器 |

## B. opt-in の探索アーム（別ブランチ）

| 変数 | ブランチ | 判定・得失 |
|---|---|---|
| `SABORI_LNS` 系9個 (`_BUDGET/_DESTROY/_FRAC/_FREQ/_ISOLATE/_QUIET/_SCHED/_STALL`) | feature/lns | 健全実装済みだが強制既定で net −35〜−53。機会費用が構造的（iso≈noiso、related<uniform）。opt-in arm 温存 |
| `SABORI_CONFLICT` (`-C`) | feature/mp | 全確定スコープ conflict 学習。健全 (247fzn -a 一致) だが net±0。overhead は枝刈り本体由来で削減不能（節長上限/dedup 両方無効を確認済み）。温存のみ |
| `SABORI_PROMOTE_DEF_BOOL` | feature/mp | bool 無差別昇格。単スレ greedy VBS では良く見えたが実並列 -j4/-j8 A/B で wash〜微負・arithmetic-target 実 regression → **不採用確定、再挑戦しない** |
| `SABORI_LEARN_*` 10個 / `SABORI_AUTO_LCG` / `SABORI_LIN_ORDER` | feature/lcg | LCG (clause 学習)。`-L` 静的 full で確定済みだが**内製 LCG はやらない方針**（2026-07-01 ユーザ確定、外部ソルバーのポートフォリオ持ち込みで代替）。submit も -L なし有利 |
| `SABORI_BUILDORDER_PREPRESOLVE=1` | feature/mp | 旧挙動（presolve 前 build_order）への戻し。交絡再現用で性能玉ではない |
| `SABORI_RESTART` / `SABORI_RESTART_SCALE` / `SABORI_THREADS` / `SABORI_WORKER_FULLSOLVE` | feature/mp | ポートフォリオ基盤の構成変数（手法ではない） |

## C. 既定ON機能のアブレーション（=0 で切る kill switch）

新手法ではなく既存機能の計測用オフスイッチ。

| 変数 | 切る対象 |
|---|---|
| `SABORI_NOGOOD=0` | decision-trail NoGood 学習＋伝播 |
| `SABORI_BLOOM=0` | NoGood-Bloom 重なりタイブレーク |
| `SABORI_GRADIENT=0` | 擬似勾配ヒント（値順序バイアス） |
| `SABORI_ONEHOT=0` | one-hot チャネル集約 presolve |
| `SABORI_TEMPORAL=0` | temporal_activity（Last Conflict 系・変数選択第1基準） |
| `SABORI_PROBE=0` | improvement probe（最適化の軽量サブ探索） |
| `SABORI_DECVAR_BUMP=0` | 決定変数の handle_failure bump |
| `SABORI_NG_NOBUMP=1` / `SABORI_NG_LEARN_BUMP=0` / `SABORI_NG_PROP_BUMP=0` | NoGood 由来 bump の段階別カット |
| `SABORI_BUMP_MODE` (0/1/2) / `SABORI_BUMP_STRUCT_ONLY=<name>` | 制約側 activity 配分の切り替え・制約別切り分け |
| `SABORI_DEDUP=0` | fzn 側の制約重複除去 |
| `SABORI_SEED` / `SABORI_FIX_MIXP` | RNG シード差し替え / mix_p 固定（バンディット適応停止） |

## D. 計装・診断専用（性能手法ではない）

- `SABORI_NG_AUDIT` / `SABORI_DUMP_SOL` / `SABORI_PRINT_OBJ` (feature/mp) — 計装
- `SABORI_BOUND_EXPL` (feature/mp) — bound-literal 説明の再現ハーネス。
  **不健全確定（偽 UNSAT 実証済み）・出荷禁止**

## 効果が重複するグループ

1. **probe 3兄弟**: `PROBE_ROOT` / `BOTTOMUP` / `PROMOTE_IMPACT` は同じ
   probing 基盤の変種。単体重ねがけより **mp でワーカー分化が本命**。
2. **昇格系**（死んだ activity にハンドルを与える同一発想）:
   `PROMOTE_IMPACT` / `PROMOTE_DEF_BOOL` / LCG `-A` / `CLAUSE_WITNESS`。
   4つ中3つが不採用〜温存止まり、生き残りは選別付き `PROMOTE_IMPACT` のみ。
   witness の残り玉「s を defined に置き impact 昇格に選別させる」は両者の
   合流案で期待値逓減と自己判定済み。
3. **学習系**: `SABORI_NOGOOD`（既定ON）/ `-C` conflict 学習 / LCG `LEARN_*`。
   既定 NoGood の上に載せる2つはどちらも net±0 以下。学習強化は外部ソルバー
   ポートフォリオ路線で代替する方針。
4. **最適化時サブ探索**: `SABORI_PROBE`（既定ON）と `SABORI_LNS` は incumbent
   改善の役割が重複、LNS 側の機会費用が構造的に不利。

## 見込みの薄いもの（再挑戦に新根拠が必要）

- `SABORI_CLAUSE_WITNESS`（min 版の既定用途）— 1WL 最適化まで尽くして net −6 確定
- `SABORI_PROMOTE_DEF_BOOL` — 実並列で反証済み・再挑戦しない
- LCG 系一式（`-L`/`-A` 含む）— 方針レベルで撤退済み
- `SABORI_BOUND_EXPL` — 不健全、性能議論の対象外
- `SABORI_RESTART_POLICY` の adaptive 系 — tighten 信号が死んでおり生かしても効かない

## mp 統合時の整理方針 (2026-07-10 議論、ユーザ合意)

env 変数はプロセスグローバルなので、アームをワーカーごとに変える mp では
機構自体が破綻する。統合時の整理は「削減」と「移設」の二段構え。
主目的はフラグ数削減ではなく **「パース一箇所・本籍は WorkerConfig・
env はデバッグ用上書き」への構造転換**（CP-SAT がパラメータ数百でも
回るのは単一 Parameters 構造体への集約ゆえ。問題は数でなく散らばり）。

### 1. 統合前に削除するもの
- `SABORI_PROMOTE_DEF_BOOL` — 実並列で反証済み・再挑戦しない。WorkerConfig 側も削除
- `SABORI_BOUND_EXPL` — 不健全確定ハーネス。残存自体が事故の種
- `SABORI_CLAUSE_WITNESS` の min/plain 二本立て — 残すなら mp arm 用途の一本に絞る
- 実験完結した微細計装: `NG_LEARN_BUMP` / `NG_PROP_BUMP` / `BUMP_STRUCT_ONLY`
  （一回性のアブレーション器具。削除前に `pre-flag-purge` タグを打てば復元可能）
- `RESTART_POLICY` の派生 policy 群 — 診断用に luby / scrambled 程度を残し他は畳む

**実施記録 (2026-07-11, feature/more_constraint 分)**: タグ `pre-flag-purge`
(2821fe1) を打ち、`NG_LEARN_BUMP` / `NG_PROP_BUMP` / `BUMP_STRUCT_ONLY`
(structural_mask 機構ごと) / `RESTART_POLICY` 派生7種 (adaptive/scrambled/luby
のみ残存、Adaptive は既定 enum 値なので温存) / `CLAUSE_WITNESS_PLAIN`
(**plain に一本化**・min 意味論削除、-a 解重複のためコメントで単解/最適化専用と
明記) を削除。293 ctest + golden 260 全緑、witness スモークテスト一致。
`PROMOTE_DEF_BOOL` / `BOUND_EXPL` は feature/mp にのみ存在するため mp 統合作業時に削除。

### 2. WorkerConfig へ移設するもの（env は薄い上書きに格下げ）
生き残りアーム: `PROBE_ROOT` / `BOTTOMUP` / `PROMOTE_IMPACT` / (witness) /
`LNS` / `-C`。mp のアーム割当が本籍。env は「単スレデバッグで worker0 相当を
上書きする口」のみ残す。コンストラクタに散在する getenv を config モジュール
一箇所に集約する。

### 3. 恒久的な診断スイッチとして残すもの
`SEED` / `FIX_MIXP` / `NOGOOD` / `TEMPORAL` / `GRADIENT` / `ONEHOT` / `PROBE`
の大玉 kill switch。健全性事故のトリアージ（LCG 事件・bound-nogood 事件型の
「どの機構を切ると症状が消えるか」二分探索）で毎回使う道具。

### 根拠と注意
- フラグ温存の実害はメンテコストより**交絡**（OneHotChannelAggregator の
  imp 非集約、buildorder presolve 前後差の前例）。25 bool = 2^25 の構成空間で
  テスト済みは既定+数構成のみ。空間を狭めること自体に品質価値がある
- フラグ読み取りの削除は既定値不変なら軌道不変 → golden で機械的に検証可能
- **WorkerConfig への移設は初期化順が変われば軌道可変**（buildorder の前例）
  → golden green だけでなくゲートベンチ併走で審査

## 関連

- probe 系・witness の計測詳細: presolve-gap-analysis-20260709.md
- 大負け分類: loss-taxonomy-20260709.md
- ポートフォリオ再チューニング手順: portfolio-tuning-guide.md
