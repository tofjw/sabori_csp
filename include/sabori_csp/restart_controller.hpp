/**
 * @file restart_controller.hpp
 * @brief リスタート戦略コントローラ（Adaptive Inner/Outer ループ）
 */
#ifndef SABORI_CSP_RESTART_CONTROLLER_HPP
#define SABORI_CSP_RESTART_CONTROLLER_HPP

#include <algorithm>
#include <cstddef>
#include <random>

namespace sabori_csp {

/**
 * @brief リスタート戦略を管理するクラス
 *
 * 内側ループ: conflict_limit を inner_ratio_ 倍ずつ増やしながらリスタートを繰り返す。
 * 外側ループ: cycle 終了時に prune/depth の状況に応じて outer を grow/shrink する。
 * 全パラメータと inner/outer 状態をカプセル化し、search_with_restart の
 * 2つのバリアント（通常 / 最適化）で共有する。
 *
 * 計測用に outer 調整の決定則（Policy）を差し替えられる。adaptive 系は
 * パラメータ包絡（shrink 0.99 / grow 1.2 / min / max / inner 1.01）を共有し、
 * 「決定が信号と相関しているか」だけが異なる。fixed 系（Luby）は
 * inner/outer 機構をバイパスして文献標準のリスタート列を生成する。
 * （inverted / prune_only / depth_only / always_tighten / always_widen /
 *   geometric / constant のアブレーション variant は 2026-07 に撤去
 *   — pre-flag-purge タグ参照）
 */
class RestartController {
public:
    /**
     * @brief outer 調整の決定則
     */
    enum class Policy {
        Adaptive,       ///< 現行: prune_delta>0 && depth_grew で tighten、それ以外 widen
        Scrambled,      ///< 信号を無視し、固定確率 scramble_p_ のコイントスで tighten
        Luby            ///< fixed: conflict_limit = base × Luby列（cycle 機構なし）
    };

    RestartController() = default;

    // ===== 探索開始 =====

    /**
     * @brief 新しい探索を開始する際に呼ぶ
     */
    void reset() {
        outer_ = initial_outer_ceiling_;
        inner_ = initial_conflict_limit_;
        restart_idx_ = 1;
        fixed_cur_ = fixed_base_;
        tighten_count_ = 0;
        widen_count_ = 0;
    }

    // ===== Cycle 管理 =====

    /**
     * @brief 新しいサイクルを開始（inner をリセット）
     *
     * fixed 系ポリシーではリスタート列を継続するため no-op。
     */
    void begin_cycle() {
        if (is_fixed_policy()) return;
        inner_ = initial_conflict_limit_;
    }

    /**
     * @brief 現在のコンフリクト制限を取得
     */
    int conflict_limit() const {
        if (is_fixed_policy()) {
            return static_cast<int>(fixed_cur_);
        }
        return static_cast<int>(inner_);
    }

    /**
     * @brief inner が outer 以下かどうか（内側ループの継続条件）
     *
     * fixed 系ポリシーは cycle を持たないため常に true。
     */
    bool inner_within_outer() const {
        if (is_fixed_policy()) return true;
        return inner_ <= outer_;
    }

    /**
     * @brief inner を進める（リスタート後に呼ぶ）
     */
    void advance_inner() {
        if (is_fixed_policy()) {
            ++restart_idx_;
            fixed_cur_ = fixed_base_ * static_cast<double>(luby(restart_idx_));
            return;
        }
        inner_ *= inner_ratio_;
    }

    /**
     * @brief サイクル終了時の outer 調整
     * @param prune_delta このサイクル中の NoGood prune 数
     * @param depth_grew このサイクル中に探索深度が伸びたか
     * @param rng コイントス用 RNG（Scrambled でのみ消費される）
     * @return tighten したら true、widen したら false（fixed 系は常に false）
     */
    bool end_cycle(size_t prune_delta, bool depth_grew, std::mt19937& rng) {
        bool signal = (prune_delta > 0 && depth_grew);
        bool tighten = false;
        switch (policy_) {
        case Policy::Adaptive:      tighten = signal; break;
        case Policy::Scrambled:
            tighten = std::uniform_real_distribution<double>(0.0, 1.0)(rng) < scramble_p_;
            break;
        default:  // fixed 系: outer 調整なし
            return false;
        }
        if (tighten) {
            outer_ = std::max(outer_ * outer_shrink_factor_, outer_min_);
            ++tighten_count_;
        } else {
            outer_ = std::min(outer_ * outer_grow_factor_, outer_max_);
            ++widen_count_;
        }
        return tighten;
    }

    /**
     * @brief outer を初期値にリセット（最適化で改善解が見つかったとき）
     *
     * fixed 系ポリシーではリスタート列を維持するため no-op。
     */
    void reset_outer() {
        if (is_fixed_policy()) return;
        outer_ = initial_outer_ceiling_;
    }

    // ===== アクセサ =====

    double outer() const { return outer_; }
    double activity_decay() const { return activity_decay_; }
    Policy policy() const { return policy_; }

    /**
     * @brief fixed 系ポリシー（Luby）かどうか
     *
     * fixed 系では conflict_limit() は per-node 予算ではなく
     * 「リスタートまでのグローバル fail 数」を意味する。
     */
    bool is_fixed() const { return is_fixed_policy(); }
    size_t tighten_count() const { return tighten_count_; }
    size_t widen_count() const { return widen_count_; }

    /**
     * @brief outer 調整の決定則を設定（計測用）。reset() の前に呼ぶこと。
     * @param policy 決定則
     * @param param Scrambled: tighten 確率 / Luby: base
     */
    void set_policy(Policy policy, double param = 0.0) {
        policy_ = policy;
        switch (policy) {
        case Policy::Scrambled:
            scramble_p_ = (param > 0.0 && param < 1.0) ? param : 0.5;
            break;
        case Policy::Luby:
            fixed_base_ = (param > 0.0) ? param : 100.0;
            break;
        default:
            break;
        }
    }

private:
    bool is_fixed_policy() const {
        return policy_ == Policy::Luby;
    }

    /**
     * @brief Luby 列（1-indexed）: 1,1,2,1,1,2,4,1,1,2,1,1,2,4,8,...
     */
    static size_t luby(size_t i) {
        while (true) {
            size_t k = 1;
            while ((size_t{1} << k) - 1 < i) ++k;
            if ((size_t{1} << k) - 1 == i) return size_t{1} << (k - 1);
            i -= (size_t{1} << (k - 1)) - 1;
        }
    }

    // パラメータ
    double initial_conflict_limit_ = 2.0;
    double inner_ratio_ = 1.01;
    double initial_outer_ceiling_ = 4.0;
    double outer_min_ = 3.0;
    double outer_max_ = 10000.0;
    double outer_grow_factor_ = 1.2;
    double outer_shrink_factor_ = 0.99;
    double activity_decay_ = 0.99;

    Policy policy_ = Policy::Adaptive;
    double scramble_p_ = 0.5;     ///< Scrambled の tighten 確率
    double fixed_base_ = 100.0;   ///< Luby の base

    // 状態
    double inner_ = 2.0;
    double outer_ = 4.0;
    size_t restart_idx_ = 1;      ///< fixed 系のリスタート番号（1-indexed）
    double fixed_cur_ = 100.0;    ///< fixed 系の現在の conflict_limit
    size_t tighten_count_ = 0;    ///< 計測用: tighten 決定の回数
    size_t widen_count_ = 0;      ///< 計測用: widen 決定の回数
};

} // namespace sabori_csp

#endif // SABORI_CSP_RESTART_CONTROLLER_HPP
