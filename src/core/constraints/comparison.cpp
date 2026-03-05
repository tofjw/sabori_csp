#include "sabori_csp/constraints/comparison.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>

namespace sabori_csp {

// ============================================================================
// IntEqConstraint implementation
// ============================================================================

IntEqConstraint::IntEqConstraint(VariablePtr x, VariablePtr y)
    : Constraint(extract_var_ids({x, y}))
    , x_id_(x->id())
    , y_id_(y->id()) {
}

std::string IntEqConstraint::name() const {
    return "int_eq";
}

PresolveResult IntEqConstraint::presolve(Model& model) {
    bool changed = false;
    const auto& vars = model.variables();
    auto& x_var = *vars[x_id_];
    auto& y_var = *vars[y_id_];
    auto& x_dom = x_var.domain();
    auto& y_dom = y_var.domain();

    if (x_dom.is_bounds_only() || y_dom.is_bounds_only()) {
        // bounds intersection
        auto new_min = std::max(x_var.min(), y_var.min());
        auto new_max = std::min(x_var.max(), y_var.max());
        if (new_min > new_max) return PresolveResult::Contradiction;
        if (new_min > x_var.min()) {
            x_dom.remove_below(new_min);
            if (x_dom.empty()) return PresolveResult::Contradiction;
            changed = true;
        }
        if (new_max < x_var.max()) {
            x_dom.remove_above(new_max);
            if (x_dom.empty()) return PresolveResult::Contradiction;
            changed = true;
        }
        if (new_min > y_var.min()) {
            y_dom.remove_below(new_min);
            if (y_dom.empty()) return PresolveResult::Contradiction;
            changed = true;
        }
        if (new_max < y_var.max()) {
            y_dom.remove_above(new_max);
            if (y_dom.empty()) return PresolveResult::Contradiction;
            changed = true;
        }
    } else {
        // sparse set: use contains() to avoid set construction
        std::vector<Domain::value_type> buf;
        x_dom.copy_values_to(buf);
        for (auto v : buf) {
            if (!y_dom.contains(v)) {
                if (!x_var.remove(v)) return PresolveResult::Contradiction;
                changed = true;
            }
        }
        y_dom.copy_values_to(buf);
        for (auto v : buf) {
            if (!x_dom.contains(v)) {
                if (!y_var.remove(v)) return PresolveResult::Contradiction;
                changed = true;
            }
        }
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool IntEqConstraint::on_instantiate(Model& model, int save_point,
                                      size_t var_idx, size_t internal_var_idx, Domain::value_type value,
                                      Domain::value_type prev_min,
                                      Domain::value_type prev_max) {
    // 基底クラスの 2WL 処理
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // x == y なので、一方が確定したら他方も同じ値に固定（キューイング）
    if (model.is_instantiated(x_id_) && !model.is_instantiated(y_id_)) {
        auto val = model.value(x_id_);
        if (!model.contains(y_id_, val)) {
            return false;
        }
        model.enqueue_instantiate(y_id_, val);
    }
    if (model.is_instantiated(y_id_) && !model.is_instantiated(x_id_)) {
        auto val = model.value(y_id_);
        if (!model.contains(x_id_, val)) {
            return false;
        }
        model.enqueue_instantiate(x_id_, val);
    }

    return true;
}

bool IntEqConstraint::on_set_min(Model& model, int /*save_point*/,
                                  size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type new_min,
                                  Domain::value_type /*old_min*/) {
    // x == y: bounds を相互伝播
    if (var_idx == x_id_) {
        model.enqueue_set_min(y_id_, new_min);
    } else if (var_idx == y_id_) {
        model.enqueue_set_min(x_id_, new_min);
    }
    return true;
}

bool IntEqConstraint::on_set_max(Model& model, int /*save_point*/,
                                  size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type new_max,
                                  Domain::value_type /*old_max*/) {
    // x == y: bounds を相互伝播
    if (var_idx == x_id_) {
        model.enqueue_set_max(y_id_, new_max);
    } else if (var_idx == y_id_) {
        model.enqueue_set_max(x_id_, new_max);
    }
    return true;
}

bool IntEqConstraint::on_final_instantiate(const Model& model) {
    return model.value(x_id_) == model.value(y_id_);
}


// ============================================================================
// IntEqReifConstraint implementation
// ============================================================================

IntEqReifConstraint::IntEqReifConstraint(VariablePtr x, VariablePtr y, VariablePtr b)
    : Constraint(extract_var_ids({x, y, b}))
    , x_id_(x->id())
    , y_id_(y->id())
    , b_id_(b->id()) {
    // 注意: 内部状態は presolve() で初期化
}

std::string IntEqReifConstraint::name() const {
    return "int_eq_reif";
}

bool IntEqReifConstraint::prepare_propagation(Model& model) {
    // 2WL を初期化
    init_watches();

    // 初期整合性チェック
    // (x == y) <-> b
    // b=1 が強制で x,y に共通値がない、または b=0 が強制で x,y が同じシングルトン
    if (model.is_instantiated(b_id_)) {
        if (model.value(b_id_) == 1) {
            // x == y を満たす共通値が必要
            if (!find_new_support(model)) {
                return false;
            }
        } else {
            // x != y が必要: 両方シングルトンで同じ値なら矛盾
            if (model.is_instantiated(x_id_) && model.is_instantiated(y_id_) &&
                model.value(x_id_) == model.value(y_id_)) {
                return false;
            }
        }
    } else {
        // b 未確定: support を探しておく（あれば後続イベントでスキップできる）
        find_new_support(model);
    }

    return true;
}

bool IntEqReifConstraint::find_new_support(const Model& model) {
    // 小さい方のドメインを走査して、もう片方に contains() で存在確認
    const auto& x_dom = model.variable(x_id_)->domain();
    const auto& y_dom = model.variable(y_id_)->domain();
    bool found = false;
    if (x_dom.size() <= y_dom.size()) {
        x_dom.for_each_value([&](Domain::value_type v) {
            if (!found && y_dom.contains(v)) {
                support_value_ = v;
                found = true;
            }
        });
    } else {
        y_dom.for_each_value([&](Domain::value_type v) {
            if (!found && x_dom.contains(v)) {
                support_value_ = v;
                found = true;
            }
        });
    }
    return found;
}

PresolveResult IntEqReifConstraint::presolve(Model& model) {
    bool changed = false;
    // If b is fixed to 1, enforce x == y
    if (model.variable(b_id_)->is_assigned() && model.variable(b_id_)->assigned_value().value() == 1) {
        auto& x_dom = model.variable(x_id_)->domain();
        auto& y_dom = model.variable(y_id_)->domain();

        if (x_dom.is_bounds_only() || y_dom.is_bounds_only()) {
            auto new_min = std::max(model.variable(x_id_)->min(), model.variable(y_id_)->min());
            auto new_max = std::min(model.variable(x_id_)->max(), model.variable(y_id_)->max());
            if (new_min > new_max) return PresolveResult::Contradiction;
            if (new_min > model.variable(x_id_)->min()) {
                x_dom.remove_below(new_min);
                if (x_dom.empty()) return PresolveResult::Contradiction;
                changed = true;
            }
            if (new_max < model.variable(x_id_)->max()) {
                x_dom.remove_above(new_max);
                if (x_dom.empty()) return PresolveResult::Contradiction;
                changed = true;
            }
            if (new_min > model.variable(y_id_)->min()) {
                y_dom.remove_below(new_min);
                if (y_dom.empty()) return PresolveResult::Contradiction;
                changed = true;
            }
            if (new_max < model.variable(y_id_)->max()) {
                y_dom.remove_above(new_max);
                if (y_dom.empty()) return PresolveResult::Contradiction;
                changed = true;
            }
        } else {
            std::vector<Domain::value_type> buf;
            x_dom.copy_values_to(buf);
            for (auto v : buf) {
                if (!y_dom.contains(v)) {
                    if (!model.variable(x_id_)->remove(v)) return PresolveResult::Contradiction;
                    changed = true;
                }
            }
            y_dom.copy_values_to(buf);
            for (auto v : buf) {
                if (!x_dom.contains(v)) {
                    if (!model.variable(y_id_)->remove(v)) return PresolveResult::Contradiction;
                    changed = true;
                }
            }
        }
    }

    // If b is fixed to 0 and one variable is singleton, remove that value from the other
    if (model.variable(b_id_)->is_assigned() && model.variable(b_id_)->assigned_value().value() == 0) {
        if (model.variable(x_id_)->is_assigned()) {
            auto val = model.variable(x_id_)->assigned_value().value();
            if (model.variable(y_id_)->domain().contains(val)) {
                if (!model.variable(y_id_)->remove(val)) return PresolveResult::Contradiction;
                changed = true;
            }
        }
        if (model.variable(y_id_)->is_assigned()) {
            auto val = model.variable(y_id_)->assigned_value().value();
            if (model.variable(x_id_)->domain().contains(val)) {
                if (!model.variable(x_id_)->remove(val)) return PresolveResult::Contradiction;
                changed = true;
            }
        }
    }

    // If x and y are both singletons, fix b
    if (model.variable(x_id_)->is_assigned() && model.variable(y_id_)->is_assigned() && !model.variable(b_id_)->is_assigned()) {
        bool eq = (model.variable(x_id_)->assigned_value() == model.variable(y_id_)->assigned_value());
        if (!model.variable(b_id_)->domain().contains(eq ? 1 : 0)) {
            return PresolveResult::Contradiction;
        }
        model.variable(b_id_)->assign(eq ? 1 : 0);
        changed = true;
    }

    // If y is a singleton and x's domain doesn't contain y's value, then b = 0
    if (!model.variable(b_id_)->is_assigned() && model.variable(y_id_)->is_assigned()) {
        auto y_val = model.variable(y_id_)->assigned_value().value();
        if (!model.variable(x_id_)->domain().contains(y_val)) {
            if (!model.variable(b_id_)->domain().contains(0)) {
                return PresolveResult::Contradiction;
            }
            model.variable(b_id_)->assign(0);
            changed = true;
        }
    }

    // If x is a singleton and y's domain doesn't contain x's value, then b = 0
    if (!model.variable(b_id_)->is_assigned() && model.variable(x_id_)->is_assigned()) {
        auto x_val = model.variable(x_id_)->assigned_value().value();
        if (!model.variable(y_id_)->domain().contains(x_val)) {
            if (!model.variable(b_id_)->domain().contains(0)) {
                return PresolveResult::Contradiction;
            }
            model.variable(b_id_)->assign(0);
            changed = true;
        }
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool IntEqReifConstraint::on_instantiate(Model& model, int save_point,
                                          size_t var_idx, size_t internal_var_idx, Domain::value_type value,
                                          Domain::value_type prev_min,
                                          Domain::value_type prev_max) {
    // 基底クラスの 2WL 処理
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // 伝播ロジック（キューイング）
    if (model.is_instantiated(b_id_)) {
        if (model.value(b_id_) == 1) {
            // x == y を強制
            if (model.is_instantiated(x_id_) && !model.is_instantiated(y_id_)) {
                auto val = model.value(x_id_);
                if (!model.contains(y_id_, val)) {
                    return false;
                }
                model.enqueue_instantiate(y_id_, val);
            }
            if (model.is_instantiated(y_id_) && !model.is_instantiated(x_id_)) {
                auto val = model.value(y_id_);
                if (!model.contains(x_id_, val)) {
                    return false;
                }
                model.enqueue_instantiate(x_id_, val);
            }
        } else {
            // x != y を強制
            if (model.is_instantiated(x_id_)) {
                model.enqueue_remove_value(y_id_, model.value(x_id_));
            }
            if (model.is_instantiated(y_id_)) {
                model.enqueue_remove_value(x_id_, model.value(y_id_));
            }
        }
    }

    // x と y が両方確定したら b を決定
    if (model.is_instantiated(x_id_) && model.is_instantiated(y_id_) && !model.is_instantiated(b_id_)) {
        bool eq = (model.value(x_id_) == model.value(y_id_));
        model.enqueue_instantiate(b_id_, eq ? 1 : 0);
    }

    return true;
}

bool IntEqReifConstraint::on_set_min(Model& model, int /*save_point*/,
                                      size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type new_min,
                                      Domain::value_type /*old_min*/) {
    // (x == y) <-> b
    if (!model.is_instantiated(b_id_)) {
        // support がまだ両方のドメインに存在するなら共通値あり → スキップ
        if (model.contains(x_id_, support_value_) && model.contains(y_id_, support_value_)) {
            return true;
        }
        // support 無効 → 再探索
        if (!find_new_support(model)) {
            model.enqueue_instantiate(b_id_, 0);
        }
    } else if (model.value(b_id_) == 1) {
        // x == y: bounds を相互伝播
        if (var_idx == x_id_) {
            model.enqueue_set_min(y_id_, new_min);
        } else if (var_idx == y_id_) {
            model.enqueue_set_min(x_id_, new_min);
        }
    }
    // b = 0: bounds だけでは伝播不可
    return true;
}

bool IntEqReifConstraint::on_set_max(Model& model, int /*save_point*/,
                                      size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type new_max,
                                      Domain::value_type /*old_max*/) {
    // (x == y) <-> b
    if (!model.is_instantiated(b_id_)) {
        // support がまだ両方のドメインに存在するなら共通値あり → スキップ
        if (model.contains(x_id_, support_value_) && model.contains(y_id_, support_value_)) {
            return true;
        }
        // support 無効 → 再探索
        if (!find_new_support(model)) {
            model.enqueue_instantiate(b_id_, 0);
        }
    } else if (model.value(b_id_) == 1) {
        // x == y: bounds を相互伝播
        if (var_idx == x_id_) {
            model.enqueue_set_max(y_id_, new_max);
        } else if (var_idx == y_id_) {
            model.enqueue_set_max(x_id_, new_max);
        }
    }
    // b = 0: bounds だけでは伝播不可
    return true;
}

bool IntEqReifConstraint::on_remove_value(Model& model, int /*save_point*/,
                                           size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type removed_value) {
    (void)var_idx;

    if (!model.is_instantiated(b_id_)) {
        // support が削除されていない & 両ドメインに存在 → 共通値あり
        if (removed_value != support_value_ &&
            model.contains(x_id_, support_value_) && model.contains(y_id_, support_value_)) {
            return true;
        }
        // support 無効 → 再探索
        if (!find_new_support(model)) {
            model.enqueue_instantiate(b_id_, 0);
        }
    }

    return true;
}

bool IntEqReifConstraint::on_final_instantiate(const Model& model) {
    bool eq = (model.value(x_id_) == model.value(y_id_));
    return eq == (model.value(b_id_) == 1);
}

// ============================================================================
// IntEqImpConstraint implementation
// ============================================================================

IntEqImpConstraint::IntEqImpConstraint(VariablePtr x, VariablePtr y, VariablePtr b)
    : Constraint(extract_var_ids({x, y, b}))
    , x_id_(x->id())
    , y_id_(y->id())
    , b_id_(b->id()) {
}

std::string IntEqImpConstraint::name() const {
    return "int_eq_imp";
}

bool IntEqImpConstraint::prepare_propagation(Model& model) {
    init_watches();

    // b=1 が強制で x,y に共通値がない → 矛盾
    if (model.is_instantiated(b_id_) && model.value(b_id_) == 1) {
        bool has_common = false;
        model.variable(x_id_)->domain().for_each_value([&](Domain::value_type v) {
            if (!has_common && model.variable(y_id_)->domain().contains(v)) {
                has_common = true;
            }
        });
        if (!has_common) {
            return false;
        }
    }

    return true;
}

PresolveResult IntEqImpConstraint::presolve(Model& model) {
    bool changed = false;

    // b=1 のとき x==y を強制（ドメインの交差）
    if (model.variable(b_id_)->is_assigned() && model.variable(b_id_)->assigned_value().value() == 1) {
        auto& x_dom = model.variable(x_id_)->domain();
        auto& y_dom = model.variable(y_id_)->domain();

        if (x_dom.is_bounds_only() || y_dom.is_bounds_only()) {
            auto new_min = std::max(model.variable(x_id_)->min(), model.variable(y_id_)->min());
            auto new_max = std::min(model.variable(x_id_)->max(), model.variable(y_id_)->max());
            if (new_min > new_max) return PresolveResult::Contradiction;
            if (new_min > model.variable(x_id_)->min()) {
                x_dom.remove_below(new_min);
                if (x_dom.empty()) return PresolveResult::Contradiction;
                changed = true;
            }
            if (new_max < model.variable(x_id_)->max()) {
                x_dom.remove_above(new_max);
                if (x_dom.empty()) return PresolveResult::Contradiction;
                changed = true;
            }
            if (new_min > model.variable(y_id_)->min()) {
                y_dom.remove_below(new_min);
                if (y_dom.empty()) return PresolveResult::Contradiction;
                changed = true;
            }
            if (new_max < model.variable(y_id_)->max()) {
                y_dom.remove_above(new_max);
                if (y_dom.empty()) return PresolveResult::Contradiction;
                changed = true;
            }
        } else {
            std::vector<Domain::value_type> buf;
            x_dom.copy_values_to(buf);
            for (auto v : buf) {
                if (!y_dom.contains(v)) {
                    if (!model.variable(x_id_)->remove(v)) return PresolveResult::Contradiction;
                    changed = true;
                }
            }
            y_dom.copy_values_to(buf);
            for (auto v : buf) {
                if (!x_dom.contains(v)) {
                    if (!model.variable(y_id_)->remove(v)) return PresolveResult::Contradiction;
                    changed = true;
                }
            }
        }
    }
    // b=0: 何もしない（vacuously true）

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool IntEqImpConstraint::on_instantiate(Model& model, int save_point,
                                         size_t var_idx, size_t internal_var_idx, Domain::value_type value,
                                         Domain::value_type prev_min,
                                         Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    if (model.is_instantiated(b_id_)) {
        if (model.value(b_id_) == 1) {
            // b=1: x==y を強制
            if (model.is_instantiated(x_id_) && model.is_instantiated(y_id_)) {
                if (model.value(x_id_) != model.value(y_id_)) {
                    return false;
                }
            } else if (model.is_instantiated(x_id_)) {
                auto val = model.value(x_id_);
                if (!model.contains(y_id_, val)) return false;
                model.enqueue_instantiate(y_id_, val);
            } else if (model.is_instantiated(y_id_)) {
                auto val = model.value(y_id_);
                if (!model.contains(x_id_, val)) return false;
                model.enqueue_instantiate(x_id_, val);
            } else if (var_idx == b_id_) {
                // b が今 1 になった: x,y の bounds を同期
                auto x_min = model.var_min(x_id_);
                auto x_max = model.var_max(x_id_);
                auto y_min = model.var_min(y_id_);
                auto y_max = model.var_max(y_id_);
                auto new_min = std::max(x_min, y_min);
                auto new_max = std::min(x_max, y_max);
                if (new_min > new_max) return false;
                if (new_min > x_min) model.enqueue_set_min(x_id_, new_min);
                if (new_max < x_max) model.enqueue_set_max(x_id_, new_max);
                if (new_min > y_min) model.enqueue_set_min(y_id_, new_min);
                if (new_max < y_max) model.enqueue_set_max(y_id_, new_max);
            }
        }
        // b=0: 何もしない
    } else {
        // 対偶: (x≠y) → b=0
        if (model.is_instantiated(x_id_) && model.is_instantiated(y_id_)) {
            if (model.value(x_id_) != model.value(y_id_)) {
                model.enqueue_instantiate(b_id_, 0);
            }
            // x==y でも b=1 は強制しない（half-reification）
        } else if (model.is_instantiated(x_id_)) {
            if (!model.contains(y_id_, model.value(x_id_))) {
                model.enqueue_instantiate(b_id_, 0);
            }
        } else if (model.is_instantiated(y_id_)) {
            if (!model.contains(x_id_, model.value(y_id_))) {
                model.enqueue_instantiate(b_id_, 0);
            }
        }
    }

    return true;
}

bool IntEqImpConstraint::on_set_min(Model& model, int /*save_point*/,
                                     size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type new_min,
                                     Domain::value_type /*old_min*/) {
    if (!model.is_instantiated(b_id_)) {
        // 対偶: bounds が重ならない → b=0
        if (model.var_min(x_id_) > model.var_max(y_id_) ||
            model.var_max(x_id_) < model.var_min(y_id_)) {
            model.enqueue_instantiate(b_id_, 0);
        }
    } else if (model.value(b_id_) == 1) {
        if (var_idx == x_id_) {
            model.enqueue_set_min(y_id_, new_min);
        } else if (var_idx == y_id_) {
            model.enqueue_set_min(x_id_, new_min);
        }
    }
    return true;
}

bool IntEqImpConstraint::on_set_max(Model& model, int /*save_point*/,
                                     size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type new_max,
                                     Domain::value_type /*old_max*/) {
    if (!model.is_instantiated(b_id_)) {
        // 対偶: bounds が重ならない → b=0
        if (model.var_min(x_id_) > model.var_max(y_id_) ||
            model.var_max(x_id_) < model.var_min(y_id_)) {
            model.enqueue_instantiate(b_id_, 0);
        }
    } else if (model.value(b_id_) == 1) {
        if (var_idx == x_id_) {
            model.enqueue_set_max(y_id_, new_max);
        } else if (var_idx == y_id_) {
            model.enqueue_set_max(x_id_, new_max);
        }
    }
    return true;
}

bool IntEqImpConstraint::on_remove_value(Model& model, int /*save_point*/,
                                          size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type removed_value) {
    (void)removed_value;

    // 対偶: x が確定して y に x の値がない → b=0 (逆も)
    if (!model.is_instantiated(b_id_)) {
        if (model.is_instantiated(y_id_) && var_idx == x_id_) {
            if (!model.contains(x_id_, model.value(y_id_))) {
                model.enqueue_instantiate(b_id_, 0);
            }
        }
        if (model.is_instantiated(x_id_) && var_idx == y_id_) {
            if (!model.contains(y_id_, model.value(x_id_))) {
                model.enqueue_instantiate(b_id_, 0);
            }
        }
    }

    return true;
}

bool IntEqImpConstraint::on_final_instantiate(const Model& model) {
    // b -> (x == y): b=0 なら常に真、b=1 なら x==y が必要
    return model.value(b_id_) == 0 || (model.value(x_id_) == model.value(y_id_));
}

// ============================================================================
// IntNeConstraint implementation
// ============================================================================

IntNeConstraint::IntNeConstraint(VariablePtr x, VariablePtr y)
    : Constraint(extract_var_ids({x, y}))
    , x_id_(x->id())
    , y_id_(y->id()) {
}

std::string IntNeConstraint::name() const {
    return "int_ne";
}

PresolveResult IntNeConstraint::presolve(Model& model) {
    bool changed = false;
    // If one is singleton, remove that value from the other
    if (model.variable(x_id_)->is_assigned()) {
        auto val = model.variable(x_id_)->assigned_value().value();
        if (model.variable(y_id_)->domain().contains(val)) {
            if (!model.variable(y_id_)->remove(val)) return PresolveResult::Contradiction;
            changed = true;
        }
    }
    if (model.variable(y_id_)->is_assigned()) {
        auto val = model.variable(y_id_)->assigned_value().value();
        if (model.variable(x_id_)->domain().contains(val)) {
            if (!model.variable(x_id_)->remove(val)) return PresolveResult::Contradiction;
            changed = true;
        }
    }
    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool IntNeConstraint::on_instantiate(Model& model, int save_point,
                                      size_t var_idx, size_t internal_var_idx, Domain::value_type value,
                                      Domain::value_type prev_min,
                                      Domain::value_type prev_max) {
    // 基底クラスの 2WL 処理
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // x != y なので、一方が確定したら他方からその値を削除（キューイング）
    if (model.is_instantiated(x_id_)) {
        model.enqueue_remove_value(y_id_, model.value(x_id_));
    }
    if (model.is_instantiated(y_id_)) {
        model.enqueue_remove_value(x_id_, model.value(y_id_));
    }

    return true;
}

bool IntNeConstraint::on_final_instantiate(const Model& model) {
    return model.value(x_id_) != model.value(y_id_);
}

// ============================================================================
// IntNeReifConstraint implementation
// ============================================================================

IntNeReifConstraint::IntNeReifConstraint(VariablePtr x, VariablePtr y, VariablePtr b)
    : Constraint(extract_var_ids({x, y, b}))
    , x_id_(x->id())
    , y_id_(y->id())
    , b_id_(b->id()) {
    // 注意: 内部状態は presolve() で初期化
}

std::string IntNeReifConstraint::name() const {
    return "int_ne_reif";
}

bool IntNeReifConstraint::prepare_propagation(Model& model) {
    // 2WL を初期化
    init_watches();

    // 初期整合性チェック
    // (x != y) <-> b
    if (model.is_instantiated(b_id_)) {
        if (model.value(b_id_) == 1) {
            // x != y が必要: 両方シングルトンで同じ値なら矛盾
            if (model.is_instantiated(x_id_) && model.is_instantiated(y_id_) &&
                model.value(x_id_) == model.value(y_id_)) {
                return false;
            }
        } else {
            // x == y を満たす共通値が必要
            bool has_common = false;
            model.variable(x_id_)->domain().for_each_value([&](Domain::value_type v) {
                if (!has_common && model.variable(y_id_)->domain().contains(v)) {
                    has_common = true;
                }
            });
            if (!has_common) {
                return false;
            }
        }
    }

    return true;
}

PresolveResult IntNeReifConstraint::presolve(Model& model) {
    bool changed = false;
    // If b is fixed to 1, enforce x != y
    if (model.variable(b_id_)->is_assigned() && model.variable(b_id_)->assigned_value().value() == 1) {
        if (model.variable(x_id_)->is_assigned()) {
            auto val = model.variable(x_id_)->assigned_value().value();
            if (model.variable(y_id_)->domain().contains(val)) {
                if (!model.variable(y_id_)->remove(val)) return PresolveResult::Contradiction;
                changed = true;
            }
        }
        if (model.variable(y_id_)->is_assigned()) {
            auto val = model.variable(y_id_)->assigned_value().value();
            if (model.variable(x_id_)->domain().contains(val)) {
                if (!model.variable(x_id_)->remove(val)) return PresolveResult::Contradiction;
                changed = true;
            }
        }
    }

    // If b is fixed to 0, enforce x == y
    if (model.variable(b_id_)->is_assigned() && model.variable(b_id_)->assigned_value().value() == 0) {
        auto& x_dom = model.variable(x_id_)->domain();
        auto& y_dom = model.variable(y_id_)->domain();

        if (x_dom.is_bounds_only() || y_dom.is_bounds_only()) {
            auto new_min = std::max(model.variable(x_id_)->min(), model.variable(y_id_)->min());
            auto new_max = std::min(model.variable(x_id_)->max(), model.variable(y_id_)->max());
            if (new_min > new_max) return PresolveResult::Contradiction;
            if (new_min > model.variable(x_id_)->min()) {
                x_dom.remove_below(new_min);
                if (x_dom.empty()) return PresolveResult::Contradiction;
                changed = true;
            }
            if (new_max < model.variable(x_id_)->max()) {
                x_dom.remove_above(new_max);
                if (x_dom.empty()) return PresolveResult::Contradiction;
                changed = true;
            }
            if (new_min > model.variable(y_id_)->min()) {
                y_dom.remove_below(new_min);
                if (y_dom.empty()) return PresolveResult::Contradiction;
                changed = true;
            }
            if (new_max < model.variable(y_id_)->max()) {
                y_dom.remove_above(new_max);
                if (y_dom.empty()) return PresolveResult::Contradiction;
                changed = true;
            }
        } else {
            std::vector<Domain::value_type> buf;
            x_dom.copy_values_to(buf);
            for (auto v : buf) {
                if (!y_dom.contains(v)) {
                    if (!model.variable(x_id_)->remove(v)) return PresolveResult::Contradiction;
                    changed = true;
                }
            }
            y_dom.copy_values_to(buf);
            for (auto v : buf) {
                if (!x_dom.contains(v)) {
                    if (!model.variable(y_id_)->remove(v)) return PresolveResult::Contradiction;
                    changed = true;
                }
            }
        }
    }

    // If x and y are both singletons, fix b
    if (model.variable(x_id_)->is_assigned() && model.variable(y_id_)->is_assigned() && !model.variable(b_id_)->is_assigned()) {
        bool ne = (model.variable(x_id_)->assigned_value() != model.variable(y_id_)->assigned_value());
        if (!model.variable(b_id_)->domain().contains(ne ? 1 : 0)) {
            return PresolveResult::Contradiction;
        }
        model.variable(b_id_)->assign(ne ? 1 : 0);
        changed = true;
    }

    // If y is a singleton and x's domain doesn't contain y's value, then b = 1
    if (!model.variable(b_id_)->is_assigned() && model.variable(y_id_)->is_assigned()) {
        auto y_val = model.variable(y_id_)->assigned_value().value();
        if (!model.variable(x_id_)->domain().contains(y_val)) {
            if (!model.variable(b_id_)->domain().contains(1)) {
                return PresolveResult::Contradiction;
            }
            model.variable(b_id_)->assign(1);
            changed = true;
        }
    }

    // If x is a singleton and y's domain doesn't contain x's value, then b = 1
    if (!model.variable(b_id_)->is_assigned() && model.variable(x_id_)->is_assigned()) {
        auto x_val = model.variable(x_id_)->assigned_value().value();
        if (!model.variable(y_id_)->domain().contains(x_val)) {
            if (!model.variable(b_id_)->domain().contains(1)) {
                return PresolveResult::Contradiction;
            }
            model.variable(b_id_)->assign(1);
            changed = true;
        }
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool IntNeReifConstraint::on_instantiate(Model& model, int save_point,
                                          size_t var_idx, size_t internal_var_idx, Domain::value_type value,
                                          Domain::value_type prev_min,
                                          Domain::value_type prev_max) {
    // 基底クラスの 2WL 処理
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // 伝播ロジック（キューイング）
    if (model.is_instantiated(b_id_)) {
        if (model.value(b_id_) == 1) {
            // x != y を強制
            if (model.is_instantiated(x_id_)) {
                model.enqueue_remove_value(y_id_, model.value(x_id_));
            }
            if (model.is_instantiated(y_id_)) {
                model.enqueue_remove_value(x_id_, model.value(y_id_));
            }
        } else {
            // x == y を強制
            if (model.is_instantiated(x_id_) && !model.is_instantiated(y_id_)) {
                auto val = model.value(x_id_);
                if (!model.contains(y_id_, val)) {
                    return false;
                }
                model.enqueue_instantiate(y_id_, val);
            }
            if (model.is_instantiated(y_id_) && !model.is_instantiated(x_id_)) {
                auto val = model.value(y_id_);
                if (!model.contains(x_id_, val)) {
                    return false;
                }
                model.enqueue_instantiate(x_id_, val);
            }
        }
    }

    // x と y が両方確定したら b を決定
    if (model.is_instantiated(x_id_) && model.is_instantiated(y_id_) && !model.is_instantiated(b_id_)) {
        bool ne = (model.value(x_id_) != model.value(y_id_));
        model.enqueue_instantiate(b_id_, ne ? 1 : 0);
    }

    return true;
}

bool IntNeReifConstraint::on_final_instantiate(const Model& model) {
    bool ne = (model.value(x_id_) != model.value(y_id_));
    return ne == (model.value(b_id_) == 1);
}

bool IntNeReifConstraint::on_set_min(Model& model, int /*save_point*/,
                                      size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type new_min,
                                      Domain::value_type /*old_min*/) {
    // (x != y) <-> b
    if (!model.is_instantiated(b_id_)) {
        // bounds で x == y が不可能かチェック → b = 1
        auto x_min = model.var_min(x_id_);
        auto x_max = model.var_max(x_id_);
        auto y_min = model.var_min(y_id_);
        auto y_max = model.var_max(y_id_);
        if (x_min > y_max || x_max < y_min) {
            model.enqueue_instantiate(b_id_, 1);
        }
    } else if (model.value(b_id_) == 0) {
        // b = 0 → x == y: bounds を相互伝播
        if (var_idx == x_id_) {
            model.enqueue_set_min(y_id_, new_min);
        } else if (var_idx == y_id_) {
            model.enqueue_set_min(x_id_, new_min);
        }
    }
    // b = 1: bounds だけでは伝播不可
    return true;
}

bool IntNeReifConstraint::on_set_max(Model& model, int /*save_point*/,
                                      size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type new_max,
                                      Domain::value_type /*old_max*/) {
    // (x != y) <-> b
    if (!model.is_instantiated(b_id_)) {
        auto x_min = model.var_min(x_id_);
        auto x_max = model.var_max(x_id_);
        auto y_min = model.var_min(y_id_);
        auto y_max = model.var_max(y_id_);
        if (x_min > y_max || x_max < y_min) {
            model.enqueue_instantiate(b_id_, 1);
        }
    } else if (model.value(b_id_) == 0) {
        // b = 0 → x == y: bounds を相互伝播
        if (var_idx == x_id_) {
            model.enqueue_set_max(y_id_, new_max);
        } else if (var_idx == y_id_) {
            model.enqueue_set_max(x_id_, new_max);
        }
    }
    // b = 1: bounds だけでは伝播不可
    return true;
}

bool IntNeReifConstraint::on_remove_value(Model& model, int /*save_point*/,
                                           size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type removed_value) {
    (void)removed_value;

    // x または y から値が削除された場合、b を更新
    // int_ne_reif: (x != y) <-> b
    // y がシングルトンで、その値が x から削除された場合、x != y は確定で真、よって b = 1
    if (!model.is_instantiated(b_id_)) {
        if (model.is_instantiated(y_id_) && var_idx == x_id_) {
            auto y_val = model.value(y_id_);
            // x の現在のドメインに y_val がない場合、x != y は確定、b = 1
            if (!model.contains(x_id_, y_val)) {
                model.enqueue_instantiate(b_id_, 1);
            }
        }
        if (model.is_instantiated(x_id_) && var_idx == y_id_) {
            auto x_val = model.value(x_id_);
            // y の現在のドメインに x_val がない場合、x != y は確定、b = 1
            if (!model.contains(y_id_, x_val)) {
                model.enqueue_instantiate(b_id_, 1);
            }
        }
    }

    return true;
}

// ============================================================================
// IntLtConstraint implementation
// ============================================================================

IntLtConstraint::IntLtConstraint(VariablePtr x, VariablePtr y)
    : Constraint(extract_var_ids({x, y}))
    , x_id_(x->id())
    , y_id_(y->id()) {
}

std::string IntLtConstraint::name() const {
    return "int_lt";
}

PresolveResult IntLtConstraint::presolve(Model& model) {
    bool changed = false;
    // x < y means x.max < y and y > x.min
    auto y_max = model.var_max(y_id_);
    if (model.var_max(x_id_) > y_max - 1) {
        if (!model.variable(x_id_)->remove_above(y_max - 1)) return PresolveResult::Contradiction;
        changed = true;
    }
    auto x_min = model.var_min(x_id_);
    if (model.var_min(y_id_) < x_min + 1) {
        if (!model.variable(y_id_)->remove_below(x_min + 1)) return PresolveResult::Contradiction;
        changed = true;
    }
    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool IntLtConstraint::on_instantiate(Model& model, int save_point,
                                      size_t var_idx, size_t internal_var_idx, Domain::value_type value,
                                      Domain::value_type prev_min,
                                      Domain::value_type prev_max) {
    // 基底クラスの 2WL 処理
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // x < y: x が確定したら y の下限を更新（キューイング）
    if (model.is_instantiated(x_id_)) {
        auto x_val = model.value(x_id_);
        // y > x_val なので y の下限は x_val + 1
        model.enqueue_set_min(y_id_, x_val + 1);
    }

    // y が確定したら x の上限を更新（キューイング）
    if (model.is_instantiated(y_id_)) {
        auto y_val = model.value(y_id_);
        // x < y_val なので x の上限は y_val - 1
        model.enqueue_set_max(x_id_, y_val - 1);
    }

    return true;
}

bool IntLtConstraint::on_set_min(Model& model, int /*save_point*/,
                                  size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type new_min,
                                  Domain::value_type /*old_min*/) {
    // x < y
    // x.min が上がった → y.min >= x.min + 1
    if (var_idx == x_id_) {
        model.enqueue_set_min(y_id_, new_min + 1);
    }
    // y.min が上がっても x への制約は変わらない
    return true;
}

bool IntLtConstraint::on_set_max(Model& model, int /*save_point*/,
                                  size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type new_max,
                                  Domain::value_type /*old_max*/) {
    // x < y
    // y.max が下がった → x.max <= y.max - 1
    if (var_idx == y_id_) {
        model.enqueue_set_max(x_id_, new_max - 1);
    }
    // x.max が下がっても y への制約は変わらない
    return true;
}

bool IntLtConstraint::on_final_instantiate(const Model& model) {
    return model.value(x_id_) < model.value(y_id_);
}

// ============================================================================
// IntLeConstraint implementation
// ============================================================================

IntLeConstraint::IntLeConstraint(VariablePtr x, VariablePtr y)
    : Constraint(extract_var_ids({x, y}))
    , x_id_(x->id())
    , y_id_(y->id()) {
}

std::string IntLeConstraint::name() const {
    return "int_le";
}

PresolveResult IntLeConstraint::presolve(Model& model) {
    bool changed = false;
    // x <= y
    auto y_max = model.var_max(y_id_);
    if (model.var_max(x_id_) > y_max) {
        if (!model.variable(x_id_)->remove_above(y_max)) return PresolveResult::Contradiction;
        changed = true;
    }
    auto x_min = model.var_min(x_id_);
    if (model.var_min(y_id_) < x_min) {
        if (!model.variable(y_id_)->remove_below(x_min)) return PresolveResult::Contradiction;
        changed = true;
    }
    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool IntLeConstraint::on_instantiate(Model& model, int save_point,
                                      size_t var_idx, size_t internal_var_idx, Domain::value_type value,
                                      Domain::value_type prev_min,
                                      Domain::value_type prev_max) {
    // 基底クラスの 2WL 処理
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // x <= y: x が確定したら y の下限を更新（キューイング）
    if (model.is_instantiated(x_id_)) {
        auto x_val = model.value(x_id_);
        // y >= x_val
        model.enqueue_set_min(y_id_, x_val);
    }

    // y が確定したら x の上限を更新（キューイング）
    if (model.is_instantiated(y_id_)) {
        auto y_val = model.value(y_id_);
        // x <= y_val
        model.enqueue_set_max(x_id_, y_val);
    }

    return true;
}

bool IntLeConstraint::on_set_min(Model& model, int /*save_point*/,
                                  size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type new_min,
                                  Domain::value_type /*old_min*/) {
    // x <= y
    // x.min が上がった → y.min >= x.min
    if (var_idx == x_id_) {
        model.enqueue_set_min(y_id_, new_min);
    }
    return true;
}

bool IntLeConstraint::on_set_max(Model& model, int /*save_point*/,
                                  size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type new_max,
                                  Domain::value_type /*old_max*/) {
    // x <= y
    // y.max が下がった → x.max <= y.max
    if (var_idx == y_id_) {
        model.enqueue_set_max(x_id_, new_max);
    }
    return true;
}

bool IntLeConstraint::on_final_instantiate(const Model& model) {
    return model.value(x_id_) <= model.value(y_id_);
}

// ============================================================================
// IntLeReifConstraint implementation
// ============================================================================

IntLeReifConstraint::IntLeReifConstraint(VariablePtr x, VariablePtr y, VariablePtr b)
    : Constraint(extract_var_ids({x, y, b}))
    , x_id_(x->id())
    , y_id_(y->id())
    , b_id_(b->id()) {
}

std::string IntLeReifConstraint::name() const {
    return "int_le_reif";
}

PresolveResult IntLeReifConstraint::presolve(Model& model) {
    bool changed = false;
    // If b is fixed to 1, enforce x <= y
    if (model.variable(b_id_)->is_assigned() && model.variable(b_id_)->assigned_value().value() == 1) {
        auto y_max = model.var_max(y_id_);
        if (model.var_max(x_id_) > y_max) {
            if (!model.variable(x_id_)->remove_above(y_max)) return PresolveResult::Contradiction;
            changed = true;
        }
        auto x_min = model.var_min(x_id_);
        if (model.var_min(y_id_) < x_min) {
            if (!model.variable(y_id_)->remove_below(x_min)) return PresolveResult::Contradiction;
            changed = true;
        }
    }

    // If b is fixed to 0, enforce x > y
    if (model.variable(b_id_)->is_assigned() && model.variable(b_id_)->assigned_value().value() == 0) {
        auto y_min = model.var_min(y_id_);
        if (model.var_min(x_id_) < y_min + 1) {
            if (!model.variable(x_id_)->remove_below(y_min + 1)) return PresolveResult::Contradiction;
            changed = true;
        }
        auto x_max = model.var_max(x_id_);
        if (model.var_max(y_id_) > x_max - 1) {
            if (!model.variable(y_id_)->remove_above(x_max - 1)) return PresolveResult::Contradiction;
            changed = true;
        }
    }

    // If x and y bounds determine the relation, fix b
    auto x_max = model.var_max(x_id_);
    auto y_min = model.var_min(y_id_);
    auto x_min = model.var_min(x_id_);
    auto y_max = model.var_max(y_id_);

    if (x_max <= y_min) {
        if (!model.variable(b_id_)->domain().contains(1)) {
            return PresolveResult::Contradiction;
        }
        if (!model.variable(b_id_)->is_assigned()) {
            model.variable(b_id_)->assign(1);
            changed = true;
        }
    } else if (x_min > y_max) {
        if (!model.variable(b_id_)->domain().contains(0)) {
            return PresolveResult::Contradiction;
        }
        if (!model.variable(b_id_)->is_assigned()) {
            model.variable(b_id_)->assign(0);
            changed = true;
        }
    }

    if (model.variable(x_id_)->domain().empty() || model.variable(y_id_)->domain().empty() || model.variable(b_id_)->domain().empty()) {
        return PresolveResult::Contradiction;
    }
    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool IntLeReifConstraint::on_instantiate(Model& model, int save_point,
                                          size_t var_idx, size_t internal_var_idx, Domain::value_type value,
                                          Domain::value_type prev_min,
                                          Domain::value_type prev_max) {
    // 基底クラスの 2WL 処理
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // b が確定した場合の伝播（キューイング）
    if (model.is_instantiated(b_id_)) {
        if (model.value(b_id_) == 1) {
            // x <= y を強制
            if (model.is_instantiated(x_id_)) {
                auto x_val = model.value(x_id_);
                // y >= x_val
                model.enqueue_set_min(y_id_, x_val);
            }
            if (model.is_instantiated(y_id_)) {
                auto y_val = model.value(y_id_);
                // x <= y_val
                model.enqueue_set_max(x_id_, y_val);
            }
        } else {
            // x > y を強制
            if (model.is_instantiated(x_id_)) {
                auto x_val = model.value(x_id_);
                // y < x_val, つまり y <= x_val - 1
                model.enqueue_set_max(y_id_, x_val - 1);
            }
            if (model.is_instantiated(y_id_)) {
                auto y_val = model.value(y_id_);
                // x > y_val, つまり x >= y_val + 1
                model.enqueue_set_min(x_id_, y_val + 1);
            }
        }
    }

    // x と y の bounds から b を決定できるか
    auto x_max = model.var_max(x_id_);
    auto y_min = model.var_min(y_id_);
    auto x_min = model.var_min(x_id_);
    auto y_max = model.var_max(y_id_);

    if (!model.is_instantiated(b_id_)) {
        if (x_max <= y_min) {
            // x <= y is always true
            model.enqueue_instantiate(b_id_, 1);
        } else if (x_min > y_max) {
            // x <= y is always false
            model.enqueue_instantiate(b_id_, 0);
        }
    }

    // x と y が両方確定したら b を決定
    if (model.is_instantiated(x_id_) && model.is_instantiated(y_id_) && !model.is_instantiated(b_id_)) {
        bool le = (model.value(x_id_) <= model.value(y_id_));
        model.enqueue_instantiate(b_id_, le ? 1 : 0);
    }

    return true;
}

bool IntLeReifConstraint::on_set_min(Model& model, int /*save_point*/,
                                      size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type /*new_min*/,
                                      Domain::value_type /*old_min*/) {
    // (x <= y) <-> b
    auto x_max = model.var_max(x_id_);
    auto y_min = model.var_min(y_id_);
    auto x_min = model.var_min(x_id_);
    auto y_max = model.var_max(y_id_);

    if (!model.is_instantiated(b_id_)) {
        if (x_max <= y_min) {
            model.enqueue_instantiate(b_id_, 1);
        } else if (x_min > y_max) {
            model.enqueue_instantiate(b_id_, 0);
        }
    } else if (model.value(b_id_) == 1) {
        // x <= y: x.min が上がったら y.min も上がる
        if (var_idx == x_id_) {
            model.enqueue_set_min(y_id_, x_min);
        }
    } else {
        // b = 0 → x > y: y.min が上がったら x.min も上がる (x >= y.min + 1)
        if (var_idx == y_id_) {
            model.enqueue_set_min(x_id_, y_min + 1);
        }
    }
    return true;
}

bool IntLeReifConstraint::on_set_max(Model& model, int /*save_point*/,
                                      size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type /*new_max*/,
                                      Domain::value_type /*old_max*/) {
    // (x <= y) <-> b
    auto x_max = model.var_max(x_id_);
    auto y_min = model.var_min(y_id_);
    auto x_min = model.var_min(x_id_);
    auto y_max = model.var_max(y_id_);

    if (!model.is_instantiated(b_id_)) {
        if (x_max <= y_min) {
            model.enqueue_instantiate(b_id_, 1);
        } else if (x_min > y_max) {
            model.enqueue_instantiate(b_id_, 0);
        }
    } else if (model.value(b_id_) == 1) {
        // x <= y: y.max が下がったら x.max も下がる
        if (var_idx == y_id_) {
            model.enqueue_set_max(x_id_, y_max);
        }
    } else {
        // b = 0 → x > y: x.max が下がったら y.max も下がる (y <= x.max - 1)
        if (var_idx == x_id_) {
            model.enqueue_set_max(y_id_, x_max - 1);
        }
    }
    return true;
}

bool IntLeReifConstraint::on_final_instantiate(const Model& model) {
    bool le = (model.value(x_id_) <= model.value(y_id_));
    return le == (model.value(b_id_) == 1);
}

// ============================================================================
// IntMaxConstraint implementation
// ============================================================================

IntMaxConstraint::IntMaxConstraint(VariablePtr x, VariablePtr y, VariablePtr m)
    : Constraint(extract_var_ids({x, y, m}))
    , x_id_(x->id())
    , y_id_(y->id())
    , m_id_(m->id()) {
}

std::string IntMaxConstraint::name() const {
    return "int_max";
}

PresolveResult IntMaxConstraint::presolve(Model& model) {
    bool changed = false;
    auto x_min = model.var_min(x_id_);
    auto x_max = model.var_max(x_id_);
    auto y_min = model.var_min(y_id_);
    auto y_max = model.var_max(y_id_);

    auto m_lower = std::max(x_min, y_min);
    auto m_upper = std::max(x_max, y_max);

    if (model.var_min(m_id_) < m_lower) {
        if (!model.variable(m_id_)->remove_below(m_lower)) return PresolveResult::Contradiction;
        changed = true;
    }
    if (model.var_max(m_id_) > m_upper) {
        if (!model.variable(m_id_)->remove_above(m_upper)) return PresolveResult::Contradiction;
        changed = true;
    }

    auto m_max = model.var_max(m_id_);
    if (x_max > m_max) {
        if (!model.variable(x_id_)->remove_above(m_max)) return PresolveResult::Contradiction;
        changed = true;
    }
    if (y_max > m_max) {
        if (!model.variable(y_id_)->remove_above(m_max)) return PresolveResult::Contradiction;
        changed = true;
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool IntMaxConstraint::on_instantiate(Model& model, int save_point,
                                       size_t var_idx, size_t internal_var_idx, Domain::value_type value,
                                       Domain::value_type prev_min,
                                       Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // m が確定した場合
    if (model.is_instantiated(m_id_)) {
        auto m_val = model.value(m_id_);

        // x.max と y.max を m に制限
        if (!model.is_instantiated(x_id_)) {
            auto x_max = model.var_max(x_id_);
            if (x_max > m_val) {
                model.enqueue_set_max(x_id_, m_val);
            }
        }
        if (!model.is_instantiated(y_id_)) {
            auto y_max = model.var_max(y_id_);
            if (y_max > m_val) {
                model.enqueue_set_max(y_id_, m_val);
            }
        }

        // x または y が確定していて m と等しい場合は OK
        // 両方確定していて max != m なら矛盾
        if (model.is_instantiated(x_id_) && model.is_instantiated(y_id_)) {
            auto x_val = model.value(x_id_);
            auto y_val = model.value(y_id_);
            if (std::max(x_val, y_val) != m_val) {
                return false;
            }
        }
        // 片方だけ確定している場合
        else if (model.is_instantiated(x_id_)) {
            auto x_val = model.value(x_id_);
            if (x_val == m_val) {
                // y <= m で OK
            } else {
                // y == m が必要
                if (!model.contains(y_id_, m_val)) {
                    return false;
                }
            }
        } else if (model.is_instantiated(y_id_)) {
            auto y_val = model.value(y_id_);
            if (y_val == m_val) {
                // x <= m で OK
            } else {
                // x == m が必要
                if (!model.contains(x_id_, m_val)) {
                    return false;
                }
            }
        } else {
            // 両方未確定: 少なくとも一方が m になれる必要
            if (!model.contains(x_id_, m_val) && !model.contains(y_id_, m_val)) {
                return false;
            }
        }
    }

    // x と y が両方確定した場合、m を確定
    if (model.is_instantiated(x_id_) && model.is_instantiated(y_id_) && !model.is_instantiated(m_id_)) {
        auto x_val = model.value(x_id_);
        auto y_val = model.value(y_id_);
        auto max_val = std::max(x_val, y_val);
        model.enqueue_instantiate(m_id_, max_val);
    }

    // x または y が確定した場合、m の下限を更新
    if (model.is_instantiated(x_id_) || model.is_instantiated(y_id_)) {
        auto x_min_val = model.is_instantiated(x_id_) ? model.value(x_id_) : model.var_min(x_id_);
        auto y_min_val = model.is_instantiated(y_id_) ? model.value(y_id_) : model.var_min(y_id_);
        auto new_m_min = std::max(x_min_val, y_min_val);

        if (!model.is_instantiated(m_id_)) {
            auto m_min = model.var_min(m_id_);
            if (m_min < new_m_min) {
                model.enqueue_set_min(m_id_, new_m_min);
            }
        }
    }

    return true;
}

bool IntMaxConstraint::on_set_min(Model& model, int /*save_point*/,
                                   size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type new_min,
                                   Domain::value_type /*old_min*/) {
    // m = max(x, y)
    // x.min or y.min が上がった → m.min >= max(x.min, y.min)
    if (var_idx == x_id_ || var_idx == y_id_) {
        auto x_min = model.var_min(x_id_);
        auto y_min = model.var_min(y_id_);
        model.enqueue_set_min(m_id_, std::max(x_min, y_min));
    }
    // m.min が上がっても x, y には影響しない
    return true;
}

bool IntMaxConstraint::on_set_max(Model& model, int /*save_point*/,
                                   size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type new_max,
                                   Domain::value_type /*old_max*/) {
    // m = max(x, y)
    if (var_idx == x_id_ || var_idx == y_id_) {
        // x.max or y.max が下がった → m.max <= max(x.max, y.max)
        auto x_max = model.var_max(x_id_);
        auto y_max = model.var_max(y_id_);
        model.enqueue_set_max(m_id_, std::max(x_max, y_max));
    } else if (var_idx == m_id_) {
        // m.max が下がった → x.max <= m.max, y.max <= m.max
        model.enqueue_set_max(x_id_, new_max);
        model.enqueue_set_max(y_id_, new_max);
    }
    return true;
}

bool IntMaxConstraint::on_final_instantiate(const Model& model) {
    auto x_val = model.value(x_id_);
    auto y_val = model.value(y_id_);
    auto m_val = model.value(m_id_);
    return m_val == std::max(x_val, y_val);
}

// ============================================================================
// IntMinConstraint implementation
// ============================================================================

IntMinConstraint::IntMinConstraint(VariablePtr x, VariablePtr y, VariablePtr m)
    : Constraint(extract_var_ids({x, y, m}))
    , x_id_(x->id())
    , y_id_(y->id())
    , m_id_(m->id()) {
}

std::string IntMinConstraint::name() const {
    return "int_min";
}

PresolveResult IntMinConstraint::presolve(Model& model) {
    bool changed = false;
    auto x_min = model.var_min(x_id_);
    auto x_max = model.var_max(x_id_);
    auto y_min = model.var_min(y_id_);
    auto y_max = model.var_max(y_id_);

    auto m_lower = std::min(x_min, y_min);
    auto m_upper = std::min(x_max, y_max);

    if (model.var_min(m_id_) < m_lower) {
        if (!model.variable(m_id_)->remove_below(m_lower)) return PresolveResult::Contradiction;
        changed = true;
    }
    if (model.var_max(m_id_) > m_upper) {
        if (!model.variable(m_id_)->remove_above(m_upper)) return PresolveResult::Contradiction;
        changed = true;
    }

    auto m_min_val = model.var_min(m_id_);
    if (x_min < m_min_val) {
        if (!model.variable(x_id_)->remove_below(m_min_val)) return PresolveResult::Contradiction;
        changed = true;
    }
    if (y_min < m_min_val) {
        if (!model.variable(y_id_)->remove_below(m_min_val)) return PresolveResult::Contradiction;
        changed = true;
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool IntMinConstraint::on_instantiate(Model& model, int save_point,
                                       size_t var_idx, size_t internal_var_idx, Domain::value_type value,
                                       Domain::value_type prev_min,
                                       Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // m が確定した場合
    if (model.is_instantiated(m_id_)) {
        auto m_val = model.value(m_id_);

        // x.min と y.min を m に制限
        if (!model.is_instantiated(x_id_)) {
            auto x_min = model.var_min(x_id_);
            if (x_min < m_val) {
                model.enqueue_set_min(x_id_, m_val);
            }
        }
        if (!model.is_instantiated(y_id_)) {
            auto y_min = model.var_min(y_id_);
            if (y_min < m_val) {
                model.enqueue_set_min(y_id_, m_val);
            }
        }

        // x または y が確定していて m と等しい場合は OK
        // 両方確定していて min != m なら矛盾
        if (model.is_instantiated(x_id_) && model.is_instantiated(y_id_)) {
            auto x_val = model.value(x_id_);
            auto y_val = model.value(y_id_);
            if (std::min(x_val, y_val) != m_val) {
                return false;
            }
        }
        // 片方だけ確定している場合
        else if (model.is_instantiated(x_id_)) {
            auto x_val = model.value(x_id_);
            if (x_val == m_val) {
                // y >= m で OK
            } else {
                // y == m が必要
                if (!model.contains(y_id_, m_val)) {
                    return false;
                }
            }
        } else if (model.is_instantiated(y_id_)) {
            auto y_val = model.value(y_id_);
            if (y_val == m_val) {
                // x >= m で OK
            } else {
                // x == m が必要
                if (!model.contains(x_id_, m_val)) {
                    return false;
                }
            }
        } else {
            // 両方未確定: 少なくとも一方が m になれる必要
            if (!model.contains(x_id_, m_val) && !model.contains(y_id_, m_val)) {
                return false;
            }
        }
    }

    // x と y が両方確定した場合、m を確定
    if (model.is_instantiated(x_id_) && model.is_instantiated(y_id_) && !model.is_instantiated(m_id_)) {
        auto x_val = model.value(x_id_);
        auto y_val = model.value(y_id_);
        auto min_val = std::min(x_val, y_val);
        model.enqueue_instantiate(m_id_, min_val);
    }

    // x または y が確定した場合、m の上限を更新
    if (model.is_instantiated(x_id_) || model.is_instantiated(y_id_)) {
        auto x_max_val = model.is_instantiated(x_id_) ? model.value(x_id_) : model.var_max(x_id_);
        auto y_max_val = model.is_instantiated(y_id_) ? model.value(y_id_) : model.var_max(y_id_);
        auto new_m_max = std::min(x_max_val, y_max_val);

        if (!model.is_instantiated(m_id_)) {
            auto m_max = model.var_max(m_id_);
            if (m_max > new_m_max) {
                model.enqueue_set_max(m_id_, new_m_max);
            }
        }
    }

    return true;
}

bool IntMinConstraint::on_set_min(Model& model, int /*save_point*/,
                                   size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type new_min,
                                   Domain::value_type /*old_min*/) {
    // m = min(x, y)
    if (var_idx == m_id_) {
        // m.min が上がった → x.min >= m.min, y.min >= m.min
        model.enqueue_set_min(x_id_, new_min);
        model.enqueue_set_min(y_id_, new_min);
    } else if (var_idx == x_id_ || var_idx == y_id_) {
        // x.min or y.min が上がった → m.min >= min(x.min, y.min)
        auto x_min = model.var_min(x_id_);
        auto y_min = model.var_min(y_id_);
        model.enqueue_set_min(m_id_, std::min(x_min, y_min));
    }
    return true;
}

bool IntMinConstraint::on_set_max(Model& model, int /*save_point*/,
                                   size_t var_idx, size_t /*internal_var_idx*/, Domain::value_type new_max,
                                   Domain::value_type /*old_max*/) {
    // m = min(x, y)
    // x.max or y.max が下がった → m.max <= min(x.max, y.max)
    if (var_idx == x_id_ || var_idx == y_id_) {
        auto x_max = model.var_max(x_id_);
        auto y_max = model.var_max(y_id_);
        model.enqueue_set_max(m_id_, std::min(x_max, y_max));
    }
    // m.max が下がっても x, y には影響しない
    return true;
}

bool IntMinConstraint::on_final_instantiate(const Model& model) {
    auto x_val = model.value(x_id_);
    auto y_val = model.value(y_id_);
    auto m_val = model.value(m_id_);
    return m_val == std::min(x_val, y_val);
}

} // namespace sabori_csp
