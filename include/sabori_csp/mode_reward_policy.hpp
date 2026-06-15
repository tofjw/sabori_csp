/**
 * @file mode_reward_policy.hpp
 * @brief Activity優先/MRV優先の混合比 (mix_p) を restart ごとに適応サンプルするポリシー
 */
#ifndef SABORI_CSP_MODE_REWARD_POLICY_HPP
#define SABORI_CSP_MODE_REWARD_POLICY_HPP

#include <algorithm>
#include <cstddef>
#include <random>

namespace sabori_csp {

/**
 * @brief Activity優先 / MRV優先の混合比 (mix_p) を restart ごとに適応サンプルするポリシー
 *
 * 5段グリッドの各バケットに EMA 報酬を持ち、直近 restart の改善有無から報酬を更新、
 * 報酬比例で次の mix_p を再抽選する。RL 風の自己調整で固定ヒューリスティクスより強い
 * （docs-dev / memory: 変数選択 mix_p の EMA 強化学習が好成績）。
 *
 * 状態と抽選ロジックをカプセル化し、search_with_restart の 2バリアント（通常/最適化）で共有する。
 */
class ModeRewardPolicy {
public:
    static constexpr size_t kGridSize = 5;

    ModeRewardPolicy() = default;

    /// 現在の混合比 (0=MRV優先 .. 1=Activity優先)
    double mix_p() const { return mix_p_; }

    /**
     * @brief mix_p を固定初期化する（set_activity_first 相当）
     *
     * 注意: restart ごとの適応サンプリングが効くため、初期値としてのみ機能する。
     */
    void set_fixed(bool activity_first) {
        p_idx_ = activity_first ? (kGridSize - 1) : 0;
        mix_p_ = activity_first ? 1.0 : 0.0;
    }

    /// この restart 内で改善（SAT/probe）が起きたことを記録する
    void note_improvement() { improvement_ = true; }

    /// 到達した探索深さを観測する（直近 restart 内の最大値を保持）
    void observe_depth(size_t depth) {
        if (depth > max_depth_) max_depth_ = depth;
    }

    /**
     * @brief restart 直前: 報酬を EMA 更新し mix_p を再抽選する
     *
     * signal = 改善あり ? 2.0 : 1/(1+max_depth) を active バケット（隣接は 0.1倍）に与え、
     * r ← decay*r + (1-decay)*bucket_signal で更新。floor でクランプ後、報酬比例で抽選する。
     * improvement_ / max_depth_ は消費後リセットする。
     */
    void update_and_resample(std::mt19937& rng) {
        double signal = improvement_
            ? 2.0
            : 1.0 / static_cast<double>(1 + max_depth_);
        improvement_ = false;
        max_depth_ = 0;
        double total = 0.0;
        for (size_t i = 0; i < kGridSize; ++i) {
            double bucket_signal = 0.0;
            if (i == p_idx_) {
                bucket_signal = signal;
            } else if (i + 1 == p_idx_ || i == p_idx_ + 1) {
                bucket_signal = 0.1 * signal;
            }
            reward_[i] = kDecay * reward_[i] + (1.0 - kDecay) * bucket_signal;
            reward_[i] = std::max(reward_[i], kFloor);
            total += reward_[i];
        }
        std::uniform_real_distribution<double> dist(0.0, total);
        double pick = dist(rng);
        double acc = 0.0;
        p_idx_ = kGridSize - 1;
        for (size_t i = 0; i < kGridSize; ++i) {
            acc += reward_[i];
            if (pick < acc) {
                p_idx_ = i;
                break;
            }
        }
        mix_p_ = static_cast<double>(p_idx_) / static_cast<double>(kGridSize - 1);
    }

private:
    static constexpr double kDecay = 0.5;   ///< EMA 減衰率 α
    static constexpr double kFloor = 0.1;   ///< 報酬の下限（探索維持）

    double reward_[kGridSize] = {1.0, 1.0, 1.0, 1.0, 1.0};
    size_t p_idx_ = 2;            ///< 現在使用中のバケット
    double mix_p_ = 0.5;          ///< 現在の混合比（restart で再サンプル）
    bool improvement_ = false;    ///< この restart 内で改善が起きたか
    size_t max_depth_ = 0;        ///< 直近 restart 内で到達した最大探索深さ
};

} // namespace sabori_csp

#endif // SABORI_CSP_MODE_REWARD_POLICY_HPP
