#include "sabori_csp/constraints/global.hpp"
#include "lin_term_order.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>
#include <set>
#include <limits>

namespace sabori_csp {

// ============================================================================
// IntLinEqReifConstraint implementation
// ============================================================================

IntLinEqReifConstraint::IntLinEqReifConstraint(std::vector<int64_t> coeffs,
                                                 std::vector<VariablePtr> vars,
                                                 int64_t target,
                                                 VariablePtr b)
    : Constraint()
    , target_(target)
    , current_fixed_sum_(0)
    , min_rem_potential_(0)
    , max_rem_potential_(0)
    , unfixed_count_(0) {
    b_id_ = b->id();

    // 同一変数の係数を集約
    // 同一変数の係数を集約（初出順を保持）。
    // 注意: unordered_map のイテレーション順はポインタ値（ヒープアドレス）依存で
    // 非決定的なため、変数順として使ってはならない（ビルド毎に探索が変わるバグの元）
    std::unordered_map<Variable*, int64_t> aggregated;
    std::vector<Variable*> first_seen;
    for (size_t i = 0; i < vars.size(); ++i) {
        auto [it, inserted] = aggregated.try_emplace(vars[i], coeffs[i]);
        if (inserted) first_seen.push_back(vars[i]);
        else it->second += coeffs[i];
    }

    // 一意な変数リストと係数リストを初出順で再構築（係数が0の変数は除外）
    std::vector<VariablePtr> unique_vars;
    for (auto* var_ptr : first_seen) {
        const int64_t coeff = aggregated[var_ptr];
        if (coeff == 0) continue;
        unique_vars.push_back(var_ptr);
        coeffs_.push_back(coeff);
    }
    detail::apply_lin_term_order(unique_vars, coeffs_);


    // 全ての係数が0になった場合: b ↔ (0 == target)
    // この場合は presolve で b を確定させる
    if (coeffs_.empty()) {
        unique_vars.push_back(b);
        var_ids_ = extract_var_ids(unique_vars);
        return;
    }

    // b を末尾に追加
    unique_vars.push_back(b);

    // 変数IDキャッシュを構築
    var_ids_ = extract_var_ids(unique_vars);

    // 注意: 内部状態（current_fixed_sum_, unfixed_count_ 等）は presolve() で初期化
    // コンストラクタでは変数の状態を参照しない
}

std::string IntLinEqReifConstraint::name() const {
    return "int_lin_eq_reif";
}

PresolveResult IntLinEqReifConstraint::presolve(Model& model) {
    bool changed = false;
    // キャッシュ値ではなく変数ドメインから毎回計算
    // （イベント処理が組み上がる前なのでキャッシュは信頼できない）
    int64_t min_sum = 0;
    int64_t max_sum = 0;
    size_t n_linear = coeffs_.size();
    for (size_t i = 0; i < n_linear; ++i) {
        auto* var = model.variable(var_ids_[i]);
        int64_t c = coeffs_[i];
        if (var->is_assigned()) {
            int64_t v = var->assigned_value().value();
            min_sum += c * v;
            max_sum += c * v;
        } else if (c >= 0) {
            min_sum += c * var->min();
            max_sum += c * var->max();
        } else {
            min_sum += c * var->max();
            max_sum += c * var->min();
        }
    }

    auto* bvar = model.variable(b_id_);

    // b = 1 の場合、sum == target を強制
    if (bvar->is_assigned() && bvar->assigned_value().value() == 1) {
        // target が [min_sum, max_sum] に含まれていなければ矛盾
        if (target_ < min_sum || target_ > max_sum) {
            return PresolveResult::Contradiction;
        }
    }

    // b = 0 の場合、sum != target を強制
    if (bvar->is_assigned() && bvar->assigned_value().value() == 0) {
        // sum が target にしかなりえない場合は矛盾
        if (min_sum == target_ && max_sum == target_) {
            return PresolveResult::Contradiction;
        }
    }

    // bounds から b を推論
    if (!bvar->is_assigned()) {
        if (min_sum == target_ && max_sum == target_) {
            // sum == target が常に真 → b = 1
            if (!bvar->domain().contains(1)) {
                return PresolveResult::Contradiction;
            }
            bvar->assign(1);
            changed = true;
        } else if (target_ < min_sum || target_ > max_sum) {
            // sum == target が常に偽 → b = 0
            if (!bvar->domain().contains(0)) {
                return PresolveResult::Contradiction;
            }
            bvar->assign(0);
            changed = true;
        }
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool IntLinEqReifConstraint::on_instantiate(Model& model, int save_point,
                                              size_t var_idx, size_t internal_var_idx,
                                              Domain::value_type value,
                                              Domain::value_type prev_min,
                                              Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value, prev_min, prev_max)) {
        return false;
    }

    // b が確定した場合
    if (var_idx == b_id_) {
        int64_t min_sum = current_fixed_sum_ + min_rem_potential_;
        int64_t max_sum = current_fixed_sum_ + max_rem_potential_;

        if (value == 1) {
            // sum == target を強制
            if (target_ < min_sum || target_ > max_sum) {
                return false;
            }
        } else {
            // sum != target を強制
            if (min_sum == target_ && max_sum == target_) {
                return false;
            }
        }

        // 全線形変数が既に確定している場合は最終チェック
        if (unfixed_count_ == 0) {
            return on_final_instantiate(model);
        }
        return true;
    }

    // 線形変数が確定した場合
    size_t internal_idx = internal_var_idx;

    // Trail に保存
    save_trail_if_needed(model, save_point);

    // 差分更新
    int64_t c = coeffs_[internal_idx];
    current_fixed_sum_ += c * value;
    if (c >= 0) {
        min_rem_potential_ -= c * prev_min;
        max_rem_potential_ -= c * prev_max;
    } else {
        min_rem_potential_ -= c * prev_max;
        max_rem_potential_ -= c * prev_min;
    }
    --unfixed_count_;

    int64_t min_sum = current_fixed_sum_ + min_rem_potential_;
    int64_t max_sum = current_fixed_sum_ + max_rem_potential_;

    // b が確定している場合の矛盾チェック
    if (model.is_instantiated(b_id_)) {
        if (model.value(b_id_) == 1) {
            if (target_ < min_sum || target_ > max_sum) {
                return false;
            }
        } else {
            if (min_sum == target_ && max_sum == target_) {
                return false;
            }
        }
    } else {
        // b を推論
        if (min_sum == target_ && max_sum == target_) {
            // sum == target が常に真 → b = 1
            model.enqueue_instantiate(b_id_, 1);
        } else if (target_ < min_sum || target_ > max_sum) {
            // sum == target が常に偽 → b = 0
            model.enqueue_instantiate(b_id_, 0);
        }
    }

    // 全線形変数が確定し、かつ b も確定している場合は最終チェック
    if (unfixed_count_ == 0 && model.is_instantiated(b_id_)) {
        return on_final_instantiate(model);
    }

    return true;
}

bool IntLinEqReifConstraint::on_final_instantiate(const Model& model) {
    int64_t sum = 0;
    size_t n_linear = coeffs_.size();
    for (size_t i = 0; i < n_linear; ++i) {
        if (!model.is_instantiated(var_ids_[i])) {
            return true;  // Not ready yet
        }
        sum += coeffs_[i] * model.value(var_ids_[i]);
    }

    if (!model.is_instantiated(b_id_)) {
        return true;  // Not ready yet
    }

    bool eq = (sum == target_);
    return eq == (model.value(b_id_) == 1);
}

void IntLinEqReifConstraint::rewind_to(int save_point) {
    while (!trail_.empty() && trail_.back().first > save_point) {
        const auto& entry = trail_.back().second;
        current_fixed_sum_ = entry.fixed_sum;
        min_rem_potential_ = entry.min_pot;
        max_rem_potential_ = entry.max_pot;
        unfixed_count_ = entry.unfixed_count;
        trail_.pop_back();
    }
}

bool IntLinEqReifConstraint::prepare_propagation(Model& model) {
    // 全ての係数が0の場合の特別処理
    if (coeffs_.empty()) {
        bool trivially_true = (target_ == 0);
        if (model.is_instantiated(b_id_)) {
            bool b_val = (model.value(b_id_) == 1);
            if (b_val != trivially_true) {
                return false;  // 矛盾
            }
        }
        return true;
    }

    // 変数の現在状態に基づいて内部状態を初期化
    current_fixed_sum_ = 0;
    min_rem_potential_ = 0;
    max_rem_potential_ = 0;
    unfixed_count_ = 0;

    // linear variables は var_ids_[0..n-1] （b は var_ids_ の末尾）
    size_t n_linear = coeffs_.size();
    for (size_t i = 0; i < n_linear; ++i) {
        int64_t c = coeffs_[i];

        if (model.is_instantiated(var_ids_[i])) {
            current_fixed_sum_ += c * model.value(var_ids_[i]);
        } else {
            ++unfixed_count_;
            auto min_val = model.var_min(var_ids_[i]);
            auto max_val = model.var_max(var_ids_[i]);

            if (c >= 0) {
                min_rem_potential_ += c * min_val;
                max_rem_potential_ += c * max_val;
            } else {
                min_rem_potential_ += c * max_val;
                max_rem_potential_ += c * min_val;
            }
        }
    }

    // 2WL を初期化
    init_watches();

    // trail をクリア
    trail_.clear();

    // 初期整合性チェック
    int64_t min_sum = current_fixed_sum_ + min_rem_potential_;
    int64_t max_sum = current_fixed_sum_ + max_rem_potential_;

    if (model.is_instantiated(b_id_)) {
        if (model.value(b_id_) == 1) {
            // sum == target が必要
            if (target_ < min_sum || target_ > max_sum) {
                return false;  // 矛盾
            }
        } else {
            // sum != target が必要
            if (min_sum == target_ && max_sum == target_) {
                return false;  // 矛盾
            }
        }
    }

    return true;
}

void IntLinEqReifConstraint::save_trail_if_needed(Model& model, int save_point) {
    if (trail_.empty() || trail_.back().first != save_point) {
        trail_.push_back({save_point, {current_fixed_sum_, min_rem_potential_, max_rem_potential_, unfixed_count_}});
        model.mark_constraint_dirty(model_index(), save_point);
    }
}

bool IntLinEqReifConstraint::on_set_min(Model& model, int save_point,
                                         size_t var_idx, size_t internal_var_idx,
                                         Domain::value_type new_min,
                                         Domain::value_type old_min) {
    if (var_idx == b_id_) return true;  // b_ の変更は無視
    size_t idx = internal_var_idx;
    int64_t c = coeffs_[idx];

    if (c >= 0) {
        save_trail_if_needed(model, save_point);
        min_rem_potential_ += c * (new_min - old_min);
    } else {
        save_trail_if_needed(model, save_point);
        max_rem_potential_ += c * (new_min - old_min);
    }

    int64_t min_sum = current_fixed_sum_ + min_rem_potential_;
    int64_t max_sum = current_fixed_sum_ + max_rem_potential_;

    if (model.is_instantiated(b_id_)) {
        if (model.value(b_id_) == 1 && (target_ < min_sum || target_ > max_sum)) return false;
        if (model.value(b_id_) == 0 && min_sum == target_ && max_sum == target_) return false;
    } else {
        if (min_sum == target_ && max_sum == target_) {
            model.enqueue_instantiate(b_id_, 1);
        } else if (target_ < min_sum || target_ > max_sum) {
            model.enqueue_instantiate(b_id_, 0);
        }
    }
    return true;
}

bool IntLinEqReifConstraint::on_set_max(Model& model, int save_point,
                                         size_t var_idx, size_t internal_var_idx,
                                         Domain::value_type new_max,
                                         Domain::value_type old_max) {
    if (var_idx == b_id_) return true;  // b_ の変更は無視
    size_t idx = internal_var_idx;
    int64_t c = coeffs_[idx];

    if (c >= 0) {
        save_trail_if_needed(model, save_point);
        max_rem_potential_ += c * (new_max - old_max);
    } else {
        save_trail_if_needed(model, save_point);
        min_rem_potential_ += c * (new_max - old_max);
    }

    int64_t min_sum = current_fixed_sum_ + min_rem_potential_;
    int64_t max_sum = current_fixed_sum_ + max_rem_potential_;

    if (model.is_instantiated(b_id_)) {
        if (model.value(b_id_) == 1 && (target_ < min_sum || target_ > max_sum)) return false;
        if (model.value(b_id_) == 0 && min_sum == target_ && max_sum == target_) return false;
    } else {
        if (min_sum == target_ && max_sum == target_) {
            model.enqueue_instantiate(b_id_, 1);
        } else if (target_ < min_sum || target_ > max_sum) {
            model.enqueue_instantiate(b_id_, 0);
        }
    }
    return true;
}

bool IntLinEqReifConstraint::on_remove_value(Model& /*model*/, int /*save_point*/,
                                              size_t /*var_idx*/, size_t /*internal_var_idx*/,
                                              Domain::value_type /*removed_value*/) {
    // 境界変化は solver が on_set_min/on_set_max をディスパッチするため、
    // 内部値の除去では bounds が変わらず potentials も不変。
    return true;
}

// ============================================================================

void IntLinEqReifConstraint::init_activity(const Model& model, double* activity) const {
    int64_t max_abs = 0;
    for (auto c : coeffs_) {
        int64_t a = c < 0 ? -c : c;
        if (a > max_abs) max_abs = a;
    }
    if (max_abs <= 100) return;

    double sum_abs = 0.0;
    for (auto c : coeffs_) {
        sum_abs += std::abs(static_cast<double>(c));
    }

    for (size_t i = 0; i < coeffs_.size(); ++i) {
        size_t vid = var_ids_[i];
        if (!model.is_instantiated(vid)) {
            activity[vid] += std::abs(static_cast<double>(coeffs_[i])) / sum_abs;
        }
    }
}

// ============================================================================
// Conflict learning: 説明生成 (docs-dev/conflict-learning-design.md §6.1/§6.2)
//
// 方式: 理由リテラル(推論時点 T の bounds)から含意される bound を
// 厳密整数演算で再計算し、被説明リテラル以上の強さが出ることを検証する。
// 検証に失敗したら false (decision-approx フォールバック、健全)。
// これにより propagate 側の実装詳細に依存せず、説明のズレは
// 「弱い節」ではなく「フォールバック」として現れる。
// ============================================================================

namespace {
inline int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b, r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
}
inline int64_t ceil_div(int64_t a, int64_t b) {
    int64_t q = a / b, r = a % b;
    return (r != 0 && ((r < 0) == (b < 0))) ? q + 1 : q;
}
}  // namespace

bool IntLinEqReifConstraint::explain(const Model& model, const ExplainContext& ctx,
                                      size_t var_idx, Domain::value_type value,
                                      uint8_t lit_type, uint32_t /*aux*/,
                                      std::vector<Literal>& out) const {
    const size_t n = coeffs_.size();
    if (n == 0) return false;
    const size_t base = out.size();
    auto rollback = [&]() { out.resize(base); return false; };

    auto b_bounds = ctx.bounds_at(b_id_);

    // 推論時点 T の他項の合計範囲を計算しつつ、理由 bounds を積む
    auto sum_others = [&](size_t skip, int64_t& smin, int64_t& smax) {
        smin = 0; smax = 0;
        for (size_t j = 0; j < n; ++j) {
            const size_t vj = var_ids_[j];
            if (vj == skip) continue;
            auto [lo, hi] = ctx.bounds_at(vj);
            if (coeffs_[j] > 0) { smin += coeffs_[j] * lo; smax += coeffs_[j] * hi; }
            else               { smin += coeffs_[j] * hi; smax += coeffs_[j] * lo; }
            // 理由: 両側 bounds (root で真なら分析側で自動的に落ちる)
            out.push_back({vj, lo, Literal::Type::Geq});
            out.push_back({vj, hi, Literal::Type::Leq});
        }
    };

    if (var_idx == b_id_) {
        int64_t smin, smax;
        sum_others(/*skip=*/SIZE_MAX, smin, smax);
        if (value == 0) {
            // [b=0] ← target が到達不能
            if (smin > target_ || smax < target_) return true;
            return rollback();
        }
        // [b=1] ← 合計が target に固定
        if (smin == smax && smin == target_) return true;
        return rollback();
    }

    // x_k への推論。係数とインデックスを特定
    size_t k = SIZE_MAX;
    for (size_t j = 0; j < n; ++j) {
        if (var_ids_[j] == var_idx) { k = j; break; }
    }
    if (k == SIZE_MAX) return false;
    const int64_t a = coeffs_[k];
    if (a == 0) return false;

    // b=1 由来の bounds/Eq 推論のみ説明する (b=0 由来の ≠ 伝播は対象外)
    if (!(b_bounds.first == 1 && b_bounds.second == 1)) return rollback();
    out.push_back({b_id_, 1, Literal::Type::Eq});

    int64_t smin, smax;
    sum_others(var_idx, smin, smax);
    // a*x ∈ [target - smax, target - smin]
    int64_t implied_lo, implied_hi;
    if (a > 0) {
        implied_lo = ceil_div(target_ - smax, a);
        implied_hi = floor_div(target_ - smin, a);
    } else {
        implied_lo = ceil_div(target_ - smin, a);
        implied_hi = floor_div(target_ - smax, a);
    }

    switch (static_cast<Literal::Type>(lit_type)) {
    case Literal::Type::Geq:
        if (implied_lo >= value) return true;
        break;
    case Literal::Type::Leq:
        if (implied_hi <= value) return true;
        break;
    case Literal::Type::Eq:
        if (implied_lo == value && implied_hi == value) return true;
        break;
    }
    return rollback();
}

bool IntLinEqReifConstraint::explain_failure(const Model& model,
                                              std::vector<Literal>& out) const {
    const size_t n = coeffs_.size();
    if (n == 0) return false;
    const size_t base = out.size();
    auto rollback = [&]() { out.resize(base); return false; };

    if (!model.is_instantiated(b_id_)) return false;
    const auto bval = model.value(b_id_);

    // 現在の bounds で合計範囲を計算 (failure の種は現在事実でよい)
    int64_t smin = 0, smax = 0;
    for (size_t j = 0; j < n; ++j) {
        const size_t vj = var_ids_[j];
        const auto lo = model.var_min(vj);
        const auto hi = model.var_max(vj);
        if (coeffs_[j] > 0) { smin += coeffs_[j] * lo; smax += coeffs_[j] * hi; }
        else               { smin += coeffs_[j] * hi; smax += coeffs_[j] * lo; }
        out.push_back({vj, lo, Literal::Type::Geq});
        out.push_back({vj, hi, Literal::Type::Leq});
    }

    if (bval == 1) {
        // b=1 なのに target に到達不能
        if (smin > target_ || smax < target_) {
            out.push_back({b_id_, 1, Literal::Type::Eq});
            return true;
        }
        return rollback();
    }
    // b=0 なのに合計が target に固定
    if (smin == smax && smin == target_) {
        out.push_back({b_id_, 0, Literal::Type::Eq});
        return true;
    }
    return rollback();
}

}  // namespace sabori_csp

