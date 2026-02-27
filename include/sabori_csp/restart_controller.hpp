/**
 * @file restart_controller.hpp
 * @brief リスタート戦略コントローラ（Adaptive Inner/Outer ループ）
 */
#ifndef SABORI_CSP_RESTART_CONTROLLER_HPP
#define SABORI_CSP_RESTART_CONTROLLER_HPP

#include <algorithm>
#include <cstddef>

namespace sabori_csp {

/**
 * @brief リスタート戦略を管理するクラス
 *
 * 内側ループ: conflict_limit を inner_ratio_ 倍ずつ増やしながらリスタートを繰り返す。
 * 外側ループ: cycle 終了時に prune/depth の状況に応じて outer を grow/shrink する。
 * 全パラメータと inner/outer 状態をカプセル化し、search_with_restart の
 * 2つのバリアント（通常 / 最適化）で共有する。
 */
class RestartController {
public:
    RestartController() = default;

    // ===== 探索開始 =====

    /**
     * @brief 新しい探索を開始する際に呼ぶ
     */
    void reset() {
        outer_ = initial_outer_ceiling_;
        inner_ = initial_conflict_limit_;
    }

    // ===== Cycle 管理 =====

    /**
     * @brief 新しいサイクルを開始（inner をリセット）
     */
    void begin_cycle() {
        inner_ = initial_conflict_limit_;
    }

    /**
     * @brief 現在のコンフリクト制限を取得
     */
    int conflict_limit() const {
        return static_cast<int>(inner_);
    }

    /**
     * @brief inner が outer 以下かどうか（内側ループの継続条件）
     */
    bool inner_within_outer() const {
        return inner_ <= outer_;
    }

    /**
     * @brief inner を進める（リスタート後に呼ぶ）
     */
    void advance_inner() {
        inner_ *= inner_ratio_;
    }

    /**
     * @brief サイクル終了時の outer 調整
     * @param prune_delta このサイクル中の NoGood prune 数
     * @param depth_grew このサイクル中に探索深度が伸びたか
     */
    void end_cycle(size_t prune_delta, bool depth_grew) {
        if (prune_delta > 0 && depth_grew) {
            outer_ = std::max(outer_ * outer_shrink_factor_, outer_min_);
        } else {
            outer_ = std::min(outer_ * outer_grow_factor_, outer_max_);
        }
    }

    /**
     * @brief outer を初期値にリセット（最適化で改善解が見つかったとき）
     */
    void reset_outer() {
        outer_ = initial_outer_ceiling_;
    }

    // ===== アクセサ =====

    double outer() const { return outer_; }
    double activity_decay() const { return activity_decay_; }

private:
    // パラメータ
    double initial_conflict_limit_ = 2.0;
    double inner_ratio_ = 1.01;
    double initial_outer_ceiling_ = 4.0;
    double outer_min_ = 3.0;
    double outer_max_ = 10000.0;
    double outer_grow_factor_ = 1.2;
    double outer_shrink_factor_ = 0.99;
    double activity_decay_ = 0.99;

    // 状態
    double inner_ = 2.0;
    double outer_ = 4.0;
};

} // namespace sabori_csp

#endif // SABORI_CSP_RESTART_CONTROLLER_HPP
