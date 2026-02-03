/**
 * @file constraint.cpp
 * @brief 制約基底クラスの実装
 *
 * 2-Watched Literal (2WL) による効率的な伝播監視を提供。
 *
 * 各制約の実装は src/core/constraints/ 以下の個別ファイルに配置:
 * - constraints/comparison.cpp: 比較制約 (int_eq, int_ne, int_lt, int_le)
 */
#include "sabori_csp/constraint.hpp"
#include "sabori_csp/model.hpp"

namespace sabori_csp {

// 静的メンバの初期化
size_t Constraint::next_id_ = 0;

Constraint::Constraint()
    : id_(next_id_++)
    , w1_(-1)
    , w2_(-1) {}

Constraint::Constraint(const std::vector<VariablePtr>& vars)
    : id_(next_id_++)
    , vars_(vars)
    , w1_(-1)
    , w2_(-1) {
    init_watches();
}

void Constraint::set_variables(const std::vector<VariablePtr>& vars) {
    vars_ = vars;
    init_watches();
}

void Constraint::init_watches() {
    w1_ = -1;
    w2_ = -1;

    if (vars_.empty()) {
        return;
    }

    if (vars_.size() == 1) {
        w1_ = 0;
        w2_ = 0;
        return;
    }

    // 未確定の2変数を探す
    for (size_t i = 0; i < vars_.size(); ++i) {
        if (!vars_[i]->is_assigned()) {
            if (w1_ < 0) {
                w1_ = static_cast<int>(i);
                w2_ = static_cast<int>((i + 1) % vars_.size());
            } else {
                w2_ = static_cast<int>(i);
                break;
            }
        }
    }

    // 全て確定している場合
    if (w1_ < 0) {
        w1_ = 0;
        w2_ = (vars_.size() > 1) ? 1 : 0;
    }
}

bool Constraint::can_be_finalized() const {
    if (w1_ < 0 || w2_ < 0) {
        return true;  // 変数がない場合
    }
    return vars_[w1_]->is_assigned() && vars_[w2_]->is_assigned();
}

bool Constraint::on_instantiate(Model& /*model*/, int /*save_point*/,
                                 size_t var_idx, Domain::value_type /*value*/,
                                 Domain::value_type /*prev_min*/,
                                 Domain::value_type /*prev_max*/) {
    // 確定した変数が監視変数でなければ何もしない
    if (w1_ < 0 || vars_.empty()) {
        return true;
    }

    // var_idx に対応する変数を特定
    VariablePtr var = nullptr;
    for (size_t i = 0; i < vars_.size(); ++i) {
        // Model の変数インデックスと制約の変数リストを照合
        // 注: ここでは変数ポインタで比較するのではなく、
        // is_assigned() の状態で判断する
    }

    // w1 が確定した場合
    if (static_cast<size_t>(w1_) == var_idx ||
        (var_idx < vars_.size() && vars_[w1_]->is_assigned())) {
        // 別の未確定変数を探して監視を移す
        for (size_t idx = 0; idx < vars_.size(); ++idx) {
            if (static_cast<int>(idx) == w1_ || static_cast<int>(idx) == w2_) {
                continue;
            }
            if (!vars_[idx]->is_assigned()) {
                w1_ = static_cast<int>(idx);
                return true;
            }
        }

        // 移せない場合、w2 がまだ未確定なら OK
        if (!vars_[w2_]->is_assigned()) {
            return true;
        }
    }
    // w2 が確定した場合
    else if (static_cast<size_t>(w2_) == var_idx ||
             (var_idx < vars_.size() && vars_[w2_]->is_assigned())) {
        // 別の未確定変数を探して監視を移す
        for (size_t idx = 0; idx < vars_.size(); ++idx) {
            if (static_cast<int>(idx) == w1_ || static_cast<int>(idx) == w2_) {
                continue;
            }
            if (!vars_[idx]->is_assigned()) {
                w2_ = static_cast<int>(idx);
                return true;
            }
        }

        // 移せない場合、w1 がまだ未確定なら OK
        if (!vars_[w1_]->is_assigned()) {
            return true;
        }
    } else {
        // 監視対象外の変数が確定した場合は何もしない
        return true;
    }

    // 両方の監視変数が確定した → 最終チェック
    return on_final_instantiate();
}

bool Constraint::on_final_instantiate() {
    // デフォルトでは is_satisfied() を使用
    auto result = is_satisfied();
    return result.value_or(true);
}

} // namespace sabori_csp
