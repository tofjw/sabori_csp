#include "sabori_csp/constraints/logical.hpp"
#include "sabori_csp/model.hpp"
#include <algorithm>

namespace sabori_csp {

// ============================================================================
// ArrayBoolAndConstraint implementation
// ============================================================================

ArrayBoolAndConstraint::ArrayBoolAndConstraint(std::vector<VariablePtr> vars, VariablePtr r)
    : Constraint([&]() {
        auto ids = extract_var_ids(vars);
        ids.push_back(r->id());
        return ids;
    }())
    , n_(vars.size())
    , r_id_(r->id())
    , w1_(0)
    , w2_(n_ > 1 ? 1 : 0) {

    // 変数ID → 内部インデックスマップを構築
    for (size_t i = 0; i < n_; ++i) {
        var_id_to_idx_[var_ids_[i]] = i;
    }
    var_id_to_idx_[r_id_] = n_;

    // 注意: watch は prepare_propagation() で再初期化される
}

std::string ArrayBoolAndConstraint::name() const {
    return "array_bool_and";
}

bool ArrayBoolAndConstraint::prepare_propagation(Model& model) {
    // watch を再初期化: 0 になりうる変数を探す
    w1_ = SIZE_MAX;
    w2_ = SIZE_MAX;
    for (size_t i = 0; i < n_; ++i) {
        if (!model.is_instantiated(var_ids_[i]) || model.value(var_ids_[i]) == 0) {
            if (w1_ == SIZE_MAX) {
                w1_ = i;
            } else if (w2_ == SIZE_MAX) {
                w2_ = i;
                break;
            }
        }
    }
    // watch が2つ見つからなかった場合のフォールバック
    if (w1_ == SIZE_MAX) w1_ = 0;
    if (w2_ == SIZE_MAX) w2_ = (n_ > 1) ? 1 : 0;

    // 2WL を初期化
    init_watches();

    // 初期整合性チェック
    // r = 1 だが bi = 0 が存在する場合は矛盾
    if (model.is_instantiated(r_id_) && model.value(r_id_) == 1) {
        for (size_t i = 0; i < n_; ++i) {
            if (model.is_instantiated(var_ids_[i]) && model.value(var_ids_[i]) == 0) {
                return false;
            }
        }
    }

    // r = 0 だが全ての bi = 1 の場合は矛盾
    if (model.is_instantiated(r_id_) && model.value(r_id_) == 0) {
        bool all_one = true;
        for (size_t i = 0; i < n_; ++i) {
            if (!model.is_instantiated(var_ids_[i]) || model.value(var_ids_[i]) != 1) {
                all_one = false;
                break;
            }
        }
        if (all_one) {
            return false;
        }
    }

    return true;
}

PresolveResult ArrayBoolAndConstraint::presolve(Model& model) {
    bool changed = false;
    auto* r = model.variable(r_id_);

    // 1. bi の中に 0 が確定しているものがあれば r = 0
    for (size_t i = 0; i < n_; ++i) {
        auto* v = model.variable(var_ids_[i]);
        if (v->is_assigned() && v->assigned_value().value() == 0) {
            if (r->is_assigned()) {
                return r->assigned_value().value() == 0
                    ? PresolveResult::Unchanged : PresolveResult::Contradiction;
            }
            if (!r->domain().contains(0)) {
                return PresolveResult::Contradiction;
            }
            r->assign(0);
            return PresolveResult::Changed;
        }
    }

    // 2. 全ての bi = 1 が確定していれば r = 1
    bool all_one = true;
    for (size_t i = 0; i < n_; ++i) {
        auto* v = model.variable(var_ids_[i]);
        if (!v->is_assigned() || v->assigned_value().value() != 1) {
            all_one = false;
            break;
        }
    }
    if (all_one) {
        if (r->is_assigned()) {
            return r->assigned_value().value() == 1
                ? PresolveResult::Unchanged : PresolveResult::Contradiction;
        }
        if (!r->domain().contains(1)) {
            return PresolveResult::Contradiction;
        }
        r->assign(1);
        return PresolveResult::Changed;
    }

    // 3. r = 1 が確定していれば全ての bi = 1
    if (r->is_assigned() && r->assigned_value().value() == 1) {
        for (size_t i = 0; i < n_; ++i) {
            auto* v = model.variable(var_ids_[i]);
            if (!v->is_assigned()) {
                if (!v->domain().contains(1)) {
                    return PresolveResult::Contradiction;
                }
                v->assign(1);
                changed = true;
            } else if (v->assigned_value().value() != 1) {
                return PresolveResult::Contradiction;
            }
        }
    }

    // 4. r = 0 が確定していて、未確定の bi が1つだけなら、その bi = 0
    if (r->is_assigned() && r->assigned_value().value() == 0) {
        size_t unassigned_count = 0;
        size_t last_unassigned = SIZE_MAX;
        bool has_zero = false;

        for (size_t i = 0; i < n_; ++i) {
            auto* v = model.variable(var_ids_[i]);
            if (v->is_assigned()) {
                if (v->assigned_value().value() == 0) {
                    has_zero = true;
                    break;
                }
            } else {
                unassigned_count++;
                last_unassigned = i;
            }
        }

        if (!has_zero && unassigned_count == 1) {
            auto* last_v = model.variable(var_ids_[last_unassigned]);
            if (!last_v->domain().contains(0)) {
                return PresolveResult::Contradiction;
            }
            last_v->assign(0);
            changed = true;
        }
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool ArrayBoolAndConstraint::on_instantiate(Model& model, int save_point,
                                             size_t var_idx, size_t internal_var_idx, Domain::value_type value,
                                             Domain::value_type prev_min,
                                             Domain::value_type prev_max) {
    // 基底クラスの処理
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // O(1) で内部インデックスを取得
    auto it = var_id_to_idx_.find(var_idx);
    if (it == var_id_to_idx_.end()) {
        return true;  // この制約に関係ない変数
    }
    size_t internal_idx = it->second;

    // r が確定した場合
    if (internal_idx == n_) {
        if (value == 1) {
            // r = 1: 全ての bi = 1（キューイング）
            for (size_t i = 0; i < n_; ++i) {
                if (!model.is_instantiated(var_ids_[i])) {
                    model.enqueue_instantiate(var_ids_[i], 1);
                } else if (model.value(var_ids_[i]) != 1) {
                    return false;  // 既に 0 が確定している bi がある
                }
            }
            return true;
        }
        // r = 0: 0 になりうる bi をスキャンし、watch を再初期化
        size_t candidate_count = 0;
        size_t first_candidate = SIZE_MAX;
        size_t second_candidate = SIZE_MAX;
        size_t last_unassigned = SIZE_MAX;
        size_t unassigned_count = 0;

        for (size_t i = 0; i < n_; ++i) {
            if (!model.is_instantiated(var_ids_[i])) {
                candidate_count++;
                unassigned_count++;
                last_unassigned = i;
                if (first_candidate == SIZE_MAX) first_candidate = i;
                else if (second_candidate == SIZE_MAX) second_candidate = i;
            } else if (model.value(var_ids_[i]) == 0) {
                // 既に 0 の bi がある → r = 0 は既に充足
                return true;
            }
        }

        if (candidate_count == 0) {
            // 全 bi = 1 なので AND=1 ≠ r=0 → 矛盾
            return false;
        }

        if (unassigned_count == 1) {
            // 未確定が1つだけ → それを 0 に強制
            model.enqueue_instantiate(var_ids_[last_unassigned], 0);
        }

        // watch を有効な候補に更新
        w1_ = first_candidate;
        w2_ = (second_candidate != SIZE_MAX) ? second_candidate : first_candidate;
        return true;
    }

    // bi が確定した場合
    if (value == 0) {
        // bi = 0 → r = 0（キューイング）
        if (!model.is_instantiated(r_id_)) {
            model.enqueue_instantiate(r_id_, 0);
        } else if (model.value(r_id_) != 0) {
            return false;  // r = 1 だが bi = 0
        }
        return true;
    }

    // bi = 1 が確定した場合
    // 全ての bi = 1 なら r = 1
    bool all_one = true;
    for (size_t i = 0; i < n_; ++i) {
        if (!model.is_instantiated(var_ids_[i])) {
            all_one = false;
            break;
        } else if (model.value(var_ids_[i]) != 1) {
            all_one = false;
            break;
        }
    }
    if (all_one) {
        if (!model.is_instantiated(r_id_)) {
            model.enqueue_instantiate(r_id_, 1);
        } else if (model.value(r_id_) != 1) {
            return false;  // 全 bi=1 だが r=0 → 矛盾
        }
    }

    // r = 0 で bi = 1 が確定した場合: 2WL 処理
    if (model.is_instantiated(r_id_) && model.value(r_id_) == 0) {
        // この bi が watched だった場合、別の候補に移す
        if (internal_idx == w1_ || internal_idx == w2_) {
            size_t watched_idx = (internal_idx == w1_) ? 1 : 2;
            size_t other_watch = (internal_idx == w1_) ? w2_ : w1_;

            // 新しい watch 候補を探す
            size_t new_watch = find_unwatched_candidate(model, w1_, w2_);

            if (new_watch != SIZE_MAX) {
                // 移動可能
                move_watch(model, save_point, watched_idx, new_watch);
            } else {
                // 移動先がない: もう一方の watched 変数をチェック
                if (model.is_instantiated(var_ids_[other_watch])) {
                    if (model.value(var_ids_[other_watch]) == 1) {
                        // 両方の watch が 1 に確定 → 0 になる変数がない → 矛盾
                        // （r = 0 を満たすには少なくとも1つの bi = 0 が必要）
                        return false;
                    }
                    // other_watch = 0 なら OK（r = 0 が満たされる）
                } else {
                    // other_watch が未確定 → それを 0 に確定（キューイング）
                    model.enqueue_instantiate(var_ids_[other_watch], 0);
                }
            }
        }
    }

    return true;
}

bool ArrayBoolAndConstraint::on_final_instantiate(const Model& model) {
    bool and_result = true;
    for (size_t i = 0; i < n_; ++i) {
        if (model.value(var_ids_[i]) == 0) {
            and_result = false;
            break;
        }
    }
    return and_result == (model.value(r_id_) == 1);
}

bool ArrayBoolAndConstraint::on_last_uninstantiated(Model& model, int save_point,
                                                     size_t last_var_internal_idx) {
    (void)save_point;

    if (last_var_internal_idx == n_) {
        // r が最後の未確定変数
        bool all_one = true;
        for (size_t i = 0; i < n_; ++i) {
            if (model.value(var_ids_[i]) == 0) {
                all_one = false;
                break;
            }
        }
        model.enqueue_instantiate(r_id_, all_one ? 1 : 0);
    } else {
        // bi が最後の未確定変数
        if (model.is_instantiated(r_id_)) {
            if (model.value(r_id_) == 1) {
                // r = 1 なら bi = 1
                model.enqueue_instantiate(var_ids_[last_var_internal_idx], 1);
            } else {
                // r = 0 で他の全ての bj = 1 なら bi = 0
                bool others_all_one = true;
                for (size_t i = 0; i < n_; ++i) {
                    if (i != last_var_internal_idx) {
                        if (model.value(var_ids_[i]) == 0) {
                            others_all_one = false;
                            break;
                        }
                    }
                }
                if (others_all_one) {
                    model.enqueue_instantiate(var_ids_[last_var_internal_idx], 0);
                }
            }
        }
    }

    return true;
}

size_t ArrayBoolAndConstraint::find_unwatched_candidate(const Model& model, size_t exclude1, size_t exclude2) const {
    for (size_t i = 0; i < n_; ++i) {
        if (i == exclude1 || i == exclude2) continue;
        // 0 になりうる（未確定 or 0 を含むドメイン）
        if (!model.is_instantiated(var_ids_[i])) {
            return i;
        }
        if (model.value(var_ids_[i]) == 0) {
            return i;  // 既に 0 に確定している
        }
    }
    return SIZE_MAX;
}

void ArrayBoolAndConstraint::move_watch(Model& model, int /*save_point*/, int which_watch, size_t new_idx) {
    // 2WL はバックトラック時に復元不要
    if (which_watch == 1) {
        w1_ = new_idx;
    } else {
        w2_ = new_idx;
    }
}

// ============================================================================
// ArrayBoolOrConstraint implementation
// ============================================================================

ArrayBoolOrConstraint::ArrayBoolOrConstraint(std::vector<VariablePtr> vars, VariablePtr r)
    : Constraint([&]() {
        auto ids = extract_var_ids(vars);
        ids.push_back(r->id());
        return ids;
    }())
    , n_(vars.size())
    , r_id_(r->id())
    , w1_(0)
    , w2_(n_ > 1 ? 1 : 0) {

    // 変数ID → 内部インデックスマップを構築
    for (size_t i = 0; i < n_; ++i) {
        var_id_to_idx_[var_ids_[i]] = i;
    }
    var_id_to_idx_[r_id_] = n_;

    // 注意: watch は prepare_propagation() で再初期化される
}

std::string ArrayBoolOrConstraint::name() const {
    return "array_bool_or";
}

PresolveResult ArrayBoolOrConstraint::presolve(Model& model) {
    bool changed = false;
    auto* r = model.variable(r_id_);

    // 1. bi の中に 1 が確定しているものがあれば r = 1
    for (size_t i = 0; i < n_; ++i) {
        auto* v = model.variable(var_ids_[i]);
        if (v->is_assigned() && v->assigned_value().value() == 1) {
            if (r->is_assigned()) {
                return r->assigned_value().value() == 1
                    ? PresolveResult::Unchanged : PresolveResult::Contradiction;
            }
            if (!r->domain().contains(1)) return PresolveResult::Contradiction;
            r->assign(1);
            return PresolveResult::Changed;
        }
    }

    // 2. 全ての bi = 0 が確定していれば r = 0
    bool all_zero = true;
    for (size_t i = 0; i < n_; ++i) {
        auto* v = model.variable(var_ids_[i]);
        if (!v->is_assigned() || v->assigned_value().value() != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) {
        if (r->is_assigned()) {
            return r->assigned_value().value() == 0
                ? PresolveResult::Unchanged : PresolveResult::Contradiction;
        }
        if (!r->domain().contains(0)) return PresolveResult::Contradiction;
        r->assign(0);
        return PresolveResult::Changed;
    }

    // 3. r = 0 なら全ての bi = 0
    if (r->is_assigned() && r->assigned_value().value() == 0) {
        for (size_t i = 0; i < n_; ++i) {
            auto* v = model.variable(var_ids_[i]);
            if (!v->is_assigned()) {
                if (!v->domain().contains(0)) return PresolveResult::Contradiction;
                v->assign(0);
                changed = true;
            } else if (v->assigned_value().value() != 0) {
                return PresolveResult::Contradiction;
            }
        }
    }

    // 4. r = 1 で未確定の bi が1つだけなら、その bi = 1
    if (r->is_assigned() && r->assigned_value().value() == 1) {
        size_t unassigned_count = 0;
        size_t last_unassigned = SIZE_MAX;
        bool has_one = false;

        for (size_t i = 0; i < n_; ++i) {
            auto* v = model.variable(var_ids_[i]);
            if (v->is_assigned()) {
                if (v->assigned_value().value() == 1) {
                    has_one = true;
                    break;
                }
            } else {
                unassigned_count++;
                last_unassigned = i;
            }
        }

        if (!has_one && unassigned_count == 1) {
            auto* last_v = model.variable(var_ids_[last_unassigned]);
            if (!last_v->domain().contains(1)) return PresolveResult::Contradiction;
            last_v->assign(1);
            changed = true;
        }
    }

    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool ArrayBoolOrConstraint::on_instantiate(Model& model, int save_point,
                                            size_t var_idx, size_t internal_var_idx, Domain::value_type value,
                                            Domain::value_type prev_min,
                                            Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // O(1) で内部インデックスを取得
    auto it = var_id_to_idx_.find(var_idx);
    if (it == var_id_to_idx_.end()) return true;
    size_t internal_idx = it->second;

    // r が確定した場合
    if (internal_idx == n_) {
        if (value == 0) {
            // r = 0: 全ての bi = 0（キューイング）
            for (size_t i = 0; i < n_; ++i) {
                if (!model.is_instantiated(var_ids_[i])) {
                    model.enqueue_instantiate(var_ids_[i], 0);
                } else if (model.value(var_ids_[i]) != 0) {
                    return false;
                }
            }
        } else {
            // r = 1: 1 になりうる bi をスキャンし、watch を再初期化
            size_t candidate_count = 0;
            size_t first_candidate = SIZE_MAX;
            size_t second_candidate = SIZE_MAX;
            size_t last_unassigned = SIZE_MAX;
            size_t unassigned_count = 0;

            for (size_t i = 0; i < n_; ++i) {
                if (!model.is_instantiated(var_ids_[i])) {
                    candidate_count++;
                    unassigned_count++;
                    last_unassigned = i;
                    if (first_candidate == SIZE_MAX) first_candidate = i;
                    else if (second_candidate == SIZE_MAX) second_candidate = i;
                } else if (model.value(var_ids_[i]) == 1) {
                    // 既に 1 の bi がある → r = 1 は既に充足
                    return true;
                }
            }

            if (candidate_count == 0) {
                // 全 bi = 0 なので OR=0 ≠ r=1 → 矛盾
                return false;
            }

            if (unassigned_count == 1) {
                // 未確定が1つだけ → それを 1 に強制
                model.enqueue_instantiate(var_ids_[last_unassigned], 1);
            }

            // watch を有効な候補に更新
            w1_ = first_candidate;
            w2_ = (second_candidate != SIZE_MAX) ? second_candidate : first_candidate;
        }
        return true;
    }

    // bi が確定した場合
    if (value == 1) {
        // bi = 1 → r = 1（キューイング）
        if (!model.is_instantiated(r_id_)) {
            model.enqueue_instantiate(r_id_, 1);
        } else if (model.value(r_id_) != 1) {
            return false;
        }
        return true;
    }

    // bi = 0 が確定した場合
    // 全ての bi = 0 なら r = 0
    bool all_zero = true;
    for (size_t i = 0; i < n_; ++i) {
        if (!model.is_instantiated(var_ids_[i])) {
            all_zero = false;
            break;
        } else if (model.value(var_ids_[i]) != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) {
        if (!model.is_instantiated(r_id_)) {
            model.enqueue_instantiate(r_id_, 0);
        } else if (model.value(r_id_) != 0) {
            return false;  // 全 bi=0 だが r=1 → 矛盾
        }
    }

    // r = 1 で bi = 0 が確定した場合: 2WL 処理
    if (model.is_instantiated(r_id_) && model.value(r_id_) == 1) {
        if (internal_idx == w1_ || internal_idx == w2_) {
            size_t watched_idx = (internal_idx == w1_) ? 1 : 2;
            size_t other_watch = (internal_idx == w1_) ? w2_ : w1_;

            size_t new_watch = find_unwatched_candidate(model, w1_, w2_);

            if (new_watch != SIZE_MAX) {
                move_watch(model, save_point, watched_idx, new_watch);
            } else {
                if (model.is_instantiated(var_ids_[other_watch])) {
                    if (model.value(var_ids_[other_watch]) == 0) {
                        return false;  // 全て 0 になってしまう → r = 1 が満たせない
                    }
                } else {
                    // other_watch が未確定 → それを 1 に確定（キューイング）
                    model.enqueue_instantiate(var_ids_[other_watch], 1);
                }
            }
        }
    }

    return true;
}

bool ArrayBoolOrConstraint::on_final_instantiate(const Model& model) {
    bool or_result = false;
    for (size_t i = 0; i < n_; ++i) {
        if (model.value(var_ids_[i]) == 1) {
            or_result = true;
            break;
        }
    }
    return or_result == (model.value(r_id_) == 1);
}

bool ArrayBoolOrConstraint::on_last_uninstantiated(Model& model, int save_point,
                                                    size_t last_var_internal_idx) {
    (void)save_point;

    if (last_var_internal_idx == n_) {
        bool has_one = false;
        for (size_t i = 0; i < n_; ++i) {
            if (model.value(var_ids_[i]) == 1) {
                has_one = true;
                break;
            }
        }
        model.enqueue_instantiate(r_id_, has_one ? 1 : 0);
    } else {
        if (model.is_instantiated(r_id_)) {
            if (model.value(r_id_) == 0) {
                model.enqueue_instantiate(var_ids_[last_var_internal_idx], 0);
            } else {
                bool others_have_one = false;
                for (size_t i = 0; i < n_; ++i) {
                    if (i != last_var_internal_idx) {
                        if (model.value(var_ids_[i]) == 1) {
                            others_have_one = true;
                            break;
                        }
                    }
                }
                if (!others_have_one) {
                    model.enqueue_instantiate(var_ids_[last_var_internal_idx], 1);
                }
            }
        }
    }

    return true;
}

size_t ArrayBoolOrConstraint::find_unwatched_candidate(const Model& model, size_t exclude1, size_t exclude2) const {
    for (size_t i = 0; i < n_; ++i) {
        if (i == exclude1 || i == exclude2) continue;
        if (!model.is_instantiated(var_ids_[i])) {
            return i;
        }
        if (model.value(var_ids_[i]) == 1) {
            return i;
        }
    }
    return SIZE_MAX;
}

void ArrayBoolOrConstraint::move_watch(Model& model, int /*save_point*/, int which_watch, size_t new_idx) {
    // 2WL はバックトラック時に復元不要
    if (which_watch == 1) {
        w1_ = new_idx;
    } else {
        w2_ = new_idx;
    }
}

// ============================================================================
// BoolClauseConstraint implementation
// ============================================================================

BoolClauseConstraint::BoolClauseConstraint(std::vector<VariablePtr> pos, std::vector<VariablePtr> neg)
    : Constraint([&]() {
        auto ids = extract_var_ids(pos);
        auto nids = extract_var_ids(neg);
        ids.insert(ids.end(), nids.begin(), nids.end());
        return ids;
    }())
    , n_pos_(pos.size())
    , n_neg_(neg.size())
    , w1_(0)
    , w2_(n_pos_ + n_neg_ > 1 ? 1 : 0) {

    // 変数IDキャッシュを構築
    pos_ids_.resize(n_pos_);
    for (size_t i = 0; i < n_pos_; ++i) {
        pos_ids_[i] = pos[i]->id();
    }
    neg_ids_.resize(n_neg_);
    for (size_t i = 0; i < n_neg_; ++i) {
        neg_ids_[i] = neg[i]->id();
    }

    // 変数ID → リテラルインデックスマップを構築
    for (size_t i = 0; i < n_pos_; ++i) {
        var_id_to_lit_idx_[pos_ids_[i]] = i;
    }
    for (size_t i = 0; i < n_neg_; ++i) {
        if (var_id_to_lit_idx_.find(neg_ids_[i]) == var_id_to_lit_idx_.end()) {
            var_id_to_lit_idx_[neg_ids_[i]] = n_pos_ + i;
        }
    }

    // 注意: watch は prepare_propagation() で再初期化される
}

std::string BoolClauseConstraint::name() const {
    return "bool_clause";
}

bool BoolClauseConstraint::prepare_propagation(Model& model) {
    // watch を再初期化: 節を充足しうるリテラルを2つ探す
    w1_ = SIZE_MAX;
    w2_ = SIZE_MAX;
    for (size_t i = 0; i < n_pos_ + n_neg_; ++i) {
        if (can_satisfy(model, i)) {
            if (w1_ == SIZE_MAX) {
                w1_ = i;
            } else if (w2_ == SIZE_MAX) {
                w2_ = i;
                break;
            }
        }
    }

    // watch が見つからなかった場合のフォールバック
    if (w1_ == SIZE_MAX && n_pos_ + n_neg_ > 0) w1_ = 0;
    if (w2_ == SIZE_MAX && n_pos_ + n_neg_ > 1) w2_ = 1;
    if (w2_ == SIZE_MAX) w2_ = w1_;

    // 2WL を初期化
    init_watches();

    // 初期整合性チェック: 全てのリテラルが充足不可能なら矛盾
    bool has_satisfiable = false;
    for (size_t i = 0; i < n_pos_ + n_neg_; ++i) {
        if (can_satisfy(model, i)) {
            has_satisfiable = true;
            break;
        }
    }
    if (!has_satisfiable) {
        return false;
    }

    return true;
}

PresolveResult BoolClauseConstraint::presolve(Model& model) {
    // 既に充足しているかチェック
    for (size_t i = 0; i < n_pos_; ++i) {
        auto* v = model.variable(pos_ids_[i]);
        if (v->is_assigned() && v->assigned_value().value() == 1) {
            return PresolveResult::Unchanged;
        }
    }
    for (size_t i = 0; i < n_neg_; ++i) {
        auto* v = model.variable(neg_ids_[i]);
        if (v->is_assigned() && v->assigned_value().value() == 0) {
            return PresolveResult::Unchanged;
        }
    }

    // 充足可能なリテラルをカウント
    size_t satisfiable_count = 0;
    size_t last_satisfiable = SIZE_MAX;

    for (size_t i = 0; i < n_pos_ + n_neg_; ++i) {
        if (can_satisfy(model, i)) {
            satisfiable_count++;
            last_satisfiable = i;
        }
    }

    if (satisfiable_count == 0) {
        return PresolveResult::Contradiction;
    }

    if (satisfiable_count == 1) {
        auto var_id = get_var_id(last_satisfiable);
        auto* var = model.variable(var_id);
        Domain::value_type val = satisfying_value(last_satisfiable);
        if (!var->is_assigned()) {
            if (!var->domain().contains(val)) return PresolveResult::Contradiction;
            var->assign(val);
            return PresolveResult::Changed;
        }
    }

    return PresolveResult::Unchanged;
}

bool BoolClauseConstraint::on_instantiate(Model& model, int save_point,
                                           size_t var_idx, size_t internal_var_idx, Domain::value_type value,
                                           Domain::value_type prev_min,
                                           Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // O(1) でリテラルインデックスを取得
    auto it = var_id_to_lit_idx_.find(var_idx);
    if (it == var_id_to_lit_idx_.end()) {
        return true;  // この制約に関係ない変数
    }
    size_t assigned_lit = it->second;

    // このリテラルが節を充足したか（O(1)）
    if (is_satisfied_by(model, assigned_lit)) {
        return true;
    }

    // watch が充足していれば節は充足（O(1)）
    if (is_satisfied_by(model, w1_) || is_satisfied_by(model, w2_)) {
        return true;
    }

    // このリテラルが watched だった場合、別の候補に移す
    if (assigned_lit == w1_ || assigned_lit == w2_) {
        size_t watched_idx = (assigned_lit == w1_) ? 1 : 2;
        size_t other_watch = (assigned_lit == w1_) ? w2_ : w1_;

        // 新しい watch 候補を探す
        size_t new_watch = find_unwatched_candidate(model, w1_, w2_);

        if (new_watch != SIZE_MAX) {
            move_watch(model, save_point, watched_idx, new_watch);
        } else {
            // 移動先がない
            if (!can_satisfy(model, other_watch)) {
                // もう一方も充足不可能 → 矛盾
                return false;
            }

            size_t other_var_id = get_var_id(other_watch);
            if (model.is_instantiated(other_var_id)) {
                // 既に確定している場合
                if (!is_satisfied_by(model, other_watch)) {
                    return false;  // 充足していない → 矛盾
                }
            } else {
                // Unit propagation: other_watch を充足方向に確定
                model.enqueue_instantiate(other_var_id, satisfying_value(other_watch));
            }
        }
    }

    return true;
}

bool BoolClauseConstraint::on_final_instantiate(const Model& model) {
    // いずれかのリテラルが充足しているか
    for (size_t i = 0; i < n_pos_; ++i) {
        if (model.value(pos_ids_[i]) == 1) return true;
    }
    for (size_t i = 0; i < n_neg_; ++i) {
        if (model.value(neg_ids_[i]) == 0) return true;
    }
    return false;
}

bool BoolClauseConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                                    size_t last_var_internal_idx) {
    // 既に充足しているかチェック
    for (size_t i = 0; i < n_pos_ + n_neg_; ++i) {
        if (is_satisfied_by(model, i)) {
            return true;
        }
    }

    // 最後の未確定変数を充足方向に確定
    auto last_var_id = var_ids_ref()[last_var_internal_idx];
    size_t last_lit = SIZE_MAX;

    auto it = var_id_to_lit_idx_.find(last_var_id);
    if (it != var_id_to_lit_idx_.end()) {
        last_lit = it->second;
    }

    if (last_lit != SIZE_MAX && can_satisfy(model, last_lit)) {
        model.enqueue_instantiate(last_var_id, satisfying_value(last_lit));
        return true;
    }

    // 最後の変数でも充足できない → 矛盾
    return false;
}

bool BoolClauseConstraint::can_satisfy(const Model& model, size_t lit_idx) const {
    size_t var_id = get_var_id(lit_idx);
    if (lit_idx < n_pos_) {
        return !model.is_instantiated(var_id) || model.value(var_id) == 1;
    } else {
        return !model.is_instantiated(var_id) || model.value(var_id) == 0;
    }
}

bool BoolClauseConstraint::is_satisfied_by(const Model& model, size_t lit_idx) const {
    size_t var_id = get_var_id(lit_idx);
    if (lit_idx < n_pos_) {
        return model.is_instantiated(var_id) && model.value(var_id) == 1;
    } else {
        return model.is_instantiated(var_id) && model.value(var_id) == 0;
    }
}

Domain::value_type BoolClauseConstraint::satisfying_value(size_t lit_idx) const {
    return (lit_idx < n_pos_) ? 1 : 0;
}

size_t BoolClauseConstraint::get_var_id(size_t lit_idx) const {
    if (lit_idx < n_pos_) {
        return pos_ids_[lit_idx];
    } else {
        return neg_ids_[lit_idx - n_pos_];
    }
}

size_t BoolClauseConstraint::find_unwatched_candidate(const Model& model, size_t exclude1, size_t exclude2) const {
    for (size_t i = 0; i < n_pos_ + n_neg_; ++i) {
        if (i == exclude1 || i == exclude2) continue;
        if (can_satisfy(model, i)) {
            return i;
        }
    }
    return SIZE_MAX;
}

void BoolClauseConstraint::move_watch(Model& model, int /*save_point*/, int which_watch, size_t new_idx) {
    // 2WL はバックトラック時に復元不要
    if (which_watch == 1) {
        w1_ = new_idx;
    } else {
        w2_ = new_idx;
    }
}

// ============================================================================
// BoolNotConstraint implementation
// ============================================================================

BoolNotConstraint::BoolNotConstraint(VariablePtr a, VariablePtr b)
    : Constraint(extract_var_ids({a, b}))
    , a_id_(a->id())
    , b_id_(b->id()) {}

std::string BoolNotConstraint::name() const {
    return "bool_not";
}

PresolveResult BoolNotConstraint::presolve(Model& model) {
    bool changed = false;
    // a が確定したら b を決定
    if (model.variable(a_id_)->is_assigned() && !model.variable(b_id_)->is_assigned()) {
        auto val = 1 - model.variable(a_id_)->assigned_value().value();
        if (!model.variable(b_id_)->domain().contains(val)) {
            return PresolveResult::Contradiction;
        }
        model.variable(b_id_)->assign(val);
        changed = true;
    }

    // b が確定したら a を決定
    if (model.variable(b_id_)->is_assigned() && !model.variable(a_id_)->is_assigned()) {
        auto val = 1 - model.variable(b_id_)->assigned_value().value();
        if (!model.variable(a_id_)->domain().contains(val)) {
            return PresolveResult::Contradiction;
        }
        model.variable(a_id_)->assign(val);
        changed = true;
    }

    if (model.variable(a_id_)->domain().empty() || model.variable(b_id_)->domain().empty()) {
        return PresolveResult::Contradiction;
    }
    return changed ? PresolveResult::Changed : PresolveResult::Unchanged;
}

bool BoolNotConstraint::on_instantiate(Model& model, int save_point,
                                        size_t var_idx, size_t internal_var_idx, Domain::value_type value,
                                        Domain::value_type prev_min,
                                        Domain::value_type prev_max) {
    // 基底クラスの処理
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // a が確定したら b を決定（キューイング）
    if (model.is_instantiated(a_id_) && !model.is_instantiated(b_id_)) {
        auto val = 1 - model.value(a_id_);
        if (!model.contains(b_id_, val)) {
            return false;
        }
        model.enqueue_instantiate(b_id_, val);
    }

    // b が確定したら a を決定（キューイング）
    if (model.is_instantiated(b_id_) && !model.is_instantiated(a_id_)) {
        auto val = 1 - model.value(b_id_);
        if (!model.contains(a_id_, val)) {
            return false;
        }
        model.enqueue_instantiate(a_id_, val);
    }

    return true;
}

bool BoolNotConstraint::on_final_instantiate(const Model& model) {
    // a + b = 1 を確認
    return model.value(a_id_) != model.value(b_id_);
}

// ============================================================================
// ArrayBoolXorConstraint implementation
// ============================================================================

ArrayBoolXorConstraint::ArrayBoolXorConstraint(std::vector<VariablePtr> vars)
    : Constraint(extract_var_ids(vars))
    , n_(vars.size()) {

    for (size_t i = 0; i < n_; ++i) {
        var_id_to_idx_[var_ids_[i]] = i;
    }
}

std::string ArrayBoolXorConstraint::name() const {
    return "array_bool_xor";
}

PresolveResult ArrayBoolXorConstraint::presolve(Model& model) {
    size_t assigned_count = 0;
    size_t ones_count = 0;
    size_t last_unassigned = SIZE_MAX;

    for (size_t i = 0; i < n_; ++i) {
        auto* v = model.variable(var_ids_[i]);
        if (v->is_assigned()) {
            assigned_count++;
            if (v->assigned_value().value() == 1) {
                ones_count++;
            }
        } else {
            last_unassigned = i;
        }
    }

    if (assigned_count == n_) {
        // 全確定: パリティチェック（奇数個の1で充足）
        return (ones_count % 2 == 1) ? PresolveResult::Unchanged : PresolveResult::Contradiction;
    }

    if (assigned_count == n_ - 1) {
        // 残り1変数: パリティを合わせる
        // 現在の1の個数が偶数なら残りは1、奇数なら0
        Domain::value_type needed = (ones_count % 2 == 0) ? 1 : 0;
        auto* v = model.variable(var_ids_[last_unassigned]);
        if (!v->domain().contains(needed)) {
            return PresolveResult::Contradiction;
        }
        v->assign(needed);
        return PresolveResult::Changed;
    }

    return PresolveResult::Unchanged;
}

bool ArrayBoolXorConstraint::on_instantiate(Model& model, int save_point,
                                             size_t var_idx, size_t internal_var_idx,
                                             Domain::value_type value,
                                             Domain::value_type prev_min,
                                             Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    // 確定済み変数の数と1の個数をカウント
    size_t assigned_count = 0;
    size_t ones_count = 0;
    size_t last_unassigned = SIZE_MAX;

    for (size_t i = 0; i < n_; ++i) {
        if (model.is_instantiated(var_ids_[i])) {
            assigned_count++;
            if (model.value(var_ids_[i]) == 1) {
                ones_count++;
            }
        } else {
            last_unassigned = i;
        }
    }

    if (assigned_count == n_ - 1 && last_unassigned != SIZE_MAX) {
        // 残り1変数: パリティを合わせる
        Domain::value_type needed = (ones_count % 2 == 0) ? 1 : 0;
        model.enqueue_instantiate(var_ids_[last_unassigned], needed);
    }

    return true;
}

bool ArrayBoolXorConstraint::on_final_instantiate(const Model& model) {
    size_t ones_count = 0;
    for (size_t i = 0; i < n_; ++i) {
        if (model.value(var_ids_[i]) == 1) {
            ones_count++;
        }
    }
    return ones_count % 2 == 1;
}

bool ArrayBoolXorConstraint::on_last_uninstantiated(Model& model, int /*save_point*/,
                                                     size_t last_var_internal_idx) {
    // 他の全変数のパリティを計算
    size_t ones_count = 0;
    for (size_t i = 0; i < n_; ++i) {
        if (i != last_var_internal_idx) {
            if (model.value(var_ids_[i]) == 1) {
                ones_count++;
            }
        }
    }
    Domain::value_type needed = (ones_count % 2 == 0) ? 1 : 0;
    model.enqueue_instantiate(var_ids_[last_var_internal_idx], needed);
    return true;
}

// ============================================================================
// BoolXorConstraint implementation
// ============================================================================

BoolXorConstraint::BoolXorConstraint(VariablePtr a, VariablePtr b, VariablePtr c)
    : Constraint(extract_var_ids({a, b, c}))
    , a_id_(a->id())
    , b_id_(b->id())
    , c_id_(c->id()) {}

std::string BoolXorConstraint::name() const {
    return "bool_xor";
}

PresolveResult BoolXorConstraint::presolve(Model& model) {
    auto* va = model.variable(a_id_);
    auto* vb = model.variable(b_id_);
    auto* vc = model.variable(c_id_);

    int assigned_count = (va->is_assigned() ? 1 : 0)
                       + (vb->is_assigned() ? 1 : 0)
                       + (vc->is_assigned() ? 1 : 0);

    if (assigned_count < 2) {
        return PresolveResult::Unchanged;
    }

    // 2変数以上確定 → 残りを決定
    if (va->is_assigned() && vb->is_assigned()) {
        auto expected = (va->assigned_value().value() != vb->assigned_value().value()) ? 1 : 0;
        if (vc->is_assigned()) {
            return vc->assigned_value().value() == expected
                ? PresolveResult::Unchanged : PresolveResult::Contradiction;
        }
        if (!vc->domain().contains(expected)) return PresolveResult::Contradiction;
        vc->assign(expected);
        return PresolveResult::Changed;
    }
    if (va->is_assigned() && vc->is_assigned()) {
        auto expected = (va->assigned_value().value() != vc->assigned_value().value()) ? 1 : 0;
        if (vb->is_assigned()) {
            return vb->assigned_value().value() == expected
                ? PresolveResult::Unchanged : PresolveResult::Contradiction;
        }
        if (!vb->domain().contains(expected)) return PresolveResult::Contradiction;
        vb->assign(expected);
        return PresolveResult::Changed;
    }
    // vb->is_assigned() && vc->is_assigned()
    {
        auto expected = (vb->assigned_value().value() != vc->assigned_value().value()) ? 1 : 0;
        if (va->is_assigned()) {
            return va->assigned_value().value() == expected
                ? PresolveResult::Unchanged : PresolveResult::Contradiction;
        }
        if (!va->domain().contains(expected)) return PresolveResult::Contradiction;
        va->assign(expected);
        return PresolveResult::Changed;
    }
}

bool BoolXorConstraint::on_instantiate(Model& model, int save_point,
                                        size_t var_idx, size_t internal_var_idx, Domain::value_type value,
                                        Domain::value_type prev_min,
                                        Domain::value_type prev_max) {
    if (!Constraint::on_instantiate(model, save_point, var_idx, internal_var_idx, value,
                                     prev_min, prev_max)) {
        return false;
    }

    bool a_inst = model.is_instantiated(a_id_);
    bool b_inst = model.is_instantiated(b_id_);
    bool c_inst = model.is_instantiated(c_id_);

    int inst_count = (a_inst ? 1 : 0) + (b_inst ? 1 : 0) + (c_inst ? 1 : 0);

    if (inst_count < 2) {
        return true;  // まだ伝播できない
    }

    if (a_inst && b_inst && !c_inst) {
        auto expected = (model.value(a_id_) != model.value(b_id_)) ? 1 : 0;
        model.enqueue_instantiate(c_id_, expected);
    } else if (a_inst && c_inst && !b_inst) {
        auto expected = (model.value(a_id_) != model.value(c_id_)) ? 1 : 0;
        model.enqueue_instantiate(b_id_, expected);
    } else if (b_inst && c_inst && !a_inst) {
        auto expected = (model.value(b_id_) != model.value(c_id_)) ? 1 : 0;
        model.enqueue_instantiate(a_id_, expected);
    }
    // 3つ全部確定: on_final_instantiate でチェック

    return true;
}

bool BoolXorConstraint::on_final_instantiate(const Model& model) {
    return model.value(c_id_) == ((model.value(a_id_) != model.value(b_id_)) ? 1 : 0);
}

} // namespace sabori_csp
