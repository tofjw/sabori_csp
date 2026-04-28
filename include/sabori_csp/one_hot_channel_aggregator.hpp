/**
 * @file one_hot_channel_aggregator.hpp
 * @brief one-hot チャネリング集約: 同一 x に紐付く int_eq_reif 群を
 *        IntOneHotChannelConstraint へまとめる core presolve pass
 */
#ifndef SABORI_CSP_ONE_HOT_CHANNEL_AGGREGATOR_HPP
#define SABORI_CSP_ONE_HOT_CHANNEL_AGGREGATOR_HPP

#include "sabori_csp/model.hpp"

namespace sabori_csp {

class OneHotChannelAggregator {
public:
    /**
     * @brief 集約を実行する。Phase 1 presolve 後・prepare_propagation 前に呼ぶ。
     *
     * 同一の整数変数 x を持ち、定数引数を持つ IntEqReifConstraint 群を検出して
     * 1 個の IntOneHotChannelConstraint に置換する。集約後 compact_constraints()
     * を呼んで配列を詰める。
     *
     * @param model 対象モデル
     * @param verbose 集約発火時にログを stderr へ出すか
     * @return 矛盾検出時 false（その場合 model の一部状態は変化している）
     */
    bool aggregate(Model& model, bool verbose = false);

    /// 集約最低件数（同一 x にぶら下がる reif がこの数以上で発火）。デフォルト 2。
    void set_min_group_size(size_t n) { min_group_size_ = n; }

private:
    size_t min_group_size_ = 2;
};

} // namespace sabori_csp

#endif // SABORI_CSP_ONE_HOT_CHANNEL_AGGREGATOR_HPP
