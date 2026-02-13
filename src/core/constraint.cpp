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
    , w2_(-1)
    , is_initially_inconsistent_(false) {}

Constraint::Constraint(const std::vector<VariablePtr>& vars)
    : id_(next_id_++)
    , vars_(vars)
    , w1_(-1)
    , w2_(-1)
    , is_initially_inconsistent_(false) {
    update_var_ids();
    init_watches();
}

void Constraint::set_variables(const std::vector<VariablePtr>& vars) {
    vars_ = vars;
    update_var_ids();
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

bool Constraint::on_instantiate(Model& model, int save_point,
                                 size_t var_idx, size_t internal_var_idx,
                                 Domain::value_type /*value*/,
                                 Domain::value_type /*prev_min*/,
                                 Domain::value_type /*prev_max*/) {
    // 確定した変数が監視変数でなければ何もしない
    if (w1_ < 0 || vars_.empty()) {
        return true;
    }

    // w1 が確定した場合
    // internal_var_idx は制約内のインデックス、w1_/w2_ も制約内のインデックス
    if (static_cast<int>(internal_var_idx) == w1_ || model.is_instantiated(var_ids_[w1_])) {
        // 別の未確定変数を探して監視を移す
        for (size_t idx = 0; idx < var_ids_.size(); ++idx) {
            if (static_cast<int>(idx) == w1_ || static_cast<int>(idx) == w2_) {
                continue;
            }
            if (!model.is_instantiated(var_ids_[idx])) {
                w1_ = static_cast<int>(idx);
                return true;
            }
        }
    }
    // w2 が確定した場合
    else if (static_cast<int>(internal_var_idx) == w2_ || model.is_instantiated(var_ids_[w2_])) {
        // 別の未確定変数を探して監視を移す
        for (size_t idx = 0; idx < var_ids_.size(); ++idx) {
            if (static_cast<int>(idx) == w1_ || static_cast<int>(idx) == w2_) {
                continue;
            }
            if (!model.is_instantiated(var_ids_[idx])) {
                w2_ = static_cast<int>(idx);
                return true;
            }
        }
    } else {
        // 監視対象外の変数が確定した場合は何もしない
        return true;
    }

    return true;
}

bool Constraint::on_final_instantiate() {
    // デフォルトでは is_satisfied() を使用
    auto result = is_satisfied();
    return result.value_or(true);
}

bool Constraint::prepare_propagation(Model& /*model*/) {
    // デフォルトでは何もしない
    // サブクラスでオーバーライドして内部状態を初期化する
    return true;
}

bool Constraint::on_last_uninstantiated(Model& /*model*/, int /*save_point*/,
                                         size_t /*last_var_internal_idx*/) {
    // デフォルトでは何もしない
    // サブクラスでオーバーライドして、残りの変数のドメインを絞り込む
    return true;
}

bool Constraint::on_set_min(Model& /*model*/, int /*save_point*/,
                            size_t /*var_idx*/, size_t /*internal_var_idx*/,
                            Domain::value_type /*new_min*/,
                            Domain::value_type /*old_min*/) {
    // デフォルトでは何もしない
    return true;
}

bool Constraint::on_set_max(Model& /*model*/, int /*save_point*/,
                            size_t /*var_idx*/, size_t /*internal_var_idx*/,
                            Domain::value_type /*new_max*/,
                            Domain::value_type /*old_max*/) {
    // デフォルトでは何もしない
    return true;
}

bool Constraint::on_remove_value(Model& /*model*/, int /*save_point*/,
                                  size_t /*var_idx*/, size_t /*internal_var_idx*/,
                                  Domain::value_type /*removed_value*/) {
    // デフォルトでは何もしない
    return true;
}

void Constraint::check_initial_consistency() {
    // 全変数が確定している場合は is_satisfied() で判定
    auto result = is_satisfied();
    if (result.has_value() && !result.value()) {
        set_initially_inconsistent(true);
    }
}

bool Constraint::has_uninstantiated() const {
    if (w1_ < 0) {
        return false;
    }

    if (!vars_[w1_]->is_assigned()) {
        return true;
    }

    if (!vars_[w2_]->is_assigned()) {
        return true;
    }

    return false;
}

size_t Constraint::find_last_uninstantiated() const {
    if (w1_ < 0) {
        return SIZE_MAX;
    }

    if (vars_[w1_]->is_assigned()) {
        if (!vars_[w2_]->is_assigned()) {
	    return w2_;    
	}
    }
    else if (vars_[w2_]->is_assigned()) {
        return w1_;
    }

    return SIZE_MAX;
}

} // namespace sabori_csp
