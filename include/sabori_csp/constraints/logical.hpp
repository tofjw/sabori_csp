#ifndef SABORI_CSP_CONSTRAINTS_LOGICAL_HPP
#define SABORI_CSP_CONSTRAINTS_LOGICAL_HPP

#include "sabori_csp/constraint.hpp"
#include <vector>
#include <utility>

namespace sabori_csp {

/**
 * @brief array_bool_and制約: r = (b1 ∧ b2 ∧ ... ∧ bn)
 *
 * 2-watched literal を使用して効率的に伝播を行う。
 * - r = 1 のとき: すべての bi = 1
 * - r = 0 のとき: 少なくとも1つの bi = 0（2WL で監視）
 * - すべての bi = 1 のとき: r = 1
 * - 1つでも bi = 0 のとき: r = 0
 */
class ArrayBoolAndConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param vars bool変数の配列
     * @param r 結果変数
     */
    ArrayBoolAndConstraint(std::vector<VariablePtr> vars, VariablePtr r);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool propagate(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;
    bool on_last_uninstantiated(Model& model, int save_point,
                                size_t last_var_internal_idx) override;

    /**
     * @brief バックトラック時に watched literal の状態を復元
     */
    void rewind_to(int save_point);

protected:
    void check_initial_consistency() override;

private:
    std::vector<VariablePtr> vars_;  // bool変数の配列
    VariablePtr r_;                   // 結果変数
    size_t n_;                        // vars_ のサイズ

    // 2-watched literal: r = 0 のとき、0 になりうる変数を2つ監視
    size_t w1_;  // 1つ目の watched index
    size_t w2_;  // 2つ目の watched index

    // Trail for watched literals: (save_point, old_w1, old_w2)
    std::vector<std::tuple<int, size_t, size_t>> watch_trail_;

    // 変数ポインタ → 内部インデックス (0..n-1: vars_, n: r_)
    std::unordered_map<Variable*, size_t> var_ptr_to_idx_;

    /**
     * @brief 未確定または 0 を含むドメインを持つ変数のインデックスを探す
     * @param exclude1 除外するインデックス
     * @param exclude2 除外するインデックス
     * @return 見つかったインデックス、なければ SIZE_MAX
     */
    size_t find_unwatched_candidate(size_t exclude1, size_t exclude2) const;

    /**
     * @brief watched literal を移動する
     * @param save_point 現在のセーブポイント
     * @param which_watch 移動する watch (1 or 2)
     * @param new_idx 新しいインデックス
     */
    void move_watch(int save_point, int which_watch, size_t new_idx);
};

/**
 * @brief array_bool_or制約: r = (b1 ∨ b2 ∨ ... ∨ bn)
 *
 * 2-watched literal を使用して効率的に伝播を行う。
 */
class ArrayBoolOrConstraint : public Constraint {
public:
    ArrayBoolOrConstraint(std::vector<VariablePtr> vars, VariablePtr r);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool propagate(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;
    bool on_last_uninstantiated(Model& model, int save_point,
                                size_t last_var_internal_idx) override;

    void rewind_to(int save_point);

protected:
    void check_initial_consistency() override;

private:
    std::vector<VariablePtr> vars_;
    VariablePtr r_;
    size_t n_;

    size_t w1_;
    size_t w2_;

    std::vector<std::tuple<int, size_t, size_t>> watch_trail_;
    std::unordered_map<Variable*, size_t> var_ptr_to_idx_;

    size_t find_unwatched_candidate(size_t exclude1, size_t exclude2) const;
    void move_watch(int save_point, int which_watch, size_t new_idx);
};

/**
 * @brief bool_clause制約: ∨(pos[i]) ∨ ∨(¬neg[j])
 *
 * SAT clause: 正リテラル pos のいずれかが true、または
 * 負リテラル neg のいずれかが false であれば充足。
 *
 * 2-watched literal を使用して効率的に unit propagation を行う。
 * - pos[i] = 1 で節が充足
 * - neg[j] = 0 で節が充足
 * - 全ての pos = 0 かつ 全ての neg = 1 で矛盾
 */
class BoolClauseConstraint : public Constraint {
public:
    /**
     * @brief コンストラクタ
     * @param pos 正リテラル（これらのいずれかが 1 なら充足）
     * @param neg 負リテラル（これらのいずれかが 0 なら充足）
     */
    BoolClauseConstraint(std::vector<VariablePtr> pos, std::vector<VariablePtr> neg);

    std::string name() const override;
    std::vector<VariablePtr> variables() const override;
    std::optional<bool> is_satisfied() const override;
    bool propagate(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t var_idx, Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate() override;
    bool on_last_uninstantiated(Model& model, int save_point,
                                size_t last_var_internal_idx) override;

    void rewind_to(int save_point);

protected:
    void check_initial_consistency() override;

private:
    std::vector<VariablePtr> pos_;  // 正リテラル
    std::vector<VariablePtr> neg_;  // 負リテラル
    size_t n_pos_;                  // pos_ のサイズ
    size_t n_neg_;                  // neg_ のサイズ

    // 2-watched literal
    // w1_, w2_ はリテラルのインデックス
    // 0 <= idx < n_pos_: pos_[idx]
    // n_pos_ <= idx < n_pos_ + n_neg_: neg_[idx - n_pos_]
    size_t w1_;
    size_t w2_;

    // Trail for watched literals
    std::vector<std::tuple<int, size_t, size_t>> watch_trail_;

    // 変数ポインタ → 内部インデックス
    std::unordered_map<Variable*, size_t> var_ptr_to_idx_;

    /**
     * @brief リテラルが節を充足できるか
     * - pos リテラル: 未確定 or = 1 なら充足可能
     * - neg リテラル: 未確定 or = 0 なら充足可能
     */
    bool can_satisfy(size_t lit_idx) const;

    /**
     * @brief リテラルが既に節を充足しているか
     * - pos リテラル: = 1 なら充足
     * - neg リテラル: = 0 なら充足
     */
    bool is_satisfied_by(size_t lit_idx) const;

    /**
     * @brief リテラルを充足方向に確定させる値を取得
     * - pos リテラル: 1
     * - neg リテラル: 0
     */
    Domain::value_type satisfying_value(size_t lit_idx) const;

    /**
     * @brief リテラルに対応する変数を取得
     */
    VariablePtr get_var(size_t lit_idx) const;

    /**
     * @brief 別の watch 候補を探す
     */
    size_t find_unwatched_candidate(size_t exclude1, size_t exclude2) const;

    /**
     * @brief watch を移動
     */
    void move_watch(int save_point, int which_watch, size_t new_idx);
};

} // namespace sabori_csp

#endif // SABORI_CSP_CONSTRAINTS_LOGICAL_HPP
