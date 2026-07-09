#ifndef SABORI_CSP_CONSTRAINTS_LOGICAL_HPP
#define SABORI_CSP_CONSTRAINTS_LOGICAL_HPP

#include "sabori_csp/constraint.hpp"
#include <cstddef>
#include <unordered_map>
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


    bool prepare_propagation(Model& model) override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;
    bool on_last_uninstantiated(Model& model, int save_point,
                                size_t last_var_internal_idx) override;

    void bump_activity(const Model& model, size_t trigger_var_idx,
                       double* activity, double activity_inc,
                       bool& need_rescale, std::mt19937& rng) const override;

protected:


private:
    size_t n_;                        // bool変数の数（var_ids_ の先頭 n_ 個）
    size_t r_id_;                     // 結果変数のID（var_ids_[n_]）

    // 2-watched literal: r = 0 のとき、0 になりうる変数を2つ監視
    size_t w1_;  // 1つ目の watched index
    size_t w2_;  // 2つ目の watched index

    // 変数ID → 内部インデックス（on_instantiate用、O(1)検索）
    std::unordered_map<size_t, size_t> var_id_to_idx_;

    /**
     * @brief 未確定または 0 を含むドメインを持つ変数のインデックスを探す
     * @param model モデルへの参照
     * @param exclude1 除外するインデックス
     * @param exclude2 除外するインデックス
     * @return 見つかったインデックス、なければ SIZE_MAX
     */
    size_t find_unwatched_candidate(const Model& model, size_t exclude1, size_t exclude2) const;

    /**
     * @brief watched literal を移動する
     * @param model モデルへの参照
     * @param save_point 現在のセーブポイント
     * @param which_watch 移動する watch (1 or 2)
     * @param new_idx 新しいインデックス
     */
    void move_watch(Model& model, int save_point, int which_watch, size_t new_idx);
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


    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;
    bool on_last_uninstantiated(Model& model, int save_point,
                                size_t last_var_internal_idx) override;


protected:


private:
    size_t n_;                        // bool変数の数（var_ids_ の先頭 n_ 個）
    size_t r_id_;                     // 結果変数のID（var_ids_[n_]）

    size_t w1_;
    size_t w2_;

    // 変数ID → 内部インデックス（on_instantiate用、O(1)検索）
    std::unordered_map<size_t, size_t> var_id_to_idx_;

    size_t find_unwatched_candidate(const Model& model, size_t exclude1, size_t exclude2) const;
    void move_watch(Model& model, int save_point, int which_watch, size_t new_idx);
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


    bool prepare_propagation(Model& model) override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;
    bool on_last_uninstantiated(Model& model, int save_point,
                                size_t last_var_internal_idx) override;


protected:


private:
    size_t n_pos_;                  // 正リテラル数
    size_t n_neg_;                  // 負リテラル数

    std::vector<size_t> pos_ids_;  // 正リテラルの変数ID
    std::vector<size_t> neg_ids_;  // 負リテラルの変数ID

    // 2-watched literal
    // w1_, w2_ はリテラルのインデックス
    // 0 <= idx < n_pos_: pos_[idx]
    // n_pos_ <= idx < n_pos_ + n_neg_: neg_[idx - n_pos_]
    size_t w1_;
    size_t w2_;

    // 変数ID → リテラルインデックス（on_instantiate用、O(1)検索）
    std::unordered_map<size_t, size_t> var_id_to_lit_idx_;

    // 同じ変数が pos と neg 両方に現れる場合、節は恒真 (x ∨ ¬x)
    bool is_tautology_ = false;

    /**
     * @brief リテラルが節を充足できるか（Model参照版）
     */
    bool can_satisfy(const Model& model, size_t lit_idx) const;

    /**
     * @brief リテラルが既に節を充足しているか（Model参照版）
     */
    bool is_satisfied_by(const Model& model, size_t lit_idx) const;

    /**
     * @brief リテラルを充足方向に確定させる値を取得
     */
    Domain::value_type satisfying_value(size_t lit_idx) const;

    /**
     * @brief リテラルに対応する変数IDを取得
     */
    size_t get_var_id(size_t lit_idx) const;

    /**
     * @brief 別の watch 候補を探す
     */
    size_t find_unwatched_candidate(const Model& model, size_t exclude1, size_t exclude2) const;

    /**
     * @brief watch を移動
     */
    void move_watch(Model& model, int save_point, int which_watch, size_t new_idx);
};

/**
 * @brief bool_not制約: ¬a = b (つまり a + b = 1)
 *
 * a と b は論理否定の関係。
 * - a = 0 → b = 1
 * - a = 1 → b = 0
 * - b = 0 → a = 1
 * - b = 1 → a = 0
 */
class BoolNotConstraint : public Constraint {
public:
    BoolNotConstraint(VariablePtr a, VariablePtr b);

    std::string name() const override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;

private:
    size_t a_id_;
    size_t b_id_;
};

/**
 * @brief array_bool_xor制約: XOR(b1, b2, ..., bn) = true
 *
 * 配列の全要素のXOR（パリティ）が true（1が奇数個）であることを要求する。
 * 結果変数を持たない（FlatZinc仕様: array_bool_xor(array[int] of var bool)）。
 */
class ArrayBoolXorConstraint : public Constraint {
public:
    ArrayBoolXorConstraint(std::vector<VariablePtr> vars);

    std::string name() const override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;
    bool on_last_uninstantiated(Model& model, int save_point,
                                size_t last_var_internal_idx) override;

private:
    size_t n_;
    std::unordered_map<size_t, size_t> var_id_to_idx_;
};

/**
 * @brief bool_xor制約: c = (a XOR b)
 *
 * 3つのbool変数 a, b, c について c = (a != b) を双方向に伝播する。
 * - a, b 確定 → c = (a != b)
 * - a, c 確定 → b = (a XOR c)
 * - b, c 確定 → a = (b XOR c)
 */
class BoolXorConstraint : public Constraint {
public:
    BoolXorConstraint(VariablePtr a, VariablePtr b, VariablePtr c);

    std::string name() const override;
    PresolveResult presolve(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;

private:
    size_t a_id_;
    size_t b_id_;
    size_t c_id_;
};

/**
 * @brief 節の witness チャネリング: s = min{ i : literal_i が真 }
 *
 * bool_clause (pos ∨ ¬neg) に対し「最初に真になる literal の index」を表す
 * 補助整数変数 s (0..L-1、decision 層) を導入する。伝播力は節と等価だが、
 * (1) 探索が「節をどう充足するか」へ直接分岐できる
 * (2) 節がらみの conflict の activity が s に集約される (節単位の紛争度)
 * (3) min 意味論で s は関数的 = 全解探索でも解が重複しない
 * (4) s=i の分岐は「l_0..l_{i-1} 偽 ∧ l_i 真」= witness 対称性を自動破壊。
 * literal index は pos が先、neg が後 (i < npos は b=1 が真、以降は b=0 が真)。
 * 既存の BoolClauseConstraint (2WL) は温存し、本制約は純粋に探索ハンドル。
 * SABORI_CLAUSE_WITNESS=<最小節長> で opt-in (constraint_registry 参照)。
 */
class ClauseWitnessConstraint : public Constraint {
public:
    ClauseWitnessConstraint(const std::vector<VariablePtr>& pos,
                            const std::vector<VariablePtr>& neg,
                            VariablePtr s, bool min_semantics = true);

    std::string name() const override;

    PresolveResult presolve(Model& model) override;
    bool prepare_propagation(Model& model) override;

    bool on_instantiate(Model& model, int save_point,
                        size_t internal_var_idx,
                        Domain::value_type value,
                        Domain::value_type prev_min, Domain::value_type prev_max) override;
    bool on_final_instantiate(const Model& model) override;

    bool on_set_min(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_min, Domain::value_type old_min) override;
    bool on_set_max(Model& model, int save_point,
                    size_t internal_var_idx,
                    Domain::value_type new_max, Domain::value_type old_max) override;
    bool on_remove_value(Model& model, int save_point,
                         size_t internal_var_idx,
                         Domain::value_type removed_value) override;
    bool propagate_batch(Model& model, int save_point) override;

    void rewind_to(int save_point) override;

private:
    bool propagate_impl(Model& model, bool direct, bool& changed,
                        int save_point = -1);

    size_t n_;     ///< literal 数 (var_ids_[0..n_) = literals, var_ids_[n_] = s)
    size_t npos_;  ///< 正リテラル数
    bool min_;     ///< true=min 意味論 (関数的・-a 安全) / false=plain
                   ///< (「s 番目が真」のみ。-a では解が重複するので単解モード限定)
};

} // namespace sabori_csp

#endif // SABORI_CSP_CONSTRAINTS_LOGICAL_HPP
