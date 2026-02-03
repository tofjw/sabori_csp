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
    bool propagate() override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, Domain::value_type value,
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

    // 変数ポインタ → 内部インデックスへのマップ
    std::unordered_map<Variable*, size_t> var_ptr_to_idx_;

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
    bool propagate() override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, Domain::value_type value,
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

    // Trail: (save_point, (fixed_sum, min_pot, max_pot, unfixed_count))
    struct TrailEntry {
        int64_t fixed_sum;
        int64_t min_pot;
        int64_t max_pot;
        size_t unfixed_count;
    };
    std::vector<std::pair<int, TrailEntry>> trail_;

    // 変数ポインタ → 内部インデックスへのマップ
    std::unordered_map<Variable*, size_t> var_ptr_to_idx_;

    /**
     * @brief 値を割り当て可能か O(1) で判定 (Look-ahead)
     */
    bool can_assign(size_t internal_idx, Domain::value_type value,
                    Domain::value_type prev_min, Domain::value_type prev_max) const;
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
    bool propagate() override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;

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
    std::unordered_map<Variable*, size_t> var_ptr_to_idx_;
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
    bool propagate() override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, Domain::value_type value,
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

    // Union-Find スタイルのパス管理
    std::vector<size_t> head_;  // head[i] = parent of i (root if self)
    std::vector<size_t> tail_;  // tail[h] = tail of path with root h
    std::vector<size_t> size_;  // size[h] = size of path with root h

    // 入次数（AllDifferent 用）
    std::vector<int> in_degree_;

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
        bool is_merge;  // パス結合かどうか（false なら閉路形成）
    };
    std::vector<std::pair<int, TrailEntry>> trail_;

    // 変数ポインタ → インデックス
    std::unordered_map<Variable*, size_t> var_ptr_to_idx_;

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
    bool propagate() override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, Domain::value_type value,
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

    // Monotonic Wrapper (将来の bounds propagation 用)
    std::vector<Domain::value_type> p_min_;  // prefix min
    std::vector<Domain::value_type> p_max_;  // prefix max
    std::vector<Domain::value_type> s_min_;  // suffix min
    std::vector<Domain::value_type> s_max_;  // suffix max

    // 変数ポインタ → 内部インデックス (0: index, 1: result)
    std::unordered_map<Variable*, size_t> var_ptr_to_idx_;

    /**
     * @brief index を 0-based に変換
     */
    Domain::value_type index_to_0based(Domain::value_type idx) const;
};

} // namespace sabori_csp

#endif // SABORI_CSP_CONSTRAINTS_GLOBAL_HPP
