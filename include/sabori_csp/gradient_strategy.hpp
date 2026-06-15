/**
 * @file gradient_strategy.hpp
 * @brief 最適化探索の疑似勾配ヒント（直前の改善方向）を管理するクラス
 */
#ifndef SABORI_CSP_GRADIENT_STRATEGY_HPP
#define SABORI_CSP_GRADIENT_STRATEGY_HPP

#include "sabori_csp/domain.hpp"

#include <cstddef>
#include <limits>
#include <random>
#include <vector>

namespace sabori_csp {

class Model;

/**
 * @brief 最適化探索における疑似勾配ヒントを管理する（optimize 専用）
 *
 * 連続する改善解の差分から各変数の改善方向（勾配）を推定し、1変数を「勾配ヒント」として
 * 選ぶ。値順序付け（order_values / create_search_frame）はこのヒントを最優先で使い、
 * 改善方向側の値・分岐を先に試す。improvement probe にもヒントを与える。
 *
 * 状態（勾配ベクタ・ヒント変数/方向/基準値・eligible 変数・直前改善解）をカプセル化し、
 * Solver からは accessor 経由で参照する。
 */
class GradientStrategy {
public:
    using value_type = Domain::value_type;
    static constexpr size_t kNoVar = std::numeric_limits<size_t>::max();

    GradientStrategy() = default;

    // ===== ライフサイクル =====

    /// optimize 開始時の全クリア（勾配・ヒント・直前解をリセット）
    void clear() {
        prev_solution_.clear();
        gradient_.clear();
        var_idx_ = kNoVar;
        direction_ = 0;
    }

    /// 勾配候補となる変数（範囲が広く未確定）を収集する
    void rebuild_eligible(const Model& model);

    /// 直前の改善解を記録する（次回の勾配計算の基準）
    void set_prev_solution(const std::vector<value_type>& sol) { prev_solution_ = sol; }

    // ===== ヒント制御 =====

    /// 勾配ヒントを無効化（restart 等で探索を多様化するとき）
    void disable_hint() {
        var_idx_ = kNoVar;
        direction_ = 0;
    }

    /// ヒント変数を消費（値順序付けで一度使ったら解除する）
    void consume_hint() { var_idx_ = kNoVar; }

    /// 指定変数がアクティブな勾配ヒント対象か
    bool hint_active_for(size_t var_idx) const {
        return var_idx == var_idx_ && direction_ != 0;
    }

    int direction() const { return direction_; }
    value_type ref_val() const { return ref_val_; }

    // ===== 勾配計算 =====

    /**
     * @brief 改善解からの疑似勾配計算と勾配ヒント変数の選択
     *
     * prev_solution_ と current_best の差分から各 eligible 変数の勾配符号を更新し、
     * activity 最小（タイは var_size 最大）の1変数をヒント（var/direction/ref_val）に選ぶ。
     * 呼び出し前に disable_hint() 済み（var=kNoVar / direction=0）であることを前提とする。
     */
    void compute(const Model& model,
                 const std::vector<value_type>& current_best,
                 const std::vector<double>& activity,
                 std::mt19937& rng);

private:
    std::vector<double> gradient_;            ///< 疑似勾配（直前の改善方向）
    std::vector<size_t> eligible_vars_;       ///< 勾配を利用する変数インデックス
    std::vector<value_type> prev_solution_;   ///< 直前の改善解（差分計算の基準）
    size_t var_idx_ = kNoVar;                 ///< ヒント対象変数（kNoVar = なし）
    int direction_ = 0;                       ///< ヒント方向（+1/-1、0 = なし）
    value_type ref_val_ = 0;                  ///< ヒント基準値（ベスト解での値）
};

} // namespace sabori_csp

#endif // SABORI_CSP_GRADIENT_STRATEGY_HPP
