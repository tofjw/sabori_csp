/**
 * @file phase_reward_policy.hpp
 * @brief phase hint の適用下限 (p_min) を restart ごとに適応サンプルするポリシー
 */
#ifndef SABORI_CSP_PHASE_REWARD_POLICY_HPP
#define SABORI_CSP_PHASE_REWARD_POLICY_HPP

#include <algorithm>
#include <cstddef>
#include <random>

namespace sabori_csp {

/**
 * @brief phase hint（保存値の値順序ヒント）の適用下限 p_min を適応サンプルするポリシー
 *
 * 値順序ヒントを「activity（失敗経験）に比例した確率」で適用する際の下限を、
 * restart ごとに 5段グリッドから再抽選する。判定は
 *   p(v) = clamp(activity[v] / 平均activity, p_min, 1.0)
 * で、失敗を多く経験した変数のヒントは必ず使い、経験の無い変数は p_min の確率でしか
 * 使わない（＝大半をランダムに倒す）。
 *
 * **上端 p_min = 1.0 は「常にヒントを適用する」＝従来の既定挙動**。バンディットが
 * それを最良と学習すれば既定に戻れるので、大崩れしにくい。
 *
 * 固定値の探索は既に尽きている（2024/2025 の38問 x 8シードで p_min を
 * 0.0/0.1/0.2/0.5 と振ったが、いずれも既定を安定して上回れなかった。最良に見えた
 * 0.2 も 8シードでは 0.93 標準偏差で有意差なし）。一方で「ヒントを全く使わない」は
 * net -23 と大きく劣るため、この次元には信号がある。固定値では拾えないので自己調整に移す。
 *
 * 実装は ModeRewardPolicy と同じ EMA バンディット構造。実績のある同クラスを触ると
 * 巻き込み事故の危険があるため、あえて共通化せず並置している（将来まとめてよい）。
 */
class PhaseRewardPolicy {
public:
    static constexpr size_t kGridSize = 5;

    PhaseRewardPolicy() = default;

    /// 現在の適用下限（0=経験の無い変数はヒントを使わない .. 1=常に使う＝従来既定）
    double p_min() const { return p_min_; }

    /**
     * @brief p_min をバケット idx に固定し、以後の適応サンプリングを無効化する（A/B 用）
     */
    void pin(size_t idx) {
        if (idx >= kGridSize) idx = kGridSize - 1;
        pinned_ = true;
        idx_ = idx;
        p_min_ = static_cast<double>(idx) / static_cast<double>(kGridSize - 1);
    }

    /// この restart 内で改善（SAT/probe）が起きたことを記録する
    void note_improvement() { improvement_ = true; }

    /// 到達した探索深さを観測する（直近 restart 内の最大値を保持）
    void observe_depth(size_t depth) {
        if (depth > max_depth_) max_depth_ = depth;
    }

    /**
     * @brief restart 直前: 報酬を EMA 更新し p_min を再抽選する
     *
     * ModeRewardPolicy と同じ信号・同じ更新則を使う。
     * signal = 改善あり ? 2.0 : 1/(1+max_depth) を active バケット（隣接は 0.1倍）に与え、
     * r ← decay*r + (1-decay)*bucket_signal で更新。floor でクランプ後、報酬比例で抽選する。
     */
    void update_and_resample(std::mt19937& rng) {
        if (pinned_) { improvement_ = false; max_depth_ = 0; return; }
        double signal = improvement_
            ? 2.0
            : 1.0 / static_cast<double>(1 + max_depth_);
        improvement_ = false;
        max_depth_ = 0;
        double total = 0.0;
        for (size_t i = 0; i < kGridSize; ++i) {
            double bucket_signal = 0.0;
            if (i == idx_) {
                bucket_signal = signal;
            } else if (i + 1 == idx_ || i == idx_ + 1) {
                bucket_signal = 0.1 * signal;
            }
            reward_[i] = kDecay * reward_[i] + (1.0 - kDecay) * bucket_signal;
            reward_[i] = std::max(reward_[i], kFloor);
            total += reward_[i];
        }
        std::uniform_real_distribution<double> dist(0.0, total);
        double pick = dist(rng);
        double acc = 0.0;
        idx_ = kGridSize - 1;
        for (size_t i = 0; i < kGridSize; ++i) {
            acc += reward_[i];
            if (pick < acc) {
                idx_ = i;
                break;
            }
        }
        p_min_ = static_cast<double>(idx_) / static_cast<double>(kGridSize - 1);
    }

private:
    static constexpr double kDecay = 0.5;   ///< EMA 減衰率 α
    static constexpr double kFloor = 0.1;   ///< 報酬の下限（探索維持）

    double reward_[kGridSize] = {1.0, 1.0, 1.0, 1.0, 1.0};
    size_t idx_ = kGridSize - 1;  ///< 現在のバケット（初期は従来既定の p_min=1.0）
    double p_min_ = 1.0;          ///< 現在の適用下限（restart で再サンプル）
    bool improvement_ = false;
    size_t max_depth_ = 0;
    bool pinned_ = false;
};

} // namespace sabori_csp

#endif // SABORI_CSP_PHASE_REWARD_POLICY_HPP
