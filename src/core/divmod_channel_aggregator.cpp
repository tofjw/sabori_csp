/**
 * @file divmod_channel_aggregator.cpp
 */
#include "sabori_csp/divmod_channel_aggregator.hpp"
#include "sabori_csp/constraints/arithmetic.hpp"
#include "sabori_csp/constraints/global/linear.hpp"
#include <iostream>
#include <map>

namespace sabori_csp {

namespace {

/// (被除数の変数 ID, 定数除数) をキーにした div/mod のマッチング用エントリ
struct PairEntry {
    size_t div_idx = static_cast<size_t>(-1);  ///< int_div の制約インデックス
    size_t mod_idx = static_cast<size_t>(-1);  ///< int_mod の制約インデックス
    size_t q_id = 0;                           ///< 商の変数 ID
    size_t r_id = 0;                           ///< 余りの変数 ID
};

/// y が定数かつ正なら its 値を返す
std::optional<Domain::value_type> positive_constant(const Model& model, size_t var_id) {
    auto* v = model.variable(var_id);
    if (v->min() != v->max()) return std::nullopt;
    if (v->min() <= 0) return std::nullopt;
    return v->min();
}

} // namespace

bool DivModChannelAggregator::aggregate(Model& model, bool verbose) {
    applied_ = 0;
    const auto& constraints = model.constraints();

    // 1. (x_id, 定数除数 c) をキーに int_div / int_mod を突き合わせる。
    //    キーは std::map（順序付き）にして走査順を決定的にする。
    std::map<std::pair<size_t, Domain::value_type>, PairEntry> pairs;

    for (size_t ci = 0; ci < constraints.size(); ++ci) {
        if (!constraints[ci]) continue;

        if (auto* d = dynamic_cast<IntDivConstraint*>(constraints[ci].get())) {
            auto c = positive_constant(model, d->y_id());
            if (!c) continue;
            auto& e = pairs[{d->x_id(), *c}];
            // 同じ (x, c) の div が複数あっても最初の 1 本だけを置換対象にする
            // （残りは冗長制約としてそのまま残す＝健全）
            if (e.div_idx == static_cast<size_t>(-1)) {
                e.div_idx = ci;
                e.q_id = d->z_id();
            }
            continue;
        }

        if (auto* m = dynamic_cast<IntModConstraint*>(constraints[ci].get())) {
            auto c = positive_constant(model, m->y_id());
            if (!c) continue;
            auto& e = pairs[{m->x_id(), *c}];
            if (e.mod_idx == static_cast<size_t>(-1)) {
                e.mod_idx = ci;
                e.r_id = m->z_id();
            }
            continue;
        }
    }

    // 2. div/mod が揃ったペアを x = c*q + r へ書き換える。

    for (const auto& [key, e] : pairs) {
        if (e.div_idx == static_cast<size_t>(-1)) continue;
        if (e.mod_idx == static_cast<size_t>(-1)) continue;

        const size_t x_id = key.first;
        const Domain::value_type c = key.second;

        // 被除数が非負のときのみ置換可能。
        // truncated division と floor division が一致し、r >= 0 が保証されるため
        // 「x = c*q + r かつ 0 <= r < c」がユークリッド除算と厳密に等価になる。
        if (model.variable(x_id)->min() < 0) continue;

        // 変数の重複は線形制約側の前提を崩しうるので除外
        if (e.q_id == e.r_id || e.q_id == x_id || e.r_id == x_id) continue;

        // r の範囲を 0 <= r < c に絞る（線形制約だけでは表せない部分）。
        // Phase 1 presolve 済みなら既に成立しているはずだが、置換の健全性が
        // この条件に依存するので明示的に課す。
        auto* r_var = model.variable(e.r_id);
        if (r_var->min() < 0) {
            if (!r_var->remove_below(0)) return false;
        }
        if (r_var->max() > c - 1) {
            if (!r_var->remove_above(c - 1)) return false;
        }

        // 元の div/mod のラベルを引き継ぐ（例 "int_div:L2968,int_mod:L3023"）
        std::string joined_label = constraints[e.div_idx]->label();
        const std::string& mod_label = constraints[e.mod_idx]->label();
        if (!mod_label.empty()) {
            if (!joined_label.empty()) joined_label += ',';
            joined_label += mod_label;
        }

        if (replace_) {
            // div/mod を専用のチャネル制約 1 本で置き換える。
            // 線形だけで置換すると IntDiv/IntMod のドメインフィルタが失われるが、
            // IntDivModChannel はそれを内包した上で伝播器を 3 本 → 1 本に減らす。
            auto ch = std::make_shared<IntDivModChannelConstraint>(
                model.variable(x_id), c, model.variable(e.q_id), model.variable(e.r_id));
            ch->set_sweep_on_bounds(sweep_on_bounds_);
            ch->set_label(std::move(joined_label));
            model.replace_constraint(e.div_idx, ch);
            model.remove_constraint(e.mod_idx);
        } else {
            // 既定は線形制約の追加（div/mod はそのまま残す）。
            // c*q + r - x = 0
            std::vector<int64_t> coeffs{static_cast<int64_t>(c), 1, -1};
            std::vector<VariablePtr> vars{model.variable(e.q_id),
                                          model.variable(e.r_id),
                                          model.variable(x_id)};
            auto lin = std::make_shared<IntLinEqConstraint>(std::move(coeffs),
                                                            std::move(vars), 0);
            lin->set_label(std::move(joined_label));
            model.add_constraint(std::move(lin));
        }
        ++applied_;
    }

    if (applied_ > 0) {
        if (replace_) model.compact_constraints();
        if (verbose) {
            std::cerr << "% [verbose] DivModChannelAggregator: "
                      << applied_ << " int_div/int_mod pair -> " << applied_
                      << (replace_ ? " IntDivModChannel (replace)"
                                   : " IntLinEq (augment)") << "\n";
        }
    }

    return true;
}

} // namespace sabori_csp
