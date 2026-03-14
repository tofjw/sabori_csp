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

Constraint::Constraint(std::vector<size_t> var_ids)
    : id_(next_id_++)
    , var_ids_(std::move(var_ids))
    , w1_(-1)
    , w2_(-1)
    , is_initially_inconsistent_(false) {
    init_watches();
}

void Constraint::set_var_ids(std::vector<size_t> var_ids) {
    var_ids_ = std::move(var_ids);
    init_watches();
}

void Constraint::init_watches() {
    w1_ = -1;
    w2_ = -1;

    size_t n = var_ids_.size();
    if (n == 0) {
        return;
    }

    if (n == 1) {
        w1_ = 0;
        w2_ = 0;
        return;
    }

    // デフォルト: 先頭2変数を監視（refine_watches で修正される）
    w1_ = 0;
    w2_ = 1;
}

void Constraint::refine_watches(const Model& model) {
    size_t n = var_ids_.size();
    if (n <= 1) return;

    w1_ = -1;
    w2_ = -1;

    for (size_t i = 0; i < n; ++i) {
        if (!model.is_instantiated(var_ids_[i])) {
            if (w1_ < 0) {
                w1_ = static_cast<int>(i);
                w2_ = static_cast<int>((i + 1) % n);
            } else {
                w2_ = static_cast<int>(i);
                break;
            }
        }
    }

    if (w1_ < 0) {
        w1_ = 0;
        w2_ = 1;
    }
}

bool Constraint::on_instantiate(Model& model, int save_point,
                                 size_t var_idx, size_t internal_var_idx,
                                 Domain::value_type /*value*/,
                                 Domain::value_type /*prev_min*/,
                                 Domain::value_type /*prev_max*/) {
    // 確定した変数が監視変数でなければ何もしない
    if (w1_ < 0 || var_ids_.empty()) {
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

std::optional<bool> Constraint::is_satisfied(const Model& model) const {
    // 全変数が確定しているかチェック
    for (auto vid : var_ids_) {
        if (!model.is_instantiated(vid)) {
            return std::nullopt;
        }
    }
    // 全確定 → on_final_instantiate で判定（論理的に const）
    return const_cast<Constraint*>(this)->on_final_instantiate(model);
}

bool Constraint::on_final_instantiate(const Model& /*model*/) {
    // サブクラスでオーバーライドすること
    return true;
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

void Constraint::compute_search_var_count(const Model& model) {
    search_var_count_ = 0;
    for (size_t vid : var_ids_) {
        if (!model.is_instantiated(vid)) {
            ++search_var_count_;
        }
    }
}

void Constraint::bump_activity(const Model& model, size_t /*trigger_var_idx*/,
                               double* activity, double activity_inc,
                               bool& need_rescale) const {
    // 探索開始時に未確定だった変数数で割る（最初から固定の変数を除外）
    size_t n = search_var_count_ > 0 ? search_var_count_ : var_ids_.size();
    double inc = activity_inc / n;
    for (size_t vid : var_ids_) {
        if (model.is_instantiated(vid)) {
            bump_variable_activity(activity, vid, inc, need_rescale);
        }
    }
}

void Constraint::check_initial_consistency() {
    // デフォルト: 何もしない（presolve / prepare_propagation で検出）
}

bool Constraint::has_uninstantiated(const Model& model) const {
    if (w1_ < 0) {
        return false;
    }

    if (!model.is_instantiated(var_ids_[w1_])) {
        return true;
    }

    if (!model.is_instantiated(var_ids_[w2_])) {
        return true;
    }

    return false;
}

size_t Constraint::find_last_uninstantiated(const Model& model) const {
    if (w1_ < 0) {
        return SIZE_MAX;
    }

    if (model.is_instantiated(var_ids_[w1_])) {
        if (!model.is_instantiated(var_ids_[w2_])) {
            return w2_;
        }
    }
    else if (model.is_instantiated(var_ids_[w2_])) {
        return w1_;
    }

    return SIZE_MAX;
}

} // namespace sabori_csp
