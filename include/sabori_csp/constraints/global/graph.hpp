#ifndef SABORI_CSP_CONSTRAINTS_GLOBAL_GRAPH_HPP
#define SABORI_CSP_CONSTRAINTS_GLOBAL_GRAPH_HPP

#include "sabori_csp/constraint.hpp"
#include <unordered_map>
#include <numeric>
#include <vector>
#include <memory>

namespace sabori_csp {


/**
 * @brief circuit制約: 変数がハミルトン閉路を形成する
 *
 * 変数 x[0], x[1], ..., x[n-1] がハミルトン閉路を形成する。
 * x[i] = j は「ノード i の次はノード j」を意味する。
 *
 * 確定済みエッジが作るパスを端点リンク方式で管理:
 * - partner_[端点]: 反対側の端点（端点でのみ有効）
 * - size_[h]: head h のパスのノード数
 * - 未確定変数は必ずパスの tail、入次数 0 のノードは必ずパスの head
 *   なので、全操作が O(1)
 *
 * AllDifferent の性質も内包:
 * - 各ノードの入次数は最大1（同じ値は2回使えない）
 *
 * サブサーキット検出と枝刈り:
 * - x[i] = j のとき、partner_[i] == j なら同じパス内で閉路形成。
 *   size < n ならサブサーキットで false
 * - パス結合後 size < n なら tail から head へ戻る値を除去（事前枝刈り）
 */
class CircuitConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param vars 制約に関与する変数リスト（インデックス 0 から n-1）
     */
    explicit CircuitConstraint(std::vector<VariablePtr> vars);

    std::string name() const override;


    bool prepare_propagation(Model& model) override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;

    /**
     * @brief 残り1変数になった時の伝播
     *
     * 利用可能な値が1つだけなら、その値で確定させる。
     */
    bool on_last_uninstantiated(Model& model, int save_point,
                                 size_t last_var_internal_idx) override;

    /**
     * @brief バッチ伝播: SCC フィルタリング（強連結性 + 入次数ルール）を1回実行
     */
    bool propagate_batch(Model& model, int save_point) override;

    /**
     * @brief 指定セーブポイントまで状態を巻き戻す
     */
    void rewind_to(int save_point);

    /**
     * @brief 現在のプールサイズを取得
     */
    size_t pool_size() const { return pool_n_; }

    /**
     * @brief trigger と同じ値に確定した変数だけを bump する
     */
    void bump_activity(const Model& model, size_t trigger_var_idx,
                       double* activity, double activity_inc,
                       bool& need_rescale, std::mt19937& rng) const override;

protected:
    /**
     * @brief 初期整合性チェック
     */


private:
    size_t n_;  // ノード数
    Domain::value_type base_offset_;  // 1-based インデックスのオフセット（通常は1）

    // パス管理（端点リンク方式）
    // partner_[x] はパスの端点 x でのみ有効で、反対側の端点を指す。
    // 未確定変数 i は必ずパスの tail、入次数 0 のノード j は必ずパスの head
    // なので、結合・閉路判定は O(1) で行える。
    std::vector<size_t> partner_;  // partner_[端点] = 反対側の端点
    std::vector<size_t> size_;     // size_[h] = head h のパスのノード数（head でのみ有効）

    // occupier_[j] = ノード j へ確定済みエッジを張っている変数の内部インデックス
    //（なければ SIZE_MAX。AllDifferent チェックと activity bump に使用）
    std::vector<size_t> occupier_;

    // 未確定変数カウント（差分更新用）
    size_t unfixed_count_;

    // 値プール（Sparse Set、内部インデックス 0..n-1 を格納）
    std::vector<Domain::value_type> pool_;
    std::vector<size_t> pool_idx_;  // pool_idx_[内部インデックス] = pool_ 内の位置
    size_t pool_n_;

    // Trail
    struct TrailEntry {
        size_t i;             // 確定した変数（結合前のパスの tail）
        size_t j;             // 確定値の内部インデックス（結合前のパスの head）
        size_t h1;            // 結合後パスの head
        size_t t2;            // 結合後パスの tail
        size_t old_size_h1;
        size_t old_pool_n;
        size_t old_unfixed_count;
        bool is_merge;  // パス結合かどうか（false なら閉路形成）
    };
    std::vector<std::pair<int, TrailEntry>> trail_;

    /**
     * @brief プールから内部インデックスを削除（Sparse Set でO(1)）
     */
    void remove_from_pool(size_t value);

    /**
     * @brief ドメインが張る有向グラフを CSR 形式で構築
     *
     * 使用不能なエッジ（自己ループ、入次数確定済みノードへのエッジ、
     * サブサーキットを閉じるエッジ）は除外する。
     * 同時に入次数 (scc_indeg_) と唯一の前任候補 (scc_pred_) を数える。
     *
     * @param use_path_state true なら探索中（occupier_/partner_ が最新）、
     *                       false なら presolve（model から occupied を再計算）
     * @return 出次数 0 のノードを検出したら false
     */
    bool build_scc_graph(Model& model, bool use_path_state);

    /**
     * @brief 構築済みグラフが単一の強連結成分か判定（Tarjan, 反復実装）
     *
     * ハミルトン閉路が存在するにはドメイングラフが強連結である必要がある。
     */
    bool scc_single_component();

    // ===== SCC スクラッチ（毎回再構築、trail 不要）=====
    std::vector<int> scc_adj_head_;   // CSR: ノードごとの開始位置 (n+1)
    std::vector<int> scc_adj_;        // CSR: エッジ先ノード
    std::vector<int> scc_index_, scc_low_;       // Tarjan
    std::vector<int> scc_stack_, scc_dfs_stack_, scc_edge_pos_;
    std::vector<char> scc_onstack_;
    std::vector<int> scc_indeg_, scc_pred_;      // 入次数と唯一の前任候補
    std::vector<char> scc_occupied_;             // presolve 用: 前任確定済みノード
};


/**
 * @brief subcircuit制約: 変数が単一の部分閉路を形成する
 *
 * 変数 x[0], ..., x[n-1] は 1..n（base_offset ずれ）の順列で、x[i] = j は
 * 「ノード i の次はノード j」を表す。x[i] = i（自己ループ）は「ノード i は
 * 閉路に含まれない（out）」を意味し、x[i] != i のノード（in）が過不足なく
 * 単一の閉路を成す。全ノードが自己ループ（空の部分閉路）も充足解。
 *
 * circuit との違い:
 * - 自己ループを許容し、それを out ノードとして扱う。
 * - 閉路が全 in ノードを含むかで妥当性を判定（circuit の size==n に対し
 *   subcircuit は size==in_count_、in_count_ は「非自己ループの確定エッジ数」）。
 * - 妥当な閉路が閉じた時点で、残りの未確定ノードを全て自己ループ（out）に強制。
 *
 * alldifferent は暗黙に含意（順列）。registry で AllDifferent を併設する。
 */
class SubcircuitConstraint : public Constraint {
public:
    explicit SubcircuitConstraint(std::vector<VariablePtr> vars);

    std::string name() const override;

    bool prepare_propagation(Model& model) override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;

    bool on_last_uninstantiated(Model& model, int save_point,
                                 size_t last_var_internal_idx) override;

    /**
     * @brief バッチ伝播: 到達可能性フィルタを1回実行
     */
    bool propagate_batch(Model& model, int save_point) override;

    void rewind_to(int save_point) override;

    void bump_activity(const Model& model, size_t trigger_var_idx,
                       double* activity, double activity_inc,
                       bool& need_rescale, std::mt19937& rng) const override;

private:
    size_t n_;
    Domain::value_type base_offset_;

    // パス管理（circuit と同じ端点リンク方式。in ノードのパスのみを扱う）
    std::vector<size_t> partner_;   // partner_[端点] = 反対側の端点
    std::vector<size_t> size_;      // size_[head] = head のパスの in ノード数
    std::vector<size_t> occupier_;  // occupier_[j] = ノード j に確定エッジを張る変数（自己ループ含む）

    size_t unfixed_count_;  // 未確定変数の数
    size_t in_count_;       // 非自己ループの確定エッジ数（= 閉路に含まれる in ノードの数）

    // 値プール（Sparse Set、alldifferent 用）
    std::vector<Domain::value_type> pool_;
    std::vector<size_t> pool_idx_;
    size_t pool_n_;

    // Trail
    struct TrailEntry {
        size_t i;             // 確定した変数
        size_t j;             // 確定値の内部インデックス
        size_t h1;            // merge 前の i のパスの head
        size_t t2;            // merge 前の j のパスの tail
        size_t old_size_h1;
        size_t old_pool_n;
        size_t old_unfixed_count;
        size_t old_in_count;
        uint8_t kind;         // 0=self-loop(out), 1=merge, 2=closure
    };
    std::vector<std::pair<int, TrailEntry>> trail_;

    void remove_from_pool(size_t value);
    void rebuild_state(Model& model);  // ctor / prepare_propagation 共通の状態再構築

    /**
     * @brief 到達可能性フィルタ（必須ノードの相互到達性による刈り込み）
     *
     * 閉路は全ての必須ノード（自己ループ不可のノード）を通らなければならない。
     * よって必須ノード m0 から前向き・後ろ向きの両方で到達できるノードだけが
     * 閉路に入りうる。それ以外のノードは自己ループ（out）に強制できる。
     *
     * ステートレス（毎回モデルから再構築）で trail を持たないため backtrack 安全。
     * 既定 OFF。SABORI_SUBCIRCUIT_REACH=1 で有効化する opt-in 機能。
     *
     * @param in_presolve presolve 中は Domain を直接操作、探索中は enqueue する
     * @return false なら矛盾
     */
    bool filter_reachability(Model& model, bool in_presolve, bool* changed = nullptr);

    // filter_reachability の作業バッファ（呼び出しごとの再確保を避ける）
    std::vector<uint8_t> reach_fwd_;
    std::vector<uint8_t> reach_bwd_;
    std::vector<size_t> reach_stack_;
    std::vector<size_t> succ_start_;   // CSR: ノード i の後続の開始位置
    std::vector<size_t> succ_list_;    // CSR: 後続ノード列（自己ループ除く）
    std::vector<size_t> pred_start_;
    std::vector<size_t> pred_list_;
    std::vector<uint8_t> frag_occupied_;  // 確定弧が入っているノード
    std::vector<size_t> frag_succ_;       // 確定した非自己ループ後続
    std::vector<size_t> frag_id_;         // ノード -> 所属断片（SIZE_MAX = 自由）
    std::vector<size_t> frag_head_;
    std::vector<size_t> frag_tail_;
};


// ============================================================================
// Inverse constraint
// ============================================================================

/**
 * @brief inverse 制約: f[i] = j <-> invf[j] = i
 *
 * f と invf は同サイズの配列で、互いに逆関数の関係を持つ。
 * 暗黙的に f と invf は各々 all_different。
 * FlatZinc では 1-indexed（値域は 1..n）。
 *
 * var_ids_ レイアウト: [f[0], ..., f[n-1], invf[0], ..., invf[n-1]]
 */
class InverseConstraint : public Constraint {
public:
    /**
     * @param f       配列 f
     * @param invf    配列 invf
     * @param f_offset    f の値域の最小値 = min(index_set(invf))。f[i] の値を invf の内部
     *                    インデックスに変換するために使用 (j = v - f_offset)。
     * @param invf_offset invf の値域の最小値 = min(index_set(f))。invf[i] の値を f の内部
     *                    インデックスに変換するために使用。
     *
     * 通常の対称ケース (FlatZinc 1-indexed): f_offset = invf_offset = 1
     * 0-indexed: f_offset = invf_offset = 0
     */
    InverseConstraint(std::vector<VariablePtr> f, std::vector<VariablePtr> invf,
                      int64_t f_offset = 0, int64_t invf_offset = 0);

    std::string name() const override;

    PresolveResult presolve(Model& model) override;
    bool prepare_propagation(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    std::optional<bool> is_satisfied(const Model& model) const override;
    bool on_final_instantiate(const Model& model) override;
    bool on_remove_value(Model& model, int save_point,
                         size_t internal_var_idx,
                         Domain::value_type removed_value) override;
    bool on_set_min(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_min, Domain::value_type old_min) override;
    bool on_set_max(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_max, Domain::value_type old_max) override;

    void rewind_to(int save_point) override;

private:
    size_t n_;             ///< 配列サイズ
    int64_t f_offset_;     ///< f の値域の最小値 (= min(index_set(invf)))
    int64_t invf_offset_;  ///< invf の値域の最小値 (= min(index_set(f)))
};

/**
 * @brief tree 制約（無向）: 選択部分グラフが根 r の木を成す
 *
 * fzn_tree(N, E, from, to, r, ns, es) に対応。
 * - ns[n] (var bool): ノード n が部分グラフに含まれるか
 * - es[e] (var bool): 辺 e が部分グラフに含まれるか
 * - r (var int): 根ノード（1-based ノードID）。ns[r] は真でなければならない
 * - from[e], to[e]: 辺 e の両端ノード（定数、1-based）
 *
 * 選択された ns/es が単一の木（連結かつ非閉路、辺数 = ノード数 - 1）を成す。
 *
 * 伝播:
 * - es[e]=1 → ns[from[e]]=1, ns[to[e]]=1（辺の両端は選択）
 * - ns[n]=0 → n を端点に持つ辺 es[e]=0
 * - 根: ns[r]=1、ns[n]=0 なら r≠n
 * - 閉路検出/防止: es[e]=1 のたびに現在の選択辺から union-find を構築し、両端が
 *   既に同成分なら閉路 → 矛盾。両端が既に同成分の未確定辺は es[e]=0 に強制（閉路予防）。
 * - 全確定時に木性（単一成分・非閉路・辺数=ノード数-1・ns[r]）を検証。
 *
 * union-find は毎回モデル状態から再構築するステートレス方式（trail/rewind 不要、
 * backtrack 安全）。O(E) per es=1 イベント。
 */
class TreeConstraint : public Constraint {
public:
    /**
     * @param ns   ノード選択 bool 変数（size N）
     * @param es   辺選択 bool 変数（size E）
     * @param r    根ノード変数
     * @param from 各辺の始点ノードID（1-based）
     * @param to   各辺の終点ノードID（1-based）
     */
    TreeConstraint(std::vector<VariablePtr> ns, std::vector<VariablePtr> es,
                   VariablePtr r, std::vector<int> from, std::vector<int> to);

    std::string name() const override;

    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;

private:
    size_t n_;  // ノード数
    size_t e_;  // 辺数
    Domain::value_type node_base_;  // ノードIDのオフセット（通常 1）

    std::vector<int> efrom_;  // 辺 e の始点（0-based 内部インデックス）
    std::vector<int> eto_;    // 辺 e の終点（0-based 内部インデックス）
    std::vector<std::vector<size_t>> incident_;  // ノード → 接続辺インデックス

    // var_ids_ レイアウト: [ns(0..n-1), es(n..n+e-1), r(n+e)]
    size_t ns_idx(size_t node) const { return node; }
    size_t es_idx(size_t edge) const { return n_ + edge; }
    size_t r_internal() const { return n_ + e_; }

    /// 現在 es=1 の辺から union-find を構築（par/sz を埋める）。閉路があれば false。
    bool build_uf(const Model& model, std::vector<size_t>& par, std::vector<size_t>& sz) const;
};

} // namespace sabori_csp

#endif // SABORI_CSP_CONSTRAINTS_GLOBAL_GRAPH_HPP
