#ifndef SABORI_CSP_CONSTRAINTS_GLOBAL_EXTENSIONAL_HPP
#define SABORI_CSP_CONSTRAINTS_GLOBAL_EXTENSIONAL_HPP

#include "sabori_csp/constraint.hpp"
#include <unordered_map>
#include <numeric>
#include <vector>
#include <memory>

namespace sabori_csp {


/**
 * @brief table_int制約: 変数の値の組み合わせがタプル集合に含まれる
 *
 * Compact Table (CT) アルゴリズムで実装。
 * ビットセットで有効タプルを管理し、ドメインフィルタリングを行う。
 */
class TableConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param vars 制約に関与する変数リスト
     * @param flat_tuples タプルをフラットに並べた配列（arity * num_tuples 要素）
     */
    TableConstraint(std::vector<VariablePtr> vars,
                    std::vector<Domain::value_type> flat_tuples);

    std::string name() const override;


    PresolveResult presolve(Model& model) override;
    bool prepare_propagation(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;

    /**
     * @brief 残り1変数になった時の伝播
     */
    bool on_last_uninstantiated(Model& model, int save_point,
                                 size_t last_var_internal_idx) override;

    /**
     * @brief 値削除時のインクリメンタル伝播
     */
    bool on_remove_value(Model& model, int save_point,
                         size_t internal_var_idx,
                         Domain::value_type removed_value) override;

    /**
     * @brief 下限更新時のインクリメンタル伝播
     */
    bool on_set_min(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;

    /**
     * @brief 上限更新時のインクリメンタル伝播
     */
    bool on_set_max(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;

    /**
     * @brief 指定セーブポイントまで状態を巻き戻す
     */
    void rewind_to(int save_point);

protected:


private:
    size_t arity_;
    size_t num_tuples_;
    size_t num_words_;

    std::vector<Domain::value_type> flat_tuples_;  ///< is_satisfied 用コピー

    /// supports ストレージモード: false=dense bitset, true=sorted tuple-index list
    bool use_sparse_ = false;
    /// dense モード: フラットビットセット supports_data_[get_support_offset(var, val) + w]
    /// sparse モード: 全 (var,val) のタプル index を flat に並べたもの
    ///                各 (var,val) のリストは [start, start+length) のスライス
    std::vector<uint64_t> supports_data_;     ///< dense モード時のみ使用
    std::vector<uint32_t> sparse_supports_;   ///< sparse モード時のみ使用 (sorted tuple indices)
    std::vector<uint32_t> sparse_lengths_;    ///< sparse モード時の各 (var,val) のリスト長
    /// 各変数の値→supports_data_内オフセット（フラット配列）
    struct VarSupportInfo {
        Domain::value_type min_val;
        size_t range_size;
        size_t flat_offset;  ///< supports_offsets_flat_ 内のオフセット
    };
    std::vector<VarSupportInfo> var_support_info_;
    static constexpr size_t NO_SUPPORT = SIZE_MAX;
    std::vector<size_t> supports_offsets_flat_;  ///< NO_SUPPORT = サポートなし
    /// 有効タプルのビットマスク
    std::vector<uint64_t> current_table_;
    /// current_table_ の最後の非ゼロ word インデックス (テーブルが空なら 0)
    size_t last_nz_word_;
    /// Residual support (dense): 各 (var, value) ペアの前回サポート word index
    mutable std::vector<size_t> residual_words_;
    /// Residual support (sparse): 各 (var, value) ペアの前回サポート tuple index (リスト内位置)
    mutable std::vector<uint32_t> sparse_residual_idx_;
    /// sparse モード時の on_instantiate / prepare_propagation 用 scratch (num_words_ サイズ)
    std::vector<uint64_t> scratch_mask_;

    struct TrailEntry {
        std::vector<std::pair<size_t, uint64_t>> word_diffs;  ///< (word_idx, old_value)
        size_t old_last_nz_word;
    };
    std::vector<std::pair<int, TrailEntry>> trail_;

    /// Save-on-write generation counter（同一レベルでの重複 word 保存を防止）
    int trail_generation_ = 0;
    /// word_saved_at_[w] = w が保存された時点の generation
    std::vector<int> word_saved_at_;

    /// filter_domains 用: 変更 word 追跡で has_support スキップ
    int filter_gen_ = 0;
    std::vector<int> word_modified_at_;

    /**
     * @brief trail に空エントリ作成 + generation 更新（同一レベルでの重複保存を防止）
     */
    void save_trail_if_needed(Model& model, int save_point);

    /**
     * @brief word 単位の save-on-write ヘルパー
     */
    inline void save_word(size_t w) {
        if (word_saved_at_[w] != trail_generation_) {
            word_saved_at_[w] = trail_generation_;
            trail_.back().second.word_diffs.push_back({w, current_table_[w]});
        }
    }

    /**
     * @brief 指定 (var,val) のサポートを current_table_ から取り除く
     *        (current_table_ &= ~supports[var,val])
     * @param internal_idx 変数インデックス
     * @param val 取り除く値
     * @return 1個でも word が変わったら true
     */
    bool clear_supports_for(size_t internal_idx, Domain::value_type val);

    /**
     * @brief 各変数のドメインからサポートのない値を除去
     * @param model モデル参照
     * @param skip_var_idx スキップする変数の内部インデックス（-1 でスキップなし）
     * @return 矛盾がなければ true
     */
    bool filter_domains(Model& model, int skip_var_idx);

    /**
     * @brief 指定変数の指定値の supports_data_ 内オフセットを返す
     * @return オフセット。サポートなしなら NO_SUPPORT
     */
    size_t get_support_offset(size_t var_idx, Domain::value_type val) const {
        const auto& info = var_support_info_[var_idx];
        auto diff = val - info.min_val;
        if (diff < 0 || static_cast<size_t>(diff) >= info.range_size)
            return NO_SUPPORT;
        return supports_offsets_flat_[info.flat_offset + static_cast<size_t>(diff)];
    }

    /**
     * @brief 指定変数の指定値にサポートがあるか
     */
    bool has_support(size_t var_idx, Domain::value_type value) const;

    /**
     * @brief テーブルが空かチェック
     */
    bool table_is_empty() const;
};


/**
 * @brief regular制約: DFA（決定性有限オートマトン）による正規言語制約
 *
 * 変数列 x[0..n-1] が DFA の受理する文字列であることを要求する。
 * DFA は状態数 Q、シンボル数 S、遷移テーブル、初期状態、受理状態集合で定義。
 *
 * Forward/backward reachability による GAC フィルタリングを実装。
 */
class RegularConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param vars 入力変数列
     * @param num_states 状態数 Q（状態 1..Q）
     * @param num_symbols シンボル数 S（シンボル 1..S）
     * @param transition_flat 遷移テーブル（Q×S をフラット化、1-indexed、0=不受理）
     * @param initial_state 初期状態
     * @param accepting_states 受理状態集合
     */
    RegularConstraint(std::vector<VariablePtr> vars,
                      int num_states, int num_symbols,
                      std::vector<int> transition_flat,
                      int initial_state,
                      std::vector<int> accepting_states);

    std::string name() const override;

    PresolveResult presolve(Model& model) override;
    bool prepare_propagation(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
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

    /**
     * @brief バッチ伝播: DFA フルパス（compute_and_filter）を1回実行
     */
    bool propagate_batch(Model& model, int save_point) override;

    void rewind_to(int save_point) override;

    void bump_activity(const Model& model, size_t trigger_var_idx,
                       double* activity, double activity_inc,
                       bool& need_rescale, std::mt19937& rng) const override;

private:
    int Q_;   ///< 状態数
    int S_;   ///< シンボル数
    /// transition_[q * (S_+1) + s] = next state (0 = fail)
    /// 行0・列0 は全て 0（不受理）
    std::vector<int> transition_;
    int q0_;  ///< 初期状態
    std::vector<bool> accepting_;  ///< accepting_[q] = true if q is accepting

    size_t n_;  ///< 変数の数

    /// 各位置で到達可能な状態集合（フラット uint8 配列、キャッシュ局所性のため）
    /// reachable_from_[i*reach_stride_ + q] は位置 i に入る時点で q が到達可能か
    /// reachable_to_[i*reach_stride_ + q] は位置 i から q 経由で受理状態まで逆到達可能か
    size_t reach_stride_ = 0;          ///< = Q_ + 1
    std::vector<uint8_t> reachable_from_;
    std::vector<uint8_t> reachable_to_;

    /// 1 回の compute_and_filter 内で 3 パス共通の dom 値展開バッファ
    /// vals_buf_[vals_offset_[i] .. vals_offset_[i+1]) に位置 i の有効シンボルを格納
    std::vector<int> vals_buf_;
    std::vector<size_t> vals_offset_;

    /// no-op early-out 用キャッシュ: 前回 compute_and_filter 完了時の (min, max, size)
    std::vector<Domain::value_type> prev_min_;
    std::vector<Domain::value_type> prev_max_;
    std::vector<size_t>             prev_size_;
    bool                            prev_valid_ = false;

    /// Trail: save points where mark_constraint_dirty was called
    /// reachable sets are always recomputed from model state, no need to save them
    std::vector<int> trail_save_points_;

    /// 遷移テーブル参照
    int transition(int q, int s) const {
        return transition_[q * (S_ + 1) + s];
    }

    /// Forward/backward reachability を計算してドメインをフィルタ
    bool compute_and_filter(Model& model);
};

} // namespace sabori_csp

#endif // SABORI_CSP_CONSTRAINTS_GLOBAL_EXTENSIONAL_HPP
