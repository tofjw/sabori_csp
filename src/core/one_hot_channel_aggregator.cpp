/**
 * @file one_hot_channel_aggregator.cpp
 */
#include "sabori_csp/one_hot_channel_aggregator.hpp"
#include "sabori_csp/constraints/comparison.hpp"
#include "sabori_csp/constraints/global.hpp"
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

namespace sabori_csp {

namespace {

struct ReifCandidate {
    size_t constraint_idx;
    size_t x_id;       // 非定数側の整数変数 ID
    size_t b_id;       // ブール側の変数 ID
    Domain::value_type value;  // 定数側の値
    std::string label; // 元の制約のラベル（FZN 行番号など）
};

bool is_constant(const Model& model, size_t var_id) {
    auto* v = model.variable(var_id);
    return v->min() == v->max();
}

} // namespace

bool OneHotChannelAggregator::aggregate(Model& model, bool verbose) {
    const auto& constraints = model.constraints();

    // 1. IntEqReifConstraint で「片側が定数」のものを候補化し、x_id でグループ化
    std::unordered_map<size_t, std::vector<ReifCandidate>> groups;

    for (size_t ci = 0; ci < constraints.size(); ++ci) {
        if (!constraints[ci]) continue;
        auto* eq_reif = dynamic_cast<IntEqReifConstraint*>(constraints[ci].get());
        if (!eq_reif) continue;

        size_t xid = eq_reif->x_id();
        size_t yid = eq_reif->y_id();
        size_t bid = eq_reif->b_id();

        bool x_const = is_constant(model, xid);
        bool y_const = is_constant(model, yid);
        if (x_const == y_const) {
            // 両方定数 / 両方変数 → 集約対象外
            continue;
        }

        ReifCandidate cand;
        cand.constraint_idx = ci;
        cand.b_id = bid;
        cand.label = eq_reif->label();
        if (y_const) {
            cand.x_id = xid;
            cand.value = model.variable(yid)->min();
        } else {
            cand.x_id = yid;
            cand.value = model.variable(xid)->min();
        }
        groups[cand.x_id].push_back(cand);
    }

    // 2. min_group_size 以上のグループのみ処理。values 重複チェック。
    size_t aggregated_groups = 0;
    size_t aggregated_reifs = 0;
    std::string first_sample_label;

    for (auto& [xid, cands] : groups) {
        if (cands.size() < min_group_size_) continue;

        // value 重複と b 重複を除く
        std::unordered_set<Domain::value_type> seen_values;
        std::unordered_set<size_t> seen_bs;
        std::vector<ReifCandidate> uniq;
        uniq.reserve(cands.size());
        for (const auto& c : cands) {
            if (seen_values.count(c.value)) continue;
            if (seen_bs.count(c.b_id)) continue;
            // b_id == x_id のような自己参照は安全のため除外
            if (c.b_id == c.x_id) continue;
            seen_values.insert(c.value);
            seen_bs.insert(c.b_id);
            uniq.push_back(c);
        }
        if (uniq.size() < min_group_size_) continue;

        // 値順にソート（決定的にするため）
        std::sort(uniq.begin(), uniq.end(),
                  [](const ReifCandidate& a, const ReifCandidate& b) {
                      return a.value < b.value;
                  });

        // 3. IntOneHotChannelConstraint を構築
        std::vector<Domain::value_type> values;
        std::vector<VariablePtr> bools;
        values.reserve(uniq.size());
        bools.reserve(uniq.size());
        for (const auto& c : uniq) {
            values.push_back(c.value);
            bools.push_back(model.variable(c.b_id));
        }
        auto x_var = model.variable(xid);
        auto new_cst = std::make_shared<IntOneHotChannelConstraint>(
            x_var, std::move(values), std::move(bools));

        // 元の reif 群のラベルをカンマ連結して集約後制約に引き継ぐ
        // 例: "int_eq_reif:L9000,int_eq_reif:L9001,..."。ラベル未設定の元
        // 制約は空文字を貢献し、結果に空セグメントが残る（追跡可能性は最小）。
        // ラベル未設定（空文字）の元制約はスキップして見やすくする。
        std::string joined_label;
        for (size_t k = 0; k < uniq.size(); ++k) {
            if (uniq[k].label.empty()) continue;
            if (!joined_label.empty()) joined_label += ',';
            joined_label += uniq[k].label;
        }
        if (verbose && first_sample_label.empty()) {
            first_sample_label = joined_label;  // verbose 出力用に 1 件保持
        }
        new_cst->set_label(std::move(joined_label));

        // 4. 最初の slot を置換、残りを nullify
        size_t first_idx = uniq.front().constraint_idx;
        model.replace_constraint(first_idx, new_cst);
        for (size_t k = 1; k < uniq.size(); ++k) {
            model.remove_constraint(uniq[k].constraint_idx);
        }

        ++aggregated_groups;
        aggregated_reifs += uniq.size();
    }

    if (aggregated_groups > 0) {
        model.compact_constraints();
        if (verbose) {
            std::cerr << "% [verbose] OneHotChannelAggregator: "
                      << aggregated_reifs << " int_eq_reif -> "
                      << aggregated_groups << " IntOneHotChannel\n";
            if (!first_sample_label.empty()) {
                std::cerr << "% [verbose]   sample label: "
                          << first_sample_label << "\n";
            }
        }
    }

    return true;
}

} // namespace sabori_csp
