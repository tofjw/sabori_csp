/**
 * @file divmod_channel_aggregator.hpp
 * @brief ユークリッド除算チャネリング集約: 同一の被除数・同一の定数除数を持つ
 *        int_div / int_mod のペアを 1 本の線形制約へまとめる core presolve pass
 */
#ifndef SABORI_CSP_DIVMOD_CHANNEL_AGGREGATOR_HPP
#define SABORI_CSP_DIVMOD_CHANNEL_AGGREGATOR_HPP

#include "sabori_csp/model.hpp"

namespace sabori_csp {

/**
 * @brief int_div(x, c, q) と int_mod(x, c, r) のペアを x = c*q + r へ置換する pass
 *
 * MiniZinc の grid indexing イディオム（`(n-1) div W`, `(n-1) mod W`）は
 * FlatZinc で同一被除数・同一定数除数の div/mod ペアに分解される。
 * IntModConstraint は余り側から被除数へ bounds を返さない（被除数のドメインを
 * 絞るのは除数・余りが確定したときのみ）ため、この分解のままだと
 * 「被除数の値が削れても座標に伝わらない」片方向の伝播になってしまう。
 *
 * x >= 0 かつ c > 0 のとき、ユークリッド除算の一意性から
 *   q = x div c  かつ  r = x mod c   <=>   x = c*q + r  かつ  0 <= r < c
 * が成り立つので、div/mod 2 本を線形 1 本へ **置換** できる（追加ではない）。
 * 線形制約は両方向に bounds 伝播するため、q・r の変化が x に、
 * x の変化が q・r に届くようになる。
 */
class DivModChannelAggregator {
public:
    /**
     * @brief 集約を実行する。Phase 1 presolve 後・prepare_propagation 前に呼ぶ。
     *
     * @param model 対象モデル
     * @param verbose 集約発火時にログを stderr へ出すか
     * @return 矛盾検出時 false（その場合 model の一部状態は変化している）
     */
    bool aggregate(Model& model, bool verbose = false);

    /**
     * @brief 元の div/mod を削除するか（既定 false = 線形制約を追加するだけ）
     *
     * true のとき div/mod を IntDivModChannelConstraint 1 本へ置き換える
     * （伝播器 3 本 → 1 本）。false（既定）のときは線形制約 c*q + r - x = 0 を
     * 追加するだけで div/mod は残す。
     */
    void set_replace(bool v) { replace_ = v; }

    /// 置換モードで生成する IntDivModChannel の値走査ポリシー（既定 false）
    void set_sweep_on_bounds(bool v) { sweep_on_bounds_ = v; }

    /**
     * @brief 直前の aggregate() で書き換えたペア数
     *
     * 0 のときモデルは一切変更されていない。呼び出し側は
     * build_constraint_watch_list() の再構築をこれで条件付けること
     * （無条件に再構築すると、集約が発火しないモデルでも探索軌道が変わる）。
     */
    size_t applied() const { return applied_; }

private:
    bool replace_ = false;
    bool sweep_on_bounds_ = false;
    size_t applied_ = 0;
};

} // namespace sabori_csp

#endif // SABORI_CSP_DIVMOD_CHANNEL_AGGREGATOR_HPP
