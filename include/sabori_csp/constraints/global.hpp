/**
 * @file global.hpp
 * @brief グローバル制約クラス (all_different, int_lin_eq)
 */
#ifndef SABORI_CSP_CONSTRAINTS_GLOBAL_HPP
#define SABORI_CSP_CONSTRAINTS_GLOBAL_HPP

#include "sabori_csp/constraint.hpp"
#include <unordered_map>
#include <numeric>

namespace sabori_csp {

/**
 * @brief all_different制約: 全ての変数が異なる値を取る
 *
 * Sparse Set を使用した値プールで効率的に重複をチェック。
 * 鳩の巣原理による枝刈りを実行。
 */
class AllDifferentConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param vars 制約に関与する変数リスト
     */
    explicit AllDifferentConstraint(std::vector<VariablePtr> vars);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool prepare_propagation(Model& model) override;
    bool presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

    /**
     * @brief 残り1変数になった時の伝播
     *
     * 利用可能な値が1つだけなら、その値で確定させる。
     */
    bool on_last_uninstantiated(Model& model, int save_point,
                                 size_t last_var_internal_idx) override;

    /**
     * @brief 指定セーブポイントまで状態を巻き戻す
     */
    void rewind_to(int save_point);

    /**
     * @brief 現在のプールサイズを取得
     */
    size_t pool_size() const { return pool_n_; }

protected:
    /**
     * @brief 初期整合性チェック
     */
    void check_initial_consistency() override;

private:
    // 値プール（Sparse Set）
    std::vector<Domain::value_type> pool_values_;  // Dense 配列
    std::unordered_map<Domain::value_type, size_t> pool_sparse_;  // 値→インデックス
    size_t pool_n_;  // 有効な値の数

    // 未確定変数カウント（差分更新用）
    size_t unfixed_count_;

    // Trail: (save_point, (old_pool_n, old_unfixed_count))
    struct TrailEntry {
        size_t old_pool_n;
        size_t old_unfixed_count;
    };
    std::vector<std::pair<int, TrailEntry>> pool_trail_;

    /**
     * @brief プールから値を削除
     */
    bool remove_from_pool(int save_point, Domain::value_type value);
};

/**
 * @brief int_lin_eq制約: Σ(coeffs[i] * vars[i]) == target_sum
 *
 * O(1) の差分更新で bounds consistency を維持。
 */
class IntLinEqConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param coeffs 係数リスト
     * @param vars 変数リスト
     * @param target_sum 目標値
     */
    IntLinEqConstraint(std::vector<int64_t> coeffs,
                       std::vector<VariablePtr> vars,
                       int64_t target_sum);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool prepare_propagation(Model& model) override;
    bool presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

    /**
     * @brief 残り1変数になった時の伝播
     *
     * 残りの変数の値を一意に決定できる場合は確定させる。
     */
    bool on_last_uninstantiated(Model& model, int save_point,
                                 size_t last_var_internal_idx) override;

    /**
     * @brief 下限更新時のインクリメンタル伝播
     */
    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;

    /**
     * @brief 上限更新時のインクリメンタル伝播
     */
    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;

    /**
     * @brief 値削除時のインクリメンタル伝播
     */
    bool on_remove_value(Model& model, int save_point,
                         size_t var_idx, size_t internal_var_idx,
                         Domain::value_type removed_value) override;

    /**
     * @brief 指定セーブポイントまで状態を巻き戻す
     */
    void rewind_to(int save_point);

    /**
     * @brief 目標値を取得
     */
    int64_t target_sum() const { return target_sum_; }

    /**
     * @brief 係数リストを取得
     */
    const std::vector<int64_t>& coeffs() const { return coeffs_; }

protected:
    /**
     * @brief 初期整合性チェック
     */
    void check_initial_consistency() override;

private:
    std::vector<int64_t> coeffs_;
    int64_t target_sum_;

    // 現在の状態
    int64_t current_fixed_sum_;   // 確定した変数の c*v の和
    int64_t min_rem_potential_;   // 未確定変数の最小ポテンシャル
    int64_t max_rem_potential_;   // 未確定変数の最大ポテンシャル

    // 未確定変数カウント（差分更新用）
    size_t unfixed_count_;

    // Trail: (save_point, (fixed_sum, min_pot, max_pot))
    struct TrailEntry {
        int64_t fixed_sum;
        int64_t min_pot;
        int64_t max_pot;
    };
    std::vector<std::pair<int, TrailEntry>> trail_;

    /**
     * @brief trail 保存ヘルパー
     */
    void save_trail_if_needed(Model& model, int save_point);

    /**
     * @brief infeasibility チェックのみ（bounds 変化なし時用）
     */
    bool check_feasibility();

    /**
     * @brief total_max 減少時の伝播（c>0: set_min, c<0: set_max）
     * @param skip_idx トリガー変数の内部インデックス（スキップ）
     */
    bool propagate_lower_bounds(Model& model, size_t skip_idx);

    /**
     * @brief total_min 増加時の伝播（c>0: set_max, c<0: set_min）
     * @param skip_idx トリガー変数の内部インデックス（スキップ）
     */
    bool propagate_upper_bounds(Model& model, size_t skip_idx);
};

/**
 * @brief int_lin_le制約: Σ(coeffs[i] * vars[i]) <= bound
 */
class IntLinLeConstraint : public Constraint {
public:
    IntLinLeConstraint(std::vector<int64_t> coeffs,
                       std::vector<VariablePtr> vars,
                       int64_t bound);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool prepare_propagation(Model& model) override;
    bool presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

    /**
     * @brief 下限更新時のインクリメンタル伝播
     */
    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;

    /**
     * @brief 上限更新時のインクリメンタル伝播
     */
    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;

    /**
     * @brief 値削除時のインクリメンタル伝播
     */
    bool on_remove_value(Model& model, int save_point,
                         size_t var_idx, size_t internal_var_idx,
                         Domain::value_type removed_value) override;

    void rewind_to(int save_point);

protected:
    /**
     * @brief 初期整合性チェック
     */
    void check_initial_consistency() override;

private:
    std::vector<int64_t> coeffs_;
    int64_t bound_;
    int64_t current_fixed_sum_;
    int64_t min_rem_potential_;

    struct TrailEntry {
        int64_t fixed_sum;
        int64_t min_pot;
    };
    std::vector<std::pair<int, TrailEntry>> trail_;

    /**
     * @brief trail 保存ヘルパー
     */
    void save_trail_if_needed(Model& model, int save_point);
};

/**
 * @brief circuit制約: 変数がハミルトン閉路を形成する
 *
 * 変数 x[0], x[1], ..., x[n-1] がハミルトン閉路を形成する。
 * x[i] = j は「ノード i の次はノード j」を意味する。
 *
 * Union-Find スタイルでパスを管理:
 * - head[i]: ノード i の親ポインタ (root なら自分自身)
 * - tail[h]: h が root のパスの末尾ノード
 * - size[h]: h が root のパスのサイズ
 *
 * AllDifferent の性質も内包:
 * - 各ノードの入次数は最大1（同じ値は2回使えない）
 *
 * サブサーキット検出:
 * - x[i] = j のとき、find(i) == find(j) なら同じパス内で閉路形成
 * - そのとき size < n ならサブサーキットで false
 */
class CircuitConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param vars 制約に関与する変数リスト（インデックス 0 から n-1）
     */
    explicit CircuitConstraint(std::vector<VariablePtr> vars);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool prepare_propagation(Model& model) override;
    bool presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

    /**
     * @brief 残り1変数になった時の伝播
     *
     * 利用可能な値が1つだけなら、その値で確定させる。
     */
    bool on_last_uninstantiated(Model& model, int save_point,
                                 size_t last_var_internal_idx) override;

    /**
     * @brief 指定セーブポイントまで状態を巻き戻す
     */
    void rewind_to(int save_point);

    /**
     * @brief 現在のプールサイズを取得
     */
    size_t pool_size() const { return pool_n_; }

protected:
    /**
     * @brief 初期整合性チェック
     */
    void check_initial_consistency() override;

private:
    size_t n_;  // ノード数
    Domain::value_type base_offset_;  // 1-based インデックスのオフセット（通常は1）

    // Union-Find スタイルのパス管理
    std::vector<size_t> head_;  // head[i] = parent of i (root if self)
    std::vector<size_t> tail_;  // tail[h] = tail of path with root h
    std::vector<size_t> size_;  // size[h] = size of path with root h

    // 入次数（AllDifferent 用）
    std::vector<int> in_degree_;

    // 未確定変数カウント（差分更新用）
    size_t unfixed_count_;

    // 値プール（Sparse Set）
    std::vector<Domain::value_type> pool_;
    std::unordered_map<Domain::value_type, size_t> pool_idx_;
    size_t pool_n_;

    // Trail
    struct TrailEntry {
        size_t h1;
        size_t old_tail_h1;
        size_t h2;
        size_t old_size_h1;
        Domain::value_type j;
        size_t old_pool_n;
        size_t old_unfixed_count;
        bool is_merge;  // パス結合かどうか（false なら閉路形成）
    };
    std::vector<std::pair<int, TrailEntry>> trail_;

    /**
     * @brief ノード i を含むパスの root を探す
     */
    size_t find(size_t i) const;

    /**
     * @brief プールから値を削除（Sparse Set でO(1)）
     */
    void remove_from_pool(Domain::value_type value);
};

/**
 * @brief int_lin_ne制約: Σ(coeffs[i] * vars[i]) != target
 *
 * 線形不等式制約。全変数が確定したときに和が target と等しくないことを確認。
 * 残り1変数の場合、禁止値を除外する。
 */
class IntLinNeConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param coeffs 係数リスト
     * @param vars 変数リスト
     * @param target 禁止値
     */
    IntLinNeConstraint(std::vector<int64_t> coeffs,
                       std::vector<VariablePtr> vars,
                       int64_t target);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool prepare_propagation(Model& model) override;
    bool presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

    /**
     * @brief 残り1変数になった時の伝播
     *
     * 残りの変数が禁止値を取る場合、その値を除外する。
     */
    bool on_last_uninstantiated(Model& model, int save_point,
                                 size_t last_var_internal_idx) override;

    /**
     * @brief 指定セーブポイントまで状態を巻き戻す
     */
    void rewind_to(int save_point);

    /**
     * @brief 目標値を取得
     */
    int64_t target() const { return target_; }

    /**
     * @brief 係数リストを取得
     */
    const std::vector<int64_t>& coeffs() const { return coeffs_; }

protected:
    void check_initial_consistency() override;

private:
    std::vector<int64_t> coeffs_;
    int64_t target_;

    // 現在の確定変数の和
    int64_t current_fixed_sum_;

    // 未確定変数カウント
    size_t unfixed_count_;

    // Trail: (save_point, (fixed_sum, unfixed_count))
    struct TrailEntry {
        int64_t fixed_sum;
        size_t unfixed_count;
    };
    std::vector<std::pair<int, TrailEntry>> trail_;

};

/**
 * @brief int_element制約: array[index] = result を維持する
 *
 * - index: インデックス変数（1-based、MiniZinc 仕様。zero_based=true で 0-based）
 * - array: 定数整数の配列
 * - result: 結果変数
 *
 * 状態を持たないため、rewind_to は空実装。
 */
class IntElementConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param index_var インデックス変数
     * @param array 定数整数の配列
     * @param result_var 結果変数
     * @param zero_based true なら 0-based インデックス（デフォルトは 1-based）
     */
    IntElementConstraint(VariablePtr index_var,
                         std::vector<Domain::value_type> array,
                         VariablePtr result_var,
                         bool zero_based = false);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

    /**
     * @brief 残り1変数になった時の伝播
     *
     * index 確定済み → result を確定
     * result 確定済み → index の候補が1つなら確定
     */
    bool on_last_uninstantiated(Model& model, int save_point,
                                 size_t last_var_internal_idx) override;

    /**
     * @brief 下限更新時のインクリメンタル伝播
     */
    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;

    /**
     * @brief 上限更新時のインクリメンタル伝播
     */
    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;

    /**
     * @brief 指定セーブポイントまで状態を巻き戻す（空実装）
     */
    void rewind_to(int save_point);

protected:
    /**
     * @brief 初期整合性チェック
     */
    void check_initial_consistency() override;

private:
    VariablePtr index_var_;
    VariablePtr result_var_;
    std::vector<Domain::value_type> array_;
    size_t n_;
    bool zero_based_;

    // CSR: 値 -> インデックスリスト（逆引き）
    std::unordered_map<Domain::value_type, std::vector<Domain::value_type>> value_to_indices_;

    // Monotonic prefix/suffix arrays for reverse bounds propagation
    std::vector<Domain::value_type> p_min_;  // prefix min (non-increasing)
    std::vector<Domain::value_type> p_max_;  // prefix max (non-decreasing)
    std::vector<Domain::value_type> s_min_;  // suffix min (non-decreasing)
    std::vector<Domain::value_type> s_max_;  // suffix max (non-increasing)

    // Sparse Table for O(1) range min/max queries
    std::vector<std::vector<Domain::value_type>> sparse_min_;
    std::vector<std::vector<Domain::value_type>> sparse_max_;
    int log_n_;

    // 変数ポインタ → 内部インデックス (0: index, 1: result)
    std::unordered_map<Variable*, size_t> var_ptr_to_idx_;

    /**
     * @brief index を 0-based に変換
     */
    Domain::value_type index_to_0based(Domain::value_type idx) const;

    /**
     * @brief 区間 [lo, hi] (0-based) の最小値を O(1) で取得
     */
    Domain::value_type range_min(size_t lo, size_t hi) const;

    /**
     * @brief 区間 [lo, hi] (0-based) の最大値を O(1) で取得
     */
    Domain::value_type range_max(size_t lo, size_t hi) const;
};

/**
 * @brief array_int_maximum制約: m = max(x[0], x[1], ..., x[n-1])
 *
 * 変数 m は配列 x の最大値と等しい。
 *
 * 伝播ルール:
 * - m.min = max(x[i].min) - m は全ての x[i] の最小値の最大値以上
 * - m.max = max(x[i].max) - m は全ての x[i] の最大値の最大値以下
 * - x[i].max <= m.max - 各 x[i] は m の最大値以下
 * - m 確定時: 少なくとも1つの x[i] が m に等しくなれる必要がある
 */
class ArrayIntMaximumConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param m 最大値を表す変数
     * @param vars 配列変数
     */
    ArrayIntMaximumConstraint(VariablePtr m, std::vector<VariablePtr> vars);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

    bool on_last_uninstantiated(Model& model, int save_point,
                                size_t last_var_internal_idx) override;

    /**
     * @brief 指定セーブポイントまで状態を巻き戻す（状態を持たないので空実装）
     */
    void rewind_to(int save_point);

protected:
    void check_initial_consistency() override;

private:
    VariablePtr m_;                // 最大値変数
    std::vector<VariablePtr> x_;   // 配列変数
    size_t n_;                     // 配列サイズ
    size_t m_id_;                  // m_ の変数ID キャッシュ

    // 変数ポインタ → 内部インデックス (0: m, 1..n: x[0]..x[n-1])
    std::unordered_map<Variable*, size_t> var_ptr_to_idx_;
};

/**
 * @brief array_int_minimum制約: m = min(x[0], x[1], ..., x[n-1])
 *
 * 変数 m は配列 x の最小値と等しい。
 */
class ArrayIntMinimumConstraint : public Constraint {
public:
    ArrayIntMinimumConstraint(VariablePtr m, std::vector<VariablePtr> vars);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

    bool on_last_uninstantiated(Model& model, int save_point,
                                size_t last_var_internal_idx) override;

    void rewind_to(int save_point);

protected:
    void check_initial_consistency() override;

private:
    VariablePtr m_;
    std::vector<VariablePtr> x_;
    size_t n_;
    size_t m_id_;                  // m_ の変数ID キャッシュ
    std::unordered_map<Variable*, size_t> var_ptr_to_idx_;
};

/**
 * @brief int_lin_eq_reif制約: (Σ(coeffs[i] * vars[i]) == target) <-> b
 *
 * 線形等式の reified 版（双方向）。
 * b が 1 の場合、線形等式を強制。
 * b が 0 の場合、線形等式の否定（sum != target）を強制。
 * 線形変数の bounds から b を推論可能。
 */
class IntLinEqReifConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param coeffs 係数リスト
     * @param vars 変数リスト
     * @param target 目標値
     * @param b reified 変数
     */
    IntLinEqReifConstraint(std::vector<int64_t> coeffs,
                           std::vector<VariablePtr> vars,
                           int64_t target,
                           VariablePtr b);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool prepare_propagation(Model& model) override;
    bool presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

    /**
     * @brief 下限更新時のインクリメンタル伝播
     */
    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;

    /**
     * @brief 上限更新時のインクリメンタル伝播
     */
    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;

    /**
     * @brief 値削除時のインクリメンタル伝播
     */
    bool on_remove_value(Model& model, int save_point,
                         size_t var_idx, size_t internal_var_idx,
                         Domain::value_type removed_value) override;

    void rewind_to(int save_point);

protected:
    void check_initial_consistency() override;

private:
    std::vector<int64_t> coeffs_;
    int64_t target_;
    VariablePtr b_;
    int64_t current_fixed_sum_;
    int64_t min_rem_potential_;
    int64_t max_rem_potential_;
    size_t unfixed_count_;

    struct TrailEntry {
        int64_t fixed_sum;
        int64_t min_pot;
        int64_t max_pot;
        size_t unfixed_count;
    };
    std::vector<std::pair<int, TrailEntry>> trail_;
    size_t b_id_;

    /**
     * @brief trail 保存ヘルパー
     */
    void save_trail_if_needed(Model& model, int save_point);
};

/**
 * @brief int_lin_ne_reif制約: (Σ(coeffs[i] * vars[i]) != target) <-> b
 *
 * 線形不等式の reified 版（双方向）。
 * b が 1 の場合、線形不等式を強制（sum != target）。
 * b が 0 の場合、線形等式を強制（sum == target）。
 * 線形変数の bounds から b を推論可能。
 */
class IntLinNeReifConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param coeffs 係数リスト
     * @param vars 変数リスト
     * @param target 目標値
     * @param b reified 変数
     */
    IntLinNeReifConstraint(std::vector<int64_t> coeffs,
                           std::vector<VariablePtr> vars,
                           int64_t target,
                           VariablePtr b);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool prepare_propagation(Model& model) override;
    bool presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

    /**
     * @brief 下限更新時のインクリメンタル伝播
     */
    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;

    /**
     * @brief 上限更新時のインクリメンタル伝播
     */
    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;

    /**
     * @brief 値削除時のインクリメンタル伝播
     */
    bool on_remove_value(Model& model, int save_point,
                         size_t var_idx, size_t internal_var_idx,
                         Domain::value_type removed_value) override;

    void rewind_to(int save_point);

protected:
    void check_initial_consistency() override;

private:
    std::vector<int64_t> coeffs_;
    int64_t target_;
    VariablePtr b_;
    int64_t current_fixed_sum_;
    int64_t min_rem_potential_;
    int64_t max_rem_potential_;
    size_t unfixed_count_;

    struct TrailEntry {
        int64_t fixed_sum;
        int64_t min_pot;
        int64_t max_pot;
        size_t unfixed_count;
    };
    std::vector<std::pair<int, TrailEntry>> trail_;
    size_t b_id_;

    /**
     * @brief trail 保存ヘルパー
     */
    void save_trail_if_needed(Model& model, int save_point);
};

/**
 * @brief int_lin_le_reif制約: (Σ(coeffs[i] * vars[i]) <= bound) <-> b
 *
 * 線形不等式の reified 版（双方向）。
 * b が 1 の場合、線形不等式を強制。
 * b が 0 の場合、線形不等式の否定（sum > bound）を強制。
 * 線形変数の bounds から b を推論可能。
 */
class IntLinLeReifConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param coeffs 係数リスト
     * @param vars 変数リスト
     * @param bound 上限
     * @param b reified 変数
     */
    IntLinLeReifConstraint(std::vector<int64_t> coeffs,
                           std::vector<VariablePtr> vars,
                           int64_t bound,
                           VariablePtr b);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool prepare_propagation(Model& model) override;
    bool presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

    /**
     * @brief 下限更新時のインクリメンタル伝播
     */
    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;

    /**
     * @brief 上限更新時のインクリメンタル伝播
     */
    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;

    /**
     * @brief 値削除時のインクリメンタル伝播
     */
    bool on_remove_value(Model& model, int save_point,
                         size_t var_idx, size_t internal_var_idx,
                         Domain::value_type removed_value) override;

    void rewind_to(int save_point);

protected:
    void check_initial_consistency() override;

private:
    std::vector<int64_t> coeffs_;
    int64_t bound_;
    VariablePtr b_;
    int64_t current_fixed_sum_;
    int64_t min_rem_potential_;
    int64_t max_rem_potential_;
    size_t unfixed_count_;

    struct TrailEntry {
        int64_t fixed_sum;
        int64_t min_pot;
        int64_t max_pot;
        size_t unfixed_count;
    };
    std::vector<std::pair<int, TrailEntry>> trail_;
    size_t b_id_;

    /**
     * @brief trail 保存ヘルパー
     */
    void save_trail_if_needed(Model& model, int save_point);
};

/**
 * @brief int_lin_le_imp制約: b = 1 -> Σ(coeffs[i] * vars[i]) <= bound
 *
 * 半具象化（含意）版の線形不等式制約。
 * b が 1 の場合のみ、線形不等式を強制する。
 * b が 0 の場合は制約は無条件で充足される。
 *
 * _reif との違い:
 * - _reif: (P) <-> b （双方向）
 * - _imp: b -> P （単方向、b から P への含意のみ）
 */
class IntLinLeImpConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param coeffs 係数リスト
     * @param vars 変数リスト
     * @param bound 上限
     * @param b 含意変数（b = 1 のとき制約を強制）
     */
    IntLinLeImpConstraint(std::vector<int64_t> coeffs,
                          std::vector<VariablePtr> vars,
                          int64_t bound,
                          VariablePtr b);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool prepare_propagation(Model& model) override;
    bool presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

    /**
     * @brief 下限更新時のインクリメンタル伝播
     */
    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;

    /**
     * @brief 上限更新時のインクリメンタル伝播
     */
    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;

    /**
     * @brief 値削除時のインクリメンタル伝播
     */
    bool on_remove_value(Model& model, int save_point,
                         size_t var_idx, size_t internal_var_idx,
                         Domain::value_type removed_value) override;

    /**
     * @brief 指定セーブポイントまで状態を巻き戻す
     */
    void rewind_to(int save_point);

protected:
    void check_initial_consistency() override;

private:
    std::vector<int64_t> coeffs_;
    int64_t bound_;
    VariablePtr b_;  // 含意変数
    int64_t current_fixed_sum_;
    int64_t min_rem_potential_;

    struct TrailEntry {
        int64_t fixed_sum;
        int64_t min_pot;
    };
    std::vector<std::pair<int, TrailEntry>> trail_;
    size_t b_id_;

    /**
     * @brief trail 保存ヘルパー
     */
    void save_trail_if_needed(Model& model, int save_point);
};

/**
 * @brief array_var_int_element制約: array[index] = result（配列要素が変数）
 *
 * 配列要素が変数の element 制約。bounds consistency を維持。
 *
 * 伝播ルール:
 * - result.min = min { array[i].min : i ∈ dom(index) }
 * - result.max = max { array[i].max : i ∈ dom(index) }
 * - index から i を削除: array[i] と result の bounds が重ならない場合
 * - index 確定時: array[index] = result（等式として伝播）
 *
 * Bounds support tracking:
 * - result の下限をサポートするインデックス集合を追跡
 * - result の上限をサポートするインデックス集合を追跡
 * - サポートが失われたら bounds を再計算
 */
class ArrayVarIntElementConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param index インデックス変数
     * @param array 変数の配列
     * @param result 結果変数
     * @param zero_based true なら 0-based インデックス（デフォルトは 1-based）
     */
    ArrayVarIntElementConstraint(VariablePtr index,
                                  std::vector<VariablePtr> array,
                                  VariablePtr result,
                                  bool zero_based = false);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool prepare_propagation(Model& model) override;
    bool presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

    bool on_last_uninstantiated(Model& model, int save_point,
                                size_t last_var_internal_idx) override;

    /**
     * @brief 下限更新時のインクリメンタル伝播
     */
    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;

    /**
     * @brief 上限更新時のインクリメンタル伝播
     */
    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;

    /**
     * @brief 指定セーブポイントまで状態を巻き戻す
     */
    void rewind_to(int save_point);

protected:
    void check_initial_consistency() override;

private:
    VariablePtr index_;
    std::vector<VariablePtr> array_;
    VariablePtr result_;
    size_t n_;  // array size
    bool zero_based_;
    size_t index_id_;
    size_t result_id_;

    // Bounds support tracking
    // result の下限をサポートするインデックス（array[i].min が最小のもの）
    Domain::value_type current_result_min_support_;  // サポートしている最小値
    // result の上限をサポートするインデックス（array[i].max が最大のもの）
    Domain::value_type current_result_max_support_;  // サポートしている最大値

    // Trail for bounds support
    struct TrailEntry {
        Domain::value_type min_support;
        Domain::value_type max_support;
    };
    std::vector<std::pair<int, TrailEntry>> trail_;

    /**
     * @brief index を 0-based に変換
     */
    Domain::value_type index_to_0based(Domain::value_type idx) const;

    /**
     * @brief 0-based index を 1-based (or 0-based) に変換
     */
    Domain::value_type index_from_0based(size_t idx_0based) const;

    /**
     * @brief bounds consistency を維持
     * @param model モデル参照
     * @param save_point セーブポイント（-1 の場合は直接ドメイン操作）
     * @return 矛盾がなければ true
     */
    bool propagate_bounds(Model& model, int save_point = -1);

    /**
     * @brief キュー経由で bounds 伝播（探索中に使用）
     *
     * propagate_bounds と同じロジックだが、model.enqueue_* を使って
     * 他の制約にも通知が届くようにする。
     *
     * @param model モデル参照
     * @return 矛盾がなければ true
     */
    bool propagate_via_queue(Model& model);

    /**
     * @brief result の bounds support を再計算
     */
    void recompute_bounds_support(Model& model);
};

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
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool presolve(Model& model) override;
    bool prepare_propagation(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

    /**
     * @brief 残り1変数になった時の伝播
     */
    bool on_last_uninstantiated(Model& model, int save_point,
                                 size_t last_var_internal_idx) override;

    /**
     * @brief 値削除時のインクリメンタル伝播
     */
    bool on_remove_value(Model& model, int save_point,
                         size_t var_idx, size_t internal_var_idx,
                         Domain::value_type removed_value) override;

    /**
     * @brief 下限更新時のインクリメンタル伝播
     */
    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;

    /**
     * @brief 上限更新時のインクリメンタル伝播
     */
    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;

    /**
     * @brief 指定セーブポイントまで状態を巻き戻す
     */
    void rewind_to(int save_point);

protected:
    void check_initial_consistency() override;

private:
    size_t arity_;
    size_t num_tuples_;
    size_t num_words_;

    std::vector<Domain::value_type> flat_tuples_;  ///< is_satisfied 用コピー

    /// フラットビットセット: supports_data_[get_support_offset(var, val) + w]
    std::vector<uint64_t> supports_data_;
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
    /// Residual support: 各 (var, value) ペアの前回サポート word index
    mutable std::vector<size_t> residual_words_;

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
 * @brief count_eq制約: 配列 x[] の中で target に等しい要素の数 = c
 *
 * O(1) のインクリメンタル伝播で bounds consistency を維持。
 *
 * 不変条件: definite_count_ <= c <= definite_count_ + possible_count_
 *
 * - definite_count_: x[i] == target で確定済みの数
 * - possible_count_: 未確定かつ target が domain に含まれる x[i] の数
 */
class CountEqConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param x_vars 配列変数
     * @param target 定数ターゲット値
     * @param count_var カウント変数
     */
    CountEqConstraint(std::vector<VariablePtr> x_vars,
                      Domain::value_type target,
                      VariablePtr count_var);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool prepare_propagation(Model& model) override;
    bool presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

    bool on_last_uninstantiated(Model& model, int save_point,
                                 size_t last_var_internal_idx) override;

    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min,
                    Domain::value_type old_min) override;

    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max,
                    Domain::value_type old_max) override;

    bool on_remove_value(Model& model, int save_point,
                         size_t var_idx, size_t internal_var_idx,
                         Domain::value_type removed_value) override;

    void rewind_to(int save_point);

protected:
    void check_initial_consistency() override;

private:
    Domain::value_type target_;
    size_t n_;  // 配列サイズ (x[] の要素数)

    // 内部カウンタ
    size_t definite_count_;   // x[i] == target で確定済みの数
    size_t possible_count_;   // 未確定かつ target が domain に含まれる x[i] の数
    std::vector<bool> is_possible_;  // 各 x[i] が "possible" かどうか

    // Trail
    struct TrailEntry {
        size_t definite_count;
        size_t possible_count;
        std::vector<std::pair<size_t, bool>> is_possible_changes;  // (index, old_value)
    };
    std::vector<std::pair<int, TrailEntry>> trail_;

    /**
     * @brief trail 保存ヘルパー
     */
    void save_trail_if_needed(Model& model, int save_point);

    /**
     * @brief 不変条件に基づく伝播
     */
    bool propagate(Model& model);
};

/**
 * @brief Disjunctive (unary resource) 制約
 *
 * 各タスク i は開始時刻 s[i]、実行時間 d[i] を持ち、
 * 任意の2タスク i,j について s[i]+d[i] <= s[j] ∨ s[j]+d[j] <= s[i] を保証。
 * strict=false の場合、d[i]=0 のタスクは他タスクと重複可能。
 */
class DisjunctiveConstraint : public Constraint {
public:
    DisjunctiveConstraint(std::vector<VariablePtr> starts,
                          std::vector<VariablePtr> durations,
                          bool strict);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool presolve(Model& model) override;
    bool prepare_propagation(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;
    bool on_set_min(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_min, Domain::value_type old_min) override;
    bool on_set_max(Model& model, int save_point,
                    size_t var_idx, size_t internal_var_idx,
                    Domain::value_type new_max, Domain::value_type old_max) override;

    void rewind_to(int save_point) override;

protected:
    void check_initial_consistency() override;

private:
    size_t n_;          // タスク数
    bool strict_;       // strict disjunctive かどうか
    int offset_;        // 時間軸オフセット (min_start)
    int horizon_;       // 時間軸長 (max_end - min_start)

    std::vector<uint64_t> timeline_;  // ビットマップ (1=占有)

    // Trail
    struct UndoEntry {
        int block_idx;
        uint64_t old_mask;
    };
    std::vector<std::pair<int, UndoEntry>> trail_;

    // Compulsory Part tracking
    std::vector<int> cp_lo_;   // [cp_lo_[i], cp_hi_[i]) = task i の現在 CP 区間
    std::vector<int> cp_hi_;

    struct CpUndoEntry {
        size_t task_idx;
        int old_cp_lo;
        int old_cp_hi;
    };
    std::vector<std::pair<int, CpUndoEntry>> cp_trail_;

    // Task helpers
    bool task_fully_assigned(size_t task) const;
    int task_start(size_t task) const;
    int task_dur(size_t task) const;
    int task_dur_min(size_t task) const;

    // Bit operations
    bool check_conflict(int start, int len) const;
    int count_set_bits(int start, int len) const;
    int count_free_bits(int start, int len) const;
    bool check_conflict_excluding(int start, int len, size_t exclude_task) const;
    int find_first_valid_excluding(int lo, int hi, int dur, size_t exclude_task) const;
    int find_last_valid_excluding(int lo, int hi, int dur, size_t exclude_task) const;
    void set_bits_direct(int start, int len);
    void set_bits(Model& model, int save_point, int start, int len);
    void ensure_dirty_marked(Model& model, int save_point);
    int find_next_zero(int from) const;
    int find_prev_zero(int from) const;

    // Compulsory part
    bool update_compulsory_part(Model& model, int save_point, size_t task);
    bool update_compulsory_part_direct(size_t task);

    // Propagation
    bool propagate_bounds(Model& model);
    bool edge_finding(Model& model, bool direct);
};

} // namespace sabori_csp

#endif // SABORI_CSP_CONSTRAINTS_GLOBAL_HPP
